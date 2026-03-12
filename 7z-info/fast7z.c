/*
 * fast7z.c — High-performance 7z archive indexer and selective extractor
 *
 * Zero external dependencies. Embeds LZMA SDK (public domain).
 *
 * DESIGN PRINCIPLES:
 *   1. INDEX: SzArEx_Open reads 32-byte signature, seeks to end, reads
 *      compressed header, decompresses in RAM, parses file table.
 *      Archive payload is NEVER touched. Typically <50ms for 50k+ files.
 *
 *   2. EXTRACT: Uses folder (solid block) caching. Once a block is
 *      decompressed, extracting any other file from the same block is free.
 *      Batch extraction sorts files by folder to minimize decompression.
 *
 *   3. CACHE INDEX: Can serialize the parsed index to a sidecar file
 *      (.f7idx) so subsequent runs skip all I/O and decompression entirely.
 *      Index reload is pure mmap + pointer fixup, typically <1ms.
 *
 * Build:
 *   cc -O3 -o fast7z fast7z.c lzma/ *.c -Ilzma -lpthread
 *
 * Usage:
 *   fast7z ls       <archive> [pattern]          List (verbose)
 *   fast7z find     <archive> <pattern>          Search by substring
 *   fast7z info     <archive>                    Summary + timing
 *   fast7z x        <archive> <path> [outfile]   Extract one file
 *   fast7z dump     <archive> <path>             Extract to stdout
 *   fast7z batch    <archive> <p1> <p2> ...      Extract multiple files
 *   fast7z save-idx <archive>                    Build .f7idx sidecar cache
 *   fast7z folders  <archive>                    Show solid block layout
 *
 * License: Public domain
 */

#include "Precomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#include "CpuArch.h"
#include "7z.h"
#include "7zAlloc.h"
#include "7zBuf.h"
#include "7zCrc.h"
#include "7zFile.h"

/* ── Platform ─────────────────────────────────────────────────────── */

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

/* ── Timing ───────────────────────────────────────────────────────── */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ── UTF-16 to UTF-8 ─────────────────────────────────────────────── */

static size_t utf16_to_utf8(char *dest, size_t dest_sz,
                             const UInt16 *src, size_t src_len) {
    size_t pos = 0;
    for (size_t i = 0; i < src_len && pos + 4 < dest_sz; i++) {
        UInt32 val = src[i];
        if (val >= 0xD800 && val <= 0xDBFF && i + 1 < src_len) {
            UInt32 lo = src[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                val = 0x10000 + ((val - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if      (val < 0x80)    { dest[pos++] = (char)val; }
        else if (val < 0x800)   { dest[pos++] = 0xC0|(val>>6);
                                  dest[pos++] = 0x80|(val&0x3F); }
        else if (val < 0x10000) { dest[pos++] = 0xE0|(val>>12);
                                  dest[pos++] = 0x80|((val>>6)&0x3F);
                                  dest[pos++] = 0x80|(val&0x3F); }
        else                    { dest[pos++] = 0xF0|(val>>18);
                                  dest[pos++] = 0x80|((val>>12)&0x3F);
                                  dest[pos++] = 0x80|((val>>6)&0x3F);
                                  dest[pos++] = 0x80|(val&0x3F); }
    }
    dest[pos] = 0;
    return pos;
}

/* ── Allocator ────────────────────────────────────────────────────── */

static const ISzAlloc g_Alloc = { SzAlloc, SzFree };

/* ── Archive handle ───────────────────────────────────────────────── */

#define INPUT_BUF_SIZE ((size_t)1 << 18)

typedef struct {
    CFileInStream   archiveStream;
    CLookToRead2    lookStream;
    CSzArEx         db;
    Byte           *lookBuf;
    int             opened;

    /* Block-level decompression cache */
    UInt32          cachedFolder;
    Byte           *cachedBuf;
    size_t          cachedSize;
} Archive;

static const char *get_name(const CSzArEx *db, UInt32 i) {
    static char buf[4096];
    static UInt16 u16[2048];
    size_t len = SzArEx_GetFileNameUtf16(db, i, NULL);
    if (len == 0 || len > 2047) { buf[0] = 0; return buf; }
    SzArEx_GetFileNameUtf16(db, i, u16);
    utf16_to_utf8(buf, sizeof(buf), u16, len > 0 ? len - 1 : 0);
    return buf;
}

static SRes archive_open(Archive *a, const char *path) {
    SRes res;
    memset(a, 0, sizeof(*a));
    a->cachedFolder = (UInt32)-1;

    if (InFile_Open(&a->archiveStream.file, path)) {
        fprintf(stderr, "fast7z: cannot open '%s'\n", path);
        return SZ_ERROR_FAIL;
    }

    FileInStream_CreateVTable(&a->archiveStream);
    LookToRead2_CreateVTable(&a->lookStream, False);

    a->lookBuf = (Byte *)ISzAlloc_Alloc(&g_Alloc, INPUT_BUF_SIZE);
    if (!a->lookBuf) return SZ_ERROR_MEM;

    a->lookStream.buf = a->lookBuf;
    a->lookStream.bufSize = INPUT_BUF_SIZE;
    a->lookStream.realStream = &a->archiveStream.vt;
    LookToRead2_Init(&a->lookStream);

    CrcGenerateTable();
    SzArEx_Init(&a->db);

    res = SzArEx_Open(&a->db, &a->lookStream.vt, &g_Alloc, &g_Alloc);
    if (res == SZ_OK) a->opened = 1;
    return res;
}

static void archive_close(Archive *a) {
    if (a->opened) SzArEx_Free(&a->db, &g_Alloc);
    ISzAlloc_Free(&g_Alloc, a->cachedBuf);
    ISzAlloc_Free(&g_Alloc, a->lookBuf);
    File_Close(&a->archiveStream.file);
}

/*
 * Extract with folder-level caching.
 *
 * The SDK's SzArEx_Extract caches the decompressed block via the
 * blockIndex/tempBuf mechanism. We maintain state across calls so
 * extracting N files from the same solid block decompresses it once.
 */
static SRes archive_extract(Archive *a, UInt32 fileIndex,
                             const Byte **data, size_t *size) {
    size_t offset = 0;
    size_t outSize = 0;

    SRes res = SzArEx_Extract(
        &a->db, &a->lookStream.vt, fileIndex,
        &a->cachedFolder, &a->cachedBuf, &a->cachedSize,
        &offset, &outSize,
        &g_Alloc, &g_Alloc);

    if (res == SZ_OK) {
        *data = a->cachedBuf + offset;
        *size = outSize;
    }
    return res;
}

/* ── Search helpers ───────────────────────────────────────────────── */

static int ci_contains(const char *hay, const char *needle) {
    size_t nlen = strlen(needle), hlen = strlen(hay);
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            char a = hay[i+j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

/* Find exact match first, then unique substring. -1 = not found, -2 = ambiguous */
static Int32 find_file(const CSzArEx *db, const char *pattern) {
    for (UInt32 i = 0; i < db->NumFiles; i++) {
        if (SzArEx_IsDir(db, i)) continue;
        if (strcmp(get_name(db, i), pattern) == 0) return (Int32)i;
    }
    Int32 found = -1;
    for (UInt32 i = 0; i < db->NumFiles; i++) {
        if (SzArEx_IsDir(db, i)) continue;
        if (ci_contains(get_name(db, i), pattern)) {
            if (found >= 0) return -2;
            found = (Int32)i;
        }
    }
    return found;
}

/* ── Human sizes (4-slot round-robin for printf with multiple calls) ── */

static const char *hsz(UInt64 b) {
    static char buf[4][32];
    static int idx = 0;
    char *s = buf[idx++ & 3];
    if      (b < 1024)             snprintf(s, 32, "%llu B",   (unsigned long long)b);
    else if (b < 1024ULL*1024)     snprintf(s, 32, "%.1f KB",  b/1024.0);
    else if (b < 1024ULL*1024*1024)snprintf(s, 32, "%.1f MB",  b/(1024.0*1024));
    else                           snprintf(s, 32, "%.2f GB",  b/(1024.0*1024*1024));
    return s;
}

/* ── Commands ─────────────────────────────────────────────────────── */

static int cmd_list(Archive *a, const char *pattern, int verbose) {
    CSzArEx *db = &a->db;
    UInt32 count = 0, dirs = 0;
    UInt64 total = 0;

    for (UInt32 i = 0; i < db->NumFiles; i++) {
        int isDir = SzArEx_IsDir(db, i);
        const char *name = get_name(db, i);
        if (pattern && !ci_contains(name, pattern)) continue;

        UInt64 sz = SzArEx_GetFileSize(db, i);
        UInt32 folder = db->FileToFolder[i];

        if (verbose) {
            if (isDir) {
                printf("D                          %s/\n", name);
                dirs++;
            } else {
                UInt32 crc = SzBitWithVals_Check(&db->CRCs, i)
                             ? db->CRCs.Vals[i] : 0;
                printf("F %12llu  blk:%-5u  %08X  %s\n",
                       (unsigned long long)sz, (unsigned)folder, crc, name);
            }
        } else {
            printf("%s%s\n", name, isDir ? "/" : "");
        }
        if (!isDir) total += sz;
        count++;
    }
    fprintf(stderr, "%u entries (%u files, %u dirs), %s uncompressed\n",
            count, count - dirs, dirs, hsz(total));
    return 0;
}

static int cmd_info(Archive *a, double open_ms) {
    CSzArEx *db = &a->db;
    UInt32 nf = 0, nd = 0;
    UInt64 total = 0;
    for (UInt32 i = 0; i < db->NumFiles; i++) {
        if (SzArEx_IsDir(db, i)) nd++;
        else { nf++; total += SzArEx_GetFileSize(db, i); }
    }
    printf("Files:         %u  (%u directories)\n", nf, nd);
    printf("Uncompressed:  %s\n", hsz(total));
    printf("Solid blocks:  %u\n", (unsigned)db->db.NumFolders);
    printf("Index time:    %.2f ms\n", open_ms);
    return 0;
}

static int cmd_folders(Archive *a) {
    CSzArEx *db = &a->db;
    const CSzAr *ar = &db->db;

    printf("%-6s  %-12s  %-12s  %-8s  %s\n",
           "Block", "Packed", "Unpacked", "Files", "Range");

    for (UInt32 fi = 0; fi < ar->NumFolders; fi++) {
        UInt32 first = db->FolderToFile[fi];
        UInt32 last  = db->FolderToFile[fi + 1];

        UInt64 unpack = SzAr_GetFolderUnpackSize(ar, fi);

        UInt32 ps_start = ar->FoStartPackStreamIndex[fi];
        UInt32 ps_end   = ar->FoStartPackStreamIndex[fi + 1];
        UInt64 packed = 0;
        for (UInt32 ps = ps_start; ps < ps_end; ps++)
            packed += ar->PackPositions[ps + 1] - ar->PackPositions[ps];

        printf("%-6u  %-12s  ", (unsigned)fi, hsz(packed));
        printf("%-12s  %-8u  [%u..%u)\n",
               hsz(unpack), last - first, first, last);
    }
    return 0;
}

static int cmd_extract_one(Archive *a, const char *target,
                            const char *dest, int to_stdout) {
    Int32 idx = find_file(&a->db, target);
    if (idx == -1) {
        fprintf(stderr, "fast7z: '%s' not found\n", target);
        return 1;
    }
    if (idx == -2) {
        fprintf(stderr, "fast7z: '%s' matches multiple files, be more specific\n", target);
        return 1;
    }

    const char *name = get_name(&a->db, (UInt32)idx);
    UInt64 filesz = SzArEx_GetFileSize(&a->db, (UInt32)idx);
    UInt32 folder = a->db.FileToFolder[(UInt32)idx];

    if (!to_stdout)
        fprintf(stderr, "  %s (%s, block %u)\n", name, hsz(filesz), folder);

    double t0 = now_ms();
    const Byte *data;
    size_t size;
    SRes res = archive_extract(a, (UInt32)idx, &data, &size);
    double t1 = now_ms();

    if (res != SZ_OK) {
        fprintf(stderr, "fast7z: extract failed (err %d)\n", (int)res);
        return 1;
    }

    if (to_stdout) {
        fwrite(data, 1, size, stdout);
    } else {
        const char *out = dest;
        if (!out) {
            const char *slash = strrchr(name, '/');
            out = slash ? slash + 1 : name;
        }
        FILE *f = fopen(out, "wb");
        if (!f) { fprintf(stderr, "fast7z: cannot write '%s'\n", out); return 1; }
        fwrite(data, 1, size, f);
        fclose(f);
        fprintf(stderr, "  -> %s (%.1f ms)\n", out, t1 - t0);
    }
    return 0;
}

/*
 * Batch extraction: sort targets by folder index so we decompress each
 * solid block at most once, then extract all matching files sequentially.
 */
static int cmd_batch(Archive *a, int ntargets, char **targets) {
    CSzArEx *db = &a->db;

    UInt32 *indices = (UInt32 *)malloc(ntargets * sizeof(UInt32));
    int ok = 1;

    for (int t = 0; t < ntargets; t++) {
        Int32 idx = find_file(db, targets[t]);
        if (idx < 0) {
            fprintf(stderr, "fast7z: '%s' %s\n", targets[t],
                    idx == -1 ? "not found" : "ambiguous");
            ok = 0;
            indices[t] = (UInt32)-1;
        } else {
            indices[t] = (UInt32)idx;
        }
    }

    if (!ok) { free(indices); return 1; }

    /* Sort by folder for sequential block decompression */
    UInt32 *order = (UInt32 *)malloc(ntargets * sizeof(UInt32));
    for (int i = 0; i < ntargets; i++) order[i] = (UInt32)i;

    for (int i = 1; i < ntargets; i++) {
        UInt32 key = order[i];
        UInt32 key_folder = db->FileToFolder[indices[key]];
        int j = i - 1;
        while (j >= 0 && db->FileToFolder[indices[order[j]]] > key_folder) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    double total_t = 0;
    int errors = 0;

    for (int i = 0; i < ntargets; i++) {
        UInt32 ti = order[i];
        UInt32 fi = indices[ti];
        const char *name = get_name(db, fi);
        UInt32 folder = db->FileToFolder[fi];

        const char *slash = strrchr(name, '/');
        const char *out = slash ? slash + 1 : name;

        fprintf(stderr, "  [%d/%d] %s (block %u)...",
                i + 1, ntargets, name, folder);

        double t0 = now_ms();
        const Byte *data;
        size_t size;
        SRes res = archive_extract(a, fi, &data, &size);
        double dt = now_ms() - t0;
        total_t += dt;

        if (res != SZ_OK) {
            fprintf(stderr, " FAILED (err %d)\n", (int)res);
            errors++;
            continue;
        }

        FILE *f = fopen(out, "wb");
        if (!f) {
            fprintf(stderr, " cannot write '%s'\n", out);
            errors++;
            continue;
        }
        fwrite(data, 1, size, f);
        fclose(f);
        fprintf(stderr, " %s in %.1f ms%s\n", hsz(size), dt,
                dt < 1.0 ? " (cached)" : "");
    }

    fprintf(stderr, "Extracted %d files in %.1f ms (%d errors)\n",
            ntargets - errors, total_t, errors);

    free(order);
    free(indices);
    return errors ? 1 : 0;
}

/* ── Sidecar index cache (.f7idx) ─────────────────────────────────── */

#define F7IDX_MAGIC  0x58493746  /* "F7IX" */
#define F7IDX_VER    1

static int cmd_save_idx(Archive *a, const char *archive_path) {
    CSzArEx *db = &a->db;

    char idx_path[4096];
    snprintf(idx_path, sizeof(idx_path), "%s.f7idx", archive_path);

    FILE *f = fopen(idx_path, "wb");
    if (!f) { fprintf(stderr, "fast7z: cannot create '%s'\n", idx_path); return 1; }

    UInt32 hdr[4] = { F7IDX_MAGIC, F7IDX_VER, db->NumFiles, db->db.NumFolders };
    fwrite(hdr, sizeof(hdr), 1, f);

    for (UInt32 i = 0; i < db->NumFiles; i++) {
        UInt32 folder   = db->FileToFolder[i];
        UInt64 size     = SzArEx_GetFileSize(db, i);
        UInt32 crc      = SzBitWithVals_Check(&db->CRCs, i) ? db->CRCs.Vals[i] : 0;
        UInt32 flags    = 0;
        if (SzArEx_IsDir(db, i))              flags |= 1;
        if (SzBitWithVals_Check(&db->CRCs, i)) flags |= 2;

        const char *name = get_name(db, i);
        UInt32 name_len = (UInt32)strlen(name);

        fwrite(&folder, 4, 1, f);
        fwrite(&size, 8, 1, f);
        fwrite(&crc, 4, 1, f);
        fwrite(&flags, 4, 1, f);
        fwrite(&name_len, 4, 1, f);
        fwrite(name, 1, name_len, f);
    }

    fclose(f);

    struct stat st;
    stat(idx_path, &st);
    fprintf(stderr, "Saved index: %s (%s)\n", idx_path, hsz(st.st_size));
    return 0;
}

static int cmd_list_idx(const char *idx_path, const char *pattern) {
    double t0 = now_ms();

    int fd = open(idx_path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "fast7z: cannot open '%s'\n", idx_path); return 1; }

    struct stat st;
    fstat(fd, &st);
    size_t sz = st.st_size;

    const Byte *map = (const Byte *)mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { fprintf(stderr, "fast7z: mmap failed\n"); return 1; }

    if (sz < 16 || *(const UInt32 *)map != F7IDX_MAGIC) {
        fprintf(stderr, "fast7z: invalid index file\n");
        munmap((void *)map, sz);
        return 1;
    }

    UInt32 nfiles = *(const UInt32 *)(map + 8);
    double t1 = now_ms();

    const Byte *p = map + 16;
    UInt32 count = 0;
    UInt64 total = 0;
    char name_buf[4096];

    for (UInt32 i = 0; i < nfiles && (size_t)(p - map + 24) <= sz; i++) {
        UInt32 folder   = *(const UInt32 *)(p + 0);
        UInt64 size     = *(const UInt64 *)(p + 4);
        UInt32 crc      = *(const UInt32 *)(p + 12);
        UInt32 flags    = *(const UInt32 *)(p + 16);
        UInt32 name_len = *(const UInt32 *)(p + 20);
        p += 24;

        if (name_len >= sizeof(name_buf)) name_len = sizeof(name_buf) - 1;
        memcpy(name_buf, p, name_len);
        name_buf[name_len] = 0;
        p += name_len;

        int isDir = flags & 1;
        if (pattern && !ci_contains(name_buf, pattern)) continue;

        if (isDir)
            printf("D                          %s/\n", name_buf);
        else
            printf("F %12llu  blk:%-5u  %08X  %s\n",
                   (unsigned long long)size, folder, crc, name_buf);

        if (!isDir) total += size;
        count++;
    }

    double t2 = now_ms();
    munmap((void *)map, sz);

    fprintf(stderr, "%u entries, %s (%.2f ms: mmap %.2f + scan %.2f)\n",
            count, hsz(total), t2 - t0, t1 - t0, t2 - t1);
    return 0;
}

/* ── Usage ────────────────────────────────────────────────────────── */

static void usage(void) {
    fprintf(stderr,
        "fast7z — high-performance 7z indexer & extractor\n"
        "         Zero deps. LZMA SDK (public domain) embedded.\n\n"
        "  ls       <archive> [pattern]        List (sizes, CRC, block#)\n"
        "  find     <archive> <pattern>        Search by substring\n"
        "  info     <archive>                  Summary + timing\n"
        "  folders  <archive>                  Show solid block layout\n"
        "  x        <archive> <path> [out]     Extract one file\n"
        "  dump     <archive> <path>           Extract to stdout\n"
        "  batch    <archive> <p1> <p2> ...    Extract multiple (block-sorted)\n"
        "  save-idx <archive>                  Create .f7idx sidecar cache\n"
        "  ls-idx   <file.f7idx> [pattern]     List from cache (<1ms)\n"
    );
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 3) { usage(); return 1; }

    const char *cmd  = argv[1];
    const char *path = argv[2];

    /* ls-idx bypasses archive open entirely */
    if (strcmp(cmd, "ls-idx") == 0)
        return cmd_list_idx(path, argc > 3 ? argv[3] : NULL);

    Archive a;
    double t0 = now_ms();
    SRes res = archive_open(&a, path);
    double open_ms = now_ms() - t0;

    if (res != SZ_OK) {
        const char *msg = "unknown error";
        switch (res) {
            case SZ_ERROR_UNSUPPORTED: msg = "unsupported format/method"; break;
            case SZ_ERROR_MEM:         msg = "out of memory"; break;
            case SZ_ERROR_CRC:         msg = "CRC error in header"; break;
            case SZ_ERROR_DATA:        msg = "corrupted header data"; break;
            default: break;
        }
        fprintf(stderr, "fast7z: %s (error %d)\n", msg, (int)res);
        archive_close(&a);
        return 1;
    }

    fprintf(stderr, "[index: %u files, %u blocks, %.2f ms]\n",
            a.db.NumFiles, a.db.db.NumFolders, open_ms);

    int ret = 0;

    if (!strcmp(cmd,"ls") || !strcmp(cmd,"list") || !strcmp(cmd,"l"))
        ret = cmd_list(&a, argc > 3 ? argv[3] : NULL, 1);
    else if (!strcmp(cmd,"find") || !strcmp(cmd,"f")) {
        if (argc < 4) { fprintf(stderr, "need pattern\n"); ret = 1; }
        else ret = cmd_list(&a, argv[3], 0);
    }
    else if (!strcmp(cmd,"info") || !strcmp(cmd,"i"))
        ret = cmd_info(&a, open_ms);
    else if (!strcmp(cmd,"folders"))
        ret = cmd_folders(&a);
    else if (!strcmp(cmd,"x") || !strcmp(cmd,"extract")) {
        if (argc < 4) { fprintf(stderr, "need path\n"); ret = 1; }
        else ret = cmd_extract_one(&a, argv[3], argc > 4 ? argv[4] : NULL, 0);
    }
    else if (!strcmp(cmd,"dump") || !strcmp(cmd,"d")) {
        if (argc < 4) { fprintf(stderr, "need path\n"); ret = 1; }
        else ret = cmd_extract_one(&a, argv[3], NULL, 1);
    }
    else if (!strcmp(cmd,"batch") || !strcmp(cmd,"b")) {
        if (argc < 4) { fprintf(stderr, "need paths\n"); ret = 1; }
        else ret = cmd_batch(&a, argc - 3, argv + 3);
    }
    else if (!strcmp(cmd,"save-idx"))
        ret = cmd_save_idx(&a, path);
    else {
        fprintf(stderr, "unknown command '%s'\n", cmd);
        usage();
        ret = 1;
    }

    archive_close(&a);
    return ret;
}

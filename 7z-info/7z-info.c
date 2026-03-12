/*
 * 7z-info.c — Deep diagnostic tool for 7z archive internals
 *
 * Shows everything: header layout, compression methods, solid block
 * structure, file distribution across blocks, extraction cost estimates,
 * worst-case/best-case random access analysis.
 *
 * Build:
 *   cc -O3 -o 7z-info 7z-info.c lzma/ *.c -Ilzma -lpthread
 *
 * Usage:
 *   7z-info <archive.7z>
 *   7z-info <archive.7z> --blocks          # detailed per-block breakdown
 *   7z-info <archive.7z> --files [pattern] # per-file extraction cost
 *
 * License: Public domain
 */

#include "Precomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>

#include "CpuArch.h"
#include "7z.h"
#include "7zAlloc.h"
#include "7zBuf.h"
#include "7zCrc.h"
#include "7zFile.h"

/* ── Timing ───────────────────────────────────────────────────────── */

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
static double now_ms(void) { return now_us() / 1000.0; }

/* ── Allocator ────────────────────────────────────────────────────── */

static const ISzAlloc g_Alloc = { SzAlloc, SzFree };

/* ── Human-readable helpers ───────────────────────────────────────── */

/* 4-slot round-robin so you can use hsz() multiple times in one printf */
static const char *hsz(UInt64 b) {
    static char buf[4][48];
    static int idx = 0;
    char *s = buf[idx++ & 3];
    if      (b == 0)                snprintf(s, 48, "0 B");
    else if (b < 1024)             snprintf(s, 48, "%llu B", (unsigned long long)b);
    else if (b < 1024ULL*1024)     snprintf(s, 48, "%.1f KB (%llu)", b/1024.0, (unsigned long long)b);
    else if (b < 1024ULL*1024*1024)snprintf(s, 48, "%.2f MB (%llu)", b/(1024.0*1024), (unsigned long long)b);
    else                           snprintf(s, 48, "%.3f GB (%llu)", b/(1024.0*1024*1024), (unsigned long long)b);
    return s;
}

static const char *hsz_short(UInt64 b) {
    static char buf[4][32];
    static int idx = 0;
    char *s = buf[idx++ & 3];
    if      (b == 0)                snprintf(s, 32, "0");
    else if (b < 1024)             snprintf(s, 32, "%llu B", (unsigned long long)b);
    else if (b < 1024ULL*1024)     snprintf(s, 32, "%.1f KB", b/1024.0);
    else if (b < 1024ULL*1024*1024)snprintf(s, 32, "%.1f MB", b/(1024.0*1024));
    else                           snprintf(s, 32, "%.2f GB", b/(1024.0*1024*1024));
    return s;
}

static const char *method_name(UInt32 id) {
    switch (id) {
        case 0x00:     return "Copy";
        case 0x03:     return "Delta";
        case 0x21:     return "LZMA2";
        case 0x030101: return "LZMA";
        case 0x030401: return "PPMd";
        case 0x040108: return "Deflate";
        case 0x040109: return "Deflate64";
        case 0x040202: return "BZip2";
        case 0x030103: return "BCJ (x86)";
        case 0x0303011B: return "BCJ2 (x86)";
        case 0x030205: return "PPC";
        case 0x03030401: return "IA-64";
        case 0x030501: return "ARM";
        case 0x030701: return "ARM-Thumb";
        case 0x030805: return "SPARC";
        case 0x06F10701: return "AES-256";
        default: return "Unknown";
    }
}

static const char *pct(UInt64 part, UInt64 whole) {
    static char buf[4][16];
    static int idx = 0;
    char *s = buf[idx++ & 3];
    if (whole == 0) snprintf(s, 16, "N/A");
    else snprintf(s, 16, "%.1f%%", (double)part / whole * 100.0);
    return s;
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

static const char *get_name(const CSzArEx *db, UInt32 i) {
    static char buf[4096];
    static UInt16 u16[2048];
    size_t len = SzArEx_GetFileNameUtf16(db, i, NULL);
    if (len == 0 || len > 2047) { buf[0] = 0; return buf; }
    SzArEx_GetFileNameUtf16(db, i, u16);
    utf16_to_utf8(buf, sizeof(buf), u16, len > 0 ? len - 1 : 0);
    return buf;
}

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

/* ── Separator / section helpers ──────────────────────────────────── */

static void sep(void) {
    printf("────────────────────────────────────────────────────────────────────────\n");
}

static void section(const char *title) {
    printf("\n");
    sep();
    printf("  %s\n", title);
    sep();
}

/* ── Main diagnostic output ───────────────────────────────────────── */

#define INPUT_BUF_SIZE ((size_t)1 << 18)

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IOLBF, 0);  /* line-buffered even when redirected */

    if (argc < 2) {
        fprintf(stderr,
            "7z-info — Deep diagnostic tool for 7z archive internals\n\n"
            "Usage:\n"
            "  7z-info <archive.7z>                   Full diagnostic report\n"
            "  7z-info <archive.7z> --blocks           Detailed per-block breakdown\n"
            "  7z-info <archive.7z> --files [pattern]  Per-file extraction cost\n"
        );
        return 1;
    }

    const char *path = argv[1];
    int show_blocks = 0, show_files = 0;
    const char *file_pattern = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--blocks") == 0) show_blocks = 1;
        else if (strcmp(argv[i], "--files") == 0) {
            show_files = 1;
            if (i + 1 < argc && argv[i+1][0] != '-') file_pattern = argv[++i];
        }
    }

    /* ── Read raw header info before SDK opens it ─────────────────── */

    struct stat file_st;
    if (stat(path, &file_st) != 0) {
        fprintf(stderr, "7z-info: cannot stat '%s'\n", path);
        return 1;
    }
    UInt64 archive_size = file_st.st_size;

    FILE *raw = fopen(path, "rb");
    if (!raw) { fprintf(stderr, "7z-info: cannot open '%s'\n", path); return 1; }

    Byte sig[32];
    if (fread(sig, 1, 32, raw) != 32) {
        fprintf(stderr, "7z-info: file too small for 7z\n");
        fclose(raw);
        return 1;
    }

    static const Byte k7zSig[6] = {'7','z',0xBC,0xAF,0x27,0x1C};
    if (memcmp(sig, k7zSig, 6) != 0) {
        fprintf(stderr, "7z-info: not a 7z archive (bad signature)\n");
        fclose(raw);
        return 1;
    }

    Byte ver_major = sig[6], ver_minor = sig[7];
    UInt32 start_hdr_crc = GetUi32(sig + 8);
    UInt64 next_hdr_offset = GetUi64(sig + 12);
    UInt64 next_hdr_size   = GetUi64(sig + 20);
    UInt32 next_hdr_crc    = GetUi32(sig + 28);

    /* Read first byte of the next header to check if it's EncodedHeader */
    UInt64 hdr_abs_pos = 32 + next_hdr_offset;
    Byte hdr_first_byte = 0;
    if (fseeko(raw, hdr_abs_pos, SEEK_SET) == 0)
        fread(&hdr_first_byte, 1, 1, raw);
    fclose(raw);

    int is_encoded = (hdr_first_byte == 0x17);

    UInt64 payload_size = next_hdr_offset; /* data between sig header and next header */

    /* ── Open with SDK (timed) ────────────────────────────────────── */

    CFileInStream archiveStream;
    CLookToRead2 lookStream;
    CSzArEx db;

    if (InFile_Open(&archiveStream.file, path)) {
        fprintf(stderr, "7z-info: cannot open '%s'\n", path);
        return 1;
    }

    FileInStream_CreateVTable(&archiveStream);
    LookToRead2_CreateVTable(&lookStream, False);

    Byte *lookBuf = (Byte *)ISzAlloc_Alloc(&g_Alloc, INPUT_BUF_SIZE);
    lookStream.buf = lookBuf;
    lookStream.bufSize = INPUT_BUF_SIZE;
    lookStream.realStream = &archiveStream.vt;
    LookToRead2_INIT(&lookStream);

    double t_crc = now_ms();
    CrcGenerateTable();
    double t_crc_done = now_ms();

    SzArEx_Init(&db);

    double t_open = now_ms();
    SRes res = SzArEx_Open(&db, &lookStream.vt, &g_Alloc, &g_Alloc);
    double t_open_done = now_ms();

    if (res != SZ_OK) {
        fprintf(stderr, "7z-info: failed to open (error %d)\n", (int)res);
        ISzAlloc_Free(&g_Alloc, lookBuf);
        File_Close(&archiveStream.file);
        return 1;
    }

    const CSzAr *ar = &db.db;

    /* ── Collect statistics ───────────────────────────────────────── */

    UInt32 num_files = 0, num_dirs = 0, num_empty = 0;
    UInt64 total_uncompressed = 0;
    UInt64 smallest_file = UINT64_MAX, largest_file = 0;
    UInt32 has_crc_count = 0, has_mtime_count = 0, has_attrib_count = 0;

    for (UInt32 i = 0; i < db.NumFiles; i++) {
        if (SzArEx_IsDir(&db, i)) { num_dirs++; continue; }
        UInt64 sz = SzArEx_GetFileSize(&db, i);
        if (sz == 0) { num_empty++; }
        num_files++;
        total_uncompressed += sz;
        if (sz < smallest_file) smallest_file = sz;
        if (sz > largest_file)  largest_file = sz;
        if (SzBitWithVals_Check(&db.CRCs, i)) has_crc_count++;
        if (SzBitWithVals_Check(&db.MTime, i)) has_mtime_count++;
        if (SzBitWithVals_Check(&db.Attribs, i)) has_attrib_count++;
    }
    if (num_files == 0) smallest_file = 0;

    /* Per-folder stats */
    UInt64 total_packed = 0;
    UInt64 smallest_block_unpacked = UINT64_MAX, largest_block_unpacked = 0;
    UInt64 smallest_block_packed = UINT64_MAX, largest_block_packed = 0;

    typedef struct {
        UInt32 folder_idx;
        UInt32 first_file, last_file;
        UInt32 num_files;
        UInt64 packed, unpacked;
        double ratio;
        double est_decompress_s;  /* rough estimate */
        UInt32 num_coders;
        UInt32 method_ids[4];
    } FolderStat;

    FolderStat *fstats = (FolderStat *)calloc(ar->NumFolders, sizeof(FolderStat));

    for (UInt32 fi = 0; fi < ar->NumFolders; fi++) {
        fstats[fi].folder_idx = fi;
        fstats[fi].first_file = db.FolderToFile[fi];
        fstats[fi].last_file  = db.FolderToFile[fi + 1];
        fstats[fi].unpacked   = SzAr_GetFolderUnpackSize(ar, fi);

        /* Count actual non-dir files in this folder */
        for (UInt32 j = fstats[fi].first_file; j < fstats[fi].last_file; j++)
            if (!SzArEx_IsDir(&db, j)) fstats[fi].num_files++;

        /* Packed size */
        UInt32 ps_start = ar->FoStartPackStreamIndex[fi];
        UInt32 ps_end   = ar->FoStartPackStreamIndex[fi + 1];
        for (UInt32 ps = ps_start; ps < ps_end; ps++)
            fstats[fi].packed += ar->PackPositions[ps + 1] - ar->PackPositions[ps];

        total_packed += fstats[fi].packed;
        fstats[fi].ratio = fstats[fi].unpacked > 0
            ? (double)fstats[fi].packed / fstats[fi].unpacked : 0;

        /* Decompress time estimate: ~200 MB/s for LZMA, ~500 MB/s for LZMA2 */
        fstats[fi].est_decompress_s = (double)fstats[fi].unpacked / (200.0 * 1024 * 1024);

        if (fstats[fi].unpacked < smallest_block_unpacked) smallest_block_unpacked = fstats[fi].unpacked;
        if (fstats[fi].unpacked > largest_block_unpacked)  largest_block_unpacked = fstats[fi].unpacked;
        if (fstats[fi].packed < smallest_block_packed)     smallest_block_packed = fstats[fi].packed;
        if (fstats[fi].packed > largest_block_packed)      largest_block_packed = fstats[fi].packed;

        /* Parse coder info */
        CSzFolder folder;
        CSzData sd;
        sd.Data = ar->CodersData + ar->FoCodersOffsets[fi];
        sd.Size = ar->FoCodersOffsets[fi + 1] - ar->FoCodersOffsets[fi];
        if (SzGetNextFolderItem(&folder, &sd) == SZ_OK) {
            fstats[fi].num_coders = folder.NumCoders;
            for (UInt32 ci = 0; ci < folder.NumCoders && ci < 4; ci++)
                fstats[fi].method_ids[ci] = folder.Coders[ci].MethodID;
        }
    }

    if (ar->NumFolders == 0) {
        smallest_block_unpacked = smallest_block_packed = 0;
    }

    /* ═══════════════════════════════════════════════════════════════ */
    /*  OUTPUT                                                        */
    /* ═══════════════════════════════════════════════════════════════ */

    section("ARCHIVE OVERVIEW");
    printf("  File:              %s\n", path);
    printf("  Archive size:      %s\n", hsz(archive_size));
    printf("  Format version:    %u.%u\n", ver_major, ver_minor);
    printf("  Total files:       %u  (%u directories, %u empty files)\n",
           num_files, num_dirs, num_empty);
    printf("  Uncompressed:      %s\n", hsz(total_uncompressed));
    printf("  Compressed:        %s  (ratio: %s)\n",
           hsz(total_packed), pct(total_packed, total_uncompressed));
    printf("  Solid blocks:      %u\n", ar->NumFolders);
    printf("  Pack streams:      %u\n", ar->NumPackStreams);

    section("BINARY LAYOUT  (offset → content)");
    printf("  [0x%012llX]  Signature header (32 bytes)\n", 0ULL);
    printf("       Signature:    7z BC AF 27 1C\n");
    printf("       Version:      %u.%u\n", ver_major, ver_minor);
    printf("       StartHdrCRC:  0x%08X\n", start_hdr_crc);
    printf("       NextHdrOfs:   %llu (0x%llX)\n",
           (unsigned long long)next_hdr_offset, (unsigned long long)next_hdr_offset);
    printf("       NextHdrSize:  %llu (0x%llX)\n",
           (unsigned long long)next_hdr_size, (unsigned long long)next_hdr_size);
    printf("       NextHdrCRC:   0x%08X\n", next_hdr_crc);

    printf("  [0x%012llX]  Packed data streams (%s)\n",
           32ULL, hsz(payload_size));

    /* Show individual pack stream positions */
    if (ar->NumPackStreams <= 20) {
        for (UInt32 ps = 0; ps < ar->NumPackStreams; ps++) {
            UInt64 abs_start = db.dataPos + ar->PackPositions[ps];
            UInt64 ps_size = ar->PackPositions[ps + 1] - ar->PackPositions[ps];
            printf("       stream[%u]:   @0x%llX  size=%s\n",
                   ps, (unsigned long long)abs_start, hsz_short(ps_size));
        }
    } else {
        printf("       (%u pack streams, first @0x%llX, last ends @0x%llX)\n",
               ar->NumPackStreams,
               (unsigned long long)(db.dataPos + ar->PackPositions[0]),
               (unsigned long long)(db.dataPos + ar->PackPositions[ar->NumPackStreams]));
    }

    printf("  [0x%012llX]  Header / metadata (%s)%s\n",
           (unsigned long long)hdr_abs_pos, hsz(next_hdr_size),
           is_encoded ? " [LZMA-compressed]" : " [raw]");

    if (is_encoded) {
        printf("       The file index is itself compressed.\n");
        printf("       To list files, the header must be read + decompressed.\n");
    }

    printf("  [0x%012llX]  End of archive\n", (unsigned long long)archive_size);

    section("INDEX / HEADER ANALYSIS");
    printf("  Header location:   end of archive (offset 0x%llX)\n",
           (unsigned long long)hdr_abs_pos);
    printf("  Header size (on disk):  %s\n", hsz(next_hdr_size));
    printf("  Header compressed: %s\n", is_encoded ? "YES" : "NO (raw)");

    printf("  Index parse time:  %.2f ms\n", t_open_done - t_open);
    printf("    (CRC table init: %.2f ms)\n", t_crc_done - t_crc);
    printf("  I/O to read index: seek to 0x%llX + read %s + decompress\n",
           (unsigned long long)hdr_abs_pos, hsz_short(next_hdr_size));

    printf("  Metadata stored:   names");
    if (has_crc_count)   printf(", CRC (%u/%u)", has_crc_count, num_files);
    if (has_mtime_count) printf(", mtime (%u/%u)", has_mtime_count, num_files);
    if (has_attrib_count)printf(", attribs (%u/%u)", has_attrib_count, num_files);
    printf("\n");

    section("SOLID BLOCK STRUCTURE");

    if (ar->NumFolders == 1) {
        printf("  *** SINGLE SOLID BLOCK ***\n");
        printf("  The entire archive is one solid block. Extracting ANY file\n");
        printf("  requires decompressing from the beginning of the block up to\n");
        printf("  the file's offset. For a file near the end, this means\n");
        printf("  decompressing nearly all %s.\n", hsz_short(fstats[0].unpacked));
    } else if (ar->NumFolders == num_files) {
        printf("  *** NON-SOLID (one file per block) ***\n");
        printf("  Each file is in its own block — any file can be extracted\n");
        printf("  independently by reading only its compressed data.\n");
    } else {
        printf("  %u blocks spanning %u files (avg %.0f files/block)\n",
               ar->NumFolders, num_files,
               (double)num_files / ar->NumFolders);
    }

    printf("\n  Block size range:\n");
    printf("    Packed:    %s  ..  %s\n",
           hsz_short(smallest_block_packed), hsz_short(largest_block_packed));
    printf("    Unpacked:  %s  ..  %s\n",
           hsz_short(smallest_block_unpacked), hsz_short(largest_block_unpacked));

    /* Compression methods summary */
    printf("\n  Compression methods found:\n");
    {
        UInt32 seen[32];
        int nseen = 0;
        for (UInt32 fi = 0; fi < ar->NumFolders; fi++) {
            for (UInt32 ci = 0; ci < fstats[fi].num_coders; ci++) {
                UInt32 mid = fstats[fi].method_ids[ci];
                int dup = 0;
                for (int s = 0; s < nseen; s++) if (seen[s] == mid) { dup = 1; break; }
                if (!dup && nseen < 32) seen[nseen++] = mid;
            }
        }
        for (int s = 0; s < nseen; s++)
            printf("    0x%X = %s\n", seen[s], method_name(seen[s]));
    }

    section("EXTRACTION COST ANALYSIS");

    /* Best/worst case for random single file extraction */
    UInt64 worst_decompress = largest_block_unpacked;
    double worst_time_est = (double)worst_decompress / (200.0 * 1024 * 1024);

    printf("  Random single-file extraction:\n");
    printf("    Best case:   decompress %s  (smallest block or cached)\n",
           hsz_short(smallest_block_unpacked));
    printf("    Worst case:  decompress %s  (largest block)\n",
           hsz_short(largest_block_unpacked));
    printf("    Est. worst:  ~%.1f sec  (assuming ~200 MB/s LZMA decode)\n",
           worst_time_est);

    if (ar->NumFolders == 1) {
        printf("\n  ** WARNING: Monolithic solid block.\n");
        printf("     To extract file at offset N, must decompress N bytes first.\n");
        printf("     Worst case (last file): decompress all %s.\n",
               hsz_short(fstats[0].unpacked));
        printf("     Consider re-archiving with: 7z a -ms=off  or  7z a -ms=4m\n");
    }

    /* Distribution analysis */
    printf("\n  Files per block distribution:\n");
    if (ar->NumFolders > 0) {
        UInt32 min_fpb = UINT32_MAX, max_fpb = 0;
        UInt64 sum_fpb = 0;
        for (UInt32 fi = 0; fi < ar->NumFolders; fi++) {
            UInt32 n = fstats[fi].num_files;
            if (n < min_fpb) min_fpb = n;
            if (n > max_fpb) max_fpb = n;
            sum_fpb += n;
        }
        printf("    min=%u  max=%u  avg=%.1f\n",
               min_fpb, max_fpb, (double)sum_fpb / ar->NumFolders);
    }

    /* File size distribution */
    if (num_files > 0) {
        printf("\n  File size distribution:\n");
        printf("    Smallest:  %s\n", hsz(smallest_file));
        printf("    Largest:   %s\n", hsz(largest_file));
        printf("    Average:   %s\n", hsz(total_uncompressed / num_files));

        /* Buckets */
        UInt32 b_tiny=0, b_small=0, b_med=0, b_large=0, b_huge=0;
        for (UInt32 i = 0; i < db.NumFiles; i++) {
            if (SzArEx_IsDir(&db, i)) continue;
            UInt64 sz = SzArEx_GetFileSize(&db, i);
            if      (sz < 1024)             b_tiny++;
            else if (sz < 1024*1024)        b_small++;
            else if (sz < 10*1024*1024)     b_med++;
            else if (sz < 100*1024*1024)    b_large++;
            else                            b_huge++;
        }
        printf("    < 1 KB:    %u  (%s)\n", b_tiny, pct(b_tiny, num_files));
        printf("    1KB-1MB:   %u  (%s)\n", b_small, pct(b_small, num_files));
        printf("    1MB-10MB:  %u  (%s)\n", b_med, pct(b_med, num_files));
        printf("    10MB-100MB:%u  (%s)\n", b_large, pct(b_large, num_files));
        printf("    > 100MB:   %u  (%s)\n", b_huge, pct(b_huge, num_files));
    }

    section("RANDOM ACCESS RATING");
    {
        /* Score from 1-5 based on how friendly this archive is for random access */
        int score;
        const char *verdict;

        if (ar->NumFolders == num_files && num_files > 0) {
            score = 5; verdict = "EXCELLENT — non-solid, direct file access";
        } else if (ar->NumFolders > 1 && largest_block_unpacked < 16ULL*1024*1024) {
            score = 4; verdict = "GOOD — small blocks, fast random extraction";
        } else if (ar->NumFolders > 1 && largest_block_unpacked < 256ULL*1024*1024) {
            score = 3; verdict = "FAIR — moderate blocks, some decompression cost";
        } else if (ar->NumFolders > 1) {
            score = 2; verdict = "POOR — large solid blocks, slow random access";
        } else if (ar->NumFolders == 1 && fstats[0].unpacked < 256ULL*1024*1024) {
            score = 2; verdict = "POOR — single block but small enough to buffer";
        } else {
            score = 1; verdict = "TERRIBLE — monolithic solid, must decompress linearly";
        }

        const char *stars[] = {"", "*", "**", "***", "****", "*****"};
        printf("  %s  [%d/5]  %s\n", stars[score], score, verdict);

        if (score <= 2) {
            printf("\n  Recommendations:\n");
            printf("    Re-archive with smaller blocks:\n");
            printf("      7z a -ms=off  archive_new.7z ./files/    # no solid (best random access)\n");
            printf("      7z a -ms=1m   archive_new.7z ./files/    # 1MB blocks (good compromise)\n");
            printf("      7z a -ms=16m  archive_new.7z ./files/    # 16MB blocks\n");
            printf("      7z a -ms=100f archive_new.7z ./files/    # 100 files per block\n");
            if (ar->NumFolders == 1)
                printf("\n    Or extract everything once and work from the extracted copy.\n");
        }
    }

    /* ── Measured decompression performance ────────────────────────── */

    if (ar->NumFolders > 0 && num_files > 0) {
        section("MEASURED DECOMPRESSION PERFORMANCE");
        #define BENCH_SAMPLES 100
        printf("  Sampling %d blocks spread across the archive...\n\n", BENCH_SAMPLES);

        UInt32 bench_blocks[BENCH_SAMPLES];
        int bench_count = 0;

        /* Pick up to BENCH_SAMPLES blocks evenly spread across the archive */
        if (ar->NumFolders <= BENCH_SAMPLES) {
            for (UInt32 fi = 0; fi < ar->NumFolders; fi++)
                bench_blocks[bench_count++] = fi;
        } else {
            for (int s = 0; s < BENCH_SAMPLES; s++) {
                UInt32 fi = (UInt32)((UInt64)s * (ar->NumFolders - 1) / (BENCH_SAMPLES - 1));
                bench_blocks[bench_count++] = fi;
            }
        }

        printf("  %-8s  %-12s  %-12s  %-7s  %10s  %-10s  %s\n",
               "Block", "Packed", "Unpacked", "Ratio", "Time(us)", "Throughput", "File");
        sep();

        /* Extraction state for SzArEx_Extract */
        UInt32 extractBlockIdx = (UInt32)-1;
        Byte  *extractBuf = NULL;
        size_t extractBufSize = 0;

        double total_bench_time = 0;
        UInt64 total_bench_unpacked = 0;
        UInt64 total_bench_packed = 0;

        for (int s = 0; s < bench_count; s++) {
            UInt32 fi = bench_blocks[s];

            /* Find a non-dir, non-empty file in this block */
            UInt32 test_file = (UInt32)-1;
            for (UInt32 j = fstats[fi].first_file; j < fstats[fi].last_file; j++) {
                if (!SzArEx_IsDir(&db, j) && SzArEx_GetFileSize(&db, j) > 0) {
                    test_file = j;
                    break;
                }
            }
            if (test_file == (UInt32)-1) continue;

            /* Flush cache to force re-decompression */
            ISzAlloc_Free(&g_Alloc, extractBuf);
            extractBuf = NULL;
            extractBufSize = 0;
            extractBlockIdx = (UInt32)-1;

            size_t offset = 0, outSize = 0;
            double t0 = now_us();
            SRes eres = SzArEx_Extract(
                &db, &lookStream.vt, test_file,
                &extractBlockIdx, &extractBuf, &extractBufSize,
                &offset, &outSize, &g_Alloc, &g_Alloc);
            double dt_us = now_us() - t0;

            if (eres != SZ_OK) {
                printf("  %-8u  %-12s  %-12s  %-7s  FAILED (err %d)\n",
                       fi, hsz_short(fstats[fi].packed),
                       hsz_short(fstats[fi].unpacked),
                       pct(fstats[fi].packed, fstats[fi].unpacked),
                       (int)eres);
                continue;
            }

            double throughput_mbs = (dt_us > 0)
                ? (fstats[fi].unpacked / (1024.0 * 1024.0)) / (dt_us / 1e6)
                : 0;
            total_bench_time += dt_us;
            total_bench_unpacked += fstats[fi].unpacked;
            total_bench_packed += fstats[fi].packed;

            printf("  %-8u  %-12s  %-12s  %-7s  %8.0f  %8.1f MB/s  %s\n",
                   fi,
                   hsz_short(fstats[fi].packed),
                   hsz_short(fstats[fi].unpacked),
                   pct(fstats[fi].packed, fstats[fi].unpacked),
                   dt_us, throughput_mbs,
                   get_name(&db, test_file));
        }

        ISzAlloc_Free(&g_Alloc, extractBuf);
        extractBuf = NULL;

        /* Summary */
        double total_bench_ms = total_bench_time / 1000.0;
        printf("\n  Benchmark summary (%d blocks sampled):\n", bench_count);
        printf("    Total packed read:      %s\n", hsz(total_bench_packed));
        printf("    Total unpacked:         %s\n", hsz(total_bench_unpacked));
        printf("    Total time:             %.2f ms  (%.0f us)\n", total_bench_ms, total_bench_time);
        if (total_bench_time > 0) {
            double avg_throughput = (total_bench_unpacked / (1024.0*1024.0)) / (total_bench_time / 1e6);
            printf("    Avg throughput:         %.1f MB/s\n", avg_throughput);
            /* Extrapolate full archive extraction time */
            double full_est = (double)total_uncompressed / (avg_throughput * 1024 * 1024);
            printf("    Est. full extraction:   %.1f sec (%.0f MB/s across %s)\n",
                   full_est, avg_throughput, hsz_short(total_uncompressed));
        }
    }

    if (show_blocks) {
        section("DETAILED BLOCK LISTING");
        for (UInt32 fi = 0; fi < ar->NumFolders; fi++) {
            printf("\n  Block %u:\n", fi);
            printf("    Files:     %u  [idx %u..%u)\n",
                   fstats[fi].num_files, fstats[fi].first_file, fstats[fi].last_file);
            printf("    Packed:    %s\n", hsz(fstats[fi].packed));
            printf("    Unpacked:  %s\n", hsz(fstats[fi].unpacked));
            printf("    Ratio:     %s\n", pct(fstats[fi].packed, fstats[fi].unpacked));

            /* Pack stream absolute position */
            UInt32 ps_idx = ar->FoStartPackStreamIndex[fi];
            UInt64 abs_pos = db.dataPos + ar->PackPositions[ps_idx];
            printf("    Disk pos:  0x%llX\n", (unsigned long long)abs_pos);

            printf("    Coders:    ");
            for (UInt32 ci = 0; ci < fstats[fi].num_coders; ci++) {
                if (ci > 0) printf(" → ");
                printf("%s", method_name(fstats[fi].method_ids[ci]));
            }
            printf("\n");

            if (SzBitWithVals_Check(&ar->FolderCRCs, fi))
                printf("    Block CRC: 0x%08X\n", ar->FolderCRCs.Vals[fi]);

            printf("    Est. time: %.2f sec\n", fstats[fi].est_decompress_s);

            /* First/last few files */
            UInt32 shown = 0;
            for (UInt32 j = fstats[fi].first_file; j < fstats[fi].last_file; j++) {
                if (SzArEx_IsDir(&db, j)) continue;
                UInt64 off = db.UnpackPositions[j] -
                             db.UnpackPositions[fstats[fi].first_file];
                if (shown < 3 || j >= fstats[fi].last_file - 2) {
                    printf("      %c @%-12llu  %10s  %s\n",
                           shown < 3 ? ' ' : ' ',
                           (unsigned long long)off,
                           hsz_short(SzArEx_GetFileSize(&db, j)),
                           get_name(&db, j));
                } else if (shown == 3) {
                    printf("      ... (%u more files) ...\n",
                           fstats[fi].num_files - 5);
                }
                shown++;
            }
        }
    }

    /* ── Optional: per-file extraction cost ────────────────────────── */

    if (show_files) {
        section("PER-FILE EXTRACTION COST");
        printf("  %-55s %10s %10s %7s  blk  %10s %10s  %s\n",
               "File", "Size", "Packed", "Ratio",
               "BlkUnpack", "MustDecomp", "Note");
        sep();

        for (UInt32 i = 0; i < db.NumFiles; i++) {
            if (SzArEx_IsDir(&db, i)) continue;
            const char *name = get_name(&db, i);
            if (file_pattern && !ci_contains(name, file_pattern)) continue;

            UInt32 folder = db.FileToFolder[i];
            UInt64 filesz = SzArEx_GetFileSize(&db, i);

            /* Empty files may have no folder (0xFFFFFFFF) */
            if (folder >= ar->NumFolders) {
                printf("  %-55.55s %10s %10s %7s  ---  %10s %10s  %s\n",
                       name, "0", "-", "-", "-", "-", "empty / no block");
                continue;
            }

            /* Offset of this file within its block */
            UInt64 file_off_in_block = db.UnpackPositions[i] -
                                       db.UnpackPositions[db.FolderToFile[folder]];
            UInt64 block_unpack = fstats[folder].unpacked;
            UInt64 block_packed = fstats[folder].packed;

            /* To extract: decompress from block start up to file end */
            UInt64 must_decompress = file_off_in_block + filesz;
            if (must_decompress > block_unpack) must_decompress = block_unpack;

            /* Estimate packed bytes for this file (proportional) */
            UInt64 file_packed = (block_unpack > 0)
                ? (UInt64)((double)filesz / block_unpack * block_packed)
                : 0;

            const char *ratio_str = pct(file_packed, filesz);

            const char *note = "";
            if (fstats[folder].num_files == 1)
                note = "sole file in block";
            else if (file_off_in_block == 0)
                note = "first in block";
            else if (must_decompress > block_unpack * 9 / 10)
                note = "EXPENSIVE (near block end)";
            else if (must_decompress < block_unpack / 10)
                note = "cheap (near block start)";

            printf("  %-55.55s %10s %10s %7s  %-3u  %10s %10s  %s\n",
                   name,
                   hsz_short(filesz),
                   hsz_short(file_packed),
                   ratio_str,
                   folder,
                   hsz_short(block_unpack),
                   hsz_short(must_decompress),
                   note);
        }
    }

    /* ── Cleanup ──────────────────────────────────────────────────── */

    free(fstats);
    SzArEx_Free(&db, &g_Alloc);
    ISzAlloc_Free(&g_Alloc, lookBuf);
    File_Close(&archiveStream.file);

    printf("\n");
    return 0;
}

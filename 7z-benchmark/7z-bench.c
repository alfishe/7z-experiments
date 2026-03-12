/*
 * 7z-bench.c — 7z archive compression benchmark with realistic fileset
 *
 * Creates real .7z archives from a test fileset using different
 * dictionary and solid block size settings, then benchmarks extraction
 * using the SzArEx_Extract API (same fast path as fast7z).
 *
 * Key metrics per configuration:
 *   - Compression ratio (archive size / original)
 *   - Archive creation time (via 7zz CLI)
 *   - Full extraction throughput (MB/s via SzArEx_Extract)
 *   - Random single-file extraction time (worst case: file in middle
 *     of a solid block requires decompressing from block start)
 *
 * Build:
 *   make 7z-bench
 *
 * Usage:
 *   7z-bench                     # run with defaults (generate testdata/)
 *   7z-bench --folders N         # number of subfolders (default: 200)
 *   7z-bench --output report.md  # output file (default: bench_report.md)
 *   7z-bench --quick             # fewer dict sizes
 *   7z-bench --7z PATH           # path to 7z binary (default: 7zz)
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

#include "CpuArch.h"
#include "7z.h"
#include "7zAlloc.h"
#include "7zBuf.h"
#include "7zCrc.h"
#include "7zFile.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <errno.h>
#endif

/* ── Timing ───────────────────────────────────────────────────────── */

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ── Allocator ────────────────────────────────────────────────────── */

static const ISzAlloc g_Alloc = { SzAlloc, SzFree };

/* ── Archive wrapper (from fast7z.c) ──────────────────────────────── */

#define INPUT_BUF_SIZE (1 << 18)

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

static SRes archive_open(Archive *a, const char *path) {
    SRes res;
    memset(a, 0, sizeof(*a));
    a->cachedFolder = (UInt32)-1;

    if (InFile_Open(&a->archiveStream.file, path)) {
        return SZ_ERROR_FAIL;
    }

    FileInStream_CreateVTable(&a->archiveStream);
    LookToRead2_CreateVTable(&a->lookStream, False);

    a->lookBuf = (Byte *)ISzAlloc_Alloc(&g_Alloc, INPUT_BUF_SIZE);
    if (!a->lookBuf) return SZ_ERROR_MEM;

    a->lookStream.buf = a->lookBuf;
    a->lookStream.bufSize = INPUT_BUF_SIZE;
    a->lookStream.realStream = &a->archiveStream.vt;
    LookToRead2_INIT(&a->lookStream);

    SzArEx_Init(&a->db);

    res = SzArEx_Open(&a->db, &a->lookStream.vt, &g_Alloc, &g_Alloc);
    if (res == SZ_OK) a->opened = 1;
    return res;
}

static void archive_close(Archive *a) {
    if (a->cachedBuf) ISzAlloc_Free(&g_Alloc, a->cachedBuf);
    if (a->opened) SzArEx_Free(&a->db, &g_Alloc);
    ISzAlloc_Free(&g_Alloc, a->lookBuf);
    File_Close(&a->archiveStream.file);
}

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

/* ── Fileset generation ──────────────────────────────────────────── */

static void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void gen_file_content(Byte *buf, size_t size, uint32_t *rng, int file_type) {
    size_t pos = 0;
    while (pos < size) {
        *rng = *rng * 1103515245 + 12345;
        switch (file_type % 4) {
        case 0: /* Structured / code-like */
        {
            int pat_id = (*rng >> 16) & 0x1F;
            int line_len = 20 + ((*rng >> 8) & 0x3F);
            for (int j = 0; j < line_len && pos < size; j++)
                buf[pos++] = (Byte)(pat_id * 7 + j);
            if (pos < size) buf[pos++] = '\n';
            break;
        }
        case 1: /* JSON/config-like */
        {
            const char *keys[] = {"\"name\":", "\"value\":", "\"type\":", "\"id\":",
                                   "\"path\":", "\"size\":", "\"hash\":", "\"tag\":"};
            int k = (*rng >> 16) & 7;
            const char *key = keys[k];
            size_t klen = strlen(key);
            for (size_t j = 0; j < klen && pos < size; j++)
                buf[pos++] = (Byte)key[j];
            int vlen = 4 + ((*rng >> 8) & 0x1F);
            for (int j = 0; j < vlen && pos < size; j++) {
                *rng = *rng * 1103515245 + 12345;
                buf[pos++] = 'a' + ((*rng >> 16) % 26);
            }
            if (pos < size) buf[pos++] = '\n';
            break;
        }
        case 2: /* Binary with structure */
        {
            Byte base = (Byte)(*rng >> 16);
            int run = 8 + ((*rng >> 8) & 0x1F);
            for (int j = 0; j < run && pos < size; j++) {
                *rng = *rng * 1103515245 + 12345;
                if ((*rng & 0xF) < 11)
                    buf[pos++] = base + (Byte)((*rng >> 16) & 0x07);
                else
                    buf[pos++] = (Byte)(*rng >> 16);
            }
            break;
        }
        default: /* Random */
        {
            int run = 16 + ((*rng >> 8) & 0x3F);
            for (int j = 0; j < run && pos < size; j++) {
                *rng = *rng * 1103515245 + 12345;
                buf[pos++] = (Byte)(*rng >> 16);
            }
            break;
        }
        }
    }
}

static int generate_testdata(const char *base_dir, int nfolders) {
    uint32_t rng = 0xDEADBEEF;
    int total_files = 0;
    size_t total_bytes = 0;

    fprintf(stderr, "Generating test fileset in '%s/' (%d folders)...\n",
            base_dir, nfolders);
    mkdirs(base_dir);

    const char *extensions[] = {
        "json", "xml", "txt", "cfg", "log", "dat", "bin", "csv",
        "html", "js", "css", "py", "c", "h", "md", "yaml"
    };

    for (int folder = 0; folder < nfolders; folder++) {
        char dir_path[512];
        snprintf(dir_path, sizeof(dir_path), "%s/%03d", base_dir, folder);
        mkdirs(dir_path);

        rng = rng * 1103515245 + 12345;
        int nfiles = 1 + ((rng >> 16) % 20);

        for (int fi = 0; fi < nfiles; fi++) {
            rng = rng * 1103515245 + 12345;
            size_t fsize;
            int size_class = (rng >> 16) % 10;
            rng = rng * 1103515245 + 12345;
            if (size_class < 4)
                fsize = 256 + ((rng >> 16) % 3840);
            else if (size_class < 7)
                fsize = 4096 + ((rng >> 16) % 28672);
            else if (size_class < 9)
                fsize = 32768 + ((rng >> 16) % 65536);
            else
                fsize = 98304 + ((rng >> 16) % 32768);

            rng = rng * 1103515245 + 12345;
            const char *ext = extensions[(rng >> 16) % 16];

            char fpath[512];
            snprintf(fpath, sizeof(fpath), "%s/file_%02d.%s", dir_path, fi, ext);

            Byte *buf = (Byte *)malloc(fsize);
            rng = rng * 1103515245 + 12345;
            gen_file_content(buf, fsize, &rng, (int)(rng >> 16));

            FILE *f = fopen(fpath, "wb");
            if (f) {
                fwrite(buf, 1, fsize, f);
                fclose(f);
                total_files++;
                total_bytes += fsize;
            }
            free(buf);
        }
    }

    fprintf(stderr, "Generated %d files in %d folders (%.1f MB total)\n",
            total_files, nfolders, total_bytes / (1024.0 * 1024.0));
    return total_files;
}

/* Check if testdata/ directory exists and has content */
static int testdata_exists(const char *base_dir) {
    DIR *d = opendir(base_dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] != '.') count++;
    }
    closedir(d);
    return count > 0;
}

/* Recursive helpers for arbitrary directory depth */
static void scan_dir_recursive(const char *path, size_t *total_size, int *file_count) {
    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char fpath[1024];
        snprintf(fpath, sizeof(fpath), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(fpath, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir_recursive(fpath, total_size, file_count);
        } else if (S_ISREG(st.st_mode)) {
            if (total_size) *total_size += st.st_size;
            if (file_count) (*file_count)++;
        }
    }
    closedir(d);
}

/* Get total size of testdata/ */
static size_t testdata_total_size(const char *base_dir) {
    size_t total = 0;
    scan_dir_recursive(base_dir, &total, NULL);
    return total;
}

/* Count files in testdata/ */
static int testdata_file_count(const char *base_dir) {
    int count = 0;
    scan_dir_recursive(base_dir, NULL, &count);
    return count;
}

/* ── Benchmark configuration ──────────────────────────────────────── */

typedef struct {
    const char *dict_label;
    const char *block_label;
} BenchConfig;

typedef struct {
    const char *dict_label;
    const char *block_label;

    double   comp_ratio;        /* archive_size / original_size */
    double   create_time_s;     /* time to create archive */
    double   full_extract_s;    /* time to extract all files */
    double   full_extract_mbs;  /* MB/s */
    double   block_extract_ms;  /* worst-case: decompress largest solid block */
    size_t   archive_size;
    size_t   original_size;
    int      num_files;
    int      num_folders;       /* solid blocks in archive */

    /* Random single-file extraction samples */
    double   rnd_extract_min_ms;
    double   rnd_extract_avg_ms;
    double   rnd_extract_max_ms;
    double   rnd_extract_p50_ms;

    int      valid;
} BenchResult;

#define MAX_RESULTS 256
#define RND_SAMPLES 20

/* ── Run one benchmark ────────────────────────────────────────────── */

static BenchResult run_one(const char *sevenz_bin, const char *testdata_dir,
                            const char *dict_label, const char *block_label,
                            size_t original_size, int num_files) {
    BenchResult r;
    memset(&r, 0, sizeof(r));
    r.dict_label = dict_label;
    r.block_label = block_label;
    r.original_size = original_size;
    r.num_files = num_files;

    /* Archive filename */
    char archive_path[256];
    snprintf(archive_path, sizeof(archive_path),
             "bench_d%s_b%s.7z", dict_label, block_label);

    fprintf(stderr, "  dict=%-6s block=%-6s  ", dict_label, block_label);

    /* ── Phase 1: Create archive using 7z CLI ───────────────────── */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "%s a -t7z -m0=lzma2:d=%s -ms=%s -mx=5 -mmt=1 -bso0 -bsp0 '%s' '%s/*' 2>&1",
             sevenz_bin, dict_label, block_label, archive_path, testdata_dir);

    double t0 = now_s();
    int rc = system(cmd);
    r.create_time_s = now_s() - t0;

    if (rc != 0) {
        fprintf(stderr, "ARCHIVE CREATION FAILED (rc=%d)\n", rc);
        return r;
    }

    /* Get archive size */
    struct stat ast;
    if (stat(archive_path, &ast) != 0) {
        fprintf(stderr, "CANNOT STAT ARCHIVE\n");
        return r;
    }
    r.archive_size = ast.st_size;
    r.comp_ratio = (double)r.archive_size / original_size;

    /* ── Phase 2: Benchmark full extraction via SzArEx_Extract ── */
    Archive a;
    SRes sres = archive_open(&a, archive_path);
    if (sres != SZ_OK) {
        fprintf(stderr, "ARCHIVE OPEN FAILED (res=%d)\n", sres);
        return r;
    }

    r.num_folders = (int)a.db.db.NumFolders;

    /* Full extraction: extract every file */
    t0 = now_s();
    UInt32 nfiles_in_archive = a.db.NumFiles;
    for (UInt32 i = 0; i < nfiles_in_archive; i++) {
        if (SzArEx_IsDir(&a.db, i)) continue;
        const Byte *data;
        size_t size;
        sres = archive_extract(&a, i, &data, &size);
        if (sres != SZ_OK) {
            fprintf(stderr, "EXTRACT FAILED (file %u)\n", i);
            archive_close(&a);
            return r;
        }
    }
    r.full_extract_s = now_s() - t0;
    r.full_extract_mbs = (original_size / (1024.0 * 1024.0)) / r.full_extract_s;

    /* ── Phase 3: Random single-file extraction ─────────────────── */
    /* Pick random files, invalidate cache each time to force
     * decompression from scratch (worst case) */
    double samples[RND_SAMPLES];
    uint32_t rng = 0x12345678;

    /* Collect non-dir file indices */
    UInt32 *file_indices = (UInt32 *)malloc(nfiles_in_archive * sizeof(UInt32));
    int actual_files = 0;
    for (UInt32 i = 0; i < nfiles_in_archive; i++) {
        if (!SzArEx_IsDir(&a.db, i))
            file_indices[actual_files++] = i;
    }

    for (int s = 0; s < RND_SAMPLES; s++) {
        /* Pick random file */
        rng = rng * 1103515245 + 12345;
        int pick = (rng >> 16) % actual_files;
        UInt32 fi = file_indices[pick];

        /* Invalidate folder cache to simulate cold extraction */
        if (a.cachedBuf) {
            ISzAlloc_Free(&g_Alloc, a.cachedBuf);
            a.cachedBuf = NULL;
            a.cachedSize = 0;
            a.cachedFolder = (UInt32)-1;
        }

        const Byte *data;
        size_t size;
        double ts = now_s();
        archive_extract(&a, fi, &data, &size);
        samples[s] = (now_s() - ts) * 1000.0; /* ms */
    }
    free(file_indices);
    archive_close(&a);

    /* Sort samples for percentiles */
    for (int i = 0; i < RND_SAMPLES - 1; i++)
        for (int j = i + 1; j < RND_SAMPLES; j++)
            if (samples[j] < samples[i]) {
                double t = samples[i]; samples[i] = samples[j]; samples[j] = t;
            }

    r.rnd_extract_min_ms = samples[0];
    r.rnd_extract_max_ms = samples[RND_SAMPLES - 1];
    r.rnd_extract_p50_ms = samples[RND_SAMPLES / 2];
    double sum = 0;
    for (int i = 0; i < RND_SAMPLES; i++) sum += samples[i];
    r.rnd_extract_avg_ms = sum / RND_SAMPLES;

    /* ── Phase 4: Measure single block decompression time ────────── */
    /* Reopen archive, invalidate cache, extract first file from largest
     * folder to measure worst-case block decompression time */
    {
        Archive a2;
        sres = archive_open(&a2, archive_path);
        if (sres == SZ_OK) {
            /* Find a file in the middle of the archive */
            UInt32 mid_file = nfiles_in_archive / 2;
            while (mid_file < nfiles_in_archive && SzArEx_IsDir(&a2.db, mid_file))
                mid_file++;
            if (mid_file < nfiles_in_archive) {
                const Byte *data;
                size_t size;
                double ts = now_s();
                archive_extract(&a2, mid_file, &data, &size);
                r.block_extract_ms = (now_s() - ts) * 1000.0;
            }
            archive_close(&a2);
        }
    }

    r.valid = 1;

    fprintf(stderr, "ratio=%.1f%%  create=%.1fs  extract=%.1f MB/s  "
            "block=%.1fms  rnd_p50=%.1fms  blocks=%d\n",
            r.comp_ratio * 100.0, r.create_time_s, r.full_extract_mbs,
            r.block_extract_ms, r.rnd_extract_p50_ms, r.num_folders);

    /* Clean up archive */
    unlink(archive_path);

    return r;
}

/* ── Report generation ────────────────────────────────────────────── */

static void emit_report(FILE *out, BenchResult *results, int nresults,
                          double total_time, size_t original_size,
                          int num_files, int num_folders_gen,
                          const char **dicts, int ndicts,
                          const char **blocks, int nblocks) {
    fprintf(out, "# 7z Compression Benchmark Report\n\n");

    fprintf(out, "## System & Test Parameters\n\n");
#ifndef _WIN32
    struct utsname un;
    if (uname(&un) == 0)
        fprintf(out, "| Parameter | Value |\n|---|---|\n"
                "| OS | %s %s %s |\n", un.sysname, un.release, un.machine);
    else
        fprintf(out, "| Parameter | Value |\n|---|---|\n");
#else
    fprintf(out, "| Parameter | Value |\n|---|---|\n");
#endif

    fprintf(out, "| Test data | %d files in %d folders (%.1f MB) |\n",
            num_files, num_folders_gen, original_size / (1024.0 * 1024.0));
    fprintf(out, "| Extraction API | SzArEx_Extract (fast7z method) |\n");
    fprintf(out, "| Random samples | %d cold extractions per config |\n", RND_SAMPLES);
    fprintf(out, "| Total benchmark time | %.1f sec |\n\n", total_time);

    /* ── Main results table ───────────────────────────────────────── */

    fprintf(out, "## Compression & Extraction Performance\n\n");
    fprintf(out, "| Dict | Block | Ratio | Archive | Create(s) | "
            "Extract MB/s | Blocks | Block(ms) | Rnd p50(ms) | Rnd max(ms) |\n");
    fprintf(out, "|------|-------|------:|--------:|----------:|"
            "------------:|-------:|----------:|------------:|------------:|\n");

    for (int i = 0; i < nresults; i++) {
        BenchResult *r = &results[i];
        if (!r->valid) continue;
        char sz_buf[32];
        if (r->archive_size > 1048576)
            snprintf(sz_buf, sizeof(sz_buf), "%.1fM", r->archive_size / (1024.0*1024.0));
        else
            snprintf(sz_buf, sizeof(sz_buf), "%.0fK", r->archive_size / 1024.0);
        fprintf(out, "| %s | %s | %.1f%% | %s | %.1f | %.1f | %d | %.1f | %.1f | %.1f |\n",
                r->dict_label, r->block_label,
                r->comp_ratio * 100.0, sz_buf,
                r->create_time_s,
                r->full_extract_mbs,
                r->num_folders,
                r->block_extract_ms,
                r->rnd_extract_p50_ms,
                r->rnd_extract_max_ms);
    }

    /* ── Random Access Detail Table ──────────────────────────────── */

    fprintf(out, "\n## Random Single-File Extraction Time\n\n");
    fprintf(out, "Cold extraction of a random file (folder cache invalidated).\n\n");
    fprintf(out, "| Dict | Block | Min(ms) | Avg(ms) | P50(ms) | Max(ms) | Verdict |\n");
    fprintf(out, "|------|-------|--------:|--------:|--------:|--------:|--------:|\n");

    for (int i = 0; i < nresults; i++) {
        BenchResult *r = &results[i];
        if (!r->valid) continue;
        const char *verdict;
        if (r->rnd_extract_p50_ms < 5) verdict = "Instant";
        else if (r->rnd_extract_p50_ms < 50) verdict = "Fast";
        else if (r->rnd_extract_p50_ms < 200) verdict = "OK";
        else if (r->rnd_extract_p50_ms < 1000) verdict = "Slow";
        else verdict = "**Very slow**";

        fprintf(out, "| %s | %s | %.1f | %.1f | %.1f | %.1f | %s |\n",
                r->dict_label, r->block_label,
                r->rnd_extract_min_ms, r->rnd_extract_avg_ms,
                r->rnd_extract_p50_ms, r->rnd_extract_max_ms, verdict);
    }

    /* ── Recommendations ──────────────────────────────────────────── */

    fprintf(out, "\n## Recommendations\n\n");

    int best_ratio = -1, best_extract = -1, best_rnd = -1;
    double best_ratio_v = 1.0, best_extract_v = 0, best_rnd_v = 1e9;

    for (int i = 0; i < nresults; i++) {
        if (!results[i].valid) continue;
        if (results[i].comp_ratio < best_ratio_v) {
            best_ratio_v = results[i].comp_ratio;
            best_ratio = i;
        }
        if (results[i].full_extract_mbs > best_extract_v) {
            best_extract_v = results[i].full_extract_mbs;
            best_extract = i;
        }
        if (results[i].rnd_extract_p50_ms < best_rnd_v) {
            best_rnd_v = results[i].rnd_extract_p50_ms;
            best_rnd = i;
        }
    }

    if (best_ratio >= 0) {
        BenchResult *r = &results[best_ratio];
        fprintf(out, "**Best compression:** dict=%s block=%s → %.1f%% ratio\n",
                r->dict_label, r->block_label, r->comp_ratio * 100.0);
        fprintf(out, "  `7z a -m0=lzma2:d=%s -ms=%s archive.7z`\n\n",
                r->dict_label, r->block_label);
    }
    if (best_rnd >= 0) {
        BenchResult *r = &results[best_rnd];
        fprintf(out, "**Best random access:** dict=%s block=%s → p50=%.1f ms\n",
                r->dict_label, r->block_label, r->rnd_extract_p50_ms);
        fprintf(out, "  `7z a -m0=lzma2:d=%s -ms=%s archive.7z`\n\n",
                r->dict_label, r->block_label);
    }
    if (best_extract >= 0) {
        BenchResult *r = &results[best_extract];
        fprintf(out, "**Best full extraction:** dict=%s block=%s → %.1f MB/s\n",
                r->dict_label, r->block_label, r->full_extract_mbs);
        fprintf(out, "  `7z a -m0=lzma2:d=%s -ms=%s archive.7z`\n\n",
                r->dict_label, r->block_label);
    }

    /* ── Mermaid: Random access time vs block size ────────────────── */

    fprintf(out, "\n## Random Access Time vs Block Size\n\n");
    fprintf(out, "```mermaid\nxychart-beta\n");
    fprintf(out, "    title \"Random file extraction p50 (ms, log-ish, lower=better)\"\n");
    fprintf(out, "    x-axis [");
    for (int b = 0; b < nblocks; b++)
        fprintf(out, "%s\"%s\"", b ? ", " : "", blocks[b]);
    fprintf(out, "]\n");

    double max_rnd = 0;
    for (int i = 0; i < nresults; i++)
        if (results[i].valid && results[i].rnd_extract_p50_ms > max_rnd)
            max_rnd = results[i].rnd_extract_p50_ms;
    fprintf(out, "    y-axis \"Milliseconds\" 0 --> %.0f\n", max_rnd * 1.1);

    for (int d = 0; d < ndicts; d++) {
        fprintf(out, "    line [");
        for (int b = 0; b < nblocks; b++) {
            double ms = 0;
            for (int i = 0; i < nresults; i++) {
                if (results[i].valid &&
                    strcmp(results[i].dict_label, dicts[d]) == 0 &&
                    strcmp(results[i].block_label, blocks[b]) == 0)
                    ms = results[i].rnd_extract_p50_ms;
            }
            fprintf(out, "%s%.1f", b ? ", " : "", ms);
        }
        fprintf(out, "]\n");
    }
    fprintf(out, "```\n");

    /* ── Mermaid: Block decompression time vs block size ──────────── */

    fprintf(out, "\n## Block Decompression Time vs Block Size\n\n");
    fprintf(out, "```mermaid\nxychart-beta\n");
    fprintf(out, "    title \"Single block decompression (ms, lower=better)\"\n");
    fprintf(out, "    x-axis [");
    for (int b = 0; b < nblocks; b++)
        fprintf(out, "%s\"%s\"", b ? ", " : "", blocks[b]);
    fprintf(out, "]\n");

    double max_block = 0;
    for (int i = 0; i < nresults; i++)
        if (results[i].valid && results[i].block_extract_ms > max_block)
            max_block = results[i].block_extract_ms;
    fprintf(out, "    y-axis \"Milliseconds\" 0 --> %.0f\n", max_block * 1.1);

    for (int d = 0; d < ndicts; d++) {
        fprintf(out, "    bar [");
        for (int b = 0; b < nblocks; b++) {
            double ms = 0;
            for (int i = 0; i < nresults; i++) {
                if (results[i].valid &&
                    strcmp(results[i].dict_label, dicts[d]) == 0 &&
                    strcmp(results[i].block_label, blocks[b]) == 0)
                    ms = results[i].block_extract_ms;
            }
            fprintf(out, "%s%.1f", b ? ", " : "", ms);
        }
        fprintf(out, "]\n");
    }
    fprintf(out, "```\n");

    /* ── Mermaid: Compression ratio vs block size ─────────────────── */

    fprintf(out, "\n## Compression Ratio vs Block Size\n\n");
    fprintf(out, "```mermaid\nxychart-beta\n");
    fprintf(out, "    title \"Archive ratio (%%, smaller=better)\"\n");
    fprintf(out, "    x-axis [");
    for (int b = 0; b < nblocks; b++)
        fprintf(out, "%s\"%s\"", b ? ", " : "", blocks[b]);
    fprintf(out, "]\n");
    fprintf(out, "    y-axis \"Ratio (%%)\" 60 --> 70\n");

    for (int d = 0; d < ndicts; d++) {
        fprintf(out, "    line [");
        for (int b = 0; b < nblocks; b++) {
            double ratio = 0;
            for (int i = 0; i < nresults; i++) {
                if (results[i].valid &&
                    strcmp(results[i].dict_label, dicts[d]) == 0 &&
                    strcmp(results[i].block_label, blocks[b]) == 0)
                    ratio = results[i].comp_ratio * 100.0;
            }
            fprintf(out, "%s%.1f", b ? ", " : "", ratio);
        }
        fprintf(out, "]\n");
    }
    fprintf(out, "```\n");

    fprintf(out, "### Key Takeaways\n\n");
    fprintf(out, "- **Solid block size** is the primary control for random access speed.\n");
    fprintf(out, "  Smaller blocks = faster single-file extraction, but worse compression.\n");
    fprintf(out, "- **Dictionary size** primarily affects compression ratio.\n");
    fprintf(out, "  Diminishing returns above 4-16MB for typical filesets.\n");
    fprintf(out, "- For archives you'll extract individual files from, prioritize block size.\n");
    fprintf(out, "- For backup/archival with full extraction only, use solid for best ratio.\n");
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    int num_folders = 200;
    const char *output_file = "bench_report.md";
    const char *sevenz_bin = "7zz";
    int quick = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--folders") == 0 && i + 1 < argc)
            num_folders = atoi(argv[++i]);
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            output_file = argv[++i];
        else if (strcmp(argv[i], "--7z") == 0 && i + 1 < argc)
            sevenz_bin = argv[++i];
        else if (strcmp(argv[i], "--quick") == 0)
            quick = 1;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "7z-bench — 7z archive compression benchmark\n\n"
                "  --folders N    Number of test subfolders (default: 200)\n"
                "  --output FILE  Report file (default: bench_report.md)\n"
                "  --7z PATH      Path to 7z binary (default: 7zz)\n"
                "  --quick        Fewer dict sizes for faster run\n"
            );
            return 0;
        }
    }

    CrcGenerateTable();

    /* ── Generate or reuse test fileset ──────────────────────────── */
    const char *testdata_dir = "testdata";
    if (!testdata_exists(testdata_dir)) {
        generate_testdata(testdata_dir, num_folders);
    } else {
        fprintf(stderr, "Reusing existing '%s/' directory\n", testdata_dir);
    }

    size_t original_size = testdata_total_size(testdata_dir);
    int num_files = testdata_file_count(testdata_dir);
    fprintf(stderr, "Test data: %d files, %.1f MB\n\n",
            num_files, original_size / (1024.0 * 1024.0));

    /* ── Define test matrix ─────────────────────────────────────── */
    const char *dicts_full[] =  { "64k", "256k", "1m", "4m", "16m", "64m" };
    const char *dicts_quick[] = { "64k", "1m", "16m" };
    const char *blocks[] = {
        "16k", "64k", "128k", "256k", "512k",
        "1m", "4m", "16m", "32m", "on"  /* "on" = fully solid */
    };

    const char **dicts = quick ? dicts_quick : dicts_full;
    int ndicts = quick ? 3 : 6;
    int nblocks = 10;

    int nresults = ndicts * nblocks;
    fprintf(stderr, "Running %d benchmark combinations...\n\n", nresults);

    BenchResult *results = (BenchResult *)calloc(nresults, sizeof(BenchResult));
    double t_total = now_s();

    int ri = 0;
    for (int d = 0; d < ndicts; d++) {
        for (int b = 0; b < nblocks; b++) {
            results[ri] = run_one(sevenz_bin, testdata_dir,
                                   dicts[d], blocks[b],
                                   original_size, num_files);
            ri++;
        }
    }

    double total_time = now_s() - t_total;
    fprintf(stderr, "\nAll benchmarks complete in %.1f sec\n", total_time);

    /* ── Generate report ────────────────────────────────────────── */
    FILE *out = stdout;
    if (output_file) {
        out = fopen(output_file, "w");
        if (!out) { fprintf(stderr, "Cannot write to '%s'\n", output_file); out = stdout; }
    }

    emit_report(out, results, nresults, total_time,
                original_size, num_files, num_folders,
                dicts, ndicts, blocks, nblocks);

    if (out != stdout) {
        fclose(out);
        fprintf(stderr, "Report written to '%s'\n", output_file);
    }

    free(results);
    return 0;
}

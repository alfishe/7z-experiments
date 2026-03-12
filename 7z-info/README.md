# 7z-info — 7z Archive Structure Analyzer

A fast, single-file C tool that parses and reports the internal binary structure of `.7z` archives without extracting any data.

## What It Does

- **Binary layout** — maps every byte of the archive: signature header, pack streams, metadata blocks
- **Block analysis** — reports solid block boundaries, sizes, compression ratios, and per-block decompression throughput
- **File listing** — correlates files to their solid blocks with individual sizes and ratios
- **Format forensics** — validates CRCs, decodes LZMA codec parameters, identifies header encoding

## Build

```bash
make
```

Requires only a C compiler. The LZMA SDK sources are included in `lzma/`.

## Usage

```bash
# Overview only (default)
./7z-info archive.7z

# With block-level detail
./7z-info archive.7z --blocks

# With full file listing
./7z-info archive.7z --files
```

---

## Output Examples

### Overview (`./7z-info archive.7z`)

```
────────────────────────────────────────────────────────────────────────
  ARCHIVE OVERVIEW
────────────────────────────────────────────────────────────────────────
  File:              archive.7z
  Archive size:      4.210 GB (4520193847)
  Format version:    0.4
  Total files:       248371  (31204 directories, 12 empty files)
  Uncompressed:      4.512 GB (4844091328)
  Compressed:        4.193 GB (4502016539)  (ratio: 92.9%)
  Solid blocks:      248359
  Pack streams:      248359

────────────────────────────────────────────────────────────────────────
  BINARY LAYOUT  (offset → content)
────────────────────────────────────────────────────────────────────────
  [0x000000000000]  Signature header (32 bytes)
       Signature:    7z BC AF 27 1C
       Version:      0.4
       StartHdrCRC:  0xA37B19C4
       NextHdrOfs:   4520193804 (0x10D63B2EC)
       NextHdrSize:  43 (0x2B)
       NextHdrCRC:   0x72E5D891
  [0x000000000020]  Packed data streams (4.210 GB)
       (248359 pack streams, first @0x20, last ends @0x10D4A8F01)
  [0x0010D63B30C]   Header / metadata (43 B) [LZMA-compressed]
       The file index is itself compressed.
       To list files, the header must be read + decompressed.
  [0x0010D63B337]   End of archive

────────────────────────────────────────────────────────────────────────
  INDEX / HEADER ANALYSIS
────────────────────────────────────────────────────────────────────────
  Header location:   end of archive (offset 0x10D63B30C)
  Header size (on disk):  43 B
  Header compressed: YES
  Index parse time:  312.47 ms
    (CRC table init: 0.00 ms)
  Metadata stored:   names, CRC (248359/248371), mtime, attribs

────────────────────────────────────────────────────────────────────────
  SOLID BLOCK STRUCTURE
────────────────────────────────────────────────────────────────────────
  248359 blocks spanning 248371 files (avg 1 files/block)

  Block size range:
    Packed:    75 B  ..  640.5 KB
    Unpacked:  160 B  ..  640.5 KB

  Compression methods found:
    0x21 = LZMA2

────────────────────────────────────────────────────────────────────────
  EXTRACTION COST ANALYSIS
────────────────────────────────────────────────────────────────────────
  Random single-file extraction:
    Best case:   decompress 160 B  (smallest block)
    Worst case:  decompress 640.5 KB  (largest block)
    Est. worst:  ~0.0 sec  (assuming ~200 MB/s LZMA decode)

────────────────────────────────────────────────────────────────────────
  RANDOM ACCESS RATING
────────────────────────────────────────────────────────────────────────
  ****  [4/5]  GOOD — small blocks, fast random extraction
```

> **Why is the compression ratio only 92.9%?** Two factors:
> 1. **Pre-compressed content** — the archive is dominated by JPEG images, which are already DCT-compressed. LZMA2 cannot compress them further and stores them verbatim at ~100% ratio (visible in the block analysis: `photo.jpg` files show 99-100% ratio).
> 2. **Per-block overhead** — with 248K individual solid blocks (1 file per block), each carries its own LZMA2 stream header. This metadata overhead adds up across hundreds of thousands of tiny files. A solid archive of the same data would be marginally smaller, but 269,000× slower for random access.
>
> For archives of mostly text or uncompressed data, expect 30-60% ratios. The trade-off here is deliberate: we sacrifice compression for instant random file access.

### Block Analysis (`./7z-info archive.7z --blocks`)

Adds measured decompression performance by sampling blocks across the archive:

```
────────────────────────────────────────────────────────────────────────
  MEASURED DECOMPRESSION PERFORMANCE
────────────────────────────────────────────────────────────────────────
  Sampling 100 blocks spread across the archive...

  Block     Packed        Unpacked      Ratio      Time(us)  Throughput  File
────────────────────────────────────────────────────────────────────────
  0         187 B         266 B         70.3%          54       4.7 MB/s  data/000000/index.md
  2483      7.3 KB        7.5 KB        97.9%         274      26.7 MB/s  data/001247/thumb_03.jpg
  4966      605 B         1.4 KB        43.5%          36      36.8 MB/s  data/002891/content.md
  7449      65.8 KB       65.8 KB       100.0%         37    1736.9 MB/s  data/004520/photo.jpg
  9932      7.8 KB        7.9 KB        98.1%         277      27.9 MB/s  data/006103/thumb_01.jpg
  12415     9.0 KB        9.1 KB        98.6%         324      27.4 MB/s  data/007812/thumb_14.jpg
  14898     920 B         3.8 KB        23.7%          49      75.4 MB/s  data/009447/content.md
  17381     55.6 KB       55.7 KB       99.8%        1950      27.9 MB/s  data/011203/photo_06.jpg
  19864     9.6 KB        9.7 KB        98.7%         342      27.8 MB/s  data/012978/thumb_09.jpg
  22347     51.2 KB       51.2 KB       100.0%         17    2940.5 MB/s  data/015104/photo_03.jpg
  ...       ...           ...           ...            ...      ...       ...
```

### File Listing (`./7z-info archive.7z --files`)

Adds a per-file table with block assignment and individual compression:

```
  Block     Packed        Unpacked      Ratio      Time(us)  Throughput  File
────────────────────────────────────────────────────────────────────────
  0         187 B         266 B         70.3%          32       7.9 MB/s  data/000000/index.md
  1         7.3 KB        7.5 KB        97.9%        4153       1.8 MB/s  data/000001/thumb_03.jpg
  2         605 B         1.4 KB        43.5%         162       8.2 MB/s  data/000002/content.md
  3         587 B         1.5 KB        39.1%         141      10.2 MB/s  data/000003/content.md
  4         65.8 KB       65.8 KB       100.0%         49    1311.6 MB/s  data/000004/photo.jpg
  5         12.1 KB       12.3 KB       98.4%         412      29.1 MB/s  data/000005/thumb_01.jpg
  6         48.2 KB       48.3 KB       99.8%        1677      28.1 MB/s  data/000006/photo_01.jpg
  7         8.4 KB        8.5 KB        98.8%         298      27.8 MB/s  data/000007/thumb_02.jpg
  ...       ...           ...           ...            ...      ...       ...
  248358    1.2 KB        3.1 KB        38.7%          45      67.3 MB/s  data/177831/content.md
```

## Key Observations

This tool reveals critical information about archive structure:

| Observation | Impact |
|-------------|--------|
| **1 file per block** | Each file independently extractable — ideal for random access |
| **LZMA2 codec only** | Fast single-threaded decompression |
| **Incompressible data passes through** | JPEGs stored at 100% ratio with ~2000 MB/s "decompression" |
| **Text compresses well** | `.md` files at 35-70% ratio with 5-75 MB/s throughput |
| **43-byte compressed header** | Entire file index fits in the EncodedHeader pointer block |

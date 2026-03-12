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
# Full overview
./7z-info archive.7z

# With file listing
./7z-info archive.7z --files

# With block details
./7z-info archive.7z --blocks
```

## Output Example

```
  File:              recipes.7z
  Archive size:      36.610 GB
  Total files:       1,381,243  (177,832 directories)
  Solid blocks:      1,381,215
  Compressed:        36.593 GB  (ratio: 99.0%)
```

## Key Findings

This tool was instrumental in discovering that the `recipes.7z` test archive uses **1 file per solid block** (1,381,215 blocks for 1,381,243 files), meaning each file can be extracted independently without decompressing unrelated data — ideal for random access.

# 7z-benchmark — LZMA2 Compression vs Random Access Benchmark

Systematic benchmarking of how 7z archive parameters (dictionary size, solid block size) affect **random single-file extraction latency** — the metric that matters most for archive-as-filesystem use cases.

## What It Does

- Creates a test dataset of mixed files (images, text, binary) across many folders
- Compresses with a matrix of dictionary sizes × solid block sizes
- Measures **cold random extraction** latency (p50, avg, max) for each configuration
- Generates a Markdown report with performance tables and recommendations

## Build

```bash
make
```

## Usage

```bash
# Full benchmark (all configurations, takes ~1 hour)
./7z-bench --output report.md

# Quick mode (subset of configs)
./7z-bench --quick --output report.md
```

## Key Results

### Solid Block Size vs Extraction Latency

| Block Size | p50 Latency | Verdict |
|------------|-------------|---------|
| 16 KB | **0.1 ms** | ⚡ Instant |
| 64 KB | 0.3 ms | ⚡ Instant |
| 128 KB | 1.3 ms | ⚡ Instant |
| 256 KB | 4.9 ms | ✅ Fast |
| 512 KB | 12.4 ms | ✅ Fast |
| 1 MB | 24.8 ms | ✅ Fast |
| 4 MB | 104.5 ms | ⚠ OK |
| 16 MB | 451.0 ms | ❌ Slow |
| Solid (off) | **26,897 ms** | 🔴 Unusable |

### Recommended Command

```bash
# Best random access with reasonable compression:
7z a -m0=lzma2:d=1m -ms=16k archive.7z files/
```

This creates an archive where any individual file can be extracted in **< 1 ms**.

## Parameters Tested

| Parameter | Values |
|-----------|--------|
| Dictionary size (`d=`) | 64K, 1M, 16M |
| Solid block size (`ms=`) | 16K, 64K, 128K, 256K, 512K, 1M, 4M, 16M, 32M, `on` (solid) |
| Extraction method | `SzArEx_Extract` (LZMA SDK C API) |
| Samples per config | 20 cold extractions |

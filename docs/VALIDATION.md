# Validation and performance

DiskScope 0.1.0 preview, 2026-11-15. Fixtures were disposable directories and generated SQLite results. No whole-system-volume stress scan was performed.

## Environments

| Target | Build and execution evidence |
|---|---|
| Windows 11 x64, build 26300, NTFS | Native MSYS2 UCRT64 GCC 16.2.0. NTFS fixtures exercised long UTF-16 paths, emoji, hardlinks, sparse files, compression, a real junction, and advanced through 70 directory levels with bounded search handles. The standard-profile metadata helper and its pause boundary were exercised. The portable GUI opened a native Windows window and the CLI produced a standalone SQLite snapshot with a Windows-system-only PATH. |
| Ubuntu 24.04.4 LTS amd64 | WSL 1, GCC 13.3.0, GTK 4.14.5. The Linux scanner ran through its `fstatat` and mountinfo compatibility branch. Real same-device bind-mount, permission denial, symlink, sparse, file-replacement, and 1,000-level fixtures were exercised. The depth test used a descriptor limit of 12. Xvfb exercised GTK with the default renderer and `--cairo`. |
| Native Linux kernel, desktop Wayland, Windows clean VM | NOT RUN. WSL 1 lacks native `openat2`/`statx` support and transforms invalid UTF-8 filenames before they reach the scanner. The raw-filename case reports SKIP there. The Windows portable test removed MSYS2 from PATH on the build machine. |

## CTest results

| Suite | Outcome |
|---|---|
| Windows native UCRT64 Release, GUI enabled | 16/16 groups PASS |
| Ubuntu 24.04.4 WSL 1 Release, GUI enabled | 17/17 groups PASS |
| Ubuntu 24.04.4 WSL 1 ASan/UBSan, GUI enabled | 17/17 groups PASS |
| Ubuntu 24.04.4 WSL 1 non-root smoke | CLI scan, export, compare and Xvfb snapshot import PASS with both renderers |

CTest groups can contain multiple assertions, fixtures and environment-dependent subcases. The Linux raw-filename subcase is SKIP on WSL 1. ASan/UBSan instrument project C code.

## Acceptance evidence

| IDs | Evidence and remaining coverage |
|---|---|
| A01–A05, A08 | Checked 64-bit arithmetic and BLOB order; fixed 2,017 / 12,288 / 8,192-byte logical, allocated, and unique fixture; reversed insertion order, unknown and conflicting observations; regular-file scope in output. |
| A06–A07 | Sparse on both backends and NTFS compression exercised. Reflink physical exclusivity is outside the calculation. |
| A09–A13, A15–A18 | Platform tests cover permissions and races, junctions or symlinks, same-device bind mount, special file handling, metadata failures, original name encoding and deep paths. Native Linux invalid-byte storage and modern syscall coverage are NOT RUN. |
| A14, A32–A34 | Windows placeholder flags and metadata race tests use synthetic metadata. A live cloud provider is NOT RUN. A scanner-only Linux trace showed no fixture-file payload opens or reads. Windows ETW file I/O trace is SKIP because the current session lacked permission. |
| A19–A24, A37–A39 | Real worker/CLI fixture; simulated pause, late cancellation commit and worker loss; oversized and deeply nested IPC rejected; WAL backup; schema and path validation; held readers, SQLite FULL, export ENOSPC and ending reserve. |
| A25–A27 | Partial-scan differences on identical roots, exact decimal JSON strings, CSV formula/quote handling and native names. CLI scan/export/compare exercised end-to-end. |
| A28–A31 | Paged model tests cover stale queries, eviction, prefetch and global retained capacity. Treemap geometry and hit testing exercised in C. A complete desktop interaction, small-screen, keyboard and display scale matrix is NOT RUN. |
| A35–A36 | Portable ZIP ran with a Windows-only PATH. The Ubuntu package was extracted and launched with the declared runtime libraries present. Clean deployment and complete interactive desktop coverage are NOT RUN. |

## Synthetic performance

Ubuntu 24.04.4 WSL 1, x86-64, i7-10700K, 16 logical processors, about 24 GiB WSL-reported RAM, disposable WSL filesystem. C17 with GCC 13, release-style `-O2 -DNDEBUG`, the pinned SQLite 3.54.1 and the Ubuntu libraries documented in [Dependencies](DEPENDENCIES.md). The canonical dataset had 100,000 directories and 900,000 files plus the root, 32-byte names and depth 2. The standalone result was 420,581,376 bytes. SQL page measurements are 32 queries of 512 entries and include application result conversion.

| Probe | Measured |
|---|---:|
| Validated standalone open | 9.25 s |
| Wide result name-sorted SQL page p95 | 123.27 ms |
| Wide result top allocation SQL page p95 | 167.15 ms |
| Isolated optimized headless probe peak RSS | 13,136 KiB |
| Headless paged model with 1M-row test fixture, including creation | 6.55 s, 38,820 KiB peak RSS |
| Flat 1M-row stress append / finalize / JSONL export | 13.835 s / 7.334 s / 19.400 s |
| Flat 1M-row stress test peak RSS | 47,548 KiB |
| Combined canonical fixture creation and probe under ASan/UBSan | 53.74 s, 1,038,496 KiB peak RSS |

The flat stress export wrote 435,778,308 bytes. The optimized headless figures exclude the GTK desktop. The combined sanitizer run exceeded 768 MiB. The project targets of total process RSS no greater than 768 MiB, interaction p95 no greater than 100 ms, and ordinary local cancellation timing remain unverified for the full desktop. The mocked cancellation fixture intentionally kept its worker alive for more than two seconds.

To repeat the canonical probe:

```sh
cmake --preset release
cmake --build --preset release --target diskscope-benchmark
build/release/bin/diskscope-benchmark --build /path/to/empty-directory --samples 32
build/release/bin/diskscope-benchmark --probe /path/to/result/benchmark.dscan
```

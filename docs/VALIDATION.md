# Validation and performance

## 0.1.2 preview

| Evidence | Result |
|---|---|
| Native Windows 11 x64, build 26300 | UCRT64 Release CTest 20/20 groups PASS. The retention symlink subcase reports SKIP without symlink privilege. A separate real NTFS junction fixture preserved aged caches through the junction alias and pruned the same eligible caches through the ordinary base. |
| Extracted Windows portable ZIP | The packaged CLI and both GTK renderers ran with a Windows-system-only PATH, an isolated profile and a separate working directory. The CLI reported 8 fixture observations and 17 logical bytes, with hardlinks and a real junction, a 345-unit UTF-16 source path and a 1,174-unit cache base. Both renderers loaded the bundled GTK libraries and opened private imports beyond 1,240 UTF-16 units. Clean Windows VM NOT RUN because the host had no accessible guest. |
| Ubuntu 24.04.4 amd64 on WSL 1 | Debug, Release and ASan/UBSan each passed 20/20 CTest groups. The raw-filename subcase reports SKIP under WSL 1. The release DEB was extracted as an ordinary user. Xvfb exercised the default and Cairo renderers and headless Weston exercised Cairo at GDK scale 2. The CLI scanned a disposable root, then the GUI opened a snapshot and recovered cached rows with the original fixture moved. |
| Fresh Ubuntu 24.04 amd64 package install | GitHub Actions installed the CI-built DEB and its runtime dependencies in the [official Ubuntu container](https://hub.docker.com/_/ubuntu/tags?name=24.04) at the digest recorded in the workflow. The CLI, GTK import and startup recovery smoke passed under an ordinary user. GCC, CMake and Ninja were absent at runtime. [CI run](https://github.com/mk0000001/DiskScope/actions/runs/43716280965) |
| Cache retention | Fixtures cover age, count, byte budget, newest recoverable cache, live leases, cancellation, foreign and linked directories, unknown files, and atomic replacement of the primary file. The GTK smoke checks that displayed private imports keep a lease until the app exits. |
| Diagnostic report | Integration tests cover default redaction, root opt-in, environment-only output, cancellation preserving the destination, and source database protection. The real GTK save dialog in the extracted Ubuntu package wrote both the redacted and root-enabled JSON. The only changed JSON fields were `paths_redacted` and `scan.root`. |

### GUI interaction and memory

A 0.1.2 Release build ran on Ubuntu 24.04.4 WSL 1, i7-10700K, GTK 4.14.5 with Cairo at GDK scale 1, and Xvfb at 1440×1000×24 and 96 DPI. The synthetic SQLite result has 1,000,001 rows including the root, 420,585,472 bytes, and SHA-256 `703c6c496edcabcd11fba29f7c41954aebc6795bf895f3d13b9f444321692aca`. The app was timed from launch through the first painted root row, then from XTest clicks or the last search digit through the expected AT-SPI row and its painted pixels. The startup import warmed the cache. There were 20 samples per interaction and the table uses nearest-rank quantiles.

| Input to expected painted result | p50 | p95 | Max |
|---|---:|---:|---:|
| Expand root | 285.14 ms | 307.95 ms | 322.71 ms |
| Switch to Top 100 files | 1,014.46 ms | 1,149.41 ms | 1,152.08 ms |
| Change the last digit in an exact path search | 10,413.88 ms | 12,582.35 ms | 13,172.52 ms |

Launch through the first painted row took 14.98 s. Search first changed the result region at 224.21 ms p95, including the 200 ms debounce. Each expected-result interaction exceeded the 100 ms p95 target. The GTK process peaked at 144,224 KiB (140.84 MiB) by `wait4`, corroborated by sampled RSS. This was a snapshot-viewer workload without a scan worker. The ≥500-event mixed scan/scroll/search/selection workload, total RSS during active collection and UI cancellation latency were NOT RUN.

To reproduce on Ubuntu, install `xvfb xauth dbus-daemon at-spi2-core python3-pyatspi python3-xlib fonts-noto-cjk` for the test harness. Point the command to the generated fixture file printed by the C benchmark. Use `--diagnostics-only` with a `--small` fixture to exercise report saving.

```sh
cmake --preset release
cmake --build --preset release
cmake --build --preset release --target diskscope-benchmark
build/release/bin/diskscope-benchmark --build /path/to/empty-directory --samples 32
dbus-run-session -- xvfb-run -a -s '-screen 0 1440x1000x24 -dpi 96' \
  /usr/bin/python3 tests/perf/desktop.py build/release/bin/diskscope /path/to/result/benchmark.dscan --samples 20
```

## 0.1.1 preview

| Evidence | Result |
|---|---|
| Native Windows 11 x64 UCRT64 Release | 18/18 CTest groups PASS. Extracted portable CLI scanned an NTFS fixture with a 1,194 UTF-16-unit cache base and a Windows-system-only PATH. Default and Cairo GTK imports reopened private snapshots at 1,263 and 1,261 units respectively. All three SQLite snapshots passed quick_check. Windows long-path opt-in was disabled on the test host. |
| Ubuntu 24.04.4 amd64 on WSL 1 | Release 18/18 and ASan/UBSan 18/18 CTest groups PASS. The native raw-filename subcase reports SKIP under WSL 1. |
| X11 / headless Weston | Xvfb covered default GTK and Cairo. Headless Weston covered Wayland with Cairo at GDK scale 2. Startup recovery opened the committed cache with the original fixture directory moved. On WSL 1, the default Wayland EGL presentation path could fault in Mesa/Gallium. The application retains its default renderer and offers `--cairo`. |
| Version compatibility and recovery | v1 snapshots opened, backed up and reopened with volume data absent. v2 malformed volume data was rejected. A distinct child process kept an active lease after the controller lease closed. A separate fixture left committed observations in a WAL on process exit and recovered them without reopening the original root. |
| Export scope | Integration tests cover a selected file, nested folders, filtered matches across pagination, invalid and non-UTF-8 paths, cancellation and preservation of existing outputs. Both OSes ran CLI selected-ID export. |

The 0.1.1 run omitted a clean Windows 11 VM, live cloud provider, desktop fractional scale and keyboard matrix, end-to-end GUI responsiveness, and specialized filesystems.

## 0.1.0 baseline

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

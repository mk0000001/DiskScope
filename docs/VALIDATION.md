# Validation and performance

## Windows performance preview

### Repeated 30,000-file checks

The published 0.1.3 portable CLI and this candidate ran in the same offline Windows 11 build 26100 Sandbox, under the same restricted identity and job limits. Both scanned the same guest NTFS copy of the 30,000 ordinary four-byte files and 120 directories. Both ran the default profile, used the same runtime DLLs, and received a fresh cache and output path on each trial. Timing covered creation of the CLI process through its exit after saving the `.dscan`; staging, comparison, cleanup and the sandbox controller were outside that window. Each series used one warm-up per executable and seven measured pairs, with alternating order and opposite starting orders across the two series.

| Series | Published 0.1.3 median | Candidate median | Median paired speedup | Pairs at half time or below |
|---|---:|---:|---:|---:|
| 1 | 4.101 s | 1.307 s | 3.100× | 7/7 |
| 2 | 4.167 s | 1.339 s | 3.037× | 7/7 |

All 32 saved snapshots, including the warm-ups, passed SQLite `quick_check` and exact comparison with the reference on raw names and paths, file identities, times, links, issues, hardlink representatives, and all three size metrics and quality states. The comparison accepted a snapshot against itself and rejected a copy with changed modification times. These figures describe this fixture in the isolated VM. They do not quantify the gain from the WAL change alone, a whole-volume scan, or the worst-case duration of an individual Windows flush.

The current source was compiled in the guest with the pinned Release tools and previously generated CMake/Ninja graph, relocated to its read-only inputs and scratch. The replay preserved compiler flags, command and link order, and skipped the archive-remove command only for an absent archive. It used `windres --use-temp-file` for resource compilation. A fresh CMake configure still exits with `0xc0000142` under this restricted identity, so this is a replay of the saved graph, not a fresh CMake configure. A bounded ETW capture observed the BCrypt DLL opening and two denied file operations; the trace did not establish which operation caused the loader failure. The release uses the binaries measured with the original write restrictions, without expanding CNG or KsecDD permissions.

The native store group passed all 11 subtests. Direct execution of the 22 test groups recorded by CMake passed 21 groups. `integration.retention` reached its size-limit subtest and failed at `SetEndOfFile` while preparing a 5 GiB sparse fixture on the quota-bound scratch volume. The size-limit assertion consequently did not run. The portable runtime passed CLI scan/save with a Windows-only `PATH` and the GUI executable's `--help` startup. Interactive GTK, a new Ubuntu package and power-loss simulation were **NOT RUN** in this follow-up. The logo files keep their previous SHA-256 values.

## Earlier isolation and flush investigation

### 2026-11-18 flush investigation

The user selected execution only after the security-audit skill's OS sandbox conditions are satisfied. After the authorized Windows Sandbox setup and user restart, the Windows build 26100 guest passed negative checks for offline operation, read-only inputs, scratch-only file writes, private-registry write/DACL denial, per-user file-stream quota, restricted privileges, and CPU/memory/process/deadline limits. The guest has 4 GiB RAM; target jobs use a 20% CPU hard cap, explicit memory/process limits, an empty allowlisted environment and a private NTFS scratch volume. Target builds and tests in this follow-up run only through that bounded execution controller.

The pinned SQLite implementation synchronizes the WAL before copying pages during a passive checkpoint. With a reader pinned at an intermediate commit, that checkpoint can advance backfill without completing it. The application then takes its incomplete-checkpoint fallback and synchronizes the same stable WAL again. The new guard accepts that prior synchronization only after successful backfill progress with known, unchanged WAL frame counts at the sole writer. Errors, uncertain counts and incomplete checkpoints without progress retain the explicit sync. The maintenance interval, cache reclamation threshold, final `FULL` transaction, backup API and publication barriers remain unchanged. This removes a demonstrated duplicate call; it does not establish the cause below Windows' API of every recorded long flush.

A Windows-only regression, `/store/managed-wal-syncs-once-for-partial-backfill`, pins an intermediate reader and forwards and counts actual WAL-handle flushes while backfill advances. Before the production guard it failed with `(2 == 1)`. After the guard it passed, as did all 11 subtests in the native store group, including the pinned-reader no-progress case. A separate source review found no Critical or Important blockers in this guard or regression. These are barrier and committed-result tests, not power-loss simulation.

The fresh GUI-enabled Release build completed using the CMake Ninja-generated command graph executed serially through the sandbox runner. Native Ninja's global subprocess pipe is denied in an AppContainer, so its normal build orchestration is **NOT RUN** in this environment. Configure-only compiler/ABI/feature/pthread probes are **SKIP**, reusing the same pinned native compiler profile; actual C/RC compilation, archiving and linking ran. The executor preserves command and object/library order, decomposes only CMake's literal `cd . && ...` wrapper, rejects response files/dynamic dependencies/validation edges, and uses windres' official `--use-temp-file` option with the same RC flags and inputs. The full verbose CTest run failed six of 22 groups: `integration.cli`, `integration.windows_cache`, `integration.session`, `integration.worker_process`, `integration.recovery` and `integration.retention`. The cache test reports `DS_IO` on saving inside its protected user-only cache; the worker-process test receives zero of three expected bytes. The product's private cache DACL deliberately omits a package SID, which is incompatible with AppContainer access. These failures remain unresolved; the product privacy ACL will not be weakened to accommodate the harness.

A later offline guest used a normal dummy account with a scratch-backed profile, an exact user-only write-restricted Low-integrity token, disabled other local accounts, read-only inputs, a 1 GiB NTFS volume with 512 MiB user file-stream quota, and the same bounded job controller. Fourteen fresh native control groups passed: identity, device-access denial, scratch/input/outside-file boundaries, quota, actual ownership escape, privileges, registry, NUL, local pipe, derived-token restrictions, process, memory, deadline and CPU. The derived-token check explicitly requested `TOKEN_ADJUST_DEFAULT`, verified that raising Low to Medium and clearing `NO_WRITE_UP` were denied, and rechecked the original token. There was no active network adapter.

The bare NamedPipe device and its trailing-slash namespace root have different access behavior. Handles requesting root DACL/owner access did not prove mutable metadata. Trusted System controls used the same descriptors and handle options for native DACL and owner setters; separate fresh handles observed each intended change and verified restoration. All four restricted Win32/native setters rejected the valid inputs with Win32 87 / native `0xc000000d`, and trusted before/after/restored metadata matched. These are observed operation rejections, not `ACCESS_DENIED` statuses. The product entry scripts require this proof receipt together with the complete fresh control suite. An early job-accounting count of one was followed by a complete class-3 snapshot with zero assigned processes and no IDs; the earlier count alone was not treated as proof of a forced descendant.

The first ordinary CMake/Ninja configuration in that validated profile failed before CMake reached its project logic: `0xc0000142`. CMake `--version` failed identically, while the pinned Ninja/GCC version commands and a USER32-only control reached their entry points. After correcting the diagnostic DLL-list parser to accept `+` in `libstdc++-6.dll`, both individual and together-load trials validated all 33 imports and identified initialization error 1114 in libarchive/libcurl. A static BCrypt control also failed before main; a dynamic control proved `bcrypt.dll` load error 1114 (BCrypt was never called), while RtlGenRandom succeeded. No random bytes were logged.

Trusted, metadata-only queries found the public World read/write mask and Low label on CNG/KsecDD, but no exact dummy SID. Restricted read-control opens succeeded and read/write opens were denied on both. A bounded, **unaccepted infrastructure experiment** added only a non-inheriting exact dummy ACE to CNG, preserving its prior ACL entries, owner/group and mandatory label; KsecDD was unchanged. CNG read/write then opened, but BCrypt initialization still failed. The experiment did not establish the initializer's full cause or the scope of newly reachable device control operations. No product build, test or scan ran after that device permission expansion. The altered guest was discarded; its earlier passing receipts must not be reused for an altered profile. The source/toolchain, network, environment and resource restrictions were not relaxed for a host execution fallback.

This follow-up therefore has no compatible, fully validated environment for a new complete product build and through-save benchmark. The tested store regression remains useful, but the full product suite and release gate remain incomplete. A future run must establish both runtime compatibility and the selected isolation policy before executing product code; a narrowly successful crypto API call alone is insufficient evidence for a wider device permission grant.

Saved-result equivalence on the 30,000-file fixture, portable deployment and repeated 2× performance checks are **NOT RUN** for this follow-up. The logo source hashes match the preceding build. No GitHub publication is authorized until the requested repeated through-save criterion passes. Current Linux execution is **NOT RUN** because the available WSL 1 environment does not satisfy the selected sandbox policy.

**Historical measurements before this follow-up:** Native Windows 11 x64 build 26300 ran the default conservative profile against the published 0.1.3 portable executables. Both variants used the same 30,121-entry local NTFS fixture (30,000 four-byte files, 120 directories and the root), and the same runtime DLLs on the same volume. Run `diskscope-cli scan --root FIXTURE --cache-dir NEW_CACHE --output NEW_RESULT.dscan` with a fresh cache and output path each time. After one warm-up per variant, alternate which variant runs first in each pair. Measure from process launch until exit with a completed `.dscan`. The collection measurement ends at the first finalization notification. Neither measurement uses the system drive as its scan root.

### Ordinary NTFS files

The diagnostic workers used the same Release CMake compile and link commands as the portable baseline. One recorded phase boundaries only. The other also timed each relative open, combined query and close with Windows' performance counter. The same fixture ran in alternating order alongside the uninstrumented portable baseline, with one warm-up and five measured runs per variant.

| Measured interval | File-by-file probe median |
|---|---:|
| Launch to saved result and exit | 2,810 ms |
| Directory walk through delivery of observations | 2,142 ms |
| Relative opens, 30,000 calls | 1,582 ms |
| Combined metadata, 30,000 calls | 111 ms |
| Closes, 30,000 calls | 355 ms |
| SQLite appends on the overlapping writer | 624 ms |
| Wait for the remaining writer work after traversal | 8 ms |
| Final aggregation and commit | 173 ms |
| Standalone SQLite backup and publish | 67 ms |

No leaf on the fixture needed the separate-handle fallback. Open and close accounted for about 95% of measured leaf API time. Writer time overlaps the walk, and the per-run medians do not add up to the end-to-end median. The phase-only build measured 3,646 ms through save, and the uninstrumented build measured 2,693 ms. Use the package comparisons for release timing.

A follow-up Win32 SQLite VFS probe timed `FlushFileBuffers` and `WriteFile`. In an 8.39 s run the writer spent 6.77 s in appends. Fourteen flush calls of at least 100 ms accounted for 6.26 s of that writer time, and no write call took at least 2 ms. The longest flush lasted 954 ms. The test identifies the blocking OS call, and the cause below the API is undetermined.

The Windows walker now resolves the NTFS relative-open API once after verifying the scan root. The ordinary path keeps the same exact-name open, metadata and close calls for each leaf.

A portable experiment also let SQLite retain fully backfilled WAL space for rewind. The combined experiment took 2.668 s at the median against 2.653 s for the preceding local build in seven alternating pairs, with a 1.006× median paired ratio. It took 3.343 s against the published 0.1.3 at 6.487 s in the first series and 2.770 s against 4.630 s in the second, with paired speedups of 1.57× and 1.80×. Retaining the allocation gave no clear benefit and could consume projected cache headroom near the cap. The final build keeps immediate reclamation after a completed backfill.

The final portable build was measured without instrumentation, using the same 30,121-entry fixture. All 64 runtime DLLs matched the published portable by SHA-256.

| Baseline and series | Baseline time to saved result | Final build time to saved result | Paired speedup | Pairs at half time or below |
|---|---:|---:|---:|---:|
| Published 0.1.3, series 1 | 4.191 s | 2.879 s | 1.55× | 2/7 |
| Published 0.1.3, series 2 | 4.223 s | 2.592 s | 1.66× | 2/7 |
| Preceding local build | 2.561 s | 2.526 s | 1.015× | 1/5 |

The table reports medians of elapsed times and medians of within-pair ratios separately. The 2× publication criterion remained unmet. The one-time API setup removed a per-leaf check, with a small end-to-end difference compared with the preceding local build. The 38 timed snapshots matched their reference on raw entries, identities, link counts, times, issues, hardlink representatives and all three byte metrics and quality states. Each passed SQLite `quick_check`.

Windows 11 Release passed 22/22 CTest groups. Ubuntu 24.04 WSL 1 passed 21/21 groups in Debug and 21/21 with ASan/UBSan. The extracted portable passed native CLI checks for hardlinks, the junction boundary and long Unicode paths. Both default and Cairo GTK rendering opened packaged snapshots. Logo hashes matched the previous build. Clean Windows VM, physical Ubuntu and live cloud-provider tests were NOT RUN.

### Windows worker link layout

The hardlink-fixture crossover put identical workers in two installation slots, then exchanged the earlier worker and the reuse worker between the slots. Identical-image median elapsed times were within 1%. The original archive-linked reuse image remained slower in both placements. Windows job accounting of the unmodified processes measured a reduction in non-read/write I/O operations from 37,735 to 23,150 at the median. A separate phase probe with no per-file clocks measured similar SQLite backup times and reproduced the collection saving from reuse.

Using the same code, compiler options, resources, controller and library inputs, the Release experiment varied whether the linker took the Windows metadata and walk objects from a static archive or listed them explicitly before the archives. The uninstrumented reuse worker produced the following through-save medians in two five-pair series on the 4,811-entry hardlink fixture.

| Series | Reuse with archive extraction | Reuse with explicit objects |
|---|---:|---:|
| Archive first for the first pair | 0.769 s | 0.460 s |
| Explicit objects first for the first pair | 0.827 s | 0.459 s |

The CMake worker now lists those objects directly. The objects still feed the platform library used by tests. This isolates an image-layout difference in the tested toolchain. The underlying Windows mechanism for the wall-time difference remains undetermined.

### Earlier link-layout portable build

The packaged worker's `.text`, `.rdata`, `.data` and `.rsrc` sections matched the tested explicit-object image. All 64 DLLs alongside the packaged executables matched the published 0.1.3 DLLs by SHA-256. Each series below ran seven alternating pairs after warm-up, in the default conservative profile. Each scan opened a fresh cache and saved a fresh `.dscan` before exit.

| Fixture and baseline | Baseline saved-result median | Candidate saved-result median | Paired speedup | Pairs at half the baseline time or below |
|---|---:|---:|---:|---:|
| Hardlink fixture, preceding local build | 0.693 s | 0.463 s | 1.50× | 0/7 |
| Release fixture, published 0.1.3, series 1 | 4.222 s | 2.657 s | 1.57× | 2/7 |
| Release fixture, published 0.1.3, series 2 | 4.831 s | 2.865 s | 1.66× | 1/7 |

The publication criterion remained unmet. The release fixture contains single-link files, whose ordinary relative-open path already uses one handle. All 42 timed snapshots passed SQLite `quick_check` and matched their series baseline in root and policy, raw names, timestamps, identities, link counts, issue rows, hardlink representatives and all three byte metrics and quality states.

Windows 11 Release passed 22/22 CTest groups. Ubuntu 24.04 under WSL 1 passed 21/21 groups in Debug and 21/21 with ASan/UBSan. Windows directory symlink creation and WSL invalid UTF-8 filenames had environment-specific skipped subcases. The extracted ZIP passed native CLI checks for hardlinks, junction boundaries and long Unicode paths. Both default and Cairo GTK rendering opened a snapshot with the packaged runtime. The runtime manifest and bundled notices were verified, and the companion source archive passed 48 upstream checksum checks. The SVG and ICO SHA-256 hashes were identical to their pre-change values. Clean Windows VM, physical Ubuntu and live cloud-provider tests were NOT RUN.

### Earlier NTFS handle reuse experiments

On a separate fixture with 2,400 files and 2,400 hardlink aliases, the scanner-only probe counted 9,600 successful leaf opens and 9,600 closes in the reopening path. Reusing the successful relative handle reduced each count to 4,800. In three measurements per mode, median scanner-only time went from 528 ms to 291 ms. An instrumented worker toggled only this behavior in four alternating launch-to-save pairs and measured separate medians of 916 ms and 632 ms. Neither probe is the release benchmark. The 30,121-entry release fixture contains only single-link files and had 30,000 leaf opens and closes under either mode.

Replacing `CloseHandle` or removing the synchronous-open option did not give a repeatable benefit in the five-round native probe. The release code keeps the existing native open options, close API and one-target-operation default. Prototype fixtures covered a multi-link file under a renamed parent and a path too long for the combined query buffer. The metadata tests continued to cover sparse and compressed allocation, sharing restrictions and reparse changes. [Microsoft CloseHandle and NtClose](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntclose)

The initial archive-linked Release executables took longer through save than the preceding local build on the hardlink fixture. The broad prototype measured 0.831 s against 0.776 s across seven pairs from matching directory layouts. A narrower prototype, limited to files that already used the relative path, measured 0.976 s against 0.773 s across five pairs. Table times include both scan and save. The median of paired ratios is used for the release criterion.

| Against published 0.1.3 | 0.1.3 saved-result median | Prototype saved-result median | Paired speedup | Pairs at half time or below |
|---|---:|---:|---:|---:|
| Broad, series 1 | 4.387 s | 3.531 s | 1.42× | 1/7 |
| Broad, series 2 | 4.407 s | 3.119 s | 1.46× | 2/7 |
| Narrow | 5.910 s | 3.309 s | 1.40× | 1/7 |

All 90 timed Release snapshots passed SQLite `quick_check` and matched their comparison references on raw entries, identities, link counts, issue rows and the three aggregate metrics. That iteration kept the prototype in scratch and restored the preceding production path. The rebuilt Windows worker's `.text`, `.rdata` and `.data` sections matched the preceding local worker byte for byte, and Windows passed all 22 CTest groups again. The logo hashes stayed identical. The 2× publication criterion was unmet. The images used identical launcher and checked runtime files, and the ordinary path made one native open, one combined query and one close in both versions.

### Stage profiling

On the 30,121-entry release fixture, the native NTFS metadata path spent about 1.1 s on handle opens and closes in isolation. Alternative same-handle information classes returned the same file metadata and took longer. In a separately instrumented scan, retaining committed WAL pages until the reclamation threshold cut measured cache flush calls from 62 to 30. The scan writer spent about 0.69 s in batch writes, down from about 0.89 s in the two timing probes. Much of that work overlaps traversal. A reader pinned beyond the reclamation threshold is covered by a Windows regression that counts WAL sync calls and reopens a standalone partial backup. These phase probes do not measure the release criterion.

Reusing the UTF-16 traversal buffer saved 18.8 ms at the paired median in a three-pair scan-only probe. In a six-pair prototype comparison, directly launching the Windows worker reduced median process creation from 24.2 ms to 6.6 ms. Its standalone snapshot rows matched those produced with the GIO subprocess launcher. The launcher regression checks a path with spaces and Korean characters, binary IPC, EOF, actual exit status, no console and exclusion of an unrelated inheritable handle.

### Earlier combined candidate

Two independent seven-pair series used the same Release binaries and the procedure above. Paired speedup is the median of each round's published time divided by its candidate time.

| Series | 0.1.3 saved-result median | Candidate saved-result median | Paired speedup | Pairs at half the original time or below |
|---|---:|---:|---:|---:|
| 1 | 4.160 s | 2.600 s | 1.61× | 2/7 |
| 2 | 4.399 s | 2.617 s | 1.60× | 2/7 |

The 2× condition was unmet in both series. Windows 11 passed 22/22 CTest groups. Ubuntu 24.04 WSL 1 passed 21/21 in both Debug and ASan/UBSan. The Windows directory symlink and WSL invalid UTF-8 subcases had environment-specific skips. No whole-system-drive scan, live cloud provider or clean Windows VM was used.

A separate five-pair comparison against the previous local portable build showed a 1.006× paired median speedup. The separate saved-result medians were 2.602 s for the previous local build and 2.624 s for the current build. The end-to-end difference between these local builds was within the observed variation. All 38 measured snapshots across the three comparisons passed SQLite `quick_check` and matched on native paths, recorded sizes and quality, identities, link counts, timestamps, issues, hardlink representatives and aggregate metrics.

The extracted local Windows package saved a disposable NTFS scan with a hardlink, a junction, an extended cache path and a long Unicode source path. Default and Cairo GTK rendering opened private snapshots. The portable manifest, bundled license notices and dependency-source checksums were verified. The logo SVG and ICO hashes matched their values before the work.

### Previous complete comparison

| Round | 0.1.3 launch to saved result | Local candidate launch to saved result |
|---:|---:|---:|
| 1 | 3.320 s | 2.156 s |
| 2 | 6.455 s | 8.227 s |
| 3 | 8.325 s | 2.169 s |
| 4 | 3.362 s | 2.098 s |
| 5 | 3.324 s | 1.784 s |
| 6 | 6.341 s | 3.567 s |
| 7 | 6.147 s | 3.216 s |

The median paired speedup was **1.78×**, a **43.7%** reduction in elapsed time. One of the seven pairs reached 2×. Median times considered separately were 6.147 s and 2.169 s, and collection medians were 2.973 s and 1.845 s. Individual timings varied substantially across both builds. The paired test did not confirm the 2× release criterion. The preceding seven-pair run, before the added directory-race recheck, also fell short at 1.79× paired median.

All 14 final-run snapshots passed SQLite `quick_check`. Every saved observation matched the reference, including raw names, numeric size values and quality, file IDs, link counts, modification times, issue rows, hardlink representatives and all three aggregate metrics. Native Windows Release passed 21/21 CTest groups, including a rename during the delivery of an active batch and reopening and saving a cache with its terminal generation in the WAL. Ubuntu 24.04 amd64 under WSL 1 passed 21/21 in Debug and ASan/UBSan. Windows directory symlink creation and Linux invalid UTF-8 names had environment-specific skipped subcases. A clean Windows VM, physical Ubuntu and live cloud providers were NOT RUN.

The extracted local Windows portable ZIP ran on the same host with a Windows-system-only PATH, an isolated profile and a separate working directory. The packaged CLI saved a result from a disposable NTFS fixture with a hardlink, a junction, a 345-unit UTF-16 source path and a 1,174-unit cache base. Both GTK rendering paths opened a private snapshot using the bundled GTK libraries. The logo SVG and ICO SHA-256 hashes stayed identical to the start of the performance work.

### Earlier measurements

Before the parent-relative NTFS lookup, SQL leaf aggregation and terminal WAL changes, seven pairs used the same fixture and procedure.

| Build | Collection median | Saved-result median |
|---|---:|---:|
| Published 0.1.3 | 2.902 s | 3.312 s |
| Local candidate | 1.643 s | 2.495 s |

The saved-result median fell by 24.7%, equivalent to 1.33× throughput. The median of the seven paired throughput ratios was 1.55×. The 2× release criterion was unmet. All 14 saved results passed SQLite `quick_check` and had identical native names, observations, identities, link counts, issue rows and aggregates across all three size metrics.

A separate NTFS name-query prototype was faster in scanner-only measurements. It was excluded because the Windows filter stack can turn a query-open into an actual open, and the name-query API does not accept the no-recall open option used by the scanner. [Microsoft query-open classes](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/flt-parameters-for-irp-mj-query-open) · [Minifilter callback behavior](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/fltkernel/nc-fltkernel-pflt_pre_operation_callback)

## 0.1.3 preview

- On Windows 11 with Windows Terminal, a GUI scan of the same 2,321-entry disposable NTFS fixture was run with the previous worker and with the Windows-subsystem worker. The previous worker triggered a visible terminal host and pseudoconsole. The Windows-subsystem worker finished with no console-window events, and the live process had no console when checked. The native integration test also verifies each executable's PE subsystem and performs a real CLI scan through the worker pipes.
- A live-model regression test appends committed WAL batches while the writer stays open, then checks the insertion range, cached row identity, loading of appended rows, and query replacement. In WSL 1, the GTK live-view fixture also reproduced SQLite WAL read delays while the separate writer process was active. A system SQLite reader exhibited the same delay. Visibility between commits depends on the cache filesystem's SQLite locking behavior.
- Windows administrator retry targets the selected access-denied file or folder through a command copied to the clipboard. Execute it only from PowerShell that you opened as administrator.

### Windows scan throughput

Native Windows 11 x64 on a local NTFS volume, using 30,121 observations from 30,000 small files and 120 directories plus the root. The CLI scanned the same fixture three times per build and profile, alternating build order and using new cache and output files. The collection column is measured from process launch to the CLI's first aggregation notification, so it includes startup and about 50 ms of polling granularity. Total time includes final aggregation and writing the standalone snapshot.

| Profile | 0.1.2 collection | 0.1.3 collection | 0.1.2 total | 0.1.3 total |
|---|---:|---:|---:|---:|
| Conservative | 3.925 s | 4.328 s | 4.379 s | 4.910 s |
| Standard | 4.527 s | 3.954 s | 4.948 s | 4.544 s |

Times are medians. Standard reached 1.14× collection throughput and 1.09× end-to-end throughput on this fixture. Conservative was slower in these samples. The 2× target remains unmet. No scan of the system drive was used for this comparison.

## 0.1.2 preview

| Evidence | Result |
|---|---|
| Native Windows 11 x64, build 26300 | UCRT64 Release CTest 20/20 groups PASS. The retention symlink subcase reports SKIP without symlink privilege. A separate real NTFS junction fixture preserved aged caches through the junction alias and pruned the same eligible caches through the ordinary base. |
| Extracted Windows portable ZIP | The packaged CLI and both GTK renderers ran with a Windows-system-only PATH, an isolated profile and a separate working directory. The CLI reported 8 fixture observations and 17 logical bytes, with hardlinks and a real junction, a 345-unit UTF-16 source path and a 1,174-unit cache base. Both renderers loaded the bundled GTK libraries and opened private imports beyond 1,240 UTF-16 units. Clean Windows VM NOT RUN because the host had no accessible guest. |
| Ubuntu 24.04.4 amd64 on WSL 1 | Debug, Release and ASan/UBSan each passed 20/20 CTest groups. The raw-filename subcase reports SKIP under WSL 1. The release DEB was extracted as an ordinary user. Xvfb exercised the default and Cairo renderers and headless Weston exercised Cairo at GDK scale 2. The CLI scanned a disposable root, then the GUI opened a snapshot and recovered cached rows with the original fixture moved. |
| Fresh Ubuntu 24.04 amd64 package install | GitHub Actions installed the CI-built DEB and its runtime dependencies in the [official Ubuntu container](https://hub.docker.com/_/ubuntu/tags?name=24.04) at the digest recorded in the workflow. The CLI, GTK import and startup recovery smoke passed under an ordinary user. GCC, CMake and Ninja were absent at runtime. [CI run](https://github.com/mk0000001/DiskScope/actions/runs/43716280965) |
| Cache retention | Fixtures cover age, count, byte budget, newest recoverable cache, live leases, cancellation, foreign and linked directories, unknown files, and atomic replacement of the primary file. The GTK smoke checks that displayed private imports keep a lease until the app exits. |
| Diagnostic report | Integration tests cover default redaction, root opt-in, environment-only output, cancellation preserving the destination, and source database protection. The real GTK save dialog in the extracted Ubuntu package wrote both the redacted and root-enabled JSON. The only changed JSON fields were `paths_redacted` and `scan.root`. |

The 0.1.2 DEB uses Linux-native packaging staging. Its executable and directory modes are 0755, and the desktop entry and license documents are 0644. The previously published 0.1.1 DEB carried 0777 modes from its WSL staging filesystem. Use 0.1.2 for Ubuntu deployments.

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

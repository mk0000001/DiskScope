# Build and run DiskScope

## Ubuntu 24.04 amd64

Install the distribution build dependencies, then use CMake and Ninja from the repository root.

```sh
sudo apt install build-essential cmake ninja-build pkg-config libglib2.0-dev libgtk-4-dev libcairo2-dev libjson-glib-dev
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
build/dev/bin/diskscope
build/dev/bin/diskscope-cli scan --root ./tests --output /tmp/tests.dscan
```

For CLI/worker only, configure with `-DDS_GUI=OFF`. SQLite is fetched from the pinned upstream URL and verified during configuration. Keep `diskscope-worker` next to the controller binaries.

## Windows 11 x64

Open the MSYS2 UCRT64 shell and install the build dependencies. Use a dedicated Windows build directory if a Linux build shares the checkout.

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,gtk4,json-glib}
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows
ctest --test-dir build/windows --output-on-failure
```

From PowerShell with the UCRT64 runtime on PATH during development:

```powershell
.\build\windows\bin\diskscope.exe
.\build\windows\bin\diskscope-cli.exe scan --root '.\tests' --output '.\tests.dscan'
```

Use a small disposable folder for smoke testing. A scan uses ordinary metadata APIs, which still invoke the operating system and its storage stack.

## Build the Ubuntu package

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cpack --config build/release/CPackConfig.cmake -G DEB
sudo apt install ./diskscope_0.1.1_amd64.deb
diskscope
```

The Windows bundle and matching source archive are built with the [portable packaging procedure](../packaging/windows/README.md).

## CLI result workflows

```sh
diskscope-cli scan --root ./fixture --output result.dscan --json
diskscope-cli export --input result.dscan --format jsonl --output report.jsonl
diskscope-cli export --input result.dscan --selected-id 2 --metric allocated --format jsonl --output selected.jsonl
diskscope-cli compare --before old.dscan --after result.dscan --output changes.jsonl
```

Use `--cache-dir` to choose another local cache base. The CLI accepts `--hide-hidden`, an optional basename `--exclude-pattern`, and repeatable `--exclude-path` options. A cancellation request can still produce a committed partial snapshot. A second Ctrl+C during backup cancels its publication. Exit codes are 0 for success, 2 for gaps, 3 for cancellation, 4 for invalid usage/input, and 5 for worker, I/O or resource failure.

The export entry ID comes from a JSON or JSONL export. Selecting a file writes that row. Selecting a folder writes its row and all descendants. In the GUI, **필터 일치 전체** exports every row matching the active inputs and view type, including matches beyond the Top 100/1000 display cap. The selected metric is recorded alongside all three metric values.

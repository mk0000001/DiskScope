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

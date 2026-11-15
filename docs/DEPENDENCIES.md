# Dependencies

The build requires CMake 3.24 or later and Ninja. DiskScope uses a C17 compiler. Core and CLI link GLib/GIO and JSON-GLib. The GUI adds GTK and Cairo.

| Dependency | Minimum API | License | Distribution |
|---|---:|---|---|
| GLib and GIO | 2.80 | LGPL-2.1-or-later | Shared runtime |
| GTK | 4.14 | LGPL-2.1-or-later | GUI shared runtime and data |
| Cairo | 1.18 | LGPL-2.1-or-later OR MPL-1.1 | GUI shared runtime |
| JSON-GLib | 1.8 | LGPL-2.1-or-later | Shared runtime |
| SQLite | 3.51.3 | Public domain | Static amalgamation, pinned to 3.54.1 |

SQLite 3.54.1 includes the WAL-reset correction made in 3.51.3. CMake verifies SHA-256 on the downloaded archive. The archive also matched SQLite's published SHA3-256 `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`. [SQLite downloads](https://www.sqlite.org/download.html) · [SQLite WAL documentation](https://www.sqlite.org/wal.html) · [SQLite licensing](https://www.sqlite.org/copyright.html)

## Ubuntu 24.04 amd64 build packages

The version and SHA-256 below come from Ubuntu's APT index. They identify the test build inputs and remain separate from the declared runtime compatibility floors.

| Package | Version | SHA-256 |
|---|---|---|
| libglib2.0-dev | 2.80.0-6ubuntu3.10 | 6d1d88d68192256603c0de05d084b8ef06ed9b16753215ec2c1ef9ef5fa15014 |
| libgtk-4-dev | 4.14.5+ds-0ubuntu0.10 | 8b5d4994b4882922a11e41d6b45d1cb5cfdc6b5e2ffececa9f1f3ffc1bd9bc69 |
| libcairo2-dev | 1.18.0-3build1 | 05d0ccb271a557d30a665d9f5f1420e9e60df02f49cfe4f8917674e218e5bb3c |
| libjson-glib-dev | 1.8.0-2build2 | e28dfc12339565a48b5ed115f3933b3b0f7b860364a527a4c2a333ca9426bff |
| cmake | 3.28.3-1build7 | 4b0a7f8c0daf27b26b46997d994ae5d1ee7a3d11dfcda9f7627bb1462c162295 |
| ninja-build | 1.11.1-2 | 6a17f76a0f75586f1292f6c61a9b07fd0dec963baf6a102b17673f601c6b323a |

[Ubuntu Noble packages](https://packages.ubuntu.com/noble/)

## Windows 11 x64 build inputs

These are the exact MSYS2 UCRT64 archive versions and SHA-256 hashes consumed for this build. Package names use the prefix mingw-w64-ucrt-x86_64-. The portable ZIP also contains runtime-manifest.json with all 48 runtime package owners, corresponding source archives and checksums.

| Package | Version | SHA-256 |
|---|---|---|
| gcc | 16.2.0-7 | c7c5ac5a7413e9a3b5afa367288568058dcecce8fdd690059c5bbfa07def01b9 |
| cmake | 4.5.0-1 | 146cfea017827e0203c23fe8317442fa052650bbb59f8d40427e37687cd38222 |
| ninja | 1.13.2-1 | 514685563a84a6ebddbbeee7bbafae5f3994e363c49364fc4507fabcaa049f2b |
| pkgconf | 1~3.0.10-1 | c234b9f8280f1e931efe8e4350085f544e4a6081ffcbd2caa3e81834e72e4688 |
| cairo | 1.18.6-2 | 021c0a4d1df463d08d0cac170818a7cbaccaa5b09ac6edc7948e461cb5a46e85 |
| glib2 | 2.90.1-1 | 9a7826349279bcca0c935b01880d812be9de9609aef2205be68fd7a6128c1695 |
| gtk4 | 4.24.1-1 | 03e6fd251437d50ea8c53b90e2ece1879064fdc30d0c7675da64f1d0ce3bcde8 |
| json-glib | 1.10.8-2 | 3b07443be79b7be8062f085d4453161fc71441700494a393807f76c0abda2fc6 |

[MSYS2 UCRT64 package archive](https://repo.msys2.org/mingw/ucrt64/)

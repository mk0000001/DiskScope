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

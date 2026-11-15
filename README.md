# DiskScope

DiskScope is a read-only disk usage analyzer for Windows and Linux. It uses C17, GTK4, Cairo and SQLite. A separate worker reads directory listings and file metadata. The GUI and CLI use the same scanner.

The Korean-first GUI offers a size-sorted tree, treemap, filters, large-file and folder views, statistics, pause and cancellation, recent cache recovery, snapshot import, comparison, and scoped exports. The CLI commands are `scan`, `export`, and `compare`. The 0.1.1 preview is documented in [Known limitations](docs/LIMITATIONS.md).

The three reported sizes are logical bytes, OS-reported allocated bytes per path, and allocated bytes with duplicate hardlinks attributed to one path. These totals cover regular files and their default data stream. Filesystem metadata, directories' own blocks, alternate data streams and shared extents require separate accounting. DiskScope cannot predict free space recovered by deleting files.

- [Build and run](docs/BUILD.md)
- [Dependencies and licenses](docs/DEPENDENCIES.md)
- [Verification and performance](docs/VALIDATION.md)
- [Implementation decisions](docs/DECISIONS.md)
- [Windows portable packaging](packaging/windows/README.md)

DiskScope source is released under the [MIT license](LICENSE).

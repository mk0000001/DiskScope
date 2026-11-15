# DiskScope

DiskScope is a read-only disk usage analyzer for Windows and Linux. It uses C17, GTK4, Cairo and SQLite. A separate worker reads directory listings and file metadata. The GUI and CLI use the same scanner.

The three reported sizes are logical bytes, OS-reported allocated bytes per path, and allocated bytes with duplicate hardlinks attributed to one path. These totals cover regular files and the default data stream. Filesystem metadata, directories' own blocks, alternate data streams and shared extents require separate accounting. DiskScope cannot predict the free space recovered by deleting files.

- [Build and run](docs/BUILD.md)
- [Dependencies and licenses](docs/DEPENDENCIES.md)
- [Implementation decisions](docs/DECISIONS.md)

DiskScope source is released under the [MIT license](LICENSE).

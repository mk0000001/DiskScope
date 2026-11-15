# Implementation decisions

- The isolated project was initialized from the supplied specification in a new directory. Public commits use the project name and a non-personal commit address.
- Each result database contains one scan. The walker allocates monotonically increasing node IDs with parents before children. A later directory observation can attach an enumeration error to the same node.
- On Linux the path type retains filename bytes. On Windows it retains UTF-16LE code units, including unpaired surrogates. Display formatting escapes malformed characters and controls. File operations use the original representation.
- Scan batches are borrowed by a synchronous sink. The consumer can write immediately or copy into a bounded queue. The scan control is owned by the worker; it outlives the walker and control thread.
- SQLite is statically linked from an independently hash-checked upstream amalgamation. GTK is a GUI build dependency only. The Ubuntu build consumes distribution runtime libraries and records the corresponding distribution package versions.

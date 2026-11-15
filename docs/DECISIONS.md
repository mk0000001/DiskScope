# Implementation decisions

- The isolated project was initialized from the supplied specification in a new directory. Public commits use the project name and a non-personal commit address.
- Each result database contains one scan. The walker allocates monotonically increasing node IDs with parents before children. A later directory observation can attach an enumeration error to the same node.
- On Linux the path type retains filename bytes. On Windows it retains UTF-16LE code units, including unpaired surrogates. Display formatting escapes malformed characters and controls. File operations use the original representation.
- Scan batches are borrowed by a synchronous sink. The consumer can write immediately or copy into a bounded queue. The scan control is owned by the worker; it outlives the walker and control thread.
- SQLite is statically linked from an independently hash-checked upstream amalgamation. GTK is a GUI build dependency only. The Ubuntu build consumes distribution runtime libraries and records the corresponding distribution package versions.

- Successful finalization commits the terminal run state and the active aggregate generation together. A cancellation preserves committed observations and makes post-exit aggregation an explicit DB-only action. After worker loss, the controller reads the actual committed metadata even if a commit notification was missed.
- Import validates an untrusted standalone snapshot and creates a validated private backup for the explorer. All view queries use the private copy. Snapshot output with an existing SQLite journal is rejected before replacement.
- Scans automatically exclude the configured cache base. The UI maintains expanded tree state during collection and offers an explicit refresh. Allocated GTK page-cache memory is estimated across active lists. The headless paging probe and the desktop targets are reported separately.
- Both platforms overlap at most one helper metadata operation with enumeration in the standard profile. The conservative profile uses synchronous metadata reads. Windows still relies on path-based Win32 enumeration, with the remaining ancestor race documented in the limitations.
- The open-source release is a preview. Remaining acceptance checks and missing v1 features are recorded in the validation and limitations documents.

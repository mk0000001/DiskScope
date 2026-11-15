#ifndef DISKSCOPE_SCAN_H
#define DISKSCOPE_SCAN_H

#include "ds_path.h"
#include "ds_volume.h"

#define DS_BATCH_MAX 512u
#define DS_BATCH_MSEC 250u
#define DS_QUEUE_BYTES (16u * 1024u * 1024u)
#define DS_PENDING_MAX 4096u
#define DS_DIR_HANDLES 64u
#define DS_DEPTH_MAX 4096u
#define DS_CACHE_BUDGET (UINT64_C(2) << 30)
#define DS_CACHE_END_RESERVE (UINT64_C(64) << 20)
#define DS_CACHE_FREE_RESERVE (UINT64_C(256) << 20)

typedef enum { DS_CONSERVATIVE, DS_STANDARD } DsProfile;
typedef enum { DS_DIRECTORY = 1, DS_FILE, DS_LINK, DS_SPECIAL, DS_EXCLUDED } DsKind;
typedef enum {
    DS_REASON_NONE, DS_REASON_LINK, DS_REASON_PLACEHOLDER, DS_REASON_MOUNT,
    DS_REASON_SPECIAL, DS_REASON_POLICY, DS_REASON_CACHE, DS_REASON_PERMISSION,
    DS_REASON_CHANGED, DS_REASON_METADATA, DS_REASON_LIMIT, DS_REASON_IO
} DsReason;

typedef struct {
    DsNativePath root;
    DsNativePath cache_path; /* Private output directory to exclude. */
    DsProfile profile;
    unsigned jobs;          /* 1 conservative, 2 standard. */
    uint64_t cache_budget;
    bool include_hidden;
    const char *exclude_pattern; /* Optional native basename glob in UTF-8. */
    const DsNativePath *exclude_paths; /* Borrowed absolute paths. */
    size_t exclude_count;
    /* Optional synchronous hook, once after validating the root and before
     * any entry batches. The reading is borrowed for the duration of the call. */
    DsVolumeSink volume_sink;
    void *volume_data;
} DsScanOptions;

typedef struct {
    int64_t id, parent_id;
    DsNativePath name; /* Empty only at root; root path belongs to scan metadata. */
    DsKind kind;
    DsReason reason;
    int64_t os_code;
    DsSize logical, allocated;
    uint8_t identity[24];
    bool identity_known;
    uint64_t links;
    int64_t mtime; /* Signed UTC Unix seconds. */
    bool mtime_known;
} DsEntry;

typedef struct { const DsEntry *entries; size_t count; } DsEntryBatch;
typedef DsStatus (*DsEntrySink)(const DsEntryBatch *, void *, DsError *);
typedef void (*DsPauseCallback)(bool paused, void *user_data);

typedef struct {
    GMutex lock;
    GCond changed;
    bool pause, cancel;
    DsPauseCallback pause_changed; /* Called outside the mutex, on the walker. */
    void *pause_data;
} DsControl;

/* Initialize before sharing. Clear only after all participating threads exit. */
void ds_scan_options_init(DsScanOptions *options, DsProfile profile);
void ds_control_init(DsControl *control);
void ds_control_clear(DsControl *control);
void ds_control_pause(DsControl *control, bool paused);
void ds_control_cancel(DsControl *control);
bool ds_control_canceled(DsControl *control);
bool ds_control_checkpoint(DsControl *control);
const char *ds_reason_name(DsReason reason);

/* Batches are borrowed for the duration of the synchronous callback. An update
 * to an existing directory ID may supply a later enumeration error. */
DsStatus ds_walk(const DsScanOptions *, DsControl *, DsEntrySink, void *, DsError *);

#endif

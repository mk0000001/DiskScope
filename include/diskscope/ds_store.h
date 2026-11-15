#ifndef DISKSCOPE_STORE_H
#define DISKSCOPE_STORE_H

#include "ds_scan.h"

#define DS_STORE_APPLICATION_ID 0x4453434f
#define DS_STORE_SCHEMA_VERSION 2
#define DS_RUN_RUNNING (-1)

typedef struct DsStore DsStore;
typedef struct sqlite3 sqlite3;
typedef enum { DS_STORE_WRITER, DS_STORE_READER } DsStoreMode;
typedef enum { DS_COVERAGE_PARTIAL, DS_COVERAGE_COMPLETE } DsCoverage;

typedef struct {
    DsNativePath root;
    char *root_display;
    char *policy;
    int run_status;
    DsCoverage coverage;
    int64_t commit_seq, generation, as_of_commit_seq;
    bool aggregation_complete;
    DsCoverage aggregate_coverage;
    uint64_t entry_count, unaggregated_count;
    int64_t started_at, ended_at;
} DsStoreInfo;

/* One owner thread per connection. The writer can reopen a cache for recovery. */
DsStatus ds_store_open(const DsNativePath *path, DsStoreMode mode,
                       DsStore **out, DsError *error);
void ds_store_close(DsStore *store);
sqlite3 *ds_store_db(DsStore *store);

/* Set scan options before appending. Appends synchronously commit at most
 * DS_BATCH_MAX observations. The batch and its paths remain caller-owned. */
DsStatus ds_store_set_options(DsStore *store, const DsScanOptions *options, DsError *error);
/* Saves the root volume reading once, before appending any observations. */
DsStatus ds_store_set_volume(DsStore *store, const DsVolumeInfo *volume, DsError *error);
DsStatus ds_store_set_result(DsStore *store, DsStatus run_status, DsCoverage coverage,
                             DsError *error);
DsStatus ds_store_append(DsStore *store, const DsEntryBatch *batch, DsError *error);
DsStatus ds_finalize(DsStore *store, DsControl *control, DsError *error);
/* Atomically publishes the aggregate generation and terminal run metadata. */
DsStatus ds_finalize_result(DsStore *store, DsStatus result, DsCoverage coverage,
                             DsControl *control, DsError *error);

/* Outputs own their data. Initialize to {0} and clear after use. */
DsStatus ds_store_info(DsStore *store, DsStoreInfo *out, DsError *error);
void ds_store_info_clear(DsStoreInfo *info);
/* An older store or one without a reading returns DS_OK and captured_at = 0. */
DsStatus ds_store_volume(DsStore *store, DsVolumeInfo *out, DsError *error);
DsStatus ds_store_relative_path(DsStore *store, int64_t id, DsNativePath *out, DsError *error);

/* Persistent FIFO of already observed directory IDs. A drained FIFO yields 0. */
DsStatus ds_store_pending_push(DsStore *store, int64_t id, DsError *error);
DsStatus ds_store_pending_pop(DsStore *store, int64_t *id, DsError *error);

#endif

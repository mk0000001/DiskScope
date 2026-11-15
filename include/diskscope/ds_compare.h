#ifndef DISKSCOPE_COMPARE_H
#define DISKSCOPE_COMPARE_H

#include "ds_query.h"

typedef enum {
    DS_DIFF_ADDED, DS_DIFF_REMOVED, DS_DIFF_CHANGED, DS_DIFF_UNCONFIRMED
} DsDiffKind;

typedef struct {
    bool include_directories;
} DsCompareOptions;

typedef struct {
    DsDiffKind kind;
    DsNativePath relative_path;
    int64_t before_id, after_id;
    DsSize before_sizes[DS_METRIC_COUNT], after_sizes[DS_METRIC_COUNT];
} DsDiff;

typedef struct {
    const DsDiff *entries;
    size_t count;
    bool same_root, same_policy;
    DsCoverage before_coverage, after_coverage;
} DsDiffBatch;

/* The batch and all paths are borrowed for the synchronous callback. The first
 * callback contains metadata and count=0, even when the inputs have no differences. */
typedef DsStatus (*DsDiffSink)(const DsDiffBatch *, void *, DsError *);

DsStatus ds_compare(DsStore *before, DsStore *after, const DsCompareOptions *options,
                   DsDiffSink sink, void *user_data, DsControl *control, DsError *error);
DsStatus ds_compare_save(DsStore *before, DsStore *after, const DsNativePath *destination,
                         DsControl *control, DsError *error);

#endif

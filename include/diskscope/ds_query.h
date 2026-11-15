#ifndef DISKSCOPE_QUERY_H
#define DISKSCOPE_QUERY_H

#include "ds_store.h"

#define DS_PAGE_MAX 512u
#define DS_QUERY_ALL (-1)

typedef enum { DS_SORT_ID, DS_SORT_NAME, DS_SORT_METRIC } DsSort;

typedef struct {
    int64_t parent_id; /* 0 selects the root row, DS_QUERY_ALL selects all rows. */
    DsKind kind;      /* 0 includes every kind. */
    const char *filter; /* Optional UTF-8 substring of the display basename or path. */
    bool search_path;
    const char *extension; /* Optional ASCII-case-insensitive suffix, without the dot. */
    bool issues_only;
    DsSort sort;
    DsMetric metric;
    bool has_min_bytes, has_max_bytes;
    uint64_t min_bytes, max_bytes;
    bool has_mtime_from, has_mtime_to;
    int64_t mtime_from, mtime_to; /* Inclusive UTC Unix seconds. */
    bool descending;
    uint64_t offset;
    int64_t after_id; /* Optional streaming cursor with ascending DS_SORT_ID. */
    size_t limit;
    bool omit_total;
} DsQuery;

typedef struct {
    DsEntry entry;    /* The original observation, with its native basename. */
    char *display;
    DsSize metrics[DS_METRIC_COUNT];
    uint64_t unknown[DS_METRIC_COUNT];
    int64_t representative_id;
    uint64_t entry_count, file_count, unique_file_count;
    bool aggregated;
} DsRow;

typedef struct {
    DsStoreInfo info;
    DsRow *rows;
    size_t count;
    uint64_t total; /* Matching rows before offset/cursor, or 0 when omitted. */
} DsPage;

DsStatus ds_query_page(DsStore *reader, const DsQuery *query,
                       DsPage **out, DsControl *control, DsError *error);
void ds_page_free(DsPage *page);

#endif

#include "diskscope/ds_compare.h"
#include "../export/export_internal.h"
#include "../store/store_internal.h"

typedef struct {
    int64_t id;
    DsKind kind;
    DsReason reason;
    DsSize sizes[DS_METRIC_COUNT];
} Match;

typedef struct {
    DsStore *store;
    sqlite3_stmt *root, *child;
} Lookup;

static int lookup_open(Lookup *lookup, DsStore *store, int64_t generation)
{
    const char *select =
        "SELECT e.id,e.kind,e.reason,e.logical,e.logical_quality,e.allocated,e.allocated_quality,"
        " a.node_id,a.logical,a.logical_quality,a.allocated,a.allocated_quality,"
        " a.unique_allocated,a.unique_quality FROM entries e"
        " LEFT JOIN aggregates a ON a.generation=?1 AND a.node_id=e.id";
    lookup->store = store;
    char *sql = g_strconcat(select, " WHERE e.id=1", NULL);
    int rc = ds_sql_prepare(store, &lookup->root, sql);
    g_free(sql);
    sql = g_strconcat(select, " WHERE e.parent_id=?2 AND e.name_encoding=?3 AND e.name=?4", NULL);
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &lookup->child, sql);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(lookup->root, 1, generation);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(lookup->child, 1, generation);
    g_free(sql);
    return rc;
}

static void lookup_close(Lookup *lookup)
{
    sqlite3_finalize(lookup->child);
    sqlite3_finalize(lookup->root);
}

static int read_match(sqlite3_stmt *stmt, Match *match)
{
    *match = (Match){.sizes = {{0, DS_UNAVAILABLE}, {0, DS_UNAVAILABLE}, {0, DS_UNAVAILABLE}}};
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) return SQLITE_OK;
    if (rc != SQLITE_ROW) return rc;
    match->id = sqlite3_column_int64(stmt, 0);
    match->kind = sqlite3_column_int(stmt, 1);
    match->reason = sqlite3_column_int(stmt, 2);
    if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
        for (DsMetric metric = DS_LOGICAL; metric < DS_METRIC_COUNT; ++metric)
            if (!ds_sql_read_size(stmt, 8 + metric * 2, &match->sizes[metric])) return SQLITE_CORRUPT;
    } else if (match->kind == DS_FILE &&
        (!ds_sql_read_size(stmt, 3, &match->sizes[DS_LOGICAL]) ||
         !ds_sql_read_size(stmt, 5, &match->sizes[DS_ALLOCATED]))) {
        return SQLITE_CORRUPT;
    }
    return SQLITE_OK;
}

static int find_path(Lookup *lookup, const DsNativePath *relative, Match *match, bool *covered)
{
    *covered = true;
    sqlite3_reset(lookup->root);
    int rc = read_match(lookup->root, match);
    sqlite3_reset(lookup->root);
    if (rc != SQLITE_OK) return rc;

    size_t length = 0;
    const uint8_t *raw = g_bytes_get_data(relative->raw, &length);
    size_t unit = relative->encoding == DS_PATH_BYTES ? 1 : 2;
    for (size_t start = 0; start < length;) {
        if (match->kind != DS_DIRECTORY || match->reason != DS_REASON_NONE) *covered = false;
        if (!match->id || match->kind != DS_DIRECTORY) {
            match->id = 0;
            break;
        }
        size_t end = start;
        for (; end < length; end += unit) {
            if (unit == 1 ? raw[end] == '/' : raw[end] == '\\' && raw[end + 1] == 0)
                break;
        }
        sqlite3_stmt *stmt = lookup->child;
        sqlite3_reset(stmt);
        rc = sqlite3_bind_int64(stmt, 2, match->id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 3, relative->encoding);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob64(stmt, 4, raw + start, end - start, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = read_match(stmt, match);
        sqlite3_reset(stmt);
        sqlite3_bind_null(stmt, 4);
        if (rc != SQLITE_OK) return rc;
        start = end + unit;
    }
    return SQLITE_OK;
}

static bool different(DsSize a, DsSize b)
{
    return a.quality == DS_KNOWN && b.quality == DS_KNOWN && a.bytes != b.bytes;
}

static DsStatus compare_side(Lookup *own, Lookup *other, DsDiffBatch metadata,
                             bool before, bool include_dirs, DsDiffSink sink,
                             void *data, DsControl *control, DsError *error)
{
    DsStore *source = own->store;
    const DsSize unknown[] = {{0, DS_UNAVAILABLE}, {0, DS_UNAVAILABLE}, {0, DS_UNAVAILABLE}};
    DsQuery query = {.parent_id = DS_QUERY_ALL, .kind = include_dirs ? 0 : DS_FILE,
                     .limit = 128, .omit_total = true};
    bool comparable_coverage = metadata.same_root && metadata.same_policy &&
        metadata.before_coverage == DS_COVERAGE_COMPLETE &&
        metadata.after_coverage == DS_COVERAGE_COMPLETE;
    while (!ds_control_canceled(control)) {
        DsPage *page = NULL;
        DsStatus status = ds_query_page(source, &query, &page, control, error);
        if (status != DS_OK) return status;
        size_t count = page->count;
        for (size_t i = 0; status == DS_OK && i < count; ++i) {
            if (ds_control_canceled(control)) { status = DS_CANCELED; break; }
            const DsRow *row = &page->rows[i];
            query.after_id = row->entry.id;
            if (row->entry.kind != DS_FILE && row->entry.kind != DS_DIRECTORY) continue;

            DsDiff diff = {0};
            status = ds_store_relative_path(source, row->entry.id, &diff.relative_path, error);
            if (status != DS_OK) break;
            Match match;
            bool covered = false;
            status = ds_sql_status(other->store, find_path(other, &diff.relative_path, &match, &covered), error);
            if (status == DS_OK) {
                bool emit = true;
                if (!before) {
                    diff.kind = DS_DIFF_ADDED;
                    emit = match.kind != DS_FILE && (!include_dirs || match.kind != DS_DIRECTORY);
                } else if (!match.id || match.kind == DS_EXCLUDED) {
                    bool was_covered = false;
                    if (!match.id && covered && comparable_coverage) {
                        Match original;
                        status = ds_sql_status(source,
                            find_path(own, &diff.relative_path, &original, &was_covered), error);
                    }
                    diff.kind = was_covered ? DS_DIFF_REMOVED : DS_DIFF_UNCONFIRMED;
                } else {
                    diff.kind = DS_DIFF_CHANGED;
                    emit = row->entry.kind != match.kind ||
                           different(row->metrics[DS_LOGICAL], match.sizes[DS_LOGICAL]) ||
                           different(row->metrics[DS_ALLOCATED], match.sizes[DS_ALLOCATED]);
                    if (!emit && (row->metrics[DS_LOGICAL].quality != match.sizes[DS_LOGICAL].quality ||
                                  row->metrics[DS_ALLOCATED].quality != match.sizes[DS_ALLOCATED].quality)) {
                        diff.kind = DS_DIFF_UNCONFIRMED;
                        emit = true;
                    }
                }
                if (status == DS_OK && emit) {
                    diff.before_id = before ? row->entry.id : 0;
                    diff.after_id = before ? match.id : row->entry.id;
                    memcpy(diff.before_sizes, before ? row->metrics : unknown, sizeof diff.before_sizes);
                    memcpy(diff.after_sizes, before ? match.sizes : row->metrics, sizeof diff.after_sizes);
                    metadata.entries = &diff;
                    metadata.count = 1;
                    status = sink(&metadata, data, error);
                }
            }
            ds_path_clear(&diff.relative_path);
        }
        ds_page_free(page);
        if (status != DS_OK || !count) return status;
    }
    return DS_CANCELED;
}

DsStatus ds_compare(DsStore *before, DsStore *after, const DsCompareOptions *options,
                   DsDiffSink sink, void *user_data, DsControl *control, DsError *error)
{
    if (!before || !after || !sink) return ds_error(error, DS_INVALID, 0, "Invalid scan comparison");
    if (ds_control_canceled(control)) return DS_CANCELED;
    DsStore *stores[] = {before, after};
    DsStoreInfo info[2] = {0};
    Lookup lookup[2] = {{0}};
    bool transaction[2] = {false};
    DsStatus status = DS_OK;
    for (size_t i = 0; status == DS_OK && i < G_N_ELEMENTS(stores); ++i) {
        DsStore *store = stores[i];
        if (sqlite3_get_autocommit(store->db)) {
            status = ds_sql_status(store, ds_sql_exec(store, "BEGIN"), error);
            transaction[i] = status == DS_OK;
        }
        if (status == DS_OK) status = ds_store_info(store, &info[i], error);
        if (status == DS_OK)
            status = ds_sql_status(store, lookup_open(&lookup[i], store, info[i].generation), error);
    }
    if (status == DS_OK && (!info[0].root.raw || !info[1].root.raw))
        status = ds_error(error, DS_INVALID, 0, "Both scans require a recorded root");
    if (status == DS_OK) {
        DsDiffBatch metadata = {
            .same_root = info[0].root.encoding == info[1].root.encoding &&
                         g_bytes_equal(info[0].root.raw, info[1].root.raw),
            .same_policy = g_strcmp0(info[0].policy, info[1].policy) == 0,
            .before_coverage = info[0].coverage, .after_coverage = info[1].coverage,
        };
        status = sink(&metadata, user_data, error);
        for (size_t i = 0; status == DS_OK && i < G_N_ELEMENTS(stores); ++i)
            status = compare_side(&lookup[i], &lookup[1 - i], metadata, i == 0,
                                  options && options->include_directories, sink, user_data, control, error);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(stores); ++i) {
        lookup_close(&lookup[i]);
        ds_store_info_clear(&info[i]);
        if (transaction[i]) ds_sql_exec(stores[i], "ROLLBACK");
    }
    return status;
}

typedef struct {
    DsOutput *output;
    GString *line;
    uint64_t count;
} DiffOutput;

static DsStatus save_diff(const DsDiffBatch *batch, void *data, DsError *error)
{
    DiffOutput *out = data;
    GString *line = out->line;
    if (!batch->count) {
        g_string_printf(line, "{\"type\":\"comparison\",\"schema_version\":%d,"
            "\"same_root\":%s,\"same_policy\":%s,\"before_coverage\":\"%s\",\"after_coverage\":\"%s\"}\n",
            DS_STORE_SCHEMA_VERSION, batch->same_root ? "true" : "false",
            batch->same_policy ? "true" : "false",
            batch->before_coverage == DS_COVERAGE_COMPLETE ? "complete" : "partial",
            batch->after_coverage == DS_COVERAGE_COMPLETE ? "complete" : "partial");
        return ds_output_write(out->output, line->str, line->len, error);
    }
    const char *kinds[] = {"newly_observed", "removed", "changed", "unconfirmed"};
    const char *metrics[] = {"logical", "allocated", "unique_allocated"};
    for (size_t i = 0; i < batch->count; ++i) {
        const DsDiff *diff = &batch->entries[i];
        char *display = NULL;
        DsStatus status = ds_path_display(&diff->relative_path, &display, error);
        if (status != DS_OK) return status;
        g_string_printf(line, "{\"type\":\"difference\",\"kind\":\"%s\",\"before_id\":\"%"
            G_GINT64_FORMAT "\",\"after_id\":\"%" G_GINT64_FORMAT "\",",
            kinds[diff->kind], (gint64)diff->before_id, (gint64)diff->after_id);
        ds_json_path(line, &diff->relative_path, display);
        g_free(display);
        for (DsMetric metric = DS_LOGICAL; metric < DS_METRIC_COUNT; ++metric) {
            g_string_append_printf(line, ",\"%s\":{\"before\":", metrics[metric]);
            ds_json_size(line, diff->before_sizes[metric], 0);
            g_string_append(line, ",\"after\":");
            ds_json_size(line, diff->after_sizes[metric], 0);
            g_string_append_c(line, '}');
        }
        g_string_append(line, "}\n");
        status = ds_output_write(out->output, line->str, line->len, error);
        if (status != DS_OK) return status;
        ++out->count;
    }
    return DS_OK;
}

DsStatus ds_compare_save(DsStore *before, DsStore *after, const DsNativePath *destination,
                         DsControl *control, DsError *error)
{
    if (!before || !after) return ds_error(error, DS_INVALID, 0, "Both input scans are required");
    if (ds_control_canceled(control)) return DS_CANCELED;
    DsOutput output;
    DsStatus status = ds_output_begin(before, destination, &output, error);
    if (status != DS_OK) return status;
    if (after != before) {
        DsOutput check;
        status = ds_output_begin(after, destination, &check, error);
        ds_output_clear(&check);
    }
    DiffOutput data = {&output, g_string_new(NULL), 0};
    if (status == DS_OK) status = ds_compare(before, after, NULL, save_diff, &data, control, error);
    if (status == DS_OK) {
        g_string_printf(data.line, "{\"type\":\"footer\",\"differences\":\"%" G_GUINT64_FORMAT "\"}\n",
                        (guint64)data.count);
        status = ds_output_write(&output, data.line->str, data.line->len, error);
    }
    if (status == DS_OK) status = ds_output_finish(&output, control, error);
    g_string_free(data.line, true);
    ds_output_clear(&output);
    return status;
}

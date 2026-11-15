#include "../store/store_internal.h"

typedef struct {
    DsSize sizes[DS_METRIC_COUNT];
    int64_t unknown[DS_METRIC_COUNT];
    int64_t representative, entries, files, unique_files;
} Aggregate;

static int aggregate_file(sqlite3_stmt *entries, Aggregate *row)
{
    if (!ds_sql_read_size(entries, 2, &row->sizes[DS_LOGICAL]) ||
        !ds_sql_read_size(entries, 4, &row->sizes[DS_ALLOCATED])) return SQLITE_CORRUPT;
    row->files = 1;
    row->representative = sqlite3_column_int64(entries, 6);
    bool changed = sqlite3_column_int(entries, 7) != 0;
    DsQuality rep_quality = sqlite3_column_int(entries, 8);
    DsQuality own_quality = row->sizes[DS_ALLOCATED].quality;
    DsSize unique = {0, DS_UNAVAILABLE};
    if (changed) unique.quality = DS_CHANGED;
    else if (own_quality != DS_KNOWN) unique.quality = own_quality;
    else if (row->representative)
        unique.quality = rep_quality;
    if (row->representative == sqlite3_column_int64(entries, 0)) {
        if (unique.quality == DS_KNOWN) unique.bytes = row->sizes[DS_ALLOCATED].bytes;
        if (!changed) row->unique_files = 1;
    }
    row->sizes[DS_UNIQUE] = unique;
    for (DsMetric metric = DS_LOGICAL; metric < DS_METRIC_COUNT; ++metric)
        row->unknown[metric] = row->sizes[metric].quality != DS_KNOWN;
    return SQLITE_OK;
}

static void add_size(DsSize *total, DsSize next)
{
    if (total->quality == DS_OVERFLOW) return;
    if (next.quality == DS_OVERFLOW ||
        (next.quality == DS_KNOWN && !ds_u64_add(total->bytes, next.bytes, &total->bytes)))
        *total = (DsSize){0, DS_OVERFLOW};
}

static bool add_count(int64_t *total, int64_t next)
{
    if (next < 0 || *total > INT64_MAX - next) return false;
    *total += next;
    return true;
}

static int aggregate_directory(sqlite3_stmt *children, int64_t id, Aggregate *row)
{
    sqlite3_reset(children);
    int rc = sqlite3_bind_int64(children, 2, id);
    while (rc == SQLITE_OK && (rc = sqlite3_step(children)) == SQLITE_ROW) {
        if (sqlite3_column_type(children, 0) == SQLITE_NULL ||
            !add_count(&row->entries, sqlite3_column_int64(children, 10)) ||
            !add_count(&row->files, sqlite3_column_int64(children, 11)) ||
            !add_count(&row->unique_files, sqlite3_column_int64(children, 12)))
            return SQLITE_CORRUPT;
        for (DsMetric metric = DS_LOGICAL; metric < DS_METRIC_COUNT; ++metric) {
            int column = 1 + metric * 3;
            DsSize size;
            if (!ds_sql_read_size(children, column, &size) ||
                !add_count(&row->unknown[metric], sqlite3_column_int64(children, column + 2)))
                return SQLITE_CORRUPT;
            add_size(&row->sizes[metric], size);
        }
        rc = SQLITE_OK;
    }
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static int bind_aggregate(sqlite3_stmt *stmt, int64_t id, const Aggregate *row)
{
    sqlite3_reset(stmt);
    int rc = sqlite3_bind_int64(stmt, 2, id);
    for (DsMetric metric = DS_LOGICAL; rc == SQLITE_OK && metric < DS_METRIC_COUNT; ++metric) {
        int column = 3 + metric * 3;
        rc = ds_sql_bind_size(stmt, column, row->sizes[metric]);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, column + 2, row->unknown[metric]);
    }
    if (rc == SQLITE_OK) rc = row->representative
        ? sqlite3_bind_int64(stmt, 12, row->representative) : sqlite3_bind_null(stmt, 12);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 13, row->entries);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 14, row->files);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 15, row->unique_files);
    if (rc == SQLITE_OK) rc = ds_sql_step_done(stmt);
    return rc;
}

static int aggregate_entries(DsStore *store, int64_t generation, DsControl *control)
{
    sqlite3_stmt *entries = NULL, *children = NULL, *insert = NULL;
    int rc = ds_sql_prepare(store, &entries,
        "SELECT e.id,e.kind,e.logical,e.logical_quality,e.allocated,e.allocated_quality,"
        " o.representative_id,o.changed,r.allocated_quality FROM entries e"
        " LEFT JOIN file_objects o ON o.generation=?1 AND o.identity=e.identity AND e.kind=2"
        " LEFT JOIN entries r ON r.id=o.representative_id ORDER BY e.id DESC");
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &children,
        "SELECT a.node_id,a.logical,a.logical_quality,a.logical_unknown,"
        " a.allocated,a.allocated_quality,a.allocated_unknown,a.unique_allocated,a.unique_quality,"
        " a.unique_unknown,a.entry_count,a.file_count,a.unique_file_count FROM entries e"
        " LEFT JOIN aggregates a ON a.generation=?1 AND a.node_id=e.id WHERE e.parent_id=?2");
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &insert,
        "INSERT INTO aggregates VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15)");
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(entries, 1, generation);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(children, 1, generation);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(insert, 1, generation);

    size_t processed = 0;
    while (rc == SQLITE_OK && (rc = sqlite3_step(entries)) == SQLITE_ROW) {
        if (++processed % DS_BATCH_MAX == 0) {
            if (control && !ds_control_checkpoint(control)) { rc = SQLITE_INTERRUPT; break; }
            rc = ds_store_check_budget(store, false);
            if (rc != SQLITE_OK) break;
        }
        int64_t id = sqlite3_column_int64(entries, 0);
        Aggregate row = {.entries = 1};
        switch (sqlite3_column_int(entries, 1)) {
        case DS_DIRECTORY: rc = aggregate_directory(children, id, &row); break;
        case DS_FILE: rc = aggregate_file(entries, &row); break;
        case DS_LINK:
        case DS_SPECIAL:
        case DS_EXCLUDED:
            for (DsMetric metric = DS_LOGICAL; metric < DS_METRIC_COUNT; ++metric)
                row.sizes[metric].quality = DS_UNAVAILABLE;
            rc = SQLITE_OK;
            break;
        default: rc = SQLITE_CORRUPT; break;
        }
        if (rc == SQLITE_OK) rc = bind_aggregate(insert, id, &row);
    }
    sqlite3_finalize(insert);
    sqlite3_finalize(children);
    sqlite3_finalize(entries);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static int start_generation(DsStore *store, int64_t *generation)
{
    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt,
        "INSERT INTO generations(as_of_commit_seq,aggregation_complete,coverage,entry_count)"
        " SELECT commit_seq,0,coverage,entry_count FROM scans WHERE id=1 AND root_raw IS NOT NULL RETURNING id");
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *generation = sqlite3_column_int64(stmt, 0);
        rc = ds_sql_step_done(stmt);
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_CONSTRAINT;
    }
    sqlite3_finalize(stmt);
    return rc;
}

static int finish_generation(DsStore *store, int64_t generation)
{
    static const char *statements[] = {
        "UPDATE generations SET aggregation_complete=1 WHERE id=?1 AND entry_count="
        " (SELECT count(*) FROM aggregates WHERE generation=?1)",
        "UPDATE scans SET active_generation=?1 WHERE id=1",
        "DELETE FROM generations WHERE id<>?1",
    };
    int rc = SQLITE_OK;
    for (size_t i = 0; rc == SQLITE_OK && i < G_N_ELEMENTS(statements); ++i) {
        sqlite3_stmt *stmt = NULL;
        rc = ds_sql_prepare(store, &stmt, statements[i]);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, generation);
        if (rc == SQLITE_OK) rc = ds_sql_step_done(stmt);
        if (i == 0 && rc == SQLITE_OK && sqlite3_changes(store->db) != 1) rc = SQLITE_CORRUPT;
        sqlite3_finalize(stmt);
    }
    return rc;
}

DsStatus ds_finalize(DsStore *store, DsControl *control, DsError *error)
{
    if (!store || store->mode != DS_STORE_WRITER)
        return ds_error(error, DS_INVALID, 0, "A writable store is required for aggregation");
    if (control && !ds_control_checkpoint(control))
        return ds_error(error, DS_CANCELED, 0, "Aggregation canceled");
    int rc = ds_store_check_budget(store, false);
    if (rc == SQLITE_OK) rc = ds_sql_exec(store, "BEGIN IMMEDIATE");
    if (rc != SQLITE_OK) return ds_sql_status(store, rc, error);
    if (control) sqlite3_progress_handler(store->db, 1000, ds_sql_interrupt, control);
    int64_t generation = 0;
    rc = start_generation(store, &generation);
    if (rc == SQLITE_OK) rc = ds_hardlinks_finalize(store, generation, control);
    if (rc == SQLITE_OK) rc = aggregate_entries(store, generation, control);
    if (rc == SQLITE_OK) rc = finish_generation(store, generation);
    if (control) sqlite3_progress_handler(store->db, 0, NULL, NULL);
    if (rc == SQLITE_OK && ds_sql_progress(control)) rc = SQLITE_INTERRUPT;
    if (rc == SQLITE_OK) rc = ds_store_check_budget(store, false);
    if (rc == SQLITE_OK) rc = ds_sql_exec(store, "COMMIT");
    if (rc != SQLITE_OK) {
        DsStatus status = ds_sql_status(store, rc, error);
        ds_sql_exec(store, "ROLLBACK");
        return status;
    }
    ds_store_checkpoint(store);
    return DS_OK;
}

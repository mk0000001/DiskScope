#include "../store/store_internal.h"

typedef struct {
    DsSize sizes[DS_METRIC_COUNT];
    int64_t unknown[DS_METRIC_COUNT];
    int64_t entries, files, unique_files;
} Aggregate;

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

static int bind_directory(sqlite3_stmt *stmt, int64_t id, const Aggregate *row)
{
    sqlite3_reset(stmt);
    int rc = sqlite3_bind_int64(stmt, 2, id);
    for (DsMetric metric = DS_LOGICAL; rc == SQLITE_OK && metric < DS_METRIC_COUNT; ++metric) {
        int column = 3 + metric * 3;
        rc = ds_sql_bind_size(stmt, column, row->sizes[metric]);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, column + 2, row->unknown[metric]);
    }
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 12, row->entries);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 13, row->files);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 14, row->unique_files);
    if (rc == SQLITE_OK) rc = ds_sql_step_done(stmt);
    return rc;
}

static int aggregate_leaves(DsStore *store, int64_t generation, DsControl *control)
{
    sqlite3_stmt *bounds = NULL, *insert = NULL;
    int rc = ds_sql_prepare(store, &bounds,
        "SELECT max(id) FROM (SELECT id FROM entries WHERE id>?1 ORDER BY id LIMIT ?2)");
    /* File sizes already use the required unsigned encoding. Validate their
     * quality and encoding before copying. A missing row blocks publication. */
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &insert,
        "WITH leaves AS (SELECT e.id,e.kind=2 AS file,"
        " CASE WHEN e.kind=2 THEN e.logical END AS logical,"
        " CASE WHEN e.kind=2 THEN e.logical_quality ELSE ?5 END AS logical_quality,"
        " CASE WHEN e.kind=2 THEN e.allocated END AS allocated,"
        " CASE WHEN e.kind=2 THEN e.allocated_quality ELSE ?5 END AS allocated_quality,"
        " CASE WHEN e.kind<>2 THEN ?5 WHEN o.changed THEN ?6"
        "      WHEN e.allocated_quality<>?4 THEN e.allocated_quality"
        "      WHEN o.representative_id IS NULL THEN ?5"
        "      WHEN o.representative_id=e.id THEN e.allocated_quality"
        "      ELSE r.allocated_quality END AS unique_quality,"
        " o.representative_id,COALESCE(o.representative_id=e.id AND NOT o.changed,0) AS unique_files"
        " FROM entries e"
        " LEFT JOIN file_objects o ON o.generation=?1 AND o.identity=e.identity AND e.kind=2"
        " LEFT JOIN entries r ON r.id=NULLIF(o.representative_id,e.id)"
        " WHERE e.id>?2 AND e.id<=?3 AND (e.kind IN (3,4,5) OR (e.kind=2 AND"
        " ((e.logical_quality=?4 AND length(e.logical)=8) OR"
        "  (e.logical_quality BETWEEN ?5 AND ?6 AND e.logical IS NULL)) AND"
        " ((e.allocated_quality=?4 AND length(e.allocated)=8) OR"
        "  (e.allocated_quality BETWEEN ?5 AND ?6 AND e.allocated IS NULL)))))"
        " INSERT INTO aggregates SELECT ?1,id,logical,logical_quality,file AND logical_quality<>?4,"
        " allocated,allocated_quality,file AND allocated_quality<>?4,"
        " CASE WHEN unique_quality=?4 THEN"
        "      CASE WHEN representative_id=id THEN allocated ELSE zeroblob(8) END END,"
        " unique_quality,file AND unique_quality<>?4,representative_id,1,file,unique_files"
        " FROM leaves ORDER BY id");
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(bounds, 2, DS_BATCH_MAX);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(insert, 1, generation);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(insert, 4, DS_KNOWN);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(insert, 5, DS_UNAVAILABLE);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(insert, 6, DS_CHANGED);

    int64_t previous = 0;
    while (rc == SQLITE_OK) {
        sqlite3_reset(bounds);
        rc = sqlite3_bind_int64(bounds, 1, previous);
        if (rc != SQLITE_OK) break;
        rc = sqlite3_step(bounds);
        if (rc != SQLITE_ROW) break;
        if (sqlite3_column_type(bounds, 0) == SQLITE_NULL) {
            rc = SQLITE_OK;
            break;
        }
        int64_t last = sqlite3_column_int64(bounds, 0);
        sqlite3_reset(bounds);
        sqlite3_reset(insert);
        rc = sqlite3_bind_int64(insert, 2, previous);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(insert, 3, last);
        if (rc == SQLITE_OK) rc = ds_sql_step_done(insert);
        if (rc == SQLITE_OK && control && !ds_control_checkpoint(control)) rc = SQLITE_INTERRUPT;
        if (rc == SQLITE_OK) rc = ds_store_check_budget(store, false);
        previous = last;
    }
    sqlite3_finalize(insert);
    sqlite3_finalize(bounds);
    return rc;
}

static int aggregate_directories(DsStore *store, int64_t generation, DsControl *control)
{
    sqlite3_stmt *entries = NULL, *children = NULL, *insert = NULL;
    int rc = ds_sql_prepare(store, &entries,
        "SELECT id FROM entries WHERE kind=1 ORDER BY id DESC");
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &children,
        "SELECT a.node_id,a.logical,a.logical_quality,a.logical_unknown,"
        " a.allocated,a.allocated_quality,a.allocated_unknown,a.unique_allocated,a.unique_quality,"
        " a.unique_unknown,a.entry_count,a.file_count,a.unique_file_count FROM entries e"
        " LEFT JOIN aggregates a ON a.generation=?1 AND a.node_id=e.id WHERE e.parent_id=?2");
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &insert,
        "INSERT INTO aggregates VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,NULL,?12,?13,?14)");
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
        rc = aggregate_directory(children, id, &row);
        if (rc == SQLITE_OK) rc = bind_directory(insert, id, &row);
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

static DsStatus finalize(DsStore *store, int result, DsCoverage coverage,
                         DsControl *control, DsError *error)
{
    if (!store || store->mode != DS_STORE_WRITER)
        return ds_error(error, DS_INVALID, 0, "A writable store is required for aggregation");
    if (control && !ds_control_checkpoint(control))
        return ds_error(error, DS_CANCELED, 0, "Aggregation canceled");
    int rc = ds_store_check_budget(store, false);
    if (rc == SQLITE_OK) rc = ds_sql_exec(store, "BEGIN IMMEDIATE");
    if (rc != SQLITE_OK) return ds_sql_status(store, rc, error);
    if (result != DS_RUN_RUNNING) {
        DsStatus status = ds_store_set_result(store, (DsStatus)result, coverage, error);
        if (status != DS_OK) {
            ds_sql_exec(store, "ROLLBACK");
            return status;
        }
    }
    if (control) sqlite3_progress_handler(store->db, 1000, ds_sql_interrupt, control);
    int64_t generation = 0;
    rc = start_generation(store, &generation);
    if (rc == SQLITE_OK) rc = ds_hardlinks_finalize(store, generation, control);
    if (rc == SQLITE_OK) rc = aggregate_leaves(store, generation, control);
    if (rc == SQLITE_OK) rc = aggregate_directories(store, generation, control);
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
    ds_store_checkpoint(store, true);
    return DS_OK;
}

DsStatus ds_finalize(DsStore *store, DsControl *control, DsError *error)
{
    return finalize(store, DS_RUN_RUNNING, DS_COVERAGE_PARTIAL, control, error);
}

DsStatus ds_finalize_result(DsStore *store, DsStatus result, DsCoverage coverage,
                             DsControl *control, DsError *error)
{
    return finalize(store, result, coverage, control, error);
}

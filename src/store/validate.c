#include "diskscope/ds_snapshot.h"
#include "store_internal.h"
#include <glib/gstdio.h>

#define MAX_SNAPSHOT_BYTES (UINT64_C(64) << 30)
#define MAX_SNAPSHOT_ROWS 20000000LL

static int expect_no_rows(DsStore *store, const char *sql)
{
    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt, sql);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) return SQLITE_OK;
    return rc == SQLITE_ROW ? SQLITE_CORRUPT : rc;
}

static int expect_text(DsStore *store, const char *sql, const char *expected)
{
    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt, sql);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *result = (const char *)sqlite3_column_text(stmt, 0);
        rc = !g_strcmp0(result, expected) ? ds_sql_step_done(stmt) : SQLITE_CORRUPT;
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_CORRUPT;
    }
    sqlite3_finalize(stmt);
    return rc;
}

static int validate_entries(DsStore *store, const DsStoreInfo *info)
{
    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt,
        "SELECT e.id,e.name,e.name_encoding,e.display,e.kind,e.reason,e.logical,e.logical_quality,"
        " e.allocated,e.allocated_quality,e.identity,e.links,e.commit_seq,p.kind"
        " FROM entries e LEFT JOIN entries p ON p.id=e.parent_id");
    uint64_t seen = 0;
    while (rc == SQLITE_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (++seen > info->entry_count) { rc = SQLITE_CORRUPT; break; }
        const void *raw = sqlite3_column_blob(stmt, 1);
        size_t length = (size_t)sqlite3_column_bytes(stmt, 1);
        DsEncoding encoding = sqlite3_column_int(stmt, 2);
        const char *stored_display = (const char *)sqlite3_column_text(stmt, 3);
        int64_t id = sqlite3_column_int64(stmt, 0), seq = sqlite3_column_int64(stmt, 12);
        DsKind kind = sqlite3_column_int(stmt, 4);
        DsReason reason = sqlite3_column_int(stmt, 5);
        DsSize size;
        uint64_t links = 0;
        if (id < 1 || encoding != info->root.encoding ||
            (id == 1 && (length || kind != DS_DIRECTORY)) ||
            (id != 1 && (sqlite3_column_int(stmt, 13) != DS_DIRECTORY ||
                         !ds_path_component_valid(encoding, raw, length))) ||
            kind < DS_DIRECTORY || kind > DS_EXCLUDED || reason < DS_REASON_NONE || reason > DS_REASON_IO ||
            !ds_sql_read_size(stmt, 6, &size) || !ds_sql_read_size(stmt, 8, &size) ||
            (sqlite3_column_type(stmt, 10) != SQLITE_NULL && sqlite3_column_bytes(stmt, 10) != 24) ||
            !ds_sql_read_u64(stmt, 11, &links) || seq < 1 || seq > info->commit_seq) {
            rc = SQLITE_CORRUPT;
            break;
        }
        DsNativePath name = {0};
        char *display = NULL;
        DsStatus status = ds_path_new(encoding, raw, length, &name, NULL);
        if (status == DS_OK) status = ds_path_display(&name, &display, NULL);
        bool matches = status == DS_OK && stored_display &&
            strlen(display) == (size_t)sqlite3_column_bytes(stmt, 3) && !strcmp(display, stored_display);
        ds_path_clear(&name);
        g_free(display);
        rc = matches ? SQLITE_OK : SQLITE_CORRUPT;
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? (seen == info->entry_count ? SQLITE_OK : SQLITE_CORRUPT) : rc;
}

static int validate_metrics(DsStore *store)
{
    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt,
        "SELECT logical,logical_quality,logical_unknown,allocated,allocated_quality,allocated_unknown,"
        " unique_allocated,unique_quality,unique_unknown,entry_count,file_count,unique_file_count FROM aggregates");
    while (rc == SQLITE_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int64_t entries = sqlite3_column_int64(stmt, 9), files = sqlite3_column_int64(stmt, 10);
        int64_t unique = sqlite3_column_int64(stmt, 11);
        rc = entries < 1 || files < 0 || files > entries || unique < 0 || unique > files
            ? SQLITE_CORRUPT : SQLITE_OK;
        for (DsMetric metric = DS_LOGICAL; rc == SQLITE_OK && metric < DS_METRIC_COUNT; ++metric) {
            DsSize value;
            int col = metric * 3;
            int64_t unknown = sqlite3_column_int64(stmt, col + 2);
            if (!ds_sql_read_size(stmt, col, &value) || unknown < 0 || unknown > files)
                rc = SQLITE_CORRUPT;
        }
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static int validate_metadata(DsStore *store, const DsStoreInfo *info)
{
    if (info->commit_seq < 0 || info->entry_count > MAX_SNAPSHOT_ROWS || info->generation < 0 ||
        info->as_of_commit_seq < 0 || info->as_of_commit_seq > info->commit_seq ||
        info->run_status < DS_RUN_RUNNING || info->run_status > DS_PROTOCOL ||
        info->coverage < DS_COVERAGE_PARTIAL || info->coverage > DS_COVERAGE_COMPLETE ||
        info->aggregate_coverage < DS_COVERAGE_PARTIAL || info->aggregate_coverage > DS_COVERAGE_COMPLETE ||
        (info->coverage == DS_COVERAGE_COMPLETE && info->run_status != DS_OK) ||
        (info->generation && !info->aggregation_complete) || !info->root.raw || !info->root_display ||
        !g_bytes_get_size(info->root.raw) || !info->policy || strlen(info->policy) > 4096 ||
        !g_utf8_validate(info->policy, -1, NULL))
        return SQLITE_CORRUPT;
    char *display = NULL;
    DsStatus status = ds_path_display(&info->root, &display, NULL);
    bool matches = status == DS_OK && !strcmp(display, info->root_display);
    g_free(display);
    if (!matches) return SQLITE_CORRUPT;
    return expect_no_rows(store,
        "SELECT 1 FROM generations g,scans s WHERE g.id<>s.active_generation OR"
        " g.aggregation_complete<>1 OR g.entry_count<>(SELECT count(*) FROM aggregates a WHERE a.generation=g.id)"
        " OR g.entry_count<>(SELECT count(*) FROM entries e WHERE e.commit_seq<=g.as_of_commit_seq)"
        " UNION ALL SELECT 1 FROM file_objects o JOIN entries e ON e.id=o.representative_id"
        " WHERE e.kind<>2 OR e.identity IS NOT o.identity OR length(o.identity)<>24 OR o.changed NOT IN (0,1)"
        " LIMIT 1");
}

DsStatus ds_snapshot_open(const DsNativePath *path, DsStore **reader,
                          DsControl *control, DsError *error)
{
    if (!reader) return ds_error(error, DS_INVALID, 0, "Missing snapshot output");
    *reader = NULL;
    if (ds_control_canceled(control)) return DS_CANCELED;
    char *filename = ds_path_arg(path, error);
    if (!filename) return DS_INVALID;
    GStatBuf info;
    int present = g_stat(filename, &info);
    const char *const suffixes[] = {"-wal", "-shm", "-journal"};
    bool sidecar = false;
    for (size_t i = 0; !sidecar && i < G_N_ELEMENTS(suffixes); ++i) {
        char *related = g_strconcat(filename, suffixes[i], NULL);
        sidecar = g_file_test(related, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK);
        g_free(related);
    }
    g_free(filename);
    if (present != 0) return ds_error(error, DS_IO, 0, "Snapshot cannot be accessed");
    if (sidecar) return ds_error(error, DS_INVALID, 0, "A standalone snapshot cannot have a journal or WAL");
    if (info.st_size < 100 || !S_ISREG(info.st_mode))
        return ds_error(error, DS_INVALID, 0, "Snapshot is not a complete database file");
    if ((uint64_t)info.st_size > MAX_SNAPSHOT_BYTES)
        return ds_error(error, DS_LIMIT, 0, "Snapshot exceeds the import size limit");

    DsStore *store = NULL;
    DsStatus status = ds_store_open(path, DS_STORE_READER, &store, error);
    if (status != DS_OK) return status;
    sqlite3_progress_handler(store->db, 1000, ds_sql_interrupt, control);
    int rc = ds_sql_exec(store, "PRAGMA cell_size_check=ON; BEGIN");
    if (rc == SQLITE_OK) rc = expect_text(store, "PRAGMA journal_mode", "delete");
    if (rc == SQLITE_OK) rc = expect_text(store, "PRAGMA quick_check(1)", "ok");
    if (rc == SQLITE_OK) rc = expect_no_rows(store, "PRAGMA foreign_key_check");

    DsStoreInfo metadata = {0};
    if (rc == SQLITE_OK && ds_store_info(store, &metadata, error) != DS_OK)
        rc = ds_control_canceled(control) ? SQLITE_INTERRUPT : SQLITE_CORRUPT;
    if (rc == SQLITE_OK) rc = validate_metadata(store, &metadata);
    if (rc == SQLITE_OK) rc = validate_entries(store, &metadata);
    if (rc == SQLITE_OK) rc = validate_metrics(store);
    ds_store_info_clear(&metadata);
    if (rc == SQLITE_OK) rc = expect_no_rows(store,
        "WITH RECURSIVE tree(id,depth,bytes) AS (SELECT id,0,0 FROM entries WHERE id=1"
        " UNION ALL SELECT e.id,t.depth+1,t.bytes+length(e.name)+"
        " CASE WHEN t.depth=0 THEN 0 ELSE e.name_encoding END"
        " FROM tree t JOIN entries e ON e.parent_id=t.id WHERE t.depth<=4096 AND t.bytes<=262144)"
        " SELECT 1 FROM tree WHERE depth>4096 OR bytes>262144 LIMIT 1");
    sqlite3_progress_handler(store->db, 0, NULL, NULL);
    if (ds_control_canceled(control)) rc = SQLITE_INTERRUPT;
    if (rc == SQLITE_OK) rc = ds_sql_exec(store, "COMMIT");
    if (rc == SQLITE_OK) {
        *reader = store;
        return DS_OK;
    }
    status = ds_sql_status(store, rc, error);
    ds_store_close(store);
    return status;
}

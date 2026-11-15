#include "store_internal.h"
#include <gio/gio.h>
#include <glib/gstdio.h>

static uint64_t file_size(const char *path)
{
    GStatBuf info;
    return g_stat(path, &info) == 0 && info.st_size > 0 ? (uint64_t)info.st_size : 0;
}

int ds_store_check_budget(DsStore *store, bool collecting)
{
    char *wal = g_strconcat(store->filename, "-wal", NULL);
    char *shm = g_strconcat(store->filename, "-shm", NULL);
    uint64_t size = file_size(store->filename);
    bool fits = ds_u64_add(size, file_size(wal), &size) &&
        ds_u64_add(size, file_size(shm), &size) &&
        size < store->budget - (collecting ? DS_CACHE_END_RESERVE : 0);
    g_free(wal);
    g_free(shm);
    if (!fits) return SQLITE_FULL;

    char *parent = g_path_get_dirname(store->filename);
    GFile *dir = g_file_new_for_path(parent);
    GFileInfo *info = g_file_query_filesystem_info(dir, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, NULL, NULL);
    if (info && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
        uint64_t free_space = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        fits = free_space >= (collecting ? DS_CACHE_FREE_RESERVE : DS_CACHE_END_RESERVE);
    }
    g_clear_object(&info);
    g_object_unref(dir);
    g_free(parent);
    return fits ? SQLITE_OK : SQLITE_FULL;
}

void ds_store_checkpoint(DsStore *store)
{
    int wal_frames = 0, checkpointed = 0;
    int rc = sqlite3_wal_checkpoint_v2(store->db, NULL, SQLITE_CHECKPOINT_PASSIVE,
                                       &wal_frames, &checkpointed);
    if (rc == SQLITE_OK && wal_frames == checkpointed && wal_frames >= 4096)
        sqlite3_wal_checkpoint_v2(store->db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
}

static void fingerprint_part(GChecksum *checksum, guchar tag, const void *data, size_t length)
{
    uint8_t size[8];
    ds_u64_encode(length, size);
    g_checksum_update(checksum, &tag, 1);
    g_checksum_update(checksum, size, sizeof size);
    if (length) g_checksum_update(checksum, data, length);
}

static char *make_policy(const DsScanOptions *options)
{
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    const char *profile = options->profile == DS_STANDARD ? "standard" : "conservative";
    const char *pattern = options->exclude_pattern ? options->exclude_pattern : "";
    const char *hidden = options->include_hidden ? "include" : "exclude";
    fingerprint_part(checksum, 0, profile, strlen(profile));
    fingerprint_part(checksum, 1, pattern, strlen(pattern));
    fingerprint_part(checksum, 2, hidden, strlen(hidden));
    for (size_t i = 0; i < options->exclude_count; ++i) {
        size_t length = 0;
        const void *raw = g_bytes_get_data(options->exclude_paths[i].raw, &length);
        fingerprint_part(checksum, (guchar)options->exclude_paths[i].encoding + 2, raw, length);
    }
    char *policy = g_strdup_printf("%s;hidden=%s;sha256=%s", profile, hidden,
                                    g_checksum_get_string(checksum));
    g_checksum_free(checksum);
    return policy;
}

DsStatus ds_store_set_options(DsStore *store, const DsScanOptions *options, DsError *error)
{
    if (!store || store->mode != DS_STORE_WRITER || !options || !options->root.raw ||
        (options->exclude_count && !options->exclude_paths))
        return ds_error(error, DS_INVALID, 0, "Invalid scan options or store mode");
    uint64_t budget = options->cache_budget ? options->cache_budget : DS_CACHE_BUDGET;
    if (budget < (UINT64_C(128) << 20) || budget > INT64_MAX)
        return ds_error(error, DS_INVALID, 0, "Invalid scan cache budget");
    for (size_t i = 0; i < options->exclude_count; ++i)
        if (!options->exclude_paths[i].raw)
            return ds_error(error, DS_INVALID, 0, "Invalid excluded path");

    size_t length = 0;
    const void *raw = g_bytes_get_data(options->root.raw, &length);
    if (!length) return ds_error(error, DS_INVALID, 0, "Scan root is empty");
    char *display = NULL;
    DsStatus status = ds_path_display(&options->root, &display, error);
    if (status != DS_OK) return status;
    char *policy = make_policy(options);
    uint64_t old_budget = store->budget;
    store->budget = budget;
    int rc = ds_store_check_budget(store, true);
    sqlite3_stmt *statement = NULL;
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &statement, "UPDATE scans SET"
        " root_raw=?1,encoding=?2,root_display=?3,policy=?4,cache_budget=?5,started_at=unixepoch()"
        " WHERE id=1 AND root_raw IS NULL");
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob64(statement, 1, raw, length, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(statement, 2, options->root.encoding);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 3, display, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 4, policy, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 5, (int64_t)budget);
    if (rc == SQLITE_OK) rc = ds_sql_step_done(statement);
    if (rc == SQLITE_OK && sqlite3_changes(store->db) != 1) rc = SQLITE_CONSTRAINT;
    sqlite3_finalize(statement);
    if (rc != SQLITE_OK) store->budget = old_budget;
    g_free(policy);
    g_free(display);
    return ds_sql_status(store, rc, error);
}

DsStatus ds_store_set_result(DsStore *store, DsStatus run_status, DsCoverage coverage,
                             DsError *error)
{
    if (!store || store->mode != DS_STORE_WRITER || run_status < DS_OK || run_status > DS_PROTOCOL ||
        (coverage != DS_COVERAGE_PARTIAL && coverage != DS_COVERAGE_COMPLETE) ||
        (coverage == DS_COVERAGE_COMPLETE && run_status != DS_OK))
        return ds_error(error, DS_INVALID, 0, "Invalid scan result or store mode");
    sqlite3_stmt *statement = NULL;
    int rc = ds_sql_prepare(store, &statement, "UPDATE scans SET run_status=?1,coverage=?2,"
        " ended_at=unixepoch() WHERE id=1");
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(statement, 1, run_status);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(statement, 2, coverage);
    if (rc == SQLITE_OK) rc = ds_sql_step_done(statement);
    sqlite3_finalize(statement);
    return ds_sql_status(store, rc, error);
}

static bool valid_entry(const DsEntry *e)
{
    if (!e->name.raw || e->id < 1 || e->kind < DS_DIRECTORY || e->kind > DS_EXCLUDED ||
        e->reason < DS_REASON_NONE || e->reason > DS_REASON_IO ||
        e->logical.quality < DS_KNOWN || e->logical.quality > DS_CHANGED ||
        e->allocated.quality < DS_KNOWN || e->allocated.quality > DS_CHANGED)
        return false;
    size_t length = 0;
    const void *name = g_bytes_get_data(e->name.raw, &length);
    if (e->id == 1) return e->parent_id == 0 && e->kind == DS_DIRECTORY && length == 0;
    return e->parent_id > 0 && e->parent_id < e->id &&
        ds_path_component_valid(e->name.encoding, name, length);
}

static int write_entry(DsStore *store, sqlite3_stmt *stmt, const DsEntry *e, int64_t seq,
                       bool *inserted, DsError *error)
{
    size_t length = 0;
    const void *raw = g_bytes_get_data(e->name.raw, &length);
    char *display = NULL;
    DsStatus status = ds_path_display(&e->name, &display, error);
    if (status != DS_OK) return SQLITE_MISMATCH;
    sqlite3_reset(stmt);
    int rc = sqlite3_bind_int64(stmt, 1, e->id);
    if (rc == SQLITE_OK) rc = e->parent_id ? sqlite3_bind_int64(stmt, 2, e->parent_id)
                                          : sqlite3_bind_null(stmt, 2);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob64(stmt, 3, length ? raw : "", length, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 4, e->name.encoding);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 5, display, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 6, e->kind);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 7, e->reason);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 8, e->os_code);
    if (rc == SQLITE_OK) rc = ds_sql_bind_size(stmt, 9, e->logical);
    if (rc == SQLITE_OK) rc = ds_sql_bind_size(stmt, 11, e->allocated);
    if (rc == SQLITE_OK) rc = e->identity_known
        ? sqlite3_bind_blob(stmt, 13, e->identity, sizeof e->identity, SQLITE_STATIC)
        : sqlite3_bind_null(stmt, 13);
    if (rc == SQLITE_OK) rc = ds_sql_bind_u64(stmt, 14, e->links);
    if (rc == SQLITE_OK) rc = e->mtime_known ? sqlite3_bind_int64(stmt, 15, e->mtime)
                                            : sqlite3_bind_null(stmt, 15);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 16, seq);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            *inserted = sqlite3_column_int64(stmt, 0) == seq;
            rc = ds_sql_step_done(stmt);
        } else if (rc == SQLITE_DONE) {
            rc = SQLITE_CONSTRAINT;
        }
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    g_free(display);
    (void)store;
    return rc;
}

static int record_issue(sqlite3_stmt *stmt, const DsEntry *e)
{
    sqlite3_reset(stmt);
    int rc = sqlite3_bind_int64(stmt, 1, e->id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, e->os_code);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 3, e->reason);
    if (rc == SQLITE_OK) rc = ds_sql_step_done(stmt);
    return rc;
}

DsStatus ds_store_append(DsStore *store, const DsEntryBatch *batch, DsError *error)
{
    if (!store || store->mode != DS_STORE_WRITER || !batch ||
        (batch->count && !batch->entries) || batch->count > DS_BATCH_MAX)
        return ds_error(error, DS_INVALID, 0, "Invalid entry batch or store mode");
    if (!batch->count) return DS_OK;
    for (size_t i = 0; i < batch->count; ++i)
        if (!valid_entry(&batch->entries[i]))
            return ds_error(error, DS_INVALID, 0, "Invalid entry at batch index %" G_GSIZE_FORMAT, i);

    int rc = ds_store_check_budget(store, true);
    if (rc == SQLITE_OK) rc = ds_sql_exec(store, "BEGIN IMMEDIATE");
    if (rc != SQLITE_OK) return ds_sql_status(store, rc, error);

    sqlite3_stmt *metadata = NULL, *entry = NULL, *issue = NULL;
    rc = ds_sql_prepare(store, &metadata,
        "SELECT commit_seq,encoding FROM scans WHERE id=1 AND root_raw IS NOT NULL AND run_status=-1");
    int64_t seq = 0;
    DsEncoding encoding = 0;
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(metadata);
        if (rc == SQLITE_ROW) {
            seq = sqlite3_column_int64(metadata, 0);
            encoding = sqlite3_column_int(metadata, 1);
            rc = seq == INT64_MAX ? SQLITE_FULL : SQLITE_OK;
        } else if (rc == SQLITE_DONE) {
            rc = SQLITE_CONSTRAINT;
        }
    }
    sqlite3_finalize(metadata);
    metadata = NULL;
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &entry,
        "INSERT INTO entries(id,parent_id,name,name_encoding,display,kind,reason,os_code,"
        " logical,logical_quality,allocated,allocated_quality,identity,links,mtime,commit_seq)"
        " SELECT ?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16"
        " WHERE ?1=1 OR EXISTS(SELECT 1 FROM entries WHERE id=?2 AND kind=1)"
        " ON CONFLICT(id) DO UPDATE SET reason=excluded.reason, os_code=excluded.os_code"
        " WHERE entries.kind=1 AND excluded.kind=1 AND entries.parent_id IS excluded.parent_id"
        " AND entries.name=excluded.name AND entries.name_encoding=excluded.name_encoding"
        " RETURNING commit_seq");
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &issue,
        "INSERT OR IGNORE INTO issues VALUES(?1,'observation',?2,?3,unixepoch())");
    int64_t added = 0;
    for (size_t i = 0; rc == SQLITE_OK && i < batch->count; ++i) {
        const DsEntry *observation = &batch->entries[i];
        bool inserted = false;
        if (observation->name.encoding != encoding) {
            rc = SQLITE_MISMATCH;
            break;
        }
        rc = write_entry(store, entry, observation, seq + 1, &inserted, error);
        if (rc == SQLITE_OK && observation->reason != DS_REASON_NONE)
            rc = record_issue(issue, observation);
        added += inserted;
    }
    sqlite3_finalize(entry);
    sqlite3_finalize(issue);
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &metadata,
        "UPDATE scans SET commit_seq=?1,entry_count=entry_count+?2 WHERE id=1");
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(metadata, 1, seq + 1);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(metadata, 2, added);
    if (rc == SQLITE_OK) rc = ds_sql_step_done(metadata);
    sqlite3_finalize(metadata);
    if (rc == SQLITE_OK) rc = ds_sql_exec(store, "COMMIT");
    if (rc != SQLITE_OK) {
        DsStatus status = ds_sql_status(store, rc, error);
        ds_sql_exec(store, "ROLLBACK");
        return status;
    }
    ds_store_checkpoint(store);
    return DS_OK;
}

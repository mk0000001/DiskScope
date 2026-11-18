#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "store_internal.h"
#include <glib/gstdio.h>
#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

enum { DS_WAL_CHECKPOINT_FRAMES = 512, DS_WAL_TRUNCATE_FRAMES = 4096 };

static uint64_t file_size(const char *path)
{
    GStatBuf info;
    return g_lstat(path, &info) == 0 && info.st_size > 0 ? (uint64_t)info.st_size : 0;
}

static uint64_t cache_size(DsStore *store, uint64_t database)
{
    if (store->cache_directory) {
#ifdef G_OS_WIN32
        return ds_win_cache_usage(store->cache_directory);
#else
        GDir *directory = g_dir_open(store->cache_directory, 0, NULL);
        if (!directory) return UINT64_MAX;
        uint64_t total = 0;
        const char *basename;
        while ((basename = g_dir_read_name(directory))) {
            char *path = g_build_filename(store->cache_directory, basename, NULL);
            uint64_t size = file_size(path);
            g_free(path);
            if (!ds_u64_add(total, size, &total)) {
                total = UINT64_MAX;
                break;
            }
        }
        g_dir_close(directory);
        return total;
#endif
    }
    char *wal = g_strconcat(store->filename, "-wal", NULL);
    char *shm = g_strconcat(store->filename, "-shm", NULL);
    uint64_t total;
    bool fits = ds_u64_add(database, file_size(wal), &total) &&
        ds_u64_add(total, file_size(shm), &total);
    g_free(wal);
    g_free(shm);
    return fits ? total : UINT64_MAX;
}

static int project_commit(DsStore *store, uint64_t database, uint64_t *size)
{
    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt, "PRAGMA page_count");
    uint64_t committed = 0;
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int64_t pages = sqlite3_column_int64(stmt, 0);
        rc = pages > 0 && ds_u64_mul((uint64_t)pages, store->page_size, &committed)
            ? SQLITE_OK : SQLITE_FULL;
    }
    sqlite3_finalize(stmt);
    int cache_bytes = 0, unused = 0;
    if (rc == SQLITE_OK)
        rc = sqlite3_db_status(store->db, SQLITE_DBSTATUS_CACHE_USED, &cache_bytes, &unused, 0);
    if (rc != SQLITE_OK) return rc;
    uint64_t checkpoint_growth = committed > database ? committed - database : 0;
    /* The resident page cache bounds SQLite's unflushed pages. WAL bytes
     * already spilled to disk are included in size. A checkpoint may grow
     * the main DB while those WAL frames still occupy space. */
    return cache_bytes >= 0 && ds_u64_add(*size, (uint64_t)cache_bytes, size) &&
        ds_u64_add(*size, checkpoint_growth, size) ? SQLITE_OK : SQLITE_FULL;
}

static int check_free_space(const char *directory, uint64_t reserve)
{
#ifdef G_OS_WIN32
    wchar_t *wide = (wchar_t *)g_utf8_to_utf16(directory, -1, NULL, NULL, NULL);
    ULARGE_INTEGER available;
    bool ok = wide && GetDiskFreeSpaceExW(wide, &available, NULL, NULL);
    g_free(wide);
    if (!ok) return SQLITE_IOERR_FSTAT;
    return available.QuadPart >= reserve ? SQLITE_OK : SQLITE_FULL;
#else
    struct statvfs space;
    if (statvfs(directory, &space) || !space.f_frsize) return SQLITE_IOERR_FSTAT;
    uint64_t blocks = reserve / space.f_frsize + (reserve % space.f_frsize != 0);
    return space.f_bavail >= blocks ? SQLITE_OK : SQLITE_FULL;
#endif
}

static void truncate_wal(DsStore *store)
{
    int rc = sqlite3_wal_checkpoint_v2(store->db, "main", SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
    if (rc == SQLITE_OK) {
        store->wal_frames = 0;
        store->wal_checkpoint_at = 0;
    }
}

int ds_store_check_budget(DsStore *store, bool collecting)
{
    uint64_t database = file_size(store->filename);
    uint64_t size = cache_size(store, database);
    bool in_transaction = sqlite3_get_autocommit(store->db) == 0;
    if (in_transaction) {
        int rc = project_commit(store, database, &size);
        if (rc != SQLITE_OK) return rc;
    }
    uint64_t threshold = store->budget - DS_CACHE_END_RESERVE - (UINT64_C(64) << 10);
    uint64_t reserve = collecting || !in_transaction ? DS_CACHE_FREE_RESERVE : DS_CACHE_END_RESERVE;
    char *parent = g_path_get_dirname(store->filename);
    int rc = size >= threshold ? SQLITE_FULL : check_free_space(parent, reserve);
    uint64_t checkpoint_margin = DS_WAL_CHECKPOINT_FRAMES * (store->page_size + 24);
    if (!in_transaction && store->wal_frames &&
        (rc == SQLITE_FULL || (rc == SQLITE_OK && threshold - size < checkpoint_margin))) {
        uint64_t required = DS_CACHE_END_RESERVE;
        if (project_commit(store, database, &required) == SQLITE_OK &&
            check_free_space(parent, required) == SQLITE_OK) {
            truncate_wal(store);
            database = file_size(store->filename);
            size = cache_size(store, database);
            rc = size >= threshold ? SQLITE_FULL : check_free_space(parent, reserve);
        }
    }
    g_free(parent);
    return rc;
}

#ifdef G_OS_WIN32
static bool sync_cached_wal(DsStore *store)
{
    /* The sole cache writer owns this SQLite connection and its WAL handle. */
    sqlite3_file *wal = NULL;
    return sqlite3_file_control(store->db, "main", SQLITE_FCNTL_JOURNAL_POINTER, &wal) == SQLITE_OK &&
           wal && wal->pMethods && wal->pMethods->xSync(wal, SQLITE_SYNC_NORMAL) == SQLITE_OK;
}
#endif

void ds_store_checkpoint(DsStore *store, bool force)
{
    if (force && store->terminal_wal)
        return;
    bool due = store->wal_frames - store->wal_checkpoint_at >= DS_WAL_CHECKPOINT_FRAMES;
#ifdef G_OS_WIN32
    const bool managed = store->cache_directory != NULL;
    const bool recycle = store->wal_frames >= DS_WAL_TRUNCATE_FRAMES;
    if (managed && recycle && store->wal_checkpoint_at < DS_WAL_TRUNCATE_FRAMES)
        due = true;
#endif
    if (!force && !due)
        return;
#ifdef G_OS_WIN32
    /* Sync committed WAL at the usual interval and backfill at the recycle
     * threshold or when a budget check requests space. */
    if (!force && managed && !recycle && sync_cached_wal(store)) {
        store->wal_checkpoint_at = store->wal_frames;
        return;
    }
#endif
#ifdef G_OS_WIN32
    int prior_frames = -1, prior_checkpointed = -1;
    bool prior_valid = managed &&
        sqlite3_wal_checkpoint_v2(store->db, "main", SQLITE_CHECKPOINT_NOOP,
                                  &prior_frames, &prior_checkpointed) == SQLITE_OK &&
        prior_checkpointed >= 0 && prior_checkpointed <= prior_frames &&
        prior_frames == store->wal_frames;
#endif
    int wal_frames = 0, checkpointed = 0;
    int rc = sqlite3_wal_checkpoint_v2(store->db, "main", SQLITE_CHECKPOINT_PASSIVE,
                                       &wal_frames, &checkpointed);
#ifdef G_OS_WIN32
    /* Successful backfill progress syncs the complete stable WAL first, even
     * when a pinned reader prevents finishing the checkpoint. The sole writer
     * must observe the same log before and after that progress. No progress or
     * uncertain counts still require the explicit WAL durability barrier. */
    bool checkpoint_synced_wal = rc == SQLITE_OK && prior_valid &&
        wal_frames == prior_frames && wal_frames == store->wal_frames &&
        checkpointed > prior_checkpointed && checkpointed <= wal_frames;
    if (managed && (rc != SQLITE_OK || wal_frames != checkpointed) &&
        !checkpoint_synced_wal && !sync_cached_wal(store))
        return;
#endif
    /* A pinned reader can prevent backfill. Wait for more frames before retrying. */
    store->wal_checkpoint_at = store->wal_frames;
    if (rc == SQLITE_OK && wal_frames == checkpointed && wal_frames >= DS_WAL_TRUNCATE_FRAMES)
        truncate_wal(store);
}

DsStatus ds_store_prepare_terminal_wal(DsStore *store, DsError *error)
{
    if (!store || store->mode != DS_STORE_WRITER || !sqlite3_get_autocommit(store->db))
        return ds_error(error, DS_INVALID, 0, "Terminal WAL requires an idle writable store");
    /* FULL syncs the final transaction in the WAL before success. Readers and
     * SQLite backup still see its committed pages without a second cache write. */
    int rc = ds_sql_exec(store, "PRAGMA synchronous=FULL");
    if (rc == SQLITE_OK)
        rc = sqlite3_db_config(store->db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, NULL);
    if (rc == SQLITE_OK) store->terminal_wal = true;
    return ds_sql_status(store, rc, error);
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

static char *managed_cache(DsStore *store, const DsNativePath *path)
{
    if (!path->raw) return NULL;
    char *arg = ds_path_arg(path, NULL);
    if (!arg) return NULL;
#ifdef G_OS_WIN32
    char *directory = ds_win_extended_path(arg);
    char *parent = g_path_get_dirname(store->filename);
#else
    char *directory = g_canonicalize_filename(arg, NULL);
    char *database = g_canonicalize_filename(store->filename, NULL);
    char *parent = g_path_get_dirname(database);
    g_free(database);
#endif
    if (g_strcmp0(directory, parent)) g_clear_pointer(&directory, g_free);
    g_free(parent);
    g_free(arg);
    return directory;
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
    char *old_cache = store->cache_directory;
    store->budget = budget;
    store->cache_directory = managed_cache(store, &options->cache_path);
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
    if (rc != SQLITE_OK) {
        store->budget = old_budget;
        g_free(store->cache_directory);
        store->cache_directory = old_cache;
    } else {
        g_free(old_cache);
    }
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

static int insert_entry(DsStore *store, sqlite3_stmt *stmt, const DsEntry *e, int64_t seq,
                        bool *inserted, DsError *error)
{
    size_t length = 0;
    const void *raw = g_bytes_get_data(e->name.raw, &length);
    char *display = NULL;
    DsStatus status = ds_path_display(&e->name, &display, error);
    if (status != DS_OK) return SQLITE_MISMATCH;
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
    if (rc == SQLITE_OK) rc = ds_sql_step_done(stmt);
    if (rc == SQLITE_OK) *inserted = sqlite3_changes(store->db) == 1;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    g_free(display);
    return rc;
}

static int update_directory(DsStore *store, sqlite3_stmt **update, const DsEntry *e)
{
    if (e->kind != DS_DIRECTORY) return SQLITE_CONSTRAINT;
    if (!*update) {
        int rc = ds_sql_prepare(store, update,
            "UPDATE entries SET reason=?5,os_code=?6"
            " WHERE id=?1 AND kind=1 AND parent_id IS ?2 AND name=?3 AND name_encoding=?4"
            " AND (?1=1 OR EXISTS(SELECT 1 FROM entries WHERE id=?2 AND kind=1))");
        if (rc != SQLITE_OK) return rc;
    }

    size_t length = 0;
    const void *raw = g_bytes_get_data(e->name.raw, &length);
    sqlite3_stmt *stmt = *update;
    int rc = sqlite3_bind_int64(stmt, 1, e->id);
    if (rc == SQLITE_OK) rc = e->parent_id ? sqlite3_bind_int64(stmt, 2, e->parent_id)
                                          : sqlite3_bind_null(stmt, 2);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob64(stmt, 3, length ? raw : "", length, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 4, e->name.encoding);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 5, e->reason);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 6, e->os_code);
    if (rc == SQLITE_OK) rc = ds_sql_step_done(stmt);
    if (rc == SQLITE_OK && sqlite3_changes(store->db) != 1) rc = SQLITE_CONSTRAINT;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
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
    size_t payload = batch->count * sizeof(DsEntry);
    for (size_t i = 0; i < batch->count; ++i) {
        if (!valid_entry(&batch->entries[i]))
            return ds_error(error, DS_INVALID, 0, "Invalid entry at batch index %" G_GSIZE_FORMAT, i);
        size_t length = g_bytes_get_size(batch->entries[i].name.raw);
        if (length > DS_QUEUE_BYTES - payload)
            return ds_error(error, DS_LIMIT, 0, "Entry batch exceeds the memory budget");
        payload += length;
    }

    int rc = ds_store_check_budget(store, true);
    if (rc == SQLITE_OK) rc = ds_sql_exec(store, "BEGIN IMMEDIATE");
    if (rc != SQLITE_OK) return ds_sql_status(store, rc, error);

    sqlite3_stmt *metadata = NULL, *entry = NULL, *update = NULL, *issue = NULL;
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
        " ON CONFLICT(id) DO NOTHING");
    int64_t added = 0;
    for (size_t i = 0; rc == SQLITE_OK && i < batch->count; ++i) {
        const DsEntry *observation = &batch->entries[i];
        bool inserted = false;
        if (observation->name.encoding != encoding) {
            rc = SQLITE_MISMATCH;
            break;
        }
        rc = insert_entry(store, entry, observation, seq + 1, &inserted, error);
        if (rc == SQLITE_OK && !inserted)
            rc = update_directory(store, &update, observation);
        if (rc == SQLITE_OK && observation->reason != DS_REASON_NONE) {
            if (!issue) rc = ds_sql_prepare(store, &issue,
                "INSERT OR IGNORE INTO issues VALUES(?1,'observation',?2,?3,unixepoch())");
            if (rc == SQLITE_OK) rc = record_issue(issue, observation);
        }
        added += inserted;
    }
    sqlite3_finalize(entry);
    sqlite3_finalize(update);
    sqlite3_finalize(issue);
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &metadata,
        "UPDATE scans SET commit_seq=?1,entry_count=entry_count+?2 WHERE id=1");
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(metadata, 1, seq + 1);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(metadata, 2, added);
    if (rc == SQLITE_OK) rc = ds_sql_step_done(metadata);
    sqlite3_finalize(metadata);
    if (rc == SQLITE_OK) rc = ds_store_check_budget(store, true);
    if (rc == SQLITE_OK) rc = ds_sql_exec(store, "COMMIT");
    if (rc != SQLITE_OK) {
        DsStatus status = ds_sql_status(store, rc, error);
        ds_sql_exec(store, "ROLLBACK");
        return status;
    }
    ds_store_checkpoint(store, false);
    return DS_OK;
}

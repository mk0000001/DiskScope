#include "diskscope/ds_query.h"
#include "diskscope/ds_snapshot.h"
#include <glib/gstdio.h>
#include <sqlite3.h>
#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef struct {
    char *directory, *filename;
    DsNativePath database;
    DsStore *writer;
} Fixture;

static DsEntry node(int64_t id, int64_t parent, DsKind kind, const void *name, size_t length)
{
    DsEntry e = {
        .id = id, .parent_id = parent, .kind = kind, .logical = {1, DS_KNOWN},
        .allocated = {4096, DS_KNOWN}, .links = 1, .identity_known = kind == DS_FILE,
    };
    ds_u64_encode((uint64_t)id, e.identity);
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, name, length, &e.name, NULL), ==, DS_OK);
    return e;
}

static DsStatus append(DsStore *store, DsEntry *entries, size_t count)
{
    DsEntryBatch batch = {entries, count};
    DsStatus status = ds_store_append(store, &batch, NULL);
    for (size_t i = 0; i < count; ++i) ds_path_clear(&entries[i].name);
    return status;
}

static void setup(Fixture *f, const void *managed)
{
    const uint8_t raw_root[] = {'/', 'm', 'n', 't', '/', 0xff};
    DsScanOptions options = {.cache_budget = DS_CACHE_BUDGET};
    f->directory = g_dir_make_tmp("ds-store-XXXXXX", NULL);
    g_assert_nonnull(f->directory);
    f->filename = g_build_filename(f->directory, "scan.db", NULL);
    g_assert_cmpint(ds_path_from_arg(f->filename, &f->database, NULL), ==, DS_OK);
    if (managed)
        g_assert_cmpint(ds_path_from_arg(f->directory, &options.cache_path, NULL), ==, DS_OK);
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, raw_root, sizeof raw_root, &options.root, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_open(&f->database, DS_STORE_WRITER, &f->writer, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_set_options(f->writer, &options, NULL), ==, DS_OK);
    ds_path_clear(&options.root);
    ds_path_clear(&options.cache_path);
    DsEntry root = node(1, 0, DS_DIRECTORY, "", 0);
    g_assert_cmpint(append(f->writer, &root, 1), ==, DS_OK);
}

static void teardown(Fixture *f, const void *unused)
{
    (void)unused;
    ds_store_close(f->writer);
    ds_path_clear(&f->database);
    g_assert_cmpint(g_remove(f->filename), ==, 0);
    g_assert_cmpint(g_rmdir(f->directory), ==, 0);
    g_free(f->filename);
    g_free(f->directory);
}

static DsStore *open_reader(Fixture *f)
{
    DsStore *out = NULL;
    g_assert_cmpint(ds_store_open(&f->database, DS_STORE_READER, &out, NULL), ==, DS_OK);
    return out;
}

static DsPage *page(DsStore *reader, const DsQuery *q)
{
    DsPage *out = NULL;
    g_assert_cmpint(ds_query_page(reader, q, &out, NULL, NULL), ==, DS_OK);
    return out;
}

static void raw_identity_and_unsigned_order(Fixture *f, const void *unused)
{
    (void)unused;
    const char first[] = "\xc3\xa9";
    const char second[] = "e\xcc\x81";
    const uint8_t raw_root[] = {'/', 'm', 'n', 't', '/', 0xff};
    DsEntry entries[] = {
        node(2, 1, DS_FILE, first, sizeof first - 1),
        node(3, 1, DS_FILE, second, sizeof second - 1),
        node(4, 1, DS_FILE, "?", 1),
    };
    entries[0].logical.bytes = UINT64_MAX;
    entries[1].logical.bytes = INT64_MAX;
    entries[2].logical.quality = DS_UNAVAILABLE;
    g_assert_cmpint(append(f->writer, entries, G_N_ELEMENTS(entries)), ==, DS_OK);
    DsStore *reader = open_reader(f);
    DsQuery q = {.parent_id = 1, .sort = DS_SORT_METRIC, .descending = true, .limit = 2};
    DsPage *result = page(reader, &q);
    g_assert_cmpuint(result->total, ==, 3);
    g_assert_cmpuint(result->count, ==, 2);
    g_assert_cmpint(result->rows[0].entry.id, ==, 2);
    g_assert_cmpint(result->rows[1].entry.id, ==, 3);
    size_t length = 0;
    const void *raw = g_bytes_get_data(result->info.root.raw, &length);
    g_assert_cmpmem(raw, length, raw_root, sizeof raw_root);
    DsNativePath relative = {0};
    g_assert_cmpint(ds_store_relative_path(reader, 3, &relative, NULL), ==, DS_OK);
    raw = g_bytes_get_data(relative.raw, &length);
    g_assert_cmpmem(raw, length, second, sizeof second - 1);
    ds_path_clear(&relative);
    ds_page_free(result);
    q.offset = 2;
    result = page(reader, &q);
    g_assert_cmpuint(result->count, ==, 1);
    g_assert_cmpint(result->rows[0].entry.id, ==, 4);
    ds_page_free(result);
    ds_store_close(reader);
}

static void rejected_batch_rolls_back(Fixture *f, const void *unused)
{
    (void)unused;
    DsEntry bad[] = {
        node(2, 1, DS_FILE, "would-have-committed", 20),
        node(9, 8, DS_FILE, "missing-parent", 14),
    };
    g_assert_cmpint(append(f->writer, bad, G_N_ELEMENTS(bad)), ==, DS_INVALID);
    DsStoreInfo info = {0};
    g_assert_cmpint(ds_store_info(f->writer, &info, NULL), ==, DS_OK);
    g_assert_cmpuint(info.entry_count, ==, 1);
    g_assert_cmpint(info.commit_seq, ==, 1);
    ds_store_info_clear(&info);
    DsEntry good = node(2, 1, DS_FILE, "valid-retry", 11);
    g_assert_cmpint(append(f->writer, &good, 1), ==, DS_OK);
}

static void directory_update_in_same_batch(Fixture *f, const void *unused)
{
    (void)unused;
    DsEntry observations[] = {
        node(2, 1, DS_DIRECTORY, "directory", 9),
        node(2, 1, DS_DIRECTORY, "directory", 9),
    };
    observations[0].reason = DS_REASON_PERMISSION;
    observations[0].os_code = 5;
    observations[1].reason = DS_REASON_IO;
    observations[1].os_code = 23;
    g_assert_cmpint(append(f->writer, observations, G_N_ELEMENTS(observations)), ==, DS_OK);

    DsQuery query = {.parent_id = 1};
    DsPage *result = page(f->writer, &query);
    g_assert_cmpuint(result->info.entry_count, ==, 2);
    g_assert_cmpint(result->info.commit_seq, ==, 2);
    g_assert_cmpuint(result->count, ==, 1);
    g_assert_cmpint(result->rows[0].entry.reason, ==, DS_REASON_IO);
    g_assert_cmpint(result->rows[0].entry.os_code, ==, 23);
    ds_page_free(result);

    sqlite3_stmt *issues = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(ds_store_db(f->writer),
        "SELECT reason,os_code FROM issues WHERE node_id=2 ORDER BY reason", -1, &issues, NULL),
        ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(issues), ==, SQLITE_ROW);
    g_assert_cmpint(sqlite3_column_int(issues, 0), ==, DS_REASON_PERMISSION);
    g_assert_cmpint(sqlite3_column_int(issues, 1), ==, 5);
    g_assert_cmpint(sqlite3_step(issues), ==, SQLITE_ROW);
    g_assert_cmpint(sqlite3_column_int(issues, 0), ==, DS_REASON_IO);
    g_assert_cmpint(sqlite3_column_int(issues, 1), ==, 23);
    g_assert_cmpint(sqlite3_step(issues), ==, SQLITE_DONE);
    sqlite3_finalize(issues);

    DsEntry renamed = node(2, 1, DS_DIRECTORY, "renamed", 7);
    g_assert_cmpint(append(f->writer, &renamed, 1), ==, DS_INVALID);
    g_assert_cmpint(ds_finalize(f->writer, NULL, NULL), ==, DS_OK);
}

static int reject_generation_switch(void *data, int op, const char *table, const char *column,
                                   const char *database, const char *context)
{
    (void)data;
    (void)database;
    (void)context;
    return op == SQLITE_UPDATE && g_strcmp0(table, "scans") == 0 &&
           g_strcmp0(column, "active_generation") == 0 ? SQLITE_DENY : SQLITE_OK;
}

static void generation_blocks_publish_atomically(Fixture *f, const void *unused)
{
    (void)unused;
    DsControl control;
    ds_control_init(&control);
    ds_control_cancel(&control);
    g_assert_cmpint(ds_finalize(f->writer, &control, NULL), ==, DS_CANCELED);
    ds_control_clear(&control);
    DsStore *reader = open_reader(f);
    DsQuery q = {.parent_id = 0, .limit = 1};
    DsPage *result = page(reader, &q);
    g_assert_cmpint(result->rows[0].metrics[DS_LOGICAL].quality, ==, DS_UNAVAILABLE);
    g_assert_cmpint(result->info.generation, ==, 0);
    ds_page_free(result);

    g_assert_cmpint(ds_finalize(f->writer, NULL, NULL), ==, DS_OK);
    result = page(reader, &q);
    int64_t generation = result->info.generation;
    g_assert_cmpint(generation, >, 0);
    g_assert_cmpint(result->rows[0].metrics[DS_LOGICAL].quality, ==, DS_KNOWN);
    ds_page_free(result);
    DsEntry file = node(2, 1, DS_FILE, "newer", 5);
    g_assert_cmpint(append(f->writer, &file, 1), ==, DS_OK);
    sqlite3_set_authorizer(ds_store_db(f->writer), reject_generation_switch, NULL);
    g_assert_cmpint(ds_finalize(f->writer, NULL, NULL), !=, DS_OK);
    sqlite3_set_authorizer(ds_store_db(f->writer), NULL, NULL);
    result = page(reader, &q);
    g_assert_cmpint(result->info.generation, ==, generation);
    g_assert_cmpint(result->info.as_of_commit_seq, ==, 1);
    g_assert_cmpuint(result->info.unaggregated_count, ==, 1);
    g_assert_cmpuint(result->rows[0].metrics[DS_LOGICAL].bytes, ==, 0);
    ds_page_free(result);
    g_assert_cmpint(ds_store_set_result(f->writer, DS_OK, DS_COVERAGE_COMPLETE, NULL), ==, DS_OK);
    g_assert_cmpint(ds_finalize(f->writer, NULL, NULL), ==, DS_OK);
    result = page(reader, &q);
    g_assert_cmpint(result->info.generation, >, generation);
    g_assert_cmpuint(result->info.unaggregated_count, ==, 0);
    g_assert_cmpuint(result->rows[0].metrics[DS_LOGICAL].bytes, ==, 1);
    g_assert_cmpint(result->info.aggregate_coverage, ==, DS_COVERAGE_COMPLETE);
    ds_page_free(result);
    ds_store_close(reader);
}

static void full_preserves_observations(Fixture *f, const void *unused)
{
    (void)unused;
    sqlite3 *db = ds_store_db(f->writer);
    sqlite3_stmt *count = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA page_count", -1, &count, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(count), ==, SQLITE_ROW);
    int pages = sqlite3_column_int(count, 0);
    sqlite3_finalize(count);
    char *sql = sqlite3_mprintf("PRAGMA max_page_count=%d", pages);
    g_assert_cmpint(sqlite3_exec(db, sql, NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_free(sql);
    char *name = g_strnfill(DS_PATH_MAX, 'x');
    DsEntry file = node(2, 1, DS_FILE, name, DS_PATH_MAX);
    g_free(name);
    g_assert_cmpint(append(f->writer, &file, 1), ==, DS_LIMIT);
    DsStoreInfo info = {0};
    g_assert_cmpint(ds_store_info(f->writer, &info, NULL), ==, DS_OK);
    g_assert_cmpuint(info.entry_count, ==, 1);
    g_assert_cmpint(info.commit_seq, ==, 1);
    ds_store_info_clear(&info);
}

static void held_reader_observes_own_commit(Fixture *f, const void *unused)
{
    (void)unused;
    DsStore *reader = open_reader(f);
    sqlite3 *db = ds_store_db(reader);
    g_assert_cmpint(sqlite3_exec(db, "BEGIN; SELECT count(*) FROM entries", NULL, NULL, NULL), ==, SQLITE_OK);
    DsEntry file = node(2, 1, DS_FILE, "new", 3);
    g_assert_cmpint(append(f->writer, &file, 1), ==, DS_OK);
    DsQuery q = {.parent_id = DS_QUERY_ALL};
    DsPage *result = page(reader, &q);
    g_assert_cmpuint(result->count, ==, 1);
    ds_page_free(result);
    g_assert_cmpint(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL), ==, SQLITE_OK);
    result = page(reader, &q);
    g_assert_cmpuint(result->count, ==, 2);
    ds_page_free(result);
    ds_store_close(reader);
}

#ifdef G_OS_WIN32
static void append_wal_test_batch(Fixture *f, int64_t *id, int *frames, int *checkpointed)
{
    DsEntry entries[DS_BATCH_MAX];
    for (size_t i = 0; i < G_N_ELEMENTS(entries); ++i) {
        char name[1024];
        int length = g_snprintf(name, sizeof name, "%010" G_GINT64_FORMAT "%0960d", ++*id, 0);
        entries[i] = node(*id, 1, DS_FILE, name, (size_t)length);
    }
    g_assert_cmpint(append(f->writer, entries, G_N_ELEMENTS(entries)), ==, DS_OK);
    g_assert_cmpint(sqlite3_wal_checkpoint_v2(ds_store_db(f->writer), "main", SQLITE_CHECKPOINT_NOOP,
                                            frames, checkpointed), ==, SQLITE_OK);
}

static void managed_wal_defers_backfill(Fixture *f, const void *unused)
{
    (void)unused;
    int frames = 0, checkpointed = 0;
    int64_t id = 1;
    bool saw_sync_interval = false, saw_recycle = false;
    for (unsigned batch = 0; batch < 20 && !saw_recycle; ++batch) {
        append_wal_test_batch(f, &id, &frames, &checkpointed);
        if (!saw_sync_interval && frames >= 512 && frames < 4096) {
            g_assert_cmpint(frames, >, checkpointed);
            saw_sync_interval = true;
        } else if (saw_sync_interval && frames == 0) {
            saw_recycle = true;
        }
    }
    g_assert_true(saw_sync_interval);
    g_assert_true(saw_recycle);
    DsStore *reader = open_reader(f);
    DsStoreInfo info = {0};
    g_assert_cmpint(ds_store_info(reader, &info, NULL), ==, DS_OK);
    g_assert_cmpuint(info.entry_count, ==, (uint64_t)id);
    ds_store_info_clear(&info);
    ds_store_close(reader);
}

static HANDLE watched_wal;
static unsigned wal_sync_calls;

static BOOL WINAPI record_wal_sync(HANDLE handle)
{
    if (handle == watched_wal) ++wal_sync_calls;
    return FlushFileBuffers(handle);
}

static void managed_wal_syncs_once_for_partial_backfill(Fixture *f, const void *unused)
{
    (void)unused;
    int frames = 0, checkpointed = 0;
    int64_t id = 1;
    append_wal_test_batch(f, &id, &frames, &checkpointed);
    g_assert_cmpint(frames, >, checkpointed);
    g_assert_cmpint(frames, <, 4096);

    DsStore *reader = open_reader(f);
    sqlite3 *db = ds_store_db(f->writer);
    g_assert_cmpint(sqlite3_exec(ds_store_db(reader), "BEGIN; SELECT count(*) FROM entries",
                                NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_file *wal = NULL;
    g_assert_cmpint(sqlite3_file_control(db, "main", SQLITE_FCNTL_JOURNAL_POINTER, &wal),
                    ==, SQLITE_OK);
    g_assert_nonnull(wal);
    g_assert_cmpint(wal->pMethods->xFileControl(wal, SQLITE_FCNTL_WIN32_GET_HANDLE, &watched_wal),
                    ==, SQLITE_OK);
    sqlite3_vfs *vfs = sqlite3_vfs_find("win32-longpath");
    g_assert_nonnull(vfs);
    sqlite3_syscall_ptr original = vfs->xGetSystemCall(vfs, "FlushFileBuffers");
    g_assert_cmpint(vfs->xSetSystemCall(vfs, "FlushFileBuffers", (sqlite3_syscall_ptr)record_wal_sync),
                    ==, SQLITE_OK);
    bool advanced = false;
    unsigned flushes_for_backfill = 0;
    for (unsigned batch = 0; batch < 20 && !advanced; ++batch) {
        int prior_checkpoint = checkpointed;
        wal_sync_calls = 0;
        append_wal_test_batch(f, &id, &frames, &checkpointed);
        advanced = checkpointed > prior_checkpoint;
        flushes_for_backfill = wal_sync_calls;
    }
    g_assert_cmpint(vfs->xSetSystemCall(vfs, "FlushFileBuffers", original), ==, SQLITE_OK);
    g_assert_true(advanced);
    g_assert_cmpint(frames, >, checkpointed);
    g_assert_cmpuint(flushes_for_backfill, ==, 1);

    DsStore *latest = open_reader(f);
    DsStoreInfo info = {0};
    g_assert_cmpint(ds_store_info(latest, &info, NULL), ==, DS_OK);
    g_assert_cmpuint(info.entry_count, ==, (uint64_t)id);
    ds_store_info_clear(&info);
    ds_store_close(latest);
    ds_store_close(reader);
}

static void managed_wal_syncs_when_pinned(Fixture *f, const void *unused)
{
    (void)unused;
    DsStore *reader = open_reader(f);
    g_assert_cmpint(sqlite3_exec(ds_store_db(reader), "BEGIN; SELECT count(*) FROM entries",
                                NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_file *wal = NULL;
    sqlite3 *db = ds_store_db(f->writer);
    g_assert_cmpint(sqlite3_file_control(db, "main", SQLITE_FCNTL_JOURNAL_POINTER, &wal), ==, SQLITE_OK);
    g_assert_nonnull(wal);
    g_assert_cmpint(wal->pMethods->xFileControl(wal, SQLITE_FCNTL_WIN32_GET_HANDLE, &watched_wal),
                    ==, SQLITE_OK);
    sqlite3_vfs *vfs = sqlite3_vfs_find("win32-longpath");
    g_assert_nonnull(vfs);
    sqlite3_syscall_ptr original = vfs->xGetSystemCall(vfs, "FlushFileBuffers");
    g_assert_cmpint(vfs->xSetSystemCall(vfs, "FlushFileBuffers", (sqlite3_syscall_ptr)record_wal_sync),
                    ==, SQLITE_OK);
    int frames = 0, checkpointed = 0, pinned_frames = 0, pinned_checkpoint = 0;
    unsigned syncs_at_limit = 0;
    int64_t id = 1;
    for (unsigned batch = 0; batch < 20; ++batch) {
        append_wal_test_batch(f, &id, &frames, &checkpointed);
        if (!pinned_frames && frames >= 4096) {
            pinned_frames = frames;
            pinned_checkpoint = checkpointed;
            syncs_at_limit = wal_sync_calls;
        } else if (pinned_frames && frames >= pinned_frames + 512) {
            break;
        }
    }
    g_assert_cmpint(vfs->xSetSystemCall(vfs, "FlushFileBuffers", original), ==, SQLITE_OK);
    g_assert_cmpint(pinned_frames, >=, 4096);
    g_assert_cmpint(frames, >=, pinned_frames + 512);
    g_assert_cmpint(checkpointed, ==, pinned_checkpoint);
    g_assert_cmpuint(wal_sync_calls, >, syncs_at_limit);

    g_autofree char *filename = g_build_filename(f->directory, "partial.dscan", NULL);
    DsNativePath destination = {0};
    g_assert_cmpint(ds_path_from_arg(filename, &destination, NULL), ==, DS_OK);
    DsStore *latest = open_reader(f);
    g_assert_cmpint(ds_snapshot_save(latest, &destination, NULL, NULL), ==, DS_OK);
    ds_store_close(latest);
    DsStore *snapshot = NULL;
    g_assert_cmpint(ds_snapshot_open(&destination, &snapshot, NULL, NULL), ==, DS_OK);
    DsStoreInfo info = {0};
    g_assert_cmpint(ds_store_info(snapshot, &info, NULL), ==, DS_OK);
    g_assert_cmpuint(info.entry_count, ==, (uint64_t)id);
    g_assert_cmpint(info.coverage, ==, DS_COVERAGE_PARTIAL);
    ds_store_info_clear(&info);
    ds_store_close(snapshot);
    ds_path_clear(&destination);
    g_assert_cmpint(g_remove(filename), ==, 0);
    ds_store_close(reader);
}
#endif

static void pending_rolls_over_memory_limit(Fixture *f, const void *unused)
{
    (void)unused;
    static const size_t queued = DS_PENDING_MAX + 1;
    for (size_t start = 0; start < queued;) {
        DsEntry entries[DS_BATCH_MAX];
        size_t n = MIN(queued - start, DS_BATCH_MAX);
        for (size_t i = 0; i < n; ++i) {
            char name[32];
            int64_t id = (int64_t)(start + i + 2);
            int length = g_snprintf(name, sizeof name, "dir-%" G_GINT64_FORMAT, (gint64)id);
            entries[i] = node(id, 1, DS_DIRECTORY, name, (size_t)length);
        }
        g_assert_cmpint(append(f->writer, entries, n), ==, DS_OK);
        start += n;
    }
    for (size_t i = 0; i < queued; ++i)
        g_assert_cmpint(ds_store_pending_push(f->writer, (int64_t)i + 2, NULL), ==, DS_OK);
    for (size_t i = 0; i < queued; ++i) {
        int64_t id = 0;
        g_assert_cmpint(ds_store_pending_pop(f->writer, &id, NULL), ==, DS_OK);
        g_assert_cmpint(id, ==, (int64_t)i + 2);
    }
    int64_t empty = -1;
    g_assert_cmpint(ds_store_pending_pop(f->writer, &empty, NULL), ==, DS_OK);
    g_assert_cmpint(empty, ==, 0);
}

static void filtered_cursor(Fixture *f, const void *unused)
{
    (void)unused;
    DsEntry entries[] = {
        node(2, 1, DS_DIRECTORY, "Directory", 9),
        node(3, 2, DS_FILE, "first.BIN", 9),
        node(4, 2, DS_FILE, "next.bin", 8),
        node(5, 2, DS_FILE, "small.bin", 9),
        node(6, 2, DS_FILE, "last.log", 8),
        node(7, 1, DS_FILE, "outside.bin", 11),
    };
    for (size_t i = 1; i < G_N_ELEMENTS(entries); ++i) {
        entries[i].logical.bytes = UINT64_MAX;
        entries[i].mtime_known = true;
        entries[i].mtime = -1;
    }
    entries[3].logical.bytes = 0;
    g_assert_cmpint(append(f->writer, entries, G_N_ELEMENTS(entries)), ==, DS_OK);
    DsQuery q = {
        .parent_id = DS_QUERY_ALL, .kind = DS_FILE, .filter = "directory/", .search_path = true,
        .extension = "bin", .limit = 1, .has_min_bytes = true, .min_bytes = UINT64_MAX,
        .has_mtime_from = true, .mtime_from = -1, .has_mtime_to = true, .mtime_to = -1,
    };
    DsPage *result = page(f->writer, &q);
    g_assert_cmpuint(result->total, ==, 2);
    g_assert_cmpuint(result->count, ==, 1);
    g_assert_cmpint(result->rows[0].entry.id, ==, 3);
    q.after_id = result->rows[0].entry.id;
    ds_page_free(result);
    q.omit_total = true;
    result = page(f->writer, &q);
    g_assert_cmpuint(result->total, ==, 0);
    g_assert_cmpuint(result->count, ==, 1);
    g_assert_cmpint(result->rows[0].entry.id, ==, 4);
    ds_page_free(result);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/store/raw-identity", Fixture, NULL, setup, raw_identity_and_unsigned_order, teardown);
    g_test_add("/store/reject-batch", Fixture, NULL, setup, rejected_batch_rolls_back, teardown);
    g_test_add("/store/directory-update-in-batch", Fixture, NULL, setup,
               directory_update_in_same_batch, teardown);
    g_test_add("/store/publish-generation", Fixture, NULL, setup, generation_blocks_publish_atomically, teardown);
    g_test_add("/store/full", Fixture, NULL, setup, full_preserves_observations, teardown);
    g_test_add("/store/held-reader", Fixture, NULL, setup, held_reader_observes_own_commit, teardown);
#ifdef G_OS_WIN32
    g_test_add("/store/managed-wal-defers-backfill", Fixture, GINT_TO_POINTER(1),
               setup, managed_wal_defers_backfill, teardown);
    g_test_add("/store/managed-wal-syncs-when-pinned", Fixture, GINT_TO_POINTER(1),
               setup, managed_wal_syncs_when_pinned, teardown);
    g_test_add("/store/managed-wal-syncs-once-for-partial-backfill", Fixture, GINT_TO_POINTER(1),
               setup, managed_wal_syncs_once_for_partial_backfill, teardown);
#endif
    g_test_add("/store/pending-spill", Fixture, NULL, setup, pending_rolls_over_memory_limit, teardown);
    g_test_add("/store/filtered-cursor", Fixture, NULL, setup, filtered_cursor, teardown);
    return g_test_run();
}

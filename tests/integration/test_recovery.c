#include "diskscope/ds_recovery.h"
#include "cache_lease.h"
#include "local.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>

static char *test_program;

typedef struct {
    char *base;
    DsNativePath cache;
} Fixture;

static void fixture_open(Fixture *fixture, gconstpointer data)
{
    (void)data;
    fixture->base = g_dir_make_tmp("diskscope-recovery-XXXXXX", NULL);
    g_assert_nonnull(fixture->base);
    g_assert_cmpint(ds_path_from_arg(fixture->base, &fixture->cache, NULL), ==, DS_OK);
}

static void fixture_close(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GDir *children = g_dir_open(fixture->base, 0, NULL);
    const char *name;
    const char *entries[] = {"scan.sqlite", "scan.sqlite-wal", "scan.sqlite-shm", DS_CACHE_LEASE_FILE};
    while ((name = g_dir_read_name(children))) {
        char *folder = g_build_filename(fixture->base, name, NULL);
        for (size_t i = 0; i < G_N_ELEMENTS(entries); ++i) {
            char *path = g_build_filename(folder, entries[i], NULL);
            if (g_file_test(path, G_FILE_TEST_EXISTS))
                g_assert_cmpint(g_unlink(path), ==, 0);
            g_free(path);
        }
        g_assert_cmpint(g_rmdir(folder), ==, 0);
        g_free(folder);
    }
    g_dir_close(children);
    g_assert_cmpint(g_rmdir(fixture->base), ==, 0);
    ds_path_clear(&fixture->cache);
    g_free(fixture->base);
}

static char *scan(Fixture *fixture, const char *uuid, bool marker, bool finished, int start)
{
    char *name = g_strconcat("scan-", uuid, NULL);
    char *folder = g_build_filename(fixture->base, name, NULL);
    g_free(name);
    if (marker) {
        DsError error = {0};
        char *private_dir = ds_private_cache(&fixture->cache, &error);
        if (!private_dir) g_error("cache directory could not be created: %s", error.message);
        g_assert_cmpint(g_rename(private_dir, folder), ==, 0);
        g_free(private_dir);
        DsCacheLease *lease = ds_cache_lease_open(folder, true, false, &error);
        if (!lease) g_error("cache lease could not be created: %s", error.message);
        ds_cache_lease_close(lease);
    } else {
        g_assert_cmpint(g_mkdir(folder, 0700), ==, 0);
    }
    char *cache_db = g_build_filename(folder, "scan.sqlite", NULL);
    char *removed_source = g_build_filename(fixture->base, "source-was-removed", NULL);
    DsNativePath database = {0};
    DsScanOptions options;
    ds_scan_options_init(&options, DS_CONSERVATIVE);
    g_assert_cmpint(ds_path_from_arg(removed_source, &options.root, NULL), ==, DS_OK);
    g_assert_cmpint(ds_path_from_arg(cache_db, &database, NULL), ==, DS_OK);
    g_assert_cmpint(ds_path_from_arg(folder, &options.cache_path, NULL), ==, DS_OK);
    DsStore *store = NULL;
    g_assert_cmpint(ds_store_open(&database, DS_STORE_WRITER, &store, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_set_options(store, &options, NULL), ==, DS_OK);
    DsEntry root = {.id=1, .kind=DS_DIRECTORY};
    g_assert_cmpint(ds_path_new(options.root.encoding, NULL, 0, &root.name, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_append(store, &(DsEntryBatch){&root, 1}, NULL), ==, DS_OK);
    char *sql = sqlite3_mprintf("UPDATE scans SET started_at=%d", start);
    g_assert_cmpint(sqlite3_exec(ds_store_db(store), sql, NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_free(sql);
    if (finished)
        g_assert_cmpint(ds_finalize_result(store, DS_OK, DS_COVERAGE_COMPLETE, NULL, NULL), ==, DS_OK);
    ds_store_close(store);
    ds_path_clear(&root.name);
    ds_path_clear(&database);
    ds_path_clear(&options.root);
    ds_path_clear(&options.cache_path);
    g_free(cache_db);
    g_free(removed_source);
    return folder;
}

static void idle_recovery(Fixture *fixture, gconstpointer data)
{
    (void)data;
    char *previous = scan(fixture, "00000000-0000-4000-8000-000000000001", true, true, 100);
    char *active = scan(fixture, "00000000-0000-4000-8000-000000000002", true, false, 200);
    char *legacy = scan(fixture, "00000000-0000-4000-8000-000000000003", false, true, 300);
    DsCacheLease *controller = ds_cache_lease_open(active, false, false, NULL);
    g_assert_nonnull(controller);
    GError *spawn_error = NULL;
    GSubprocess *worker = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
        &spawn_error, test_program, "--hold-cache-lease", active, NULL);
    g_assert_no_error(spawn_error);
    g_assert_nonnull(worker);
    char ready = 0;
    g_assert_cmpint(g_input_stream_read(g_subprocess_get_stdout_pipe(worker), &ready,
                                       1, NULL, &spawn_error), ==, 1);
    g_assert_no_error(spawn_error);
    g_assert_cmpint(ready, ==, 'Y');
    ds_cache_lease_close(controller);

    DsRecoveredScan *result = NULL;
    g_assert_cmpint(ds_recovery_latest(&fixture->cache, NULL, &result, NULL), ==, DS_OK);
    g_assert_nonnull(result);
    const DsStoreInfo *info = ds_recovered_info(result);
    g_assert_cmpint(info->started_at, ==, 100);
    g_assert_cmpint(info->run_status, ==, DS_OK);
    g_assert_cmpint(info->coverage, ==, DS_COVERAGE_COMPLETE);
    g_assert_null(ds_cache_lease_open(previous, false, false, NULL));
    ds_recovered_free(result);

    g_assert_true(g_output_stream_close(g_subprocess_get_stdin_pipe(worker), NULL, &spawn_error));
    g_assert_no_error(spawn_error);
    g_assert_true(g_subprocess_wait_check(worker, NULL, &spawn_error));
    g_assert_no_error(spawn_error);
    g_object_unref(worker);
    g_assert_cmpint(ds_recovery_latest(&fixture->cache, NULL, &result, NULL), ==, DS_OK);
    g_assert_nonnull(result);
    info = ds_recovered_info(result);
    g_assert_cmpint(info->started_at, ==, 200);
    g_assert_cmpint(info->run_status, ==, DS_IO);
    g_assert_cmpint(info->coverage, ==, DS_COVERAGE_PARTIAL);
    g_assert_cmpuint(info->entry_count, ==, 1);
    g_assert_false(info->aggregation_complete);
    DsStore *reader = NULL;
    g_assert_cmpint(ds_store_open(ds_recovered_path(result), DS_STORE_READER, &reader, NULL), ==, DS_OK);
    DsStoreInfo saved = {0};
    g_assert_cmpint(ds_store_info(reader, &saved, NULL), ==, DS_OK);
    g_assert_cmpint(saved.run_status, ==, DS_IO);
    ds_store_info_clear(&saved);
    ds_store_close(reader);
    ds_recovered_free(result);
    g_free(previous);
    g_free(active);
    g_free(legacy);
}

static int append_and_exit(const char *folder)
{
    DsCacheLease *lease = ds_cache_lease_open(folder, false, false, NULL);
    g_assert_nonnull(lease);
    char *name = g_build_filename(folder, "scan.sqlite", NULL);
    DsNativePath path = {0};
    g_assert_cmpint(ds_path_from_arg(name, &path, NULL), ==, DS_OK);
    DsStore *db = NULL;
    DsError error = {0};
    if (ds_store_open(&path, DS_STORE_WRITER, &db, &error) != DS_OK)
        g_error("WAL fixture writer could not open: %s (%" G_GINT64_FORMAT ")", error.message,
                error.os_code);
    DsEntry entry = {.id = 2, .parent_id = 1, .kind = DS_FILE, .logical = {7, DS_KNOWN},
                     .allocated = {0, DS_UNAVAILABLE}, .links = 1};
    g_assert_cmpint(ds_path_from_arg("committed-before-exit", &entry.name, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_append(db, &(DsEntryBatch){&entry, 1}, NULL), ==, DS_OK);
    int frames = -1, checkpointed = -1;
    g_assert_cmpint(sqlite3_wal_checkpoint_v2(ds_store_db(db), "main", SQLITE_CHECKPOINT_NOOP,
                                            &frames, &checkpointed), ==, SQLITE_OK);
    g_assert_cmpint(frames, >, checkpointed);
    /* Keep the SQLite connection and OS lease open until the process exits. */
    _Exit(0);
}

static void recover_committed_wal(Fixture *fixture, gconstpointer data)
{
    (void)data;
    char *folder = scan(fixture, "00000000-0000-4000-8000-000000000004", true, false, 100);
    char *name = g_build_filename(folder, "scan.sqlite", NULL);
    GError *error = NULL;
    GSubprocess *worker = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
        test_program, "--append-and-exit", folder, NULL);
    g_assert_no_error(error);
    g_assert_nonnull(worker);
    g_assert_true(g_subprocess_wait_check(worker, NULL, &error));
    g_assert_no_error(error);
    g_object_unref(worker);
    char *wal = g_strconcat(name, "-wal", NULL);
    GStatBuf stat_buffer;
    g_assert_cmpint(g_stat(wal, &stat_buffer), ==, 0);
    g_assert_true(stat_buffer.st_size > 32);

    DsRecoveredScan *result = NULL;
    g_assert_cmpint(ds_recovery_latest(&fixture->cache, NULL, &result, NULL), ==, DS_OK);
    g_assert_nonnull(result);
    g_assert_cmpuint(ds_recovered_info(result)->entry_count, ==, 2);
    g_assert_cmpint(ds_recovered_info(result)->coverage, ==, DS_COVERAGE_PARTIAL);
    g_assert_cmpint(ds_recovered_info(result)->commit_seq, ==, 2);
    ds_recovered_free(result);
    g_free(wal);
    g_free(name);
    g_free(folder);
}

static void cancel_and_empty(Fixture *fixture, gconstpointer data)
{
    (void)data;
    DsRecoveredScan *result = NULL;
    g_assert_cmpint(ds_recovery_latest(&fixture->cache, NULL, &result, NULL), ==, DS_OK);
    g_assert_null(result);
    DsControl control;
    ds_control_init(&control);
    ds_control_cancel(&control);
    g_assert_cmpint(ds_recovery_latest(&fixture->cache, &control, &result, NULL), ==, DS_CANCELED);
    g_assert_null(result);
    ds_control_clear(&control);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--hold-cache-lease")) {
        DsCacheLease *lease = ds_cache_lease_open(argv[2], false, false, NULL);
        if (!lease)
            return 1;
        fputc('Y', stdout);
        fflush(stdout);
        (void)getchar();
        ds_cache_lease_close(lease);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--append-and-exit"))
        return append_and_exit(argv[2]);
    test_program = g_canonicalize_filename(argv[0], NULL);
    g_test_init(&argc, &argv, NULL);
    g_test_add("/recovery/active-skipped-and-partial-recovered", Fixture, NULL,
               fixture_open, idle_recovery, fixture_close);
    g_test_add("/recovery/cancel-and-empty", Fixture, NULL,
               fixture_open, cancel_and_empty, fixture_close);
    g_test_add("/recovery/committed-wal-after-process-exit", Fixture, NULL,
               fixture_open, recover_committed_wal, fixture_close);
    int status = g_test_run();
    g_free(test_program);
    return status;
}

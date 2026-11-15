#include "snapshot_fixture.h"
#include "diskscope/ds_query.h"
#include <sqlite3.h>

static void roundtrip_and_reject_bad_rows(void)
{
    char *directory = g_dir_make_tmp("ds-snapshot-XXXXXX", NULL);
    char *active = g_build_filename(directory, "active.db", NULL);
    char *snapshot = g_build_filename(directory, "snapshot.dscan", NULL);
    DsNativePath database = {0}, destination = {0};
    native_argument(active, &database);
    native_argument(snapshot, &destination);
    DsStore *writer = scan_fixture(&database, "file", DS_REASON_NONE, DS_COVERAGE_COMPLETE);
    g_assert_cmpint(ds_snapshot_save(writer, &destination, NULL, NULL), ==, DS_OK);
    DsStore *opened = NULL;
    g_assert_cmpint(ds_snapshot_open(&destination, &opened, NULL, NULL), ==, DS_OK);
    DsStoreInfo info = {0};
    g_assert_cmpint(ds_store_info(opened, &info, NULL), ==, DS_OK);
    g_assert_cmpuint(info.entry_count, ==, 2);
    ds_store_info_clear(&info);
    ds_store_close(opened);
    opened = NULL;

    char *journal = g_strconcat(snapshot, "-wal", NULL);
    g_assert_true(g_file_set_contents(journal, "existing sidecar", -1, NULL));
    g_assert_cmpint(ds_snapshot_save(writer, &destination, NULL, NULL), ==, DS_INVALID);
    g_assert_cmpint(g_remove(journal), ==, 0);
    g_free(journal);
    g_assert_cmpint(ds_snapshot_open(&destination, &opened, NULL, NULL), ==, DS_OK);
    ds_store_close(opened);
    opened = NULL;

    sqlite3 *raw = NULL;
    g_assert_cmpint(sqlite3_open(snapshot, &raw), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(raw, "UPDATE entries SET logical=x'00' WHERE id=2",
                                NULL, NULL, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_close(raw), ==, SQLITE_OK);
    g_assert_cmpint(ds_snapshot_open(&destination, &opened, NULL, NULL), ==, DS_INVALID);
    g_assert_null(opened);
    const char *bad[] = {
        "UPDATE entries SET name=x'00' WHERE id=2",
        "PRAGMA ignore_check_constraints=1; UPDATE entries SET parent_id=2 WHERE id=2",
        "PRAGMA user_version=999",
        "CREATE TRIGGER do_not_allow AFTER INSERT ON scans BEGIN SELECT 1; END",
        "INSERT INTO volume_info VALUES(1,1,x'00',0,NULL,1,NULL,1)",
        "INSERT INTO volume_info VALUES(1,1,NULL,4294967297,NULL,1,NULL,1)",
        "INSERT INTO volume_info VALUES(1,1,x'0000000000000001',0,x'0000000000000002',0,NULL,1)",
        "INSERT INTO volume_info VALUES(1,1,x'0000000000000001',1,NULL,1,NULL,1)",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad); ++i) {
        g_assert_cmpint(ds_snapshot_save(writer, &destination, NULL, NULL), ==, DS_OK);
        g_assert_cmpint(sqlite3_open(snapshot, &raw), ==, SQLITE_OK);
        g_assert_cmpint(sqlite3_exec(raw, bad[i], NULL, NULL, NULL), ==, SQLITE_OK);
        g_assert_cmpint(sqlite3_close(raw), ==, SQLITE_OK);
        g_assert_cmpint(ds_snapshot_open(&destination, &opened, NULL, NULL), ==, DS_INVALID);
        g_assert_null(opened);
    }
    g_assert_cmpint(ds_snapshot_save(writer, &destination, NULL, NULL), ==, DS_OK);
    g_assert_cmpint(sqlite3_open(snapshot, &raw), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(raw, "DROP TABLE volume_info; PRAGMA user_version=1",
                                NULL, NULL, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_close(raw), ==, SQLITE_OK);
    g_assert_cmpint(ds_snapshot_open(&destination, &opened, NULL, NULL), ==, DS_OK);
    DsVolumeInfo volume;
    g_assert_cmpint(ds_store_volume(opened, &volume, NULL), ==, DS_OK);
    g_assert_cmpint(volume.captured_at, ==, 0);
    g_assert_cmpint(volume.total.quality, ==, DS_UNAVAILABLE);
    /* Match the GUI import path that backs up an opened snapshot before use. */
    g_autofree char *copy = g_build_filename(directory, "legacy-copy.dscan", NULL);
    DsNativePath copied_path = {0};
    native_argument(copy, &copied_path);
    g_assert_cmpint(ds_snapshot_save(opened, &copied_path, NULL, NULL), ==, DS_OK);
    ds_store_close(opened);
    opened = NULL;
    g_assert_cmpint(ds_snapshot_open(&copied_path, &opened, NULL, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_volume(opened, &volume, NULL), ==, DS_OK);
    g_assert_cmpint(volume.captured_at, ==, 0);
    ds_store_close(opened);
    opened = NULL;
    ds_path_clear(&copied_path);
    g_assert_cmpint(g_remove(copy), ==, 0);
    g_assert_true(g_file_set_contents(snapshot, "SQLite format 3", -1, NULL));
    g_assert_cmpint(ds_snapshot_open(&destination, &opened, NULL, NULL), ==, DS_INVALID);
    g_assert_null(opened);
    ds_store_close(writer);
    ds_path_clear(&database);
    ds_path_clear(&destination);
    g_remove(snapshot);
    g_remove(active);
    g_rmdir(directory);
    g_free(snapshot);
    g_free(active);
    g_free(directory);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/snapshot/roundtrip-and-validation", roundtrip_and_reject_bad_rows);
    return g_test_run();
}

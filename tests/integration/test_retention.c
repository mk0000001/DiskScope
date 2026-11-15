#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "diskscope/ds_retention.h"
#include "cache_lease.h"
#include "local.h"
#include "snapshot_fixture.h"
#include <fcntl.h>
#include <sqlite3.h>
#ifdef G_OS_WIN32
#include "../platform/windows_fixture.h"
#else
#include <unistd.h>
#endif

typedef struct {
    char *base;
    DsNativePath cache;
    GPtrArray *folders;
} Fixture;

static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data;
    fixture->base = g_dir_make_tmp("diskscope-retention-XXXXXX", NULL);
    g_assert_nonnull(fixture->base);
    native_argument(fixture->base, &fixture->cache);
    fixture->folders = g_ptr_array_new_with_free_func(g_free);
}

static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const char *files[] = {DS_CACHE_OWNER_FILE, DS_CACHE_LEASE_FILE, "scan.sqlite",
        "scan.sqlite-wal", "scan.sqlite-shm", "import.dscan", "diagnostics.txt", "personal.csv"};
    for (guint i = 0; i < fixture->folders->len; ++i) {
        const char *folder = g_ptr_array_index(fixture->folders, i);
        for (size_t j = 0; j < G_N_ELEMENTS(files); ++j) {
            g_autofree char *path = g_build_filename(folder, files[j], NULL);
            g_unlink(path);
        }
        if (g_file_test(folder, G_FILE_TEST_IS_DIR))
            g_assert_cmpint(g_rmdir(folder), ==, 0);
    }
    g_autofree char *source = g_build_filename(fixture->base, "saved.dscan", NULL);
    if (g_file_test(source, G_FILE_TEST_EXISTS))
        g_assert_cmpint(g_unlink(source), ==, 0);
    g_ptr_array_unref(fixture->folders);
    ds_path_clear(&fixture->cache);
    g_assert_cmpint(g_rmdir(fixture->base), ==, 0);
    g_free(fixture->base);
}

static void set_age(const char *path, int64_t seconds)
{
#ifdef G_OS_WIN32
    g_autofree WCHAR *wide = (WCHAR *)g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    HANDLE file = CreateFileW(wide, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
    g_assert_true(file != INVALID_HANDLE_VALUE);
    ULARGE_INTEGER ticks = {.QuadPart = (seconds + UINT64_C(11644473600)) * UINT64_C(10000000)};
    FILETIME time = {ticks.LowPart, ticks.HighPart};
    g_assert_true(SetFileTime(file, &time, NULL, &time));
    g_assert_true(CloseHandle(file));
#else
    struct timespec times[] = {{seconds, 0}, {seconds, 0}};
    g_assert_cmpint(utimensat(AT_FDCWD, path, times, 0), ==, 0);
#endif
}

static void write_file(const char *folder, const char *name)
{
    g_autofree char *path = g_build_filename(folder, name, NULL);
    g_assert_true(g_file_set_contents(path, "test", 4, NULL));
}

static void mark(const char *folder, bool scan, int days_old)
{
    g_assert_true(ds_cache_mark_owned(folder, scan));
    g_autofree char *marker = g_build_filename(folder, DS_CACHE_OWNER_FILE, NULL);
    set_age(marker, g_get_real_time() / G_USEC_PER_SEC - days_old * INT64_C(86400));
}

static char *cache(Fixture *fixture, bool import, int days_old, DsCacheLease **held)
{
    char *folder = ds_private_cache(&fixture->cache, NULL);
    g_assert_nonnull(folder);
    g_ptr_array_add(fixture->folders, folder);
    DsCacheLease *lease = ds_cache_lease_open(folder, true, false, NULL);
    g_assert_nonnull(lease);
    if (import) {
        write_file(folder, "import.dscan");
        mark(folder, false, days_old);
    }
    if (held)
        *held = lease;
    else
        ds_cache_lease_close(lease);
    return folder;
}

static bool exists(const char *path)
{
    return g_file_test(path, G_FILE_TEST_EXISTS);
}

static void old_eligible_only(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const char *eligible = cache(fixture, true, 31, NULL);
    const char *legacy = cache(fixture, false, 31, NULL);
    write_file(legacy, "scan.sqlite");
    const char *unknown = cache(fixture, true, 31, NULL);
    write_file(unknown, "personal.csv");
    const char *bad_marker = cache(fixture, true, 31, NULL);
    write_file(bad_marker, DS_CACHE_OWNER_FILE);
    g_autofree char *marker = g_build_filename(bad_marker, DS_CACHE_OWNER_FILE, NULL);
    set_age(marker, 1000);
    const char *unleased = cache(fixture, true, 31, NULL);
    g_autofree char *lock = g_build_filename(unleased, DS_CACHE_LEASE_FILE, NULL);
    g_assert_cmpint(g_unlink(lock), ==, 0);
    const char *linked = cache(fixture, true, 31, NULL);
    g_autofree char *source = g_build_filename(fixture->base, "saved.dscan", NULL);
    g_autofree char *hardlink = g_build_filename(linked, "import.dscan", NULL);
#ifdef G_OS_WIN32
    g_autofree WCHAR *wide_source = (WCHAR *)g_utf8_to_utf16(source, -1, NULL, NULL, NULL);
    g_autofree WCHAR *wide_link = (WCHAR *)g_utf8_to_utf16(hardlink, -1, NULL, NULL, NULL);
    g_assert_true(CreateHardLinkW(wide_source, wide_link, NULL));
#else
    g_assert_cmpint(link(hardlink, source), ==, 0);
#endif

    ds_cache_prune(&fixture->cache, NULL);

    g_assert_false(exists(eligible));
    g_assert_true(exists(legacy));
    g_assert_true(exists(source));
    g_assert_true(exists(hardlink));
    g_autofree char *untouched = g_build_filename(unknown, "import.dscan", NULL);
    g_assert_true(exists(untouched));
    g_assert_true(exists(bad_marker));
    g_assert_true(exists(unleased));
}

static void holds_and_cancellation(Fixture *fixture, gconstpointer data)
{
    (void)data;
    DsCacheLease *shared = NULL;
    const char *in_use = cache(fixture, true, 31, &shared);
    const char *idle = cache(fixture, true, 31, NULL);
    DsControl control;
    ds_control_init(&control);
    ds_control_cancel(&control);
    ds_cache_prune(&fixture->cache, &control);
    ds_control_clear(&control);
    g_assert_true(exists(idle));

    ds_cache_prune(&fixture->cache, NULL);
    g_assert_true(exists(in_use));
    g_assert_false(exists(idle));
    ds_cache_lease_close(shared);
    DsCacheLease *exclusive = ds_cache_lease_open(in_use, false, true, NULL);
    g_assert_nonnull(exclusive);
    ds_cache_prune(&fixture->cache, NULL);
    g_assert_true(exists(in_use));
    ds_cache_lease_close(exclusive);
    ds_cache_prune(&fixture->cache, NULL);
    g_assert_false(exists(in_use));
}

static void preserves_replaced_database(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const char *folder = cache(fixture, true, 31, NULL);
    g_autofree char *original = g_build_filename(folder, "import.dscan", NULL);
    g_autofree char *moved = g_build_filename(fixture->base, "saved.dscan", NULL);
    g_assert_cmpint(g_rename(original, moved), ==, 0);
    write_file(folder, "import.dscan");
    ds_cache_prune(&fixture->cache, NULL);
    g_assert_true(exists(original));
    g_assert_true(exists(moved));
}

static void skips_linked_base(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const char *folder = cache(fixture, true, 31, NULL);
    g_autofree char *alias = g_build_filename(fixture->base, "cache-alias", NULL);
#ifdef G_OS_WIN32
    g_autofree WCHAR *wide_alias = (WCHAR *)g_utf8_to_utf16(alias, -1, NULL, NULL, NULL);
    g_autofree WCHAR *target = (WCHAR *)g_utf8_to_utf16(fixture->base, -1, NULL, NULL, NULL);
    if (!CreateSymbolicLinkW(wide_alias, target,
                             SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        g_test_skip("Windows directory symlink creation requires Developer Mode or privileges");
        return;
    }
#else
    g_assert_cmpint(symlink(fixture->base, alias), ==, 0);
#endif
    DsNativePath path = {0};
    native_argument(alias, &path);
    ds_cache_prune(&path, NULL);
    ds_path_clear(&path);
#ifdef G_OS_WIN32
    g_assert_true(RemoveDirectoryW(wide_alias));
#else
    g_assert_cmpint(g_unlink(alias), ==, 0);
#endif
    g_assert_true(exists(folder));
}

static void write_scan(const char *folder, int start)
{
    g_autofree char *name = g_build_filename(folder, "scan.sqlite", NULL);
    DsNativePath path = {0};
    native_argument(name, &path);
    DsStore *writer = scan_fixture(&path, "file", DS_REASON_NONE, DS_COVERAGE_COMPLETE);
    char *sql = sqlite3_mprintf("UPDATE scans SET started_at=%d", start);
    g_assert_cmpint(sqlite3_exec(ds_store_db(writer), sql, NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_free(sql);
    ds_store_close(writer);
    ds_path_clear(&path);
}

static void keeps_latest_recoverable(Fixture *fixture, gconstpointer data)
{
    (void)data;
    DsCacheLease *lease = NULL;
    const char *latest = cache(fixture, false, 35, &lease);
    write_scan(latest, 200);
    mark(latest, true, 35);
    ds_cache_lease_close(lease);
    const char *older = cache(fixture, false, 32, &lease);
    write_scan(older, 100);
    mark(older, true, 32);
    ds_cache_lease_close(lease);
    const char *empty = cache(fixture, false, 31, &lease);
    write_file(empty, "scan.sqlite");
    mark(empty, true, 31);
    ds_cache_lease_close(lease);
    ds_cache_prune(&fixture->cache, NULL);
    g_assert_true(exists(latest));
    g_assert_false(exists(older));
    g_assert_false(exists(empty));
}

static void limits_count(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const char *folders[12];
    for (size_t i = 0; i < G_N_ELEMENTS(folders); ++i)
        folders[i] = cache(fixture, true, (int)i, NULL);
    ds_cache_prune(&fixture->cache, NULL);
    for (size_t i = 0; i < G_N_ELEMENTS(folders); ++i)
        g_assert_cmpint(exists(folders[i]), ==, i < 10);
}

static void sparse_file(const char *folder)
{
    g_autofree char *path = g_build_filename(folder, "import.dscan", NULL);
    const uint64_t size = UINT64_C(5) * 1024 * 1024 * 1024;
#ifdef G_OS_WIN32
    g_autofree WCHAR *wide = (WCHAR *)g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    HANDLE file = fixture_open(wide, OPEN_EXISTING);
    DWORD returned;
    g_assert_true(DeviceIoControl(file, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL));
    LARGE_INTEGER end = {.QuadPart = size};
    g_assert_true(SetFilePointerEx(file, end, NULL, FILE_BEGIN));
    g_assert_true(SetEndOfFile(file));
    g_assert_true(CloseHandle(file));
#else
    int file = g_open(path, O_WRONLY, 0);
    g_assert_cmpint(file, >=, 0);
    g_assert_cmpint(ftruncate(file, (off_t)size), ==, 0);
    g_assert_cmpint(close(file), ==, 0);
#endif
}

static void limits_bytes(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const char *old = cache(fixture, true, 2, NULL);
    const char *recent = cache(fixture, true, 1, NULL);
    sparse_file(old);
    sparse_file(recent);
    ds_cache_prune(&fixture->cache, NULL);
    g_assert_false(exists(old));
    g_assert_true(exists(recent));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/retention/old-eligible-only", Fixture, NULL, setup, old_eligible_only, teardown);
    g_test_add("/retention/leases-and-cancellation", Fixture, NULL, setup, holds_and_cancellation, teardown);
    g_test_add("/retention/replaced-primary-file", Fixture, NULL, setup, preserves_replaced_database, teardown);
    g_test_add("/retention/linked-base", Fixture, NULL, setup, skips_linked_base, teardown);
    g_test_add("/retention/latest-recoverable", Fixture, NULL, setup, keeps_latest_recoverable, teardown);
    g_test_add("/retention/count-limit", Fixture, NULL, setup, limits_count, teardown);
    g_test_add("/retention/size-limit", Fixture, NULL, setup, limits_bytes, teardown);
    return g_test_run();
}

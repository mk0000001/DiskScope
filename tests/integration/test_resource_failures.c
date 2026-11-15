#include "diskscope/ds_query.h"
#include "diskscope/ds_export.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#ifdef DS_TEST_WRAP_WRITE
#include <errno.h>
#include <unistd.h>

static bool exhaust_output;
ssize_t __real_write(int fd, const void *data, size_t length);

ssize_t __wrap_write(int fd, const void *data, size_t length)
{
    if (exhaust_output && length > 4096) {
        errno = ENOSPC;
        return -1;
    }
    return __real_write(fd, data, length);
}
#endif

typedef struct {
    char *directory, *filename;
    DsNativePath path;
    DsStore *writer;
} Fixture;

static DsStatus append(DsStore *writer, int64_t id, const void *name, size_t length)
{
    DsEntry entry = {.id = id, .parent_id = id == 1 ? 0 : 1,
                    .kind = id == 1 ? DS_DIRECTORY : DS_FILE, .identity_known = id != 1};
    ds_u64_encode((uint64_t)id, entry.identity);
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, name, length, &entry.name, NULL), ==, DS_OK);
    DsEntryBatch batch = {&entry, 1};
    DsStatus status = ds_store_append(writer, &batch, NULL);
    ds_path_clear(&entry.name);
    return status;
}

static void setup(Fixture *f, const void *unused)
{
    (void)unused;
    f->directory = g_dir_make_tmp("ds-resources-XXXXXX", NULL);
    g_assert_nonnull(f->directory);
    f->filename = g_build_filename(f->directory, "scan.db", NULL);
    g_assert_cmpint(ds_path_from_arg(f->filename, &f->path, NULL), ==, DS_OK);
    DsScanOptions options = {.cache_budget = 2 * DS_CACHE_END_RESERVE};
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, "/root", 5, &options.root, NULL), ==, DS_OK);
    g_assert_cmpint(ds_path_from_arg(f->directory, &options.cache_path, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_open(&f->path, DS_STORE_WRITER, &f->writer, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_set_options(f->writer, &options, NULL), ==, DS_OK);
    ds_path_clear(&options.root);
    ds_path_clear(&options.cache_path);
    g_assert_cmpint(append(f->writer, 1, "", 0), ==, DS_OK);
}

static void teardown(Fixture *f, const void *unused)
{
    (void)unused;
    ds_store_close(f->writer);
    ds_path_clear(&f->path);
    g_assert_cmpint(g_remove(f->filename), ==, 0);
    g_assert_cmpint(g_rmdir(f->directory), ==, 0);
    g_free(f->filename);
    g_free(f->directory);
}

static DsStore *hold_reader(Fixture *f)
{
    DsStore *reader = NULL;
    g_assert_cmpint(ds_store_open(&f->path, DS_STORE_READER, &reader, NULL), ==, DS_OK);
    g_assert_cmpint(sqlite3_exec(ds_store_db(reader), "BEGIN; SELECT count(*) FROM entries",
                                NULL, NULL, NULL), ==, SQLITE_OK);
    return reader;
}

static uint64_t cache_size(Fixture *f)
{
    GDir *directory = g_dir_open(f->directory, 0, NULL);
    g_assert_nonnull(directory);
    uint64_t used = 0;
    const char *basename;
    while ((basename = g_dir_read_name(directory))) {
        GStatBuf info;
        char *path = g_build_filename(f->directory, basename, NULL);
        if (g_stat(path, &info) == 0) used += (uint64_t)info.st_size;
        g_free(path);
    }
    g_dir_close(directory);
    return used;
}

static void reserve_counts_cache_files(Fixture *f, const void *unused)
{
    (void)unused;
    char *name = g_build_filename(f->directory, "diagnostics-and-temp.dat", NULL);
    GFile *file = g_file_new_for_path(name);
    GFileOutputStream *output = g_file_replace(file, NULL, false, G_FILE_CREATE_PRIVATE, NULL, NULL);
    g_assert_nonnull(output);
    g_assert_true(g_seekable_truncate(G_SEEKABLE(output), DS_CACHE_END_RESERVE, NULL, NULL));
    g_assert_true(g_output_stream_close(G_OUTPUT_STREAM(output), NULL, NULL));
    g_object_unref(output);
    g_object_unref(file);

    g_assert_cmpint(append(f->writer, 2, "second", 6), ==, DS_LIMIT);
    g_assert_cmpint(ds_finalize(f->writer, NULL, NULL), ==, DS_LIMIT);
    DsStoreInfo info = {0};
    g_assert_cmpint(ds_store_info(f->writer, &info, NULL), ==, DS_OK);
    g_assert_cmpuint(info.entry_count, ==, 1);
    g_assert_cmpint(info.generation, ==, 0);
    ds_store_info_clear(&info);
    g_assert_cmpint(g_remove(name), ==, 0);
    g_free(name);
    g_assert_cmpint(append(f->writer, 2, "second", 6), ==, DS_OK);
}

static void small_full_with_held_reader(Fixture *f, const void *unused)
{
    (void)unused;
    DsStore *reader = hold_reader(f);
    sqlite3 *db = ds_store_db(f->writer);
    sqlite3_stmt *pages = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA page_count", -1, &pages, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(pages), ==, SQLITE_ROW);
    char *max = sqlite3_mprintf("PRAGMA max_page_count=%d", sqlite3_column_int(pages, 0) + 1);
    sqlite3_finalize(pages);
    g_assert_cmpint(sqlite3_exec(db, max, NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_free(max);

    char *name = g_strnfill(DS_PATH_MAX, 'x');
    g_assert_cmpint(append(f->writer, 2, name, DS_PATH_MAX), ==, DS_LIMIT);
    g_free(name);
    uint64_t used = cache_size(f);
    g_assert_true(used < (UINT64_C(1) << 20));
    g_test_message("Database footprint at injected SQLITE_FULL: %" G_GUINT64_FORMAT " bytes", used);

    DsStoreInfo info = {0};
    g_assert_cmpint(ds_store_info(reader, &info, NULL), ==, DS_OK);
    g_assert_cmpuint(info.entry_count, ==, 1);
    g_assert_cmpint(info.commit_seq, ==, 1);
    ds_store_info_clear(&info);
    ds_store_close(reader);
    g_assert_cmpint(ds_store_set_result(f->writer, DS_LIMIT, DS_COVERAGE_PARTIAL, NULL), ==, DS_OK);
}

static void writer_stops_before_reserved_wal_space(Fixture *f, const void *unused)
{
    (void)unused;
    DsStore *reader = hold_reader(f);
    char *name = g_strnfill(64u << 10, 'x');
    DsStatus status = DS_OK;
    int64_t id = 2;
    for (; id < 512 && status == DS_OK; ++id) {
        g_snprintf(name, 20, "%" G_GINT64_FORMAT, (gint64)id);
        for (char *p = name; p < name + 20; ++p) if (!*p) *p = 'x';
        status = append(f->writer, id, name, 64u << 10);
    }
    g_free(name);
    g_assert_cmpint(status, ==, DS_LIMIT);
    uint64_t used = cache_size(f);
    g_assert_true(used < DS_CACHE_END_RESERVE);
    g_test_message("Held reader left %" G_GUINT64_FORMAT " bytes used after %" G_GINT64_FORMAT " commits",
                   used, id - 2);
    DsStoreInfo info = {0};
    g_assert_cmpint(ds_store_info(reader, &info, NULL), ==, DS_OK);
    g_assert_cmpuint(info.entry_count, ==, 1);
    ds_store_info_clear(&info);
    ds_store_close(reader);
    g_assert_cmpint(ds_store_info(f->writer, &info, NULL), ==, DS_OK);
    g_assert_cmpuint(info.entry_count, ==, (uint64_t)(id - 2));
    ds_store_info_clear(&info);
}

#ifdef DS_TEST_WRAP_WRITE
static void output_failure_preserves_previous_file(Fixture *f, const void *unused)
{
    (void)unused;
    char *long_name = g_strnfill(8192, 'x');
    g_assert_cmpint(append(f->writer, 2, long_name, 8192), ==, DS_OK);
    g_free(long_name);
    char *target = g_build_filename(f->directory, "existing.json", NULL);
    g_assert_true(g_file_set_contents(target, "previous", -1, NULL));
    DsExportOptions options = {.format = DS_EXPORT_JSON};
    g_assert_cmpint(ds_path_from_arg(target, &options.destination, NULL), ==, DS_OK);

    exhaust_output = true;
    DsStatus status = ds_export(f->writer, &options, NULL, NULL);
    exhaust_output = false;
    g_assert_cmpint(status, ==, DS_LIMIT);

    char *contents = NULL;
    g_assert_true(g_file_get_contents(target, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "previous");
    g_free(contents);
    ds_path_clear(&options.destination);
    g_assert_cmpint(g_remove(target), ==, 0);
    g_free(target);
}
#endif

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/resource/cache-reserve", Fixture, NULL, setup, reserve_counts_cache_files, teardown);
    g_test_add("/resource/full-under-1mib", Fixture, NULL, setup, small_full_with_held_reader, teardown);
    g_test_add("/resource/held-reader-wal-reserve", Fixture, NULL, setup,
               writer_stops_before_reserved_wal_space, teardown);
#ifdef DS_TEST_WRAP_WRITE
    g_test_add("/resource/output-write-failure", Fixture, NULL, setup,
               output_failure_preserves_previous_file, teardown);
#endif
    return g_test_run();
}

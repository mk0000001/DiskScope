#include "diskscope/ds_query.h"
#include "diskscope/ds_snapshot.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdlib.h>

enum { FULL_DIRECTORIES = 100000, SMALL_DIRECTORIES = 1000, FILES_PER_DIRECTORY = 9 };

static const char fixture_sql[] =
    "BEGIN IMMEDIATE;"
    "WITH RECURSIVE ids(id) AS (VALUES(1) UNION ALL SELECT id+1 FROM ids WHERE id<%u),"
    " samples AS (SELECT id, CASE WHEN id=1 THEN NULL WHEN id<=%u THEN 1 ELSE 2+(id-%u)%%%u END parent,"
    " CASE WHEN id=1 THEN '' WHEN id<=%u THEN printf('directory-%%022d',id)"
    " ELSE printf('file-%%027d',id) END name, id>%u regular FROM ids)"
    " INSERT INTO entries(id,parent_id,name,name_encoding,display,kind,reason,os_code,"
    " logical,logical_quality,allocated,allocated_quality,identity,links,mtime,commit_seq)"
    " SELECT id,parent,CAST(name AS BLOB),1,name,CASE regular WHEN 1 THEN 2 ELSE 1 END,0,0,"
    " CASE WHEN regular THEN X'0000000000000021' END,CASE regular WHEN 1 THEN 0 ELSE 1 END,"
    " CASE WHEN regular THEN X'0000000000001000' END,CASE regular WHEN 1 THEN 0 ELSE 1 END,"
    " CASE WHEN regular THEN CAST(printf('%%024d',id) AS BLOB) END,"
    " X'0000000000000001',1700000000+id%%86400,1 FROM samples;"
    "UPDATE scans SET entry_count=%u,commit_seq=1 WHERE id=1; COMMIT;";

static double elapsed_ms(gint64 started)
{
    return (g_get_monotonic_time() - started) / 1000.0;
}

static DsStatus make_fixture(const char *directory, unsigned directories, char **filename, DsError *error)
{
    g_autofree char *work = g_build_filename(directory, "fixture-XXXXXX", NULL);
    if (!g_mkdtemp(work)) return ds_error(error, DS_IO, 0, "Cannot create a unique fixture folder");
    *filename = g_build_filename(work, "benchmark.dscan", NULL);
    DsNativePath output = {0};
    DsStatus status = ds_path_from_arg(*filename, &output, error);
    if (status != DS_OK) return status;

    DsStore *writer = NULL;
    DsScanOptions options;
    ds_scan_options_init(&options, DS_CONSERVATIVE);
    const char root[] = "/diskscope-synthetic";
    status = ds_path_new(DS_PATH_BYTES, root, sizeof root - 1, &options.root, error);
    if (status == DS_OK) status = ds_store_open(&output, DS_STORE_WRITER, &writer, error);
    if (status == DS_OK) status = ds_store_set_options(writer, &options, error);
    ds_path_clear(&options.root);
    ds_path_clear(&output);
    if (status != DS_OK) {
        ds_store_close(writer);
        return status;
    }

    unsigned total = 1 + directories * (1 + FILES_PER_DIRECTORY);
    unsigned last_directory = 1 + directories;
    char *script = sqlite3_mprintf(fixture_sql, total, last_directory, last_directory + 1,
                                   directories, last_directory, last_directory, total);
    sqlite3 *db = ds_store_db(writer);
    gint64 started = g_get_monotonic_time();
    int code = script ? sqlite3_exec(db, script, NULL, NULL, NULL) : SQLITE_NOMEM;
    sqlite3_free(script);
    if (code != SQLITE_OK)
        status = ds_error(error, DS_IO, code, "Synthetic insert failed: %s", sqlite3_errmsg(db));
    if (status == DS_OK) {
        g_print("build.rows=%u build.insert_ms=%.1f\n", total, elapsed_ms(started));
        status = ds_store_set_result(writer, DS_OK, DS_COVERAGE_COMPLETE, error);
    }
    if (status == DS_OK) {
        started = g_get_monotonic_time();
        status = ds_finalize(writer, NULL, error);
        if (status == DS_OK) g_print("build.finalize_ms=%.1f\n", elapsed_ms(started));
    }
    if (status == DS_OK) {
        code = sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
        if (code != SQLITE_OK)
            status = ds_error(error, DS_IO, code, "Fixture checkpoint failed: %s", sqlite3_errmsg(db));
    }
    if (status == DS_OK) {
        code = sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
        if (code != SQLITE_OK)
            status = ds_error(error, DS_IO, code, "Cannot close the fixture WAL: %s", sqlite3_errmsg(db));
    }
    ds_store_close(writer);
    if (status == DS_OK) {
        GStatBuf file_stat;
        if (g_stat(*filename, &file_stat) == 0)
            g_print("fixture.bytes=%" G_GUINT64_FORMAT "\n", (guint64)file_stat.st_size);
    }
    return status;
}

static int compare_time(const void *a, const void *b)
{
    double left = *(const double *)a, right = *(const double *)b;
    return (left > right) - (left < right);
}

static DsStatus probe_query(DsStore *reader, const char *label, DsQuery query, unsigned samples,
                            uint64_t range, DsError *error)
{
    g_autofree double *times = g_new(double, samples);
    for (unsigned sample = 0; sample < samples; ++sample) {
        query.offset = range ? ((uint64_t)sample * 7919) % range : 0;
        gint64 started = g_get_monotonic_time();
        DsPage *page = NULL;
        DsStatus status = ds_query_page(reader, &query, &page, NULL, error);
        times[sample] = elapsed_ms(started);
        if (status != DS_OK) return status;
        size_t count = page->count;
        bool valid = count == query.limit && page->rows[0].entry.id > 1 &&
                     page->rows[0].entry.parent_id == query.parent_id;
        ds_page_free(page);
        if (!valid) return ds_error(error, DS_INVALID, 0, "Unexpected page in %s workload", label);
    }
    qsort(times, samples, sizeof *times, compare_time);
    g_print("%s.samples=%u %s.p50_ms=%.2f %s.p95_ms=%.2f %s.max_ms=%.2f\n", label, samples,
            label, times[(samples + 1) / 2 - 1], label, times[(samples * 95 + 99) / 100 - 1],
            label, times[samples - 1]);
    return DS_OK;
}

static DsStatus probe_fixture(const char *filename, unsigned samples, DsError *error)
{
    DsNativePath source = {0};
    DsStatus status = ds_path_from_arg(filename, &source, error);
    if (status != DS_OK) return status;
    DsStore *reader = NULL;
    gint64 started = g_get_monotonic_time();
    status = ds_snapshot_open(&source, &reader, NULL, error);
    ds_path_clear(&source);
    if (status != DS_OK) return status;
    g_print("open.validated_ms=%.1f\n", elapsed_ms(started));

    DsQuery query = {.parent_id = 0, .sort = DS_SORT_ID, .limit = 1};
    DsPage *page = NULL;
    status = ds_query_page(reader, &query, &page, NULL, error);
    uint64_t directories = 0;
    if (status == DS_OK) {
        uint64_t count = page->info.entry_count;
        directories = count > 1 ? (count - 1) / (1 + FILES_PER_DIRECTORY) : 0;
        uint64_t files = directories * FILES_PER_DIRECTORY;
        const DsRow *root = page->rows;
        bool exact = page->count == 1 && root->aggregated &&
            (directories == SMALL_DIRECTORIES || directories == FULL_DIRECTORIES) &&
            count == 1 + directories + files && root->entry_count == count &&
            root->file_count == files && root->unique_file_count == files &&
            root->metrics[DS_LOGICAL].quality == DS_KNOWN && root->metrics[DS_LOGICAL].bytes == files * 33 &&
            root->metrics[DS_ALLOCATED].quality == DS_KNOWN && root->metrics[DS_ALLOCATED].bytes == files * 4096 &&
            root->metrics[DS_UNIQUE].quality == DS_KNOWN && root->metrics[DS_UNIQUE].bytes == files * 4096;
        if (!exact) status = ds_error(error, DS_INVALID, 0, "Fixture counts or aggregate values do not match");
        else g_print("open.directories=%" G_GUINT64_FORMAT " open.files=%" G_GUINT64_FORMAT "\n",
                     (guint64)directories, (guint64)files);
    }
    ds_page_free(page);
    if (status == DS_OK) {
        query = (DsQuery){.parent_id = 1, .sort = DS_SORT_NAME, .limit = DS_PAGE_MAX, .omit_total = true};
        status = probe_query(reader, "wide_name_page", query, samples, directories - DS_PAGE_MAX + 1, error);
    }
    if (status == DS_OK) {
        query.sort = DS_SORT_METRIC;
        query.metric = DS_ALLOCATED;
        query.descending = true;
        status = probe_query(reader, "wide_top_allocated", query, samples, 0, error);
    }
    ds_store_close(reader);
    return status;
}

int main(int argc, char **argv)
{
    g_autofree char *output = NULL;
    g_autofree char *opened = NULL;
    gboolean small = FALSE;
    gint samples = 32;
    GOptionEntry options[] = {
        {"build", 'b', 0, G_OPTION_ARG_FILENAME, &output, "Create a fixture in an existing directory", "DIR"},
        {"probe", 'p', 0, G_OPTION_ARG_FILENAME, &opened, "Open and probe a generated fixture", "FILE"},
        {"small", 0, 0, G_OPTION_ARG_NONE, &small, "Build a reduced fixture for smoke testing", NULL},
        {"samples", 'n', 0, G_OPTION_ARG_INT, &samples, "Number of samples per SQL query workload", "N"},
        {0},
    };
    g_autoptr(GOptionContext) context = g_option_context_new("- benchmark synthetic DiskScope results");
    g_option_context_add_main_entries(context, options, NULL);
    g_autoptr(GError) arguments = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &arguments) || argc != 1 ||
        (output == NULL) == (opened == NULL) || (small && !output) || samples < 1 || samples > 1000) {
        if (arguments) g_printerr("%s\n", arguments->message);
        g_printerr("Use exactly one of --build DIR and --probe FILE, with 1 to 1000 samples.\n");
        return 2;
    }

    g_autofree char *generated = NULL;
    DsError error = {0};
    DsStatus status = DS_OK;
    if (output) status = make_fixture(output, small ? SMALL_DIRECTORIES : FULL_DIRECTORIES, &generated, &error);
    const char *filename = output ? generated : opened;
    if (filename) g_print("fixture.path=%s\n", filename);
    if (status == DS_OK) status = probe_fixture(filename, (unsigned)samples, &error);
    if (status != DS_OK) g_printerr("%s\n", error.message);
    return status == DS_OK ? 0 : 1;
}

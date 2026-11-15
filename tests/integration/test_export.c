#include "snapshot_fixture.h"
#include "diskscope/ds_export.h"

static void exact_and_safe_output(void)
{
    char *directory = g_dir_make_tmp("ds-export-XXXXXX", NULL);
    char *active = g_build_filename(directory, "active.db", NULL);
    char *output = g_build_filename(directory, "output", NULL);
    DsNativePath database = {0};
    native_argument(active, &database);
    DsExportOptions options = {.format = DS_EXPORT_CSV, .bom = true};
    native_argument(output, &options.destination);
    DsStore *writer = scan_fixture(&database, " =HELLO,\"X\"\nY", DS_REASON_NONE, DS_COVERAGE_COMPLETE);
    g_assert_cmpint(ds_export(writer, &options, NULL, NULL), ==, DS_OK);
    char *contents = NULL;
    g_assert_true(g_file_get_contents(output, &contents, NULL, NULL));
    g_assert_true(g_str_has_prefix(contents, "\xef\xbb\xbf"));
    g_assert_nonnull(strstr(contents, "\"' =HELLO,\"\"X\"\"\\u000AY\""));
    g_assert_nonnull(strstr(contents, "\r\n"));
    g_free(contents);

    options.format = DS_EXPORT_JSONL;
    options.bom = false;
    g_assert_cmpint(ds_export(writer, &options, NULL, NULL), ==, DS_OK);
    g_assert_true(g_file_get_contents(output, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "\"9007199254740993\""));
    g_assert_nonnull(strstr(contents, "\"raw_path_base64\""));
    g_assert_nonnull(strstr(contents, "\"footer\""));
    g_free(contents);

    DsControl control;
    ds_control_init(&control);
    ds_control_cancel(&control);
    g_assert_true(g_file_set_contents(output, "existing export", -1, NULL));
    g_assert_cmpint(ds_export(writer, &options, &control, NULL), ==, DS_CANCELED);
    ds_control_clear(&control);
    g_assert_true(g_file_get_contents(output, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "existing export");
    g_free(contents);
    ds_path_clear(&options.destination);
    options.destination = database;
    g_assert_cmpint(ds_export(writer, &options, NULL, NULL), ==, DS_INVALID);
    ds_store_close(writer);
    ds_path_clear(&database);
    g_remove(output);
    g_remove(active);
    g_rmdir(directory);
    g_free(output);
    g_free(active);
    g_free(directory);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/export/exact-safe-output", exact_and_safe_output);
    return g_test_run();
}

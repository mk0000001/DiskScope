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
    g_assert_nonnull(strstr(contents, "\"schema_version\":1,"));
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

static void scoped_output(void)
{
    char *directory = g_dir_make_tmp("ds-export-scope-XXXXXX", NULL);
    char *active = g_build_filename(directory, "active.db", NULL);
    char *output = g_build_filename(directory, "output", NULL);
    DsNativePath database = {0};
    native_argument(active, &database);
    DsExportOptions options = {.format = DS_EXPORT_JSON, .selected_id = 3, .metric = DS_ALLOCATED};
    native_argument(output, &options.destination);

    const char *const names[] = {
        "", "other", "pick", "hold.txt", "child", "\xff.txt", "pickup.txt", "extra.bin", "also.txt"
    };
    DsEntry entries[] = {
        {.id = 1, .kind = DS_DIRECTORY},
        {.id = 2, .parent_id = 1, .kind = DS_DIRECTORY},
        {.id = 3, .parent_id = 1, .kind = DS_DIRECTORY},
        {.id = 4, .parent_id = 2, .kind = DS_FILE},
        {.id = 5, .parent_id = 3, .kind = DS_DIRECTORY},
        {.id = 6, .parent_id = 5, .kind = DS_FILE},
        {.id = 7, .parent_id = 1, .kind = DS_FILE},
        {.id = 8, .parent_id = 3, .kind = DS_FILE},
        {.id = 9, .parent_id = 5, .kind = DS_FILE},
    };
    DsStore *store = NULL;
    DsScanOptions scan = {.cache_budget = DS_CACHE_BUDGET};
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, "/root", 5, &scan.root, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_open(&database, DS_STORE_WRITER, &store, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_set_options(store, &scan, NULL), ==, DS_OK);
    ds_path_clear(&scan.root);
    for (size_t i = 0; i < G_N_ELEMENTS(entries); ++i)
        g_assert_cmpint(ds_path_new(DS_PATH_BYTES, names[i], strlen(names[i]), &entries[i].name, NULL), ==, DS_OK);
    DsEntryBatch batch = {entries, G_N_ELEMENTS(entries)};
    g_assert_cmpint(ds_store_append(store, &batch, NULL), ==, DS_OK);
    for (size_t i = 0; i < G_N_ELEMENTS(entries); ++i)
        ds_path_clear(&entries[i].name);
    g_assert_cmpint(ds_store_set_result(store, DS_OK, DS_COVERAGE_COMPLETE, NULL), ==, DS_OK);
    g_assert_cmpint(ds_finalize(store, NULL, NULL), ==, DS_OK);

    char *contents = NULL;
    g_assert_cmpint(ds_export(store, &options, NULL, NULL), ==, DS_OK);
    g_assert_true(g_file_get_contents(output, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "\"export_scope\":\"selection\""));
    g_assert_nonnull(strstr(contents, "\"selected_metric\":\"allocated\""));
    g_assert_nonnull(strstr(contents, "\"selected_id\":\"3\""));
    g_assert_nonnull(strstr(contents, "\"display_path\":\"pick\""));
    g_assert_nonnull(strstr(contents, "\"display_path\":\"pick/child\""));
    g_assert_nonnull(strstr(contents, "\"raw_path_base64\":\"cGljay9jaGlsZC//LnR4dA==\""));
    g_assert_nonnull(strstr(contents, "\"display_path\":\"pick/child/also.txt\""));
    g_assert_nonnull(strstr(contents, "\"display_path\":\"pick/extra.bin\""));
    g_assert_null(strstr(contents, "pickup.txt"));
    g_assert_null(strstr(contents, "hold.txt"));
    g_assert_nonnull(strstr(contents, "\"entries\":\"5\""));
    g_free(contents);

    options.format = DS_EXPORT_CSV;
    options.selected_id = 6;
    g_assert_cmpint(ds_export(store, &options, NULL, NULL), ==, DS_OK);
    g_assert_true(g_file_get_contents(output, &contents, NULL, NULL));
    char **lines = g_strsplit(contents, "\r\n", -1);
    g_assert_cmpuint(g_strv_length(lines), ==, 3); /* Header, selected row, trailing newline. */
    g_assert_true(g_str_has_suffix(lines[0], ",export_scope,selected_metric,selected_id"));
    g_assert_nonnull(strstr(lines[1], "\"cGljay9jaGlsZC//LnR4dA==\""));
    g_assert_true(g_str_has_suffix(lines[1], ",selection,allocated,6"));
    g_strfreev(lines);

    options.selected_id = 42;
    g_assert_cmpint(ds_export(store, &options, NULL, NULL), ==, DS_INVALID);
    char *previous = NULL;
    g_assert_true(g_file_get_contents(output, &previous, NULL, NULL));
    g_assert_cmpstr(previous, ==, contents);
    g_free(previous);
    options.selected_id = 3;
    DsControl control;
    ds_control_init(&control);
    ds_control_cancel(&control);
    g_assert_cmpint(ds_export(store, &options, &control, NULL), ==, DS_CANCELED);
    ds_control_clear(&control);
    g_assert_true(g_file_get_contents(output, &previous, NULL, NULL));
    g_assert_cmpstr(previous, ==, contents);
    g_free(previous);
    g_free(contents);

    options.format = DS_EXPORT_JSONL;
    options.selected_id = 0;
    const DsQuery view = {
        .parent_id = DS_QUERY_ALL, .kind = DS_FILE, .extension = "TXT",
        .sort = DS_SORT_METRIC, .metric = DS_UNIQUE, .descending = true,
        .offset = 2, .limit = 1,
    };
    options.metric = view.metric;
    options.view = &view;
    g_assert_cmpint(ds_export(store, &options, NULL, NULL), ==, DS_OK);
    g_assert_true(g_file_get_contents(output, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "\"export_scope\":\"view\""));
    g_assert_nonnull(strstr(contents, "\"selected_metric\":\"unique_allocated\""));
    g_assert_nonnull(strstr(contents, "pickup.txt"));
    g_assert_nonnull(strstr(contents, "hold.txt"));
    g_assert_nonnull(strstr(contents, "also.txt"));
    g_assert_nonnull(strstr(contents, "\"raw_path_base64\":\"cGljay9jaGlsZC//LnR4dA==\""));
    g_assert_null(strstr(contents, "\"kind\":\"directory\""));
    g_assert_null(strstr(contents, "extra.bin"));
    g_assert_nonnull(strstr(contents, "\"entries\":\"4\""));
    g_free(contents);

    ds_store_close(store);
    ds_path_clear(&database);
    ds_path_clear(&options.destination);
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
    g_test_add_func("/export/scoped-output", scoped_output);
    return g_test_run();
}

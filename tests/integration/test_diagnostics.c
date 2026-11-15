#include "diskscope/ds_diagnostics.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

static JsonObject *read_report(const char *path, JsonParser **out, char **bytes)
{
    GError *error = NULL;
    g_assert_true(g_file_get_contents(path, bytes, NULL, &error));
    g_assert_no_error(error);
    *out = json_parser_new();
    g_assert_true(json_parser_load_from_data(*out, *bytes, -1, &error));
    g_assert_no_error(error);
    return json_node_get_object(json_parser_get_root(*out));
}

static void redaction_and_output(void)
{
    char *folder = g_dir_make_tmp("diskscope-diagnostics-XXXXXX", NULL);
    g_assert_nonnull(folder);
    char *db_path = g_build_filename(folder, "cache-private-sentinel.sqlite", NULL);
    char *report_path = g_build_filename(folder, "diagnostics.json", NULL);
    char *source_path = g_build_filename(folder, "source-private-sentinel", NULL);
    DsNativePath db = {0}, report = {0};
    g_assert_cmpint(ds_path_from_arg(db_path, &db, NULL), ==, DS_OK);
    g_assert_cmpint(ds_path_from_arg(report_path, &report, NULL), ==, DS_OK);

    DsScanOptions scan;
    ds_scan_options_init(&scan, DS_STANDARD);
    scan.include_hidden = false;
    scan.exclude_pattern = "exclusion-private-sentinel";
    g_assert_cmpint(ds_path_from_arg(source_path, &scan.root, NULL), ==, DS_OK);
    DsStore *store = NULL;
    g_assert_cmpint(ds_store_open(&db, DS_STORE_WRITER, &store, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_set_options(store, &scan, NULL), ==, DS_OK);
    DsEntry rows[] = {
        {.id=1, .kind=DS_DIRECTORY},
        {.id=2, .parent_id=1, .kind=DS_DIRECTORY, .reason=DS_REASON_PERMISSION, .os_code=13}
    };
    g_assert_cmpint(ds_path_new(scan.root.encoding, NULL, 0, &rows[0].name, NULL), ==, DS_OK);
    g_assert_cmpint(ds_path_from_arg("filename-private-sentinel", &rows[1].name, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_append(store, &(DsEntryBatch){rows, G_N_ELEMENTS(rows)}, NULL), ==, DS_OK);
    ds_store_close(store);
    g_assert_cmpint(ds_store_open(&db, DS_STORE_READER, &store, NULL), ==, DS_OK);
    DsSessionEvent worker = {.stage=DS_CANCEL_WAITING, .stalled=true, .detail="error-private-sentinel"};
    DsDiagnosticsOptions opts = {.destination=report, .worker=&worker, .gtk_major=4, .gtk_minor=14};

    DsError failure = {0};
    if (ds_diagnostics_save(store, &opts, NULL, &failure) != DS_OK)
        g_error("diagnostics export failed: %s", failure.message);
    JsonParser *parser;
    char *text;
    JsonObject *object = read_report(report_path, &parser, &text);
    g_assert_cmpstr(json_object_get_string_member(object, "report"), ==, "diskscope-diagnostics");
    g_assert_null(strstr(text, "-private-sentinel"));
    g_assert_true(json_object_get_boolean_member(object, "paths_redacted"));
    JsonObject *summary = json_object_get_object_member(object, "scan");
    g_assert_cmpstr(json_object_get_string_member(summary, "entries"), ==, "2");
    g_assert_cmpstr(json_object_get_string_member(summary, "profile"), ==, "standard");
    g_assert_false(json_object_get_boolean_member(summary, "include_hidden"));
    JsonObject *issues = json_object_get_object_member(summary, "issues");
    g_assert_cmpstr(json_object_get_string_member(issues, "permission"), ==, "1");
    JsonObject *state = json_object_get_object_member(object, "worker");
    g_assert_cmpstr(json_object_get_string_member(state, "stage"), ==, "cancel-waiting");
    g_object_unref(parser);
    g_free(text);

    opts.include_root = true;
    g_assert_cmpint(ds_diagnostics_save(store, &opts, NULL, NULL), ==, DS_OK);
    object = read_report(report_path, &parser, &text);
    g_assert_false(json_object_get_boolean_member(object, "paths_redacted"));
    g_assert_nonnull(strstr(text, "source-private-sentinel"));
    g_assert_null(strstr(text, "filename-private-sentinel"));
    g_assert_null(strstr(text, "error-private-sentinel"));
    g_object_unref(parser);
    g_free(text);

    DsControl cancel;
    ds_control_init(&cancel);
    ds_control_cancel(&cancel);
    g_assert_cmpint(ds_diagnostics_save(store, &opts, &cancel, NULL), ==, DS_CANCELED);
    ds_control_clear(&cancel);
    object = read_report(report_path, &parser, &text);
    g_assert_false(json_object_get_boolean_member(object, "paths_redacted"));
    g_object_unref(parser);
    g_free(text);
    opts.destination = db;
    g_assert_cmpint(ds_diagnostics_save(store, &opts, NULL, NULL), ==, DS_INVALID);
    ds_store_close(store);

    opts = (DsDiagnosticsOptions){.destination=report};
    g_assert_cmpint(ds_diagnostics_save(NULL, &opts, NULL, NULL), ==, DS_OK);
    object = read_report(report_path, &parser, &text);
    g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(object, "scan")));
    g_object_unref(parser);
    g_free(text);

    for (size_t i = 0; i < G_N_ELEMENTS(rows); ++i)
        ds_path_clear(&rows[i].name);
    ds_path_clear(&scan.root);
    ds_path_clear(&report);
    ds_path_clear(&db);
    g_unlink(report_path);
    g_unlink(db_path);
    const char *sidecars[] = {"-wal", "-shm"};
    for (size_t i = 0; i < G_N_ELEMENTS(sidecars); ++i) {
        char *sidecar = g_strconcat(db_path, sidecars[i], NULL);
        g_unlink(sidecar);
        g_free(sidecar);
    }
    g_assert_cmpint(g_rmdir(folder), ==, 0);
    g_free(source_path);
    g_free(report_path);
    g_free(db_path);
    g_free(folder);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/diagnostics/redaction-and-output", redaction_and_output);
    return g_test_run();
}

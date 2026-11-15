#include "diskscope/ds_diagnostics.h"
#include "export_internal.h"
#include "../store/store_internal.h"

static void text_member(GString *report, const char *key, const char *value)
{
    ds_json_string(report, key);
    g_string_append_c(report, ':');
    if (value)
        ds_json_string(report, value);
    else
        g_string_append(report, "null");
}

static void count_member(GString *report, const char *key, uint64_t value)
{
    g_string_append_printf(report, "\"%s\":\"%" G_GUINT64_FORMAT "\"", key, (guint64)value);
}

static const char *bool_text(bool value)
{
    return value ? "true" : "false";
}

static bool safe_version(const char *version)
{
    if (!version || !*version || strlen(version) > 32)
        return false;
    for (const char *c = version; *c; ++c)
        if (!g_ascii_isdigit(*c) && *c != '.' && *c != '-')
            return false;
    return true;
}

static void environment(GString *report, const DsDiagnosticsOptions *options)
{
    g_string_append(report, "\"environment\":{");
    text_member(report, "app_version", DS_VERSION);
#ifdef G_OS_WIN32
    g_string_append(report, ",\"os\":\"windows\"");
#else
    g_string_append(report, ",\"os\":\"linux\"");
#endif
    char *version = g_get_os_info(G_OS_INFO_KEY_VERSION_ID);
    g_string_append_c(report, ',');
    text_member(report, "os_version", safe_version(version) ? version : NULL);
    g_free(version);
    g_string_append_c(report, ',');
    text_member(report, "sqlite", sqlite3_libversion());
    g_string_append_printf(report, ",\"address_bits\":%zu,\"glib\":\"%u.%u.%u\",\"gtk\":",
        sizeof(void *) * 8u, glib_major_version, glib_minor_version, glib_micro_version);
    if (options->gtk_major)
        g_string_append_printf(report, "\"%u.%u.%u\"", options->gtk_major, options->gtk_minor,
                               options->gtk_micro);
    else
        g_string_append(report, "null");
    g_string_append_c(report, '}');
}

static void worker_state(GString *report, const DsSessionEvent *event)
{
    g_string_append(report, ",\n\"worker\":");
    if (!event) {
        g_string_append(report, "null");
        return;
    }
    static const char *const stages[] = {
        "preparing", "running", "pause-requested", "paused", "finalizing",
        "cancel-requested", "cancel-waiting", "finished", "worker-lost"
    };
    g_string_append_c(report, '{');
    text_member(report, "stage", (unsigned)event->stage < G_N_ELEMENTS(stages) ? stages[event->stage] : NULL);
    g_string_append_printf(report, ",\"exited\":%s,\"stalled\":%s,\"result\":",
                           bool_text(event->exited), bool_text(event->stalled));
    if (event->exited)
        g_string_append_printf(report, "%d", event->result);
    else
        g_string_append(report, "null");
    g_string_append_c(report, '}');
}

static void policy_fields(GString *report, const char *policy)
{
    const char *profile = NULL, *hidden = "null";
    if (policy && g_str_has_prefix(policy, "conservative;")) {
        profile = "conservative";
        policy += strlen("conservative;");
    } else if (policy && g_str_has_prefix(policy, "standard;")) {
        profile = "standard";
        policy += strlen("standard;");
    }
    if (profile && g_str_has_prefix(policy, "hidden=include;"))
        hidden = "true";
    else if (profile && g_str_has_prefix(policy, "hidden=exclude;"))
        hidden = "false";
    text_member(report, "profile", profile);
    g_string_append_printf(report, ",\"include_hidden\":%s", hidden);
}

static int issue_totals(DsStore *store, uint64_t counts[DS_REASON_IO + 1])
{
    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt, "SELECT reason FROM issues");
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            int64_t reason = sqlite3_column_int64(stmt, 0);
            if (reason <= DS_REASON_NONE || reason > DS_REASON_IO) {
                rc = SQLITE_CORRUPT;
                break;
            }
            ++counts[reason];
        }
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static DsStatus scan_summary(GString *report, DsStore *store, bool include_root,
                             DsControl *control, DsError *error)
{
    g_string_append(report, ",\n\"scan\":");
    if (!store) {
        g_string_append(report, "null");
        return DS_OK;
    }

    DsStoreInfo scan = {0};
    DsVolumeInfo volume = {0};
    uint64_t issues[DS_REASON_IO + 1] = {0};
    bool transaction = sqlite3_get_autocommit(store->db) != 0;
    int rc = transaction ? ds_sql_exec(store, "BEGIN") : SQLITE_OK;
    if (rc != SQLITE_OK)
        return ds_sql_status(store, rc, error);
    if (control)
        sqlite3_progress_handler(store->db, 1000, ds_sql_interrupt, control);
    DsStatus status = ds_store_info(store, &scan, error);
    if (status == DS_OK)
        status = ds_sql_status(store, issue_totals(store, issues), error);
    if (status == DS_OK)
        status = ds_store_volume(store, &volume, error);
    if (control)
        sqlite3_progress_handler(store->db, 0, NULL, NULL);
    if (transaction)
        ds_sql_exec(store, "ROLLBACK");
    if (status != DS_OK) {
        ds_store_info_clear(&scan);
        return status;
    }

    g_string_append_printf(report, "{\"snapshot_schema\":%d,\"run_status\":%d,"
                           "\"coverage\":\"%s\",\"aggregation_complete\":%s,",
                           store->schema_version, scan.run_status,
                           scan.coverage == DS_COVERAGE_COMPLETE ? "complete" : "partial",
                           bool_text(scan.aggregation_complete));
    count_member(report, "entries", scan.entry_count);
    g_string_append_c(report, ',');
    count_member(report, "commit_seq", (uint64_t)scan.commit_seq);
    g_string_append_c(report, ',');
    count_member(report, "unaggregated_entries", scan.unaggregated_count);
    g_string_append_c(report, ',');
    policy_fields(report, scan.policy);
    g_string_append(report, ",\"native_encoding\":");
    ds_json_string(report, ds_export_encoding(scan.root.encoding));
    g_string_append_printf(report, ",\"volume_reading\":%s,\"issues\":{",
                           bool_text(volume.captured_at > 0));
    for (DsReason reason = DS_REASON_LINK; reason <= DS_REASON_IO; ++reason) {
        if (reason != DS_REASON_LINK)
            g_string_append_c(report, ',');
        count_member(report, ds_reason_name(reason), issues[reason]);
    }
    g_string_append_c(report, '}');
    if (include_root && scan.root.raw) {
        char *display = NULL;
        if (ds_path_display(&scan.root, &display, NULL) == DS_OK) {
            g_string_append_c(report, ',');
            text_member(report, "root", display);
        }
        g_free(display);
    }
    g_string_append_c(report, '}');
    ds_store_info_clear(&scan);
    return DS_OK;
}

DsStatus ds_diagnostics_save(DsStore *reader, const DsDiagnosticsOptions *options,
                             DsControl *control, DsError *error)
{
    if (!options)
        return ds_error(error, DS_INVALID, 0, "Missing diagnostic options");
    if (ds_control_canceled(control))
        return DS_CANCELED;

    bool include_root = reader && options->include_root;
    GString *report = g_string_new("{\n\"report\":\"diskscope-diagnostics\","
                                   "\"schema_version\":1,");
    g_string_append_printf(report, "\"paths_redacted\":%s,\n", bool_text(!include_root));
    environment(report, options);
    worker_state(report, options->worker);
    DsStatus status = scan_summary(report, reader, include_root, control, error);
    if (status == DS_OK && ds_control_canceled(control))
        status = DS_CANCELED;
    if (status == DS_OK) {
        g_string_append(report, "\n}\n");
        DsOutput output;
        status = ds_output_begin(reader, &options->destination, &output, error);
        if (status == DS_OK) {
            status = ds_output_write(&output, report->str, report->len, error);
            if (status == DS_OK)
                status = ds_output_finish(&output, control, error);
            ds_output_clear(&output);
        }
    }
    g_string_free(report, true);
    return status;
}

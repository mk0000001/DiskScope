#include "cli.h"
#include "diskscope/ds_session.h"

int ds_cli_scan(int argc, char **argv)
{
    char *root = NULL, *dest = NULL, *cache = NULL, *profile = NULL, *pattern = NULL;
    gint64 budget_mib = 2048;
    gboolean hide = FALSE;
    GOptionEntry entries[] = {
        {"root", 0, 0, G_OPTION_ARG_FILENAME, &root, "Scan root", "PATH"},
        {"output", 0, 0, G_OPTION_ARG_FILENAME, &dest, "Snapshot destination", "FILE"},
        {"cache-dir", 0, 0, G_OPTION_ARG_FILENAME, &cache, "Private local cache base", "PATH"},
        {"cache-mib", 0, 0, G_OPTION_ARG_INT64, &budget_mib, "Cache budget in MiB (minimum 128)", "MIB"},
        {"profile", 0, 0, G_OPTION_ARG_STRING, &profile, "conservative or standard", "MODE"},
        {"exclude-pattern", 0, 0, G_OPTION_ARG_STRING, &pattern, "Exclude matching basenames", "GLOB"},
        {"hide-hidden", 0, 0, G_OPTION_ARG_NONE, &hide, "Exclude hidden entries", NULL},
        {0}
    };
    GOptionContext *parser = g_option_context_new("--root PATH --output FILE");
    g_option_context_add_main_entries(parser, entries, NULL);
    GError *parse = NULL;
    DsError error = {0};
    DsStatus status = DS_INVALID;
    DsScanOptions options;
    ds_scan_options_init(&options, profile && strcmp(profile, "standard") == 0 ?
                                  DS_STANDARD : DS_CONSERVATIVE);
    DsNativePath output = {0};
    DsSession *session = NULL;
    if (!g_option_context_parse(parser, &argc, &argv, &parse)) {
        ds_error(&error, DS_INVALID, parse->code, "%s", parse->message);
        g_error_free(parse);
        goto done;
    }
    if (argc != 1 || !root || !dest || g_str_has_prefix(root, "--") || g_str_has_prefix(dest, "--") ||
        (profile && strcmp(profile, "conservative") != 0 && strcmp(profile, "standard") != 0) ||
        budget_mib < 128 || budget_mib > (INT64_MAX >> 20)) {
        ds_error(&error, DS_INVALID, 0, "Use scan --root PATH --output FILE. Run scan --help for options.");
        goto done;
    }
    ds_scan_options_init(&options, profile && strcmp(profile, "standard") == 0 ?
                         DS_STANDARD : DS_CONSERVATIVE);
    options.cache_budget = (uint64_t)budget_mib << 20;
    options.include_hidden = !hide;
    options.exclude_pattern = pattern;
    if (ds_path_from_arg(root, &options.root, &error) != DS_OK ||
        ds_path_from_arg(dest, &output, &error) != DS_OK ||
        (cache && ds_path_from_arg(cache, &options.cache_path, &error) != DS_OK))
        goto done;

    ds_cli_interrupt();
    session = ds_session_start(&options, NULL, NULL, &error);
    if (!session) {
        status = error.status;
        goto done;
    }
    DsSessionEvent event, prev = {.stage = DS_WORKER_LOST};
    for (;;) {
        if (ds_cli_interrupted())
            ds_session_request_cancel(session);
        ds_session_event(session, &event);
        if (event.exited)
            break;
        if (event.stage != prev.stage || event.commit_seq != prev.commit_seq) {
            const char *stage = event.stage == DS_CANCEL_WAITING ?
                "Cancellation pending; worker still alive; partial results may change" :
                event.stage == DS_CANCEL_REQUESTED ? "Cancellation requested" :
                event.stage == DS_FINALIZING ? "Aggregating committed observations" : "Scanning";
            g_printerr("%s (%" G_GUINT64_FORMAT " entries)\n", stage, event.observations);
            prev = event;
        }
        g_usleep(50000);
    }
    status = event.result;
    if (event.detail[0])
        ds_error(&error, status, 0, "%s", event.detail);
    if (event.commit_seq) {
        DsStore *reader = NULL;
        DsError save_error = {0};
        DsStatus saved = ds_store_open(ds_session_db_path(session), DS_STORE_READER, &reader, &save_error);
        if (saved == DS_OK)
            saved = ds_snapshot_save(reader, &output, NULL, &save_error);
        ds_store_close(reader);
        if (saved != DS_OK) {
            status = saved;
            error = save_error;
        }
    }
    g_printerr("Worker exited. Observed %" G_GUINT64_FORMAT " entries.\n", event.observations);

done:
    ds_session_unref(session);
    ds_path_clear(&options.root);
    ds_path_clear(&options.cache_path);
    ds_path_clear(&output);
    g_free(root);
    g_free(dest);
    g_free(cache);
    g_free(profile);
    g_free(pattern);
    g_option_context_free(parser);
    return ds_cli_result(status, &error);
}

#include "cli.h"
#include "diskscope/ds_session.h"

typedef struct {
    DsControl *control;
    unsigned ignore;
    GMutex lock;
    GCond completed;
    bool done;
} SnapshotInterrupt;

static gpointer snapshot_interrupt(gpointer ptr)
{
    SnapshotInterrupt *state = ptr;
    g_mutex_lock(&state->lock);
    while (!state->done) {
        if (ds_cli_interrupt_count() > state->ignore) {
            ds_control_cancel(state->control);
            break;
        }
        g_cond_wait_until(&state->completed, &state->lock,
                          g_get_monotonic_time() + 20 * G_TIME_SPAN_MILLISECOND);
    }
    g_mutex_unlock(&state->lock);
    return NULL;
}

int ds_cli_scan(int argc, char **argv)
{
    char *root = NULL, *dest = NULL, *cache = NULL, *profile = NULL, *pattern = NULL;
    gint64 budget_mib = 2048;
    gboolean hide = FALSE, json = FALSE;
    char **excluded = NULL;
    GOptionEntry entries[] = {
        {"root", 0, 0, G_OPTION_ARG_FILENAME, &root, "Scan root", "PATH"},
        {"output", 0, 0, G_OPTION_ARG_FILENAME, &dest, "Snapshot destination", "FILE"},
        {"cache-dir", 0, 0, G_OPTION_ARG_FILENAME, &cache, "Private local cache base", "PATH"},
        {"cache-mib", 0, 0, G_OPTION_ARG_INT64, &budget_mib, "Cache budget in MiB (minimum 128)", "MIB"},
        {"profile", 0, 0, G_OPTION_ARG_STRING, &profile, "conservative or standard", "MODE"},
        {"exclude-pattern", 0, 0, G_OPTION_ARG_STRING, &pattern, "Exclude matching basenames", "GLOB"},
        {"exclude-path", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &excluded, "Exclude a path (repeatable)", "PATH"},
        {"json", 0, 0, G_OPTION_ARG_NONE, &json, "Write a JSON completion record to stdout", NULL},
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
    DsNativePath output = {0}, paths[16] = {{0}};
    DsSession *session = NULL;
    char **args = g_strdupv(argv);
    if (!g_option_context_parse_strv(parser, &args, &parse)) {
        ds_error(&error, DS_INVALID, parse->code, "%s", parse->message);
        g_error_free(parse);
        goto done;
    }
    argc = (int)g_strv_length(args);
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

    if (excluded) {
        options.exclude_count = g_strv_length(excluded);
        if (options.exclude_count > G_N_ELEMENTS(paths)) {
            ds_error(&error, DS_INVALID, 0, "At most 16 excluded paths can be specified");
            goto done;
        }
        options.exclude_paths = paths;
        for (size_t i = 0; i < options.exclude_count; ++i) {
            char *absolute = g_canonicalize_filename(excluded[i], NULL);
            DsStatus path_status = ds_path_from_arg(absolute, &paths[i], &error);
            g_free(absolute);
            if (path_status != DS_OK)
                goto done;
        }
    }
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
        ds_session_wait_exit(session, g_get_monotonic_time() + 50 * G_TIME_SPAN_MILLISECOND);
    }
    status = event.result;
    if (event.detail[0])
        ds_error(&error, status, 0, "%s", event.detail);
    if (event.commit_seq) {
        DsStore *reader = NULL;
        DsError save_error = {0};
        DsStatus saved = ds_store_open(ds_session_db_path(session), DS_STORE_READER, &reader, &save_error);
        if (saved == DS_OK) {
            DsControl save_control;
            ds_control_init(&save_control);
            SnapshotInterrupt stop = {.control = &save_control, .ignore = status == DS_CANCELED ? 1u : 0u};
            g_mutex_init(&stop.lock);
            g_cond_init(&stop.completed);
            GThread *watch = g_thread_new("ds-backup-interrupt", snapshot_interrupt, &stop);
            saved = ds_snapshot_save(reader, &output, &save_control, &save_error);
            g_mutex_lock(&stop.lock);
            stop.done = true;
            g_cond_signal(&stop.completed);
            g_mutex_unlock(&stop.lock);
            g_thread_join(watch);
            g_cond_clear(&stop.completed);
            g_mutex_clear(&stop.lock);
            ds_control_clear(&save_control);
        }
        ds_store_close(reader);
        if (saved != DS_OK) {
            status = saved;
            error = save_error;
        }
    }
    if (json)
        g_print("{\"result\":%d,\"observations\":\"%" G_GUINT64_FORMAT
                "\",\"commit_seq\":\"%" G_GINT64_FORMAT "\"}\n",
                status, event.observations, event.commit_seq);
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
    g_strfreev(excluded);
    for (size_t i = 0; i < G_N_ELEMENTS(paths); ++i) ds_path_clear(&paths[i]);
    g_strfreev(args);
    g_option_context_free(parser);
    return ds_cli_result(status, &error);
}

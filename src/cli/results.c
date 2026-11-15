#include "cli.h"
#include "diskscope/ds_compare.h"
#include "diskscope/ds_export.h"

typedef struct { DsControl *control; gint done; } Interrupt;

static gpointer monitor(gpointer data)
{
    Interrupt *state = data;
    while (!g_atomic_int_get(&state->done)) {
        if (ds_cli_interrupted()) {
            ds_control_cancel(state->control);
            break;
        }
        g_usleep(20000);
    }
    return NULL;
}

static int command(int argc, char **argv, bool compare)
{
    char *input = NULL, *after = NULL, *format = NULL, *output = NULL;
    gboolean bom = FALSE;
    GOptionEntry export_options[] = {
        {"input", 0, 0, G_OPTION_ARG_FILENAME, &input, "Input .dscan", "FILE"},
        {"format", 0, 0, G_OPTION_ARG_STRING, &format, "csv, json or jsonl", "FORMAT"},
        {"output", 0, 0, G_OPTION_ARG_FILENAME, &output, "Destination file", "FILE"},
        {"bom", 0, 0, G_OPTION_ARG_NONE, &bom, "Write UTF-8 BOM for CSV", NULL},
        {0}
    };
    GOptionEntry compare_options[] = {
        {"before", 0, 0, G_OPTION_ARG_FILENAME, &input, "Earlier .dscan", "FILE"},
        {"after", 0, 0, G_OPTION_ARG_FILENAME, &after, "Later .dscan", "FILE"},
        {"output", 0, 0, G_OPTION_ARG_FILENAME, &output, "JSONL difference", "FILE"},
        {0}
    };
    char **args = g_strdupv(argv);
    GOptionContext *parser = g_option_context_new(compare ? "Compare snapshots" : "Export a snapshot");
    g_option_context_set_help_enabled(parser, TRUE);
    g_option_context_add_main_entries(parser, compare ? compare_options : export_options, NULL);

    DsStatus status = DS_INVALID;
    DsError error = {0};
    DsNativePath source = {0}, second = {0}, target = {0};
    DsStore *reader = NULL, *newer = NULL;
    DsExportFormat kind = DS_EXPORT_CSV;
    GError *parse_error = NULL;
    if (!g_option_context_parse_strv(parser, &args, &parse_error)) {
        ds_error(&error, DS_INVALID, parse_error->code, "%s", parse_error->message);
        g_error_free(parse_error);
        goto done;
    }
    argc = (int)g_strv_length(args);
    if (argc != 1 || !input || !output || (compare ? !after : !format)) {
        ds_error(&error, DS_INVALID, 0, "Required option missing. Use the command with --help.");
        goto done;
    }
    if (!compare) {
        if (strcmp(format, "json") == 0)
            kind = DS_EXPORT_JSON;
        else if (strcmp(format, "jsonl") == 0)
            kind = DS_EXPORT_JSONL;
        else if (strcmp(format, "csv") != 0) {
            ds_error(&error, DS_INVALID, 0, "Expected csv, json or jsonl");
            goto done;
        }
    }
    if (ds_path_from_arg(input, &source, &error) != DS_OK ||
        ds_path_from_arg(output, &target, &error) != DS_OK ||
        (compare && ds_path_from_arg(after, &second, &error) != DS_OK))
        goto done;

    DsControl control;
    ds_control_init(&control);
    Interrupt stop = {&control, 0};
    ds_cli_interrupt();
    GThread *thread = g_thread_new("ds-cli-interrupt", monitor, &stop);
    status = ds_snapshot_open(&source, &reader, &control, &error);
    if (status == DS_OK) {
        if (compare) {
            status = ds_snapshot_open(&second, &newer, &control, &error);
            if (status == DS_OK)
                status = ds_compare_save(reader, newer, &target, &control, &error);
        } else {
            DsExportOptions options = {.format=kind, .destination=target, .bom=bom};
            status = ds_export(reader, &options, &control, &error);
        }
    }
    g_atomic_int_set(&stop.done, 1);
    g_thread_join(thread);
    ds_control_clear(&control);

done:
    ds_store_close(reader);
    ds_store_close(newer);
    ds_path_clear(&source);
    ds_path_clear(&second);
    ds_path_clear(&target);
    g_strfreev(args);
    g_option_context_free(parser);
    g_free(input);
    g_free(after);
    g_free(format);
    g_free(output);
    return ds_cli_result(status, &error);
}

int ds_cli_export(int argc, char **argv) { return command(argc, argv, false); }
int ds_cli_compare(int argc, char **argv) { return command(argc, argv, true); }

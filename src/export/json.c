#include "export_internal.h"
#include "../store/store_internal.h"
#include <errno.h>
#include <glib/gstdio.h>
#ifdef G_OS_WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

const char *ds_export_encoding(DsEncoding encoding)
{
    return encoding == DS_PATH_UTF16LE ? "utf16le" : "bytes";
}

void ds_json_string(GString *line, const char *text)
{
    g_string_append_c(line, '"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        if (*p == '\\' || *p == '"') {
            g_string_append_c(line, '\\');
            g_string_append_c(line, (char)*p);
        } else if (*p < 0x20) {
            g_string_append_printf(line, "\\u%04x", (unsigned)*p);
        } else {
            g_string_append_c(line, (char)*p);
        }
    }
    g_string_append_c(line, '"');
}

void ds_json_path(GString *line, const DsNativePath *path, const char *display)
{
    size_t length = 0;
    const void *raw = path->raw ? g_bytes_get_data(path->raw, &length) : NULL;
    char *encoded = g_base64_encode(raw, length);
    g_string_append(line, "\"display_path\":");
    ds_json_string(line, display);
    g_string_append(line, ",\"raw_path_encoding\":");
    ds_json_string(line, ds_export_encoding(path->encoding));
    g_string_append(line, ",\"raw_path_base64\":");
    ds_json_string(line, encoded);
    g_free(encoded);
}

void ds_json_size(GString *line, DsSize size, uint64_t unknown)
{
    g_string_append(line, "{\"bytes\":");
    if (size.quality == DS_KNOWN)
        g_string_append_printf(line, "\"%" G_GUINT64_FORMAT "\"", (guint64)size.bytes);
    else
        g_string_append(line, "null");
    g_string_append(line, ",\"quality\":");
    ds_json_string(line, ds_quality_name(size.quality));
    g_string_append_printf(line, ",\"unknown_count\":\"%" G_GUINT64_FORMAT "\"}", (guint64)unknown);
}

DsStatus ds_output_write(DsOutput *output, const char *data, size_t length, DsError *error)
{
    while (length) {
        unsigned count = (unsigned)MIN(length, 1024u * 1024u);
#ifdef G_OS_WIN32
        if (_setmode(output->fd, _O_BINARY) < 0)
            return ds_error(error, DS_IO, errno, "Could not set binary output mode");
        int64_t written = _write(output->fd, data, count);
#else
        int64_t written = write(output->fd, data, count);
#endif
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            int code = written == 0 ? EIO : errno;
            return ds_error(error, code == ENOSPC ? DS_LIMIT : DS_IO, code, "Could not write output");
        }
        data += written;
        length -= (size_t)written;
    }
    return DS_OK;
}

DsStatus ds_output_finish(DsOutput *output, DsControl *control, DsError *error)
{
    bool closed = g_close(output->fd, NULL);
    output->fd = -1;
    if (!closed)
        return ds_error(error, errno == ENOSPC ? DS_LIMIT : DS_IO, errno, "Could not close output");
    if (ds_control_canceled(control)) return DS_CANCELED;
    return ds_output_commit(output, error);
}

static const char *kind_name(DsKind kind)
{
    const char *names[] = {"", "directory", "file", "link", "special", "excluded"};
    return (unsigned)kind < G_N_ELEMENTS(names) ? names[kind] : "invalid";
}

static const char *const metrics[] = {"logical", "allocated", "unique_allocated"};

static const char *export_scope(const DsExportOptions *options)
{
    if (options->selected_id) return "selection";
    return options->view ? "view" : "all";
}

static bool csv_has_context(const DsExportOptions *options)
{
    return options->selected_id || options->view || options->metric != DS_LOGICAL;
}

static void metadata_json(GString *line, const DsStoreInfo *info, const DsExportOptions *options)
{
    g_string_append_printf(line, "{\"type\":\"metadata\",\"schema_version\":%d,"
        "\"app_version\":\"" DS_VERSION "\",\"scope\":\"regular-file-default-data-stream\","
        "\"run_status\":%d,\"coverage\":\"%s\",\"commit_seq\":\"%" G_GINT64_FORMAT "\","
        "\"generation\":\"%" G_GINT64_FORMAT "\",\"as_of_commit_seq\":\"%" G_GINT64_FORMAT "\","
        "\"aggregation_complete\":%s,\"aggregate_coverage\":\"%s\",\"unaggregated_count\":\"%"
        G_GUINT64_FORMAT "\",\"policy\":",
        DS_JSON_SCHEMA_VERSION, info->run_status,
        info->coverage == DS_COVERAGE_COMPLETE ? "complete" : "partial",
        (gint64)info->commit_seq, (gint64)info->generation, (gint64)info->as_of_commit_seq,
        info->aggregation_complete ? "true" : "false",
        info->aggregate_coverage == DS_COVERAGE_COMPLETE ? "complete" : "partial",
        (guint64)info->unaggregated_count);
    ds_json_string(line, info->policy);
    g_string_append_printf(line, ",\"export_scope\":\"%s\",\"selected_metric\":\"%s\",\"selected_id\":",
                           export_scope(options), metrics[options->metric]);
    if (options->selected_id)
        g_string_append_printf(line, "\"%" G_GINT64_FORMAT "\"", (gint64)options->selected_id);
    else
        g_string_append(line, "null");
    g_string_append(line, ",\"root\":{");
    ds_json_path(line, &info->root, info->root_display);
    g_string_append(line, "}}");
}

static void entry_json(GString *line, const DsRow *row, const DsNativePath *relative,
                       const char *display)
{
    g_string_append_printf(line, "{\"type\":\"entry\",\"id\":\"%" G_GINT64_FORMAT
        "\",\"parent_id\":\"%" G_GINT64_FORMAT "\",\"representative_id\":\"%" G_GINT64_FORMAT
        "\",\"aggregated\":%s,\"kind\":", (gint64)row->entry.id, (gint64)row->entry.parent_id,
        (gint64)row->representative_id, row->aggregated ? "true" : "false");
    ds_json_string(line, kind_name(row->entry.kind));
    g_string_append(line, ",\"reason\":");
    ds_json_string(line, ds_reason_name(row->entry.reason));
    g_string_append(line, ",");
    ds_json_path(line, relative, display);
    for (DsMetric metric = DS_LOGICAL; metric < DS_METRIC_COUNT; ++metric) {
        g_string_append_printf(line, ",\"%s\":", metrics[metric]);
        ds_json_size(line, row->metrics[metric], row->unknown[metric]);
    }
    g_string_append(line, "}");
}

static void entry_csv(GString *line, const DsRow *row, const DsNativePath *relative,
                      const char *display, const DsExportOptions *options)
{
    size_t length = 0;
    const void *raw = g_bytes_get_data(relative->raw, &length);
    char *encoded = g_base64_encode(raw, length);
    ds_csv_field(line, display, true);
    g_string_append_printf(line, ",%s,", ds_export_encoding(relative->encoding));
    ds_csv_field(line, encoded, true);
    g_free(encoded);
    g_string_append_printf(line, ",%s,%s", kind_name(row->entry.kind), ds_reason_name(row->entry.reason));
    for (DsMetric metric = DS_LOGICAL; metric < DS_METRIC_COUNT; ++metric) {
        g_string_append_c(line, ',');
        if (row->metrics[metric].quality == DS_KNOWN)
            g_string_append_printf(line, "%" G_GUINT64_FORMAT, (guint64)row->metrics[metric].bytes);
        g_string_append_printf(line, ",%s,%" G_GUINT64_FORMAT,
            ds_quality_name(row->metrics[metric].quality), (guint64)row->unknown[metric]);
    }
    if (csv_has_context(options)) {
        g_string_append_printf(line, ",%s,%s,", export_scope(options), metrics[options->metric]);
        if (options->selected_id)
            g_string_append_printf(line, "%" G_GINT64_FORMAT, (gint64)options->selected_id);
    }
    g_string_append(line, "\r\n");
}

static void header(GString *line, const DsStoreInfo *info, const DsExportOptions *options)
{
    switch (options->format) {
    case DS_EXPORT_CSV:
        if (options->bom) g_string_append(line, "\xef\xbb\xbf");
        g_string_append(line,
            "display_path,raw_path_encoding,raw_path_base64,kind,reason,logical_bytes,logical_quality,"
            "logical_unknown_count,allocated_bytes,allocated_quality,allocated_unknown_count,"
            "unique_allocated_bytes,unique_quality,unique_unknown_count");
        if (csv_has_context(options))
            g_string_append(line, ",export_scope,selected_metric,selected_id");
        g_string_append(line, "\r\n");
        break;
    case DS_EXPORT_JSON:
        g_string_append(line, "{\"metadata\":");
        metadata_json(line, info, options);
        g_string_append(line, ",\"entries\":[\n");
        break;
    case DS_EXPORT_JSONL:
        metadata_json(line, info, options);
        g_string_append_c(line, '\n');
        break;
    }
}

typedef struct {
    DsStore *reader;
    const DsExportOptions *options;
    DsControl *control;
    DsOutput *output;
    GString *line;
    uint64_t entries;
} ExportStream;

static DsStatus write_entry(ExportStream *stream, const DsRow *row, DsError *error)
{
    if (ds_control_canceled(stream->control)) return DS_CANCELED;
    DsNativePath relative = {0};
    char *display = NULL;
    DsStatus status = ds_store_relative_path(stream->reader, row->entry.id, &relative, error);
    if (status == DS_OK) status = ds_path_display(&relative, &display, error);
    if (status == DS_OK) {
        GString *line = stream->line;
        const DsExportOptions *options = stream->options;
        g_string_truncate(line, 0);
        if (options->format == DS_EXPORT_CSV) {
            entry_csv(line, row, &relative, display, options);
        } else {
            if (options->format == DS_EXPORT_JSON && stream->entries) g_string_append(line, ",\n");
            entry_json(line, row, &relative, display);
            if (options->format == DS_EXPORT_JSONL) g_string_append_c(line, '\n');
        }
        status = ds_output_write(stream->output, line->str, line->len, error);
        if (status == DS_OK) ++stream->entries;
    }
    g_free(display);
    ds_path_clear(&relative);
    return status;
}

static DsStatus write_view(ExportStream *stream, DsError *error)
{
    const DsQuery *view = stream->options->view;
    DsQuery query = view ? *view : (DsQuery){.parent_id = DS_QUERY_ALL};
    query.sort = DS_SORT_ID;
    query.descending = false;
    query.offset = 0;
    query.after_id = 0;
    query.limit = 128;
    query.omit_total = true;
    DsStatus status = DS_OK;
    while (status == DS_OK) {
        DsPage *page = NULL;
        status = ds_query_page(stream->reader, &query, &page, stream->control, error);
        if (status != DS_OK) break;
        size_t count = page->count;
        for (size_t i = 0; status == DS_OK && i < count; ++i) {
            const DsRow *row = &page->rows[i];
            status = write_entry(stream, row, error);
            query.after_id = row->entry.id;
        }
        ds_page_free(page);
        if (!count) break;
    }
    return status;
}

static int lookup_id(sqlite3_stmt *stmt, int64_t id, int64_t *next)
{
    int rc = sqlite3_reset(stmt);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    *next = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static DsStatus write_selection(ExportStream *stream, int64_t generation, DsError *error)
{
    DsStore *reader = stream->reader;
    sqlite3_stmt *entry = NULL, *child = NULL, *sibling = NULL;
    int rc = ds_query_prepare_entry(reader, &entry, generation);
    if (rc == SQLITE_OK)
        rc = ds_sql_prepare(reader, &child,
            "SELECT id FROM entries WHERE parent_id=?1 ORDER BY display,id LIMIT 1");
    if (rc == SQLITE_OK)
        /* Seek both index ranges so equal display names do not need a linear scan. */
        rc = ds_sql_prepare(reader, &sibling,
            "SELECT coalesce("
            " (SELECT id FROM entries WHERE parent_id=e.parent_id AND display=e.display"
            "   AND id>e.id ORDER BY id LIMIT 1),"
            " (SELECT id FROM entries WHERE parent_id=e.parent_id AND display>e.display"
            "   ORDER BY display,id LIMIT 1))"
            " FROM entries e WHERE e.id=?1");
    int64_t ancestors[DS_DEPTH_MAX];
    size_t depth = 0;
    int64_t id = stream->options->selected_id;
    DsStatus status = ds_sql_status(reader, rc, error);
    while (status == DS_OK && id) {
        if (ds_control_canceled(stream->control)) { status = DS_CANCELED; break; }
        DsRow row = {0};
        rc = ds_query_step_entry(entry, id, &row);
        if (rc == SQLITE_DONE)
            status = ds_error(error, DS_INVALID, 0, "Selected export entry does not exist");
        else
            status = ds_sql_status(reader, rc, error);
        if (status == DS_OK) status = write_entry(stream, &row, error);
        bool directory = row.entry.kind == DS_DIRECTORY;
        ds_query_clear_row(&row);
        if (status != DS_OK) break;

        int64_t next = 0;
        if (directory) rc = lookup_id(child, id, &next);
        if (rc == SQLITE_OK && next) {
            if (depth == G_N_ELEMENTS(ancestors)) {
                status = ds_error(error, DS_LIMIT, 0, "Selected export exceeds the path depth limit");
                break;
            }
            ancestors[depth++] = id;
        } else {
            while (rc == SQLITE_OK && !next && depth) {
                rc = lookup_id(sibling, id, &next);
                if (!next) id = ancestors[--depth];
            }
        }
        id = next;
        status = ds_sql_status(reader, rc, error);
    }
    sqlite3_finalize(sibling);
    sqlite3_finalize(child);
    sqlite3_finalize(entry);
    return status;
}

DsStatus ds_export(DsStore *reader, const DsExportOptions *options,
                   DsControl *control, DsError *error)
{
    if (!reader || !options || options->format < DS_EXPORT_CSV || options->format > DS_EXPORT_JSONL ||
        options->metric < DS_LOGICAL || options->metric >= DS_METRIC_COUNT ||
        options->selected_id < 0 || (options->selected_id && options->view))
        return ds_error(error, DS_INVALID, 0, "Invalid export request");
    if (ds_control_canceled(control)) return DS_CANCELED;
    DsOutput output;
    DsStatus status = ds_output_begin(reader, &options->destination, &output, error);
    if (status != DS_OK) return status;
    bool transaction = sqlite3_get_autocommit(reader->db) != 0;
    if (transaction) status = ds_sql_status(reader, ds_sql_exec(reader, "BEGIN"), error);
    DsStoreInfo info = {0};
    if (status == DS_OK) status = ds_store_info(reader, &info, error);
    GString *line = g_string_new(NULL);
    ExportStream stream = {reader, options, control, &output, line, 0};
    if (status == DS_OK) {
        header(line, &info, options);
        status = ds_output_write(&output, line->str, line->len, error);
    }
    if (status == DS_OK)
        status = options->selected_id ? write_selection(&stream, info.generation, error)
                                      : write_view(&stream, error);
    ds_store_info_clear(&info);
    if (status == DS_OK && options->format != DS_EXPORT_CSV) {
        g_string_truncate(line, 0);
        if (options->format == DS_EXPORT_JSON) g_string_append(line, "\n],\"footer\":");
        g_string_append_printf(line, "{\"type\":\"footer\",\"entries\":\"%" G_GUINT64_FORMAT "\"}",
                               (guint64)stream.entries);
        if (options->format == DS_EXPORT_JSON) g_string_append_c(line, '}');
        g_string_append_c(line, '\n');
        status = ds_output_write(&output, line->str, line->len, error);
    }
    if (transaction) ds_sql_exec(reader, "ROLLBACK");
    if (status == DS_OK) status = ds_output_finish(&output, control, error);
    g_string_free(line, true);
    ds_output_clear(&output);
    return status;
}

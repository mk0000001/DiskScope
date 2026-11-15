#include "store_internal.h"

static void extension_matches(sqlite3_context *context, int argc, sqlite3_value **args)
{
    (void)argc;
    const char *name = (const char *)sqlite3_value_text(args[0]);
    const char *filter = (const char *)sqlite3_value_text(args[1]);
    if (!name || !filter) { sqlite3_result_int(context, 0); return; }
    const char *dot = strrchr(name, '.');
    const char *extension = dot && dot != name ? dot + 1 : "";
    sqlite3_result_int(context, g_ascii_strcasecmp(extension, filter) == 0);
}

static void path_matches(sqlite3_context *context, int argc, sqlite3_value **args)
{
    (void)argc;
    DsStore *store = sqlite3_user_data(context);
    const char *filter = (const char *)sqlite3_value_text(args[1]);
    if (!filter) { sqlite3_result_int(context, 0); return; }

    DsNativePath relative = {0};
    DsError error = {0};
    DsStatus status = ds_store_relative_path(store, sqlite3_value_int64(args[0]), &relative, &error);
    if (status != DS_OK) {
        sqlite3_result_error_code(context, error.os_code ? (int)error.os_code : SQLITE_CORRUPT);
        return;
    }
    char *display = NULL;
    status = ds_path_display(&relative, &display, NULL);
    ds_path_clear(&relative);
    if (status != DS_OK) {
        sqlite3_result_error_code(context, SQLITE_CORRUPT);
        return;
    }
    char *folded = g_ascii_strdown(display, -1);
    sqlite3_result_int(context, strstr(folded, filter) != NULL);
    g_free(folded);
    g_free(display);
}

int ds_query_register_functions(DsStore *store)
{
    int rc = sqlite3_create_function(store->db, "ds_extension_matches", 2,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_DIRECTONLY, NULL, extension_matches, NULL, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_create_function(store->db, "ds_path_matches", 2,
        SQLITE_UTF8 | SQLITE_DIRECTONLY, store, path_matches, NULL, NULL);
    return rc;
}

static int read_info(DsStore *store, DsStoreInfo *out)
{
    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt,
        "SELECT s.root_raw,s.encoding,s.root_display,s.policy,s.run_status,s.coverage,"
        " s.commit_seq,s.active_generation,g.as_of_commit_seq,g.aggregation_complete,"
        " g.coverage,s.entry_count,g.entry_count,s.started_at,s.ended_at"
        " FROM scans s LEFT JOIN generations g ON g.id=s.active_generation WHERE s.id=1");
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        rc = SQLITE_OK;
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            DsStatus status = ds_path_new(sqlite3_column_int(stmt, 1),
                sqlite3_column_blob(stmt, 0), (size_t)sqlite3_column_bytes(stmt, 0), &out->root, NULL);
            if (status != DS_OK) rc = SQLITE_CORRUPT;
        }
        if (rc == SQLITE_OK) {
            out->root_display = g_strdup((const char *)sqlite3_column_text(stmt, 2));
            out->policy = g_strdup((const char *)sqlite3_column_text(stmt, 3));
            out->run_status = sqlite3_column_int(stmt, 4);
            out->coverage = sqlite3_column_int(stmt, 5);
            out->commit_seq = sqlite3_column_int64(stmt, 6);
            out->generation = sqlite3_column_int64(stmt, 7);
            out->as_of_commit_seq = sqlite3_column_int64(stmt, 8);
            out->aggregation_complete = sqlite3_column_int(stmt, 9) != 0;
            out->aggregate_coverage = sqlite3_column_int(stmt, 10);
            int64_t entries = sqlite3_column_int64(stmt, 11), aggregated = sqlite3_column_int64(stmt, 12);
            if (entries < 0 || aggregated < 0 || aggregated > entries) {
                rc = SQLITE_CORRUPT;
            } else {
                out->entry_count = (uint64_t)entries;
                out->unaggregated_count = (uint64_t)(entries - aggregated);
            }
            out->started_at = sqlite3_column_int64(stmt, 13);
            out->ended_at = sqlite3_column_int64(stmt, 14);
        }
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_CORRUPT;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) ds_store_info_clear(out);
    return rc;
}

DsStatus ds_store_info(DsStore *store, DsStoreInfo *out, DsError *error)
{
    if (!store || !out) return ds_error(error, DS_INVALID, 0, "Missing store or metadata output");
    *out = (DsStoreInfo){0};
    return ds_sql_status(store, read_info(store, out), error);
}

void ds_store_info_clear(DsStoreInfo *info)
{
    if (!info) return;
    ds_path_clear(&info->root);
    g_free(info->root_display);
    g_free(info->policy);
    *info = (DsStoreInfo){0};
}

DsStatus ds_store_relative_path(DsStore *store, int64_t id, DsNativePath *out, DsError *error)
{
    if (!store || !out || id <= 0) return ds_error(error, DS_INVALID, 0, "Invalid path request");
    *out = (DsNativePath){0};
    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt, "SELECT parent_id,name,name_encoding FROM entries WHERE id=?1");
    if (rc != SQLITE_OK) return ds_sql_status(store, rc, error);
    size_t length = 0;
    DsEncoding encoding = 0;
    GPtrArray *parts = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
    while (rc == SQLITE_OK && id > 0) {
        if (parts->len > DS_DEPTH_MAX) { rc = SQLITE_TOOBIG; break; }
        sqlite3_reset(stmt);
        rc = sqlite3_bind_int64(stmt, 1, id);
        if (rc != SQLITE_OK) break;
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            if (rc == SQLITE_DONE) rc = SQLITE_CORRUPT;
            break;
        }
        int64_t parent = sqlite3_column_int64(stmt, 0);
        const void *name = sqlite3_column_blob(stmt, 1);
        size_t size = (size_t)sqlite3_column_bytes(stmt, 1);
        DsEncoding actual = sqlite3_column_int(stmt, 2);
        if (!encoding) encoding = actual;
        if (actual != encoding || (encoding != DS_PATH_BYTES && encoding != DS_PATH_UTF16LE) ||
            (id == 1 && (parent || size)) || (id != 1 && (parent < 1 || parent >= id ||
            !ds_path_component_valid(encoding, name, size)))) {
            rc = SQLITE_CORRUPT;
            break;
        }
        if (id != 1) {
            size_t separator = parts->len == 0 ? 0 : encoding == DS_PATH_BYTES ? 1 : 2;
            if (size + separator > DS_PATH_MAX - length || parts->len == DS_DEPTH_MAX) {
                rc = SQLITE_TOOBIG;
                break;
            }
            length += size + separator;
            g_ptr_array_add(parts, g_bytes_new(name, size));
        }
        id = parent;
        rc = SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    DsStatus status = ds_sql_status(store, rc, error);
    if (status == DS_OK) {
        GByteArray *raw = g_byte_array_sized_new((guint)length);
        const uint8_t slash = '/', backslash[] = {'\\', 0};
        for (guint i = parts->len; i > 0; --i) {
            gsize size = 0;
            const uint8_t *part = g_bytes_get_data(g_ptr_array_index(parts, i - 1), &size);
            if (i != parts->len) {
                if (encoding == DS_PATH_BYTES) g_byte_array_append(raw, &slash, 1);
                else g_byte_array_append(raw, backslash, sizeof backslash);
            }
            g_byte_array_append(raw, part, (guint)size);
        }
        *out = (DsNativePath){encoding, g_byte_array_free_to_bytes(raw)};
    }
    g_ptr_array_unref(parts);
    return status;
}

static void clear_row(DsRow *row)
{
    ds_path_clear(&row->entry.name);
    g_free(row->display);
}

void ds_page_free(DsPage *page)
{
    if (!page) return;
    for (size_t i = 0; i < page->count; ++i) clear_row(&page->rows[i]);
    g_free(page->rows);
    ds_store_info_clear(&page->info);
    g_free(page);
}

static int read_row(sqlite3_stmt *stmt, DsRow *row)
{
    DsEntry *e = &row->entry;
    e->id = sqlite3_column_int64(stmt, 0);
    e->parent_id = sqlite3_column_int64(stmt, 1);
    DsStatus status = ds_path_new(sqlite3_column_int(stmt, 3),
        sqlite3_column_blob(stmt, 2), (size_t)sqlite3_column_bytes(stmt, 2), &e->name, NULL);
    if (status != DS_OK) return SQLITE_CORRUPT;
    row->display = g_strdup((const char *)sqlite3_column_text(stmt, 4));
    e->kind = sqlite3_column_int(stmt, 5);
    e->reason = sqlite3_column_int(stmt, 6);
    e->os_code = sqlite3_column_int64(stmt, 7);
    if (!row->display || !ds_sql_read_size(stmt, 8, &e->logical) ||
        !ds_sql_read_size(stmt, 10, &e->allocated) || !ds_sql_read_u64(stmt, 13, &e->links))
        return SQLITE_CORRUPT;
    e->identity_known = sqlite3_column_type(stmt, 12) != SQLITE_NULL;
    if (e->identity_known) {
        if ((size_t)sqlite3_column_bytes(stmt, 12) != sizeof e->identity) return SQLITE_CORRUPT;
        memcpy(e->identity, sqlite3_column_blob(stmt, 12), sizeof e->identity);
    }
    e->mtime_known = sqlite3_column_type(stmt, 14) != SQLITE_NULL;
    e->mtime = sqlite3_column_int64(stmt, 14);
    row->aggregated = sqlite3_column_type(stmt, 15) != SQLITE_NULL;
    if (row->aggregated) {
        for (DsMetric metric = DS_LOGICAL; metric < DS_METRIC_COUNT; ++metric) {
            int col = 16 + metric * 3;
            if (!ds_sql_read_size(stmt, col, &row->metrics[metric])) return SQLITE_CORRUPT;
            int64_t unknown = sqlite3_column_int64(stmt, col + 2);
            if (unknown < 0) return SQLITE_CORRUPT;
            row->unknown[metric] = (uint64_t)unknown;
        }
        row->representative_id = sqlite3_column_int64(stmt, 25);
        row->entry_count = (uint64_t)sqlite3_column_int64(stmt, 26);
        row->file_count = (uint64_t)sqlite3_column_int64(stmt, 27);
        row->unique_file_count = (uint64_t)sqlite3_column_int64(stmt, 28);
    } else {
        for (DsMetric metric = DS_LOGICAL; metric < DS_METRIC_COUNT; ++metric)
            row->metrics[metric].quality = DS_UNAVAILABLE;
        if (e->kind == DS_FILE) {
            row->metrics[DS_LOGICAL] = e->logical;
            row->metrics[DS_ALLOCATED] = e->allocated;
            row->unknown[DS_LOGICAL] = e->logical.quality != DS_KNOWN;
            row->unknown[DS_ALLOCATED] = e->allocated.quality != DS_KNOWN;
        }
    }
    return SQLITE_OK;
}

static const char *metric_sql[] = {
    "CASE WHEN a.node_id IS NOT NULL THEN a.logical WHEN e.kind=2 THEN e.logical END",
    "CASE WHEN a.node_id IS NOT NULL THEN a.allocated WHEN e.kind=2 THEN e.allocated END",
    "a.unique_allocated",
};

static void query_filter(GString *sql, const DsQuery *query, bool cursor)
{
    g_string_append(sql, " WHERE 1");
    if (query->parent_id == 0) g_string_append(sql, " AND e.parent_id IS NULL");
    if (query->parent_id > 0) g_string_append(sql, " AND e.parent_id=?1");
    if (query->kind) g_string_append(sql, " AND e.kind=?2");
    if (query->filter && *query->filter)
        g_string_append(sql, query->search_path ? " AND ds_path_matches(e.id,lower(?3))"
            : " AND instr(lower(e.display),lower(?3))>0");
    if (cursor && query->after_id) g_string_append(sql, " AND e.id>?7");
    if (query->extension) g_string_append(sql, " AND e.kind=2 AND ds_extension_matches(e.display,?8)");
    if (query->issues_only) g_string_append(sql, " AND e.reason<>0");
    if (query->has_min_bytes) g_string_append_printf(sql, " AND (%s)>=?9", metric_sql[query->metric]);
    if (query->has_max_bytes) g_string_append_printf(sql, " AND (%s)<=?10", metric_sql[query->metric]);
    if (query->has_mtime_from) g_string_append(sql, " AND e.mtime>=?11");
    if (query->has_mtime_to) g_string_append(sql, " AND e.mtime<=?12");
}

static int bind_filter(sqlite3_stmt *stmt, const DsQuery *query)
{
    int rc = SQLITE_OK;
    if (query->parent_id > 0) rc = sqlite3_bind_int64(stmt, 1, query->parent_id);
    if (rc == SQLITE_OK && query->kind) rc = sqlite3_bind_int(stmt, 2, query->kind);
    if (rc == SQLITE_OK && query->filter && *query->filter)
        rc = sqlite3_bind_text(stmt, 3, query->filter, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK && query->extension) rc = sqlite3_bind_text(stmt, 8, query->extension, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK && query->has_min_bytes) rc = ds_sql_bind_u64(stmt, 9, query->min_bytes);
    if (rc == SQLITE_OK && query->has_max_bytes) rc = ds_sql_bind_u64(stmt, 10, query->max_bytes);
    if (rc == SQLITE_OK && query->has_mtime_from) rc = sqlite3_bind_int64(stmt, 11, query->mtime_from);
    if (rc == SQLITE_OK && query->has_mtime_to) rc = sqlite3_bind_int64(stmt, 12, query->mtime_to);
    return rc;
}

static int query_total(DsStore *store, const DsQuery *query, DsPage *page)
{
    if (query->omit_total) return SQLITE_OK;
    bool needs_aggregates = query->has_min_bytes || query->has_max_bytes;
    if (query->parent_id == DS_QUERY_ALL && !query->kind && (!query->filter || !*query->filter) &&
        !query->extension && !query->issues_only && !needs_aggregates &&
        !query->has_mtime_from && !query->has_mtime_to) {
        page->total = page->info.entry_count;
        return SQLITE_OK;
    }
    GString *sql = g_string_new("SELECT count(*) FROM entries e");
    if (needs_aggregates)
        g_string_append(sql, " LEFT JOIN aggregates a ON a.generation=?4 AND a.node_id=e.id");
    query_filter(sql, query, false);
    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt, sql->str);
    g_string_free(sql, true);
    if (rc == SQLITE_OK) rc = bind_filter(stmt, query);
    if (rc == SQLITE_OK && needs_aggregates)
        rc = sqlite3_bind_int64(stmt, 4, page->info.generation);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            page->total = (uint64_t)sqlite3_column_int64(stmt, 0);
            rc = SQLITE_OK;
        }
    }
    sqlite3_finalize(stmt);
    return rc;
}

static void query_sort(GString *sql, const DsQuery *query)
{
    const char *direction = query->descending ? "DESC" : "ASC";
    switch (query->sort) {
    case DS_SORT_ID:
        g_string_append_printf(sql, " ORDER BY e.id %s", direction);
        break;
    case DS_SORT_NAME:
        g_string_append_printf(sql, " ORDER BY e.display %s,e.id ASC", direction);
        break;
    case DS_SORT_METRIC: {
        const char *expr = metric_sql[query->metric];
        g_string_append_printf(sql, " ORDER BY (%s) IS NULL,%s %s,e.id ASC", expr, expr, direction);
        break;
    }
    }
}

static int query_rows(DsStore *store, const DsQuery *query, DsPage *page, size_t limit)
{
    GString *sql = g_string_new(
        "SELECT e.id,e.parent_id,e.name,e.name_encoding,e.display,e.kind,e.reason,e.os_code,"
        " e.logical,e.logical_quality,e.allocated,e.allocated_quality,e.identity,e.links,e.mtime,"
        " a.node_id,a.logical,a.logical_quality,a.logical_unknown,a.allocated,a.allocated_quality,"
        " a.allocated_unknown,a.unique_allocated,a.unique_quality,a.unique_unknown,a.representative_id,"
        " a.entry_count,a.file_count,a.unique_file_count"
        " FROM entries e LEFT JOIN aggregates a ON a.generation=?4 AND a.node_id=e.id");
    query_filter(sql, query, true);
    query_sort(sql, query);
    g_string_append(sql, " LIMIT ?5 OFFSET ?6");
    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt, sql->str);
    g_string_free(sql, true);
    if (rc == SQLITE_OK) rc = bind_filter(stmt, query);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 4, page->info.generation);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 5, (int64_t)limit);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 6, (int64_t)query->offset);
    if (rc == SQLITE_OK && query->after_id) rc = sqlite3_bind_int64(stmt, 7, query->after_id);

    size_t bytes = 0;
    while (rc == SQLITE_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        size_t row_bytes = (size_t)sqlite3_column_bytes(stmt, 2) +
                           (size_t)sqlite3_column_bytes(stmt, 4) + sizeof(DsRow);
        if (page->count && row_bytes > DS_QUEUE_BYTES - bytes) {
            rc = SQLITE_DONE;
            break;
        }
        bytes += row_bytes;
        rc = read_row(stmt, &page->rows[page->count++]);
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

DsStatus ds_query_page(DsStore *store, const DsQuery *query, DsPage **out,
                       DsControl *control, DsError *error)
{
    if (!store || !query || !out || query->parent_id < DS_QUERY_ALL || query->kind < 0 ||
        query->kind > DS_EXCLUDED || query->sort < DS_SORT_ID || query->sort > DS_SORT_METRIC ||
        query->metric < DS_LOGICAL || query->metric >= DS_METRIC_COUNT ||
        query->offset > INT64_MAX || query->limit > DS_PAGE_MAX || query->after_id < 0 ||
        (query->after_id && (query->sort != DS_SORT_ID || query->descending || query->offset)) ||
        (query->filter && (strlen(query->filter) > DS_PATH_MAX || !g_utf8_validate(query->filter, -1, NULL))) ||
        (query->extension && (strlen(query->extension) > DS_PATH_MAX ||
         !g_utf8_validate(query->extension, -1, NULL))) ||
        (query->has_min_bytes && query->has_max_bytes && query->min_bytes > query->max_bytes) ||
        (query->has_mtime_from && query->has_mtime_to && query->mtime_from > query->mtime_to))
        return ds_error(error, DS_INVALID, 0, "Invalid page request");
    *out = NULL;
    if (ds_sql_progress(control)) return ds_error(error, DS_CANCELED, 0, "Query canceled");
    size_t limit = query->limit ? query->limit : 128;
    DsPage *page = g_new0(DsPage, 1);
    page->rows = g_new0(DsRow, limit);

    bool transaction = sqlite3_get_autocommit(store->db) != 0;
    int rc = transaction ? ds_sql_exec(store, "BEGIN") : SQLITE_OK;
    if (control) sqlite3_progress_handler(store->db, 1000, ds_sql_interrupt, control);
    if (rc == SQLITE_OK) rc = read_info(store, &page->info);
    if (rc == SQLITE_OK) rc = query_total(store, query, page);
    if (rc == SQLITE_OK) rc = query_rows(store, query, page, limit);
    if (control) sqlite3_progress_handler(store->db, 0, NULL, NULL);
    if (rc == SQLITE_OK && ds_sql_progress(control)) rc = SQLITE_INTERRUPT;
    if (transaction) {
        if (rc == SQLITE_OK) rc = ds_sql_exec(store, "COMMIT");
        if (rc != SQLITE_OK) ds_sql_exec(store, "ROLLBACK");
    }
    if (rc == SQLITE_OK) *out = page;
    else ds_page_free(page);
    return ds_sql_status(store, rc, error);
}

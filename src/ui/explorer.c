#include "ui.h"

DsMetric ui_metric(View *v)
{
    return (DsMetric)gtk_drop_down_get_selected(v->metric);
}

static char *format_metric(const DsRow *row, DsMetric metric)
{
    DsSize value = row->metrics[metric];
    if (value.quality == DS_OVERFLOW)
        return g_strdup(UI("범위 초과"));
    if (value.quality != DS_KNOWN)
        return g_strdup(!row->aggregated && (row->entry.kind == DS_DIRECTORY ||
                        (row->entry.kind == DS_FILE && metric == DS_UNIQUE)) ?
                        UI("집계 전") : UI("확인 못함"));
    char *bytes = g_format_size_full(value.bytes, G_FORMAT_SIZE_IEC_UNITS);
    if (!row->unknown[metric])
        return bytes;
    char *partial = g_strdup_printf(UI("%s · 미확인 %" G_GUINT64_FORMAT "개"),
                                    bytes, row->unknown[metric]);
    g_free(bytes);
    return partial;
}

static const char *reason(DsReason r)
{
    const char *reasons[] = {
        "", UI("링크 제외"), UI("클라우드 자리 표시자 제외"), UI("다른 마운트 제외"),
        UI("특수 파일 제외"), UI("범위 설정으로 제외"), UI("캐시 제외"), UI("접근 거부"),
        UI("스캔 중 변경됨"), UI("메타데이터 미확인"), UI("탐색 한도"), UI("입출력 오류")
    };
    return (unsigned)r < G_N_ELEMENTS(reasons) ? reasons[r] : UI("알 수 없음");
}

static const DsRow *model_row(GObject *object)
{
    if (!GTK_IS_TREE_LIST_ROW(object))
        return object ? ds_ui_row_get(object) : NULL;
    GObject *item = gtk_tree_list_row_get_item(GTK_TREE_LIST_ROW(object));
    const DsRow *row = item ? ds_ui_row_get(item) : NULL;
    g_clear_object(&item);
    return row;  /* Kept alive by the tree row. */
}

static void setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory;
    GtkWidget *text = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
    if (GPOINTER_TO_INT(data) == 0) {
        GtkWidget *expander = gtk_tree_expander_new();
        gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), text);
        gtk_list_item_set_child(item, expander);
    } else {
        gtk_list_item_set_child(item, text);
    }
}

static void bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory;
    int column = GPOINTER_TO_INT(data);
    GObject *object = gtk_list_item_get_item(item);
    const DsRow *row = model_row(object);
    GtkWidget *text = gtk_list_item_get_child(item);
    if (column == 0) {
        GtkTreeExpander *expander = GTK_TREE_EXPANDER(text);
        gtk_tree_expander_set_list_row(expander, GTK_IS_TREE_LIST_ROW(object) ?
                                       GTK_TREE_LIST_ROW(object) : NULL);
        ui_label(gtk_tree_expander_get_child(expander), row ? row->display : UI("불러오는 중…"));
        return;
    }
    char *value = NULL;
    if (row) {
        if (column <= 3) value = format_metric(row, (DsMetric)(column - 1));
        else if (column == 4)
            value = g_strdup(row->entry.reason ? reason(row->entry.reason) :
                row->representative_id > 0 && row->representative_id != row->entry.id ?
                UI("하드링크 공유") : "");
        else if (column == 5 && row->entry.mtime_known) {
            GDateTime *date = g_date_time_new_from_unix_local(row->entry.mtime);
            if (date) {
                value = g_date_time_format(date, "%Y-%m-%d %H:%M");
                g_date_time_unref(date);
            }
        }
    }
    ui_label(text, value);
    g_free(value);
}

static void activated(GtkColumnView *view, guint position, gpointer data)
{
    (void)data;
    GListModel *model = G_LIST_MODEL(gtk_column_view_get_model(view));
    GObject *object = model ? g_list_model_get_item(model, position) : NULL;
    if (GTK_IS_TREE_LIST_ROW(object)) {
        GtkTreeListRow *r = GTK_TREE_LIST_ROW(object);
        gtk_tree_list_row_set_expanded(r, !gtk_tree_list_row_get_expanded(r));
    }
    g_clear_object(&object);
}

void ui_columns(View *v)
{
    const char *names[] = {UI("이름"), UI("논리 크기"), UI("할당량"), UI("중복 제외"), UI("상태"), UI("수정일")};
    gtk_column_view_set_show_row_separators(v->table, TRUE);
    g_signal_connect(v->table, "activate", G_CALLBACK(activated), v);
    for (size_t i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
        g_signal_connect(factory, "setup", G_CALLBACK(setup), GINT_TO_POINTER((int)i));
        g_signal_connect(factory, "bind", G_CALLBACK(bind), GINT_TO_POINTER((int)i));
        GtkColumnViewColumn *column = gtk_column_view_column_new(names[i], factory);
        gtk_column_view_column_set_resizable(column, TRUE);
        gtk_column_view_column_set_expand(column, i == 0);
        gtk_column_view_append_column(v->table, column);
        g_object_unref(column);
    }
}

static void live_model_gone(gpointer data, GObject *model)
{
    View *v = data;
    g_queue_remove(&v->live_models, model);
}

void ui_live_clear(View *v)
{
    while (!g_queue_is_empty(&v->live_models)) {
        GObject *model = g_queue_pop_head(&v->live_models);
        g_object_weak_unref(model, live_model_gone, v);
    }
    v->live_pending = 0;
    v->live_tree = false;
    v->expand_root = false;
    v->preview_pending = false;
    v->list_pending = false;
}

static GListModel *child_model(gpointer item, gpointer ptr)
{
    View *v = ptr;
    const DsRow *row = ds_ui_row_get(G_OBJECT(item));
    if (!row || row->entry.kind != DS_DIRECTORY)
        return NULL;
    DsQuery q = {.parent_id=row->entry.id, .metric=ui_metric(v), .limit=DS_PAGE_MAX,
                 .sort=v->live_tree ? DS_SORT_ID : DS_SORT_METRIC, .descending=!v->live_tree};
    GListModel *model = ds_paged_model_new(&v->db, &q);
    if (v->live_tree) {
        g_queue_push_tail(&v->live_models, model);
        g_object_weak_ref(G_OBJECT(model), live_model_gone, v);
    }
    return model;
}

static void expand_live_root(View *v)
{
    if (!v->expand_root)
        return;
    if (!g_list_model_get_n_items(v->base)) {
        if (!ds_paged_model_busy(v->base))
            ds_paged_model_refresh(v->base);
        return;
    }
    GtkTreeListModel *tree = GTK_TREE_LIST_MODEL(gtk_single_selection_get_model(v->selection));
    GtkTreeListRow *root = gtk_tree_list_model_get_row(tree, 0);
    if (root) {
        v->expand_root = false;
        gtk_tree_list_row_set_expanded(root, TRUE);
        g_object_unref(root);
    }
}

static bool parse_bytes(GtkWidget *input, uint64_t *out)
{
    const char *text = gtk_editable_get_text(GTK_EDITABLE(input));
    guint64 bytes;
    if (*text && g_ascii_string_to_unsigned(text, 10, 0, UINT64_MAX, &bytes, NULL)) {
        *out = bytes;
        return true;
    }
    return false;
}

static bool parse_time(GtkWidget *input, bool end, int64_t *out)
{
    const char *text = gtk_editable_get_text(GTK_EDITABLE(input));
    if (strlen(text) != 10)
        return false;
    char *iso = g_strconcat(text, end ? "T23:59:59Z" : "T00:00:00Z", NULL);
    GDateTime *date = g_date_time_new_from_iso8601(iso, NULL);
    g_free(iso);
    if (!date)
        return false;
    *out = g_date_time_to_unix(date);
    g_date_time_unref(date);
    return true;
}

static void selection_changed(GObject *model, GParamSpec *param, gpointer data)
{
    (void)param;
    View *v = data;
    const DsRow *row = model_row(gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(model)));
    v->export_selection = row ? row->entry.id : 0;
    v->selected_reason = row ? row->entry.reason : DS_REASON_NONE;
    v->selected_os_code = row ? row->entry.os_code : 0;
    if (row) {
        v->selected_kind = row->entry.kind;
        int64_t folder = row->entry.kind == DS_DIRECTORY ? row->entry.id : row->entry.parent_id;
        ui_details(v, row->entry.id, folder, row);
    } else {
        ui_controls(v);
    }
}

DsQuery ui_result_query(View *v, bool *tree)
{
    const char *text = gtk_editable_get_text(GTK_EDITABLE(v->search));
    const char *ext = gtk_editable_get_text(GTK_EDITABLE(v->extension));
    DsQuery query = {.parent_id=0, .filter=*text ? text : NULL, .search_path=true,
                     .extension=*ext ? ext : NULL, .metric=ui_metric(v),
                     .sort=DS_SORT_METRIC, .descending=true, .limit=DS_PAGE_MAX,
                     .issues_only=v->mode == ISSUES};
    query.has_min_bytes = parse_bytes(v->minimum, &query.min_bytes);
    query.has_max_bytes = parse_bytes(v->maximum, &query.max_bytes);
    query.has_mtime_from = parse_time(v->from, false, &query.mtime_from);
    query.has_mtime_to = parse_time(v->to, true, &query.mtime_to);
    *tree = v->mode == TREE && !query.filter && !query.extension && !query.has_min_bytes &&
            !query.has_max_bytes && !query.has_mtime_from && !query.has_mtime_to;
    if (!*tree) {
        query.parent_id = DS_QUERY_ALL;
        query.kind = v->mode == TOP_FILE ? DS_FILE : v->mode == TOP_DIR ? DS_DIRECTORY : 0;
    }
    return query;
}

void ui_refresh(View *v)
{
    ++v->generation;
    ui_live_clear(v);
    if (v->detail_task)
        g_cancellable_cancel(v->detail_task);
    v->selected = v->mapped_folder = v->focused_folder = 0;
    v->export_selection = 0;
    v->selected_kind = 0;
    v->selected_reason = DS_REASON_NONE;
    v->selected_os_code = 0;
    ds_details_clear(&v->detail);
    ui_label(v->path, "");
    ui_label(v->info, "");
    ui_label(v->legend, UI("트리맵"));
    gtk_widget_queue_draw(v->map);
    gtk_column_view_set_model(v->table, NULL);
    g_clear_object(&v->selection);
    g_clear_object(&v->base);
    gtk_stack_set_visible_child_name(GTK_STACK(v->stack), "tree");
    if (!v->db.raw)
        return;
    if (v->mode == EXTENSIONS || v->mode == DATES) {
        gtk_stack_set_visible_child_name(GTK_STACK(v->stack), "statistics");
        gtk_text_buffer_set_text(v->statistics, UI("집계 중…"), -1);
        Work *job = g_new0(Work, 1);
        job->op = STATS;
        job->metric = ui_metric(v);
        job->group_age = v->mode == DATES;
        ui_copy_path(&job->db, &v->db);
        ui_launch(v, job);
        return;
    }
    bool tree;
    DsQuery q = ui_result_query(v, &tree);
    v->live_tree = tree && ui_alive(v);
    if (v->live_tree) {
        q.sort = DS_SORT_ID;
        q.descending = false;
        v->expand_root = true;
        v->preview_pending = true;
    }
    v->base = ds_paged_model_new(&v->db, &q);
    if (v->mode == TOP_DIR || v->mode == TOP_FILE)
        ds_paged_model_set_cap(v->base, gtk_drop_down_get_selected(v->cap) ? 1000 : 100);
    GListModel *list = g_object_ref(v->base);
    if (tree)
        list = G_LIST_MODEL(gtk_tree_list_model_new(list, FALSE, FALSE, child_model, v, NULL));
    v->selection = gtk_single_selection_new(list);
    gtk_single_selection_set_autoselect(v->selection, FALSE);
    g_signal_connect(v->selection, "notify::selected-item", G_CALLBACK(selection_changed), v);
    gtk_column_view_set_model(v->table, GTK_SELECTION_MODEL(v->selection));
}

void ui_live_refresh(View *v)
{
    if (v->live_tree) {
        v->live_pending = v->live_models.length;
        v->preview_pending = true;
    } else if (v->base) {
        v->list_pending = true;
    }
}

void ui_live_tick(View *v)
{
    expand_live_root(v);
    if (v->list_pending && !ds_paged_model_busy(v->base)) {
        v->list_pending = false;
        ds_paged_model_refresh(v->base);
    }
    /* A user can expand many folders. Feed their readers in batches on the
     * existing UI timer and retain live model references only in GTK. */
    guint count = MIN(4u, MIN(v->live_pending, v->live_models.length));
    for (guint i = 0; i < count; ++i) {
        GList *link = g_queue_pop_head_link(&v->live_models);
        g_queue_push_tail_link(&v->live_models, link);
        ds_paged_model_update_appended(link->data);
    }
    v->live_pending = v->live_models.length ? v->live_pending - count : 0;
    if (v->preview_pending && !v->detail_task) {
        v->preview_pending = false;
        Work *job = g_new0(Work, 1);
        job->op = DETAILS;
        job->folder = v->focused_folder ? v->focused_folder : 1;
        job->metric = ui_metric(v);
        job->preview = true;
        ui_copy_path(&job->db, &v->db);
        ui_launch(v, job);
    }
}

void ui_details(View *v, int64_t id, int64_t folder, const DsRow *row)
{
    if (!v->db.raw || v->mode == EXTENSIONS || v->mode == DATES)
        return;
    v->selected = id;
    v->focused_folder = folder;
    ds_path_clear(&v->detail.path);
    if (row) {
        char *logical = format_metric(row, DS_LOGICAL);
        char *alloc = format_metric(row, DS_ALLOCATED);
        char *unique = format_metric(row, DS_UNIQUE);
        uint64_t count = row->entry.kind == DS_FILE ? 1 : row->file_count;
        g_autofree char *files = row->entry.kind == DS_DIRECTORY && !row->aggregated ?
            g_strdup(UI("파일 수 집계 전")) : g_strdup_printf(UI("파일 %" G_GUINT64_FORMAT "개"), count);
        char *stats = g_strdup_printf(UI("논리 크기\n%s\n\nOS 보고 할당량\n%s\n\n중복 제외 할당량\n%s"
            "\n\n%s · 링크 %" G_GUINT64_FORMAT "개\n%s"),
            logical, alloc, unique, files, row->entry.links, reason(row->entry.reason));
        ui_label(v->info, stats);
        g_free(stats);
        g_free(unique);
        g_free(alloc);
        g_free(logical);
    } else {
        ui_label(v->info, "");
        v->selected_kind = 0;
        v->selected_reason = DS_REASON_NONE;
        v->selected_os_code = 0;
    }
    ++v->generation;
    Work *job = g_new0(Work, 1);
    job->op = DETAILS;
    job->id = id;
    job->folder = v->mapped_folder == folder ? 0 : folder;
    job->preview = ui_alive(v);
    if (job->preview && job->folder)
        v->preview_pending = false;
    job->metric = ui_metric(v);
    ui_copy_path(&job->db, &v->db);
    ui_launch(v, job);
    gtk_widget_queue_draw(v->map);
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    (void)area;
    View *v = data;
    g_clear_pointer(&v->layout, ds_treemap_free);
    cairo_set_source_rgb(cr, 0.11, 0.15, 0.20);
    cairo_paint(cr);
    if (!v->detail.map)
        return;
    DsRect rect = {0, 0, width, height};
    if (ds_treemap_layout((const DsTreemapItem *)v->detail.map->data,
        v->detail.map->len, &rect, &v->layout, NULL) != DS_OK)
        return;
    for (size_t i = 0; i < v->layout->count; ++i) {
        DsTreemapCell *cell = &v->layout->cells[i];
        uint32_t hue = (uint32_t)((uint64_t)cell->id * UINT64_C(2654435761));
        double s = (double)(hue & 255) / 255.0;
        bool selected = cell->id && cell->id == v->selected;
        cairo_set_source_rgb(cr, selected ? 0.96 : 0.18 + 0.20 * s,
                             selected ? 0.69 : 0.43 + 0.19 * s, 0.64 - 0.17 * s);
        cairo_rectangle(cr, cell->rect.x, cell->rect.y,
                        MAX(0.0, cell->rect.width - 1), MAX(0.0, cell->rect.height - 1));
        cairo_fill(cr);
    }
}

static void map_clicked(GtkGestureClick *click, int count, double x, double y, gpointer data)
{
    (void)click;
    (void)count;
    View *v = data;
    int64_t id = ds_treemap_hit(v->layout, x, y);
    if (id <= 0)
        return;
    v->export_selection = id;
    GListModel *model = G_LIST_MODEL(gtk_column_view_get_model(v->table));
    guint n = model ? MIN(g_list_model_get_n_items(model), 512u) : 0;
    for (guint i = 0; i < n; ++i) {
        GObject *object = g_list_model_get_item(model, i);
        const DsRow *row = model_row(object);
        bool found = row && row->entry.id == id;
        g_clear_object(&object);
        if (found) {
            gtk_single_selection_set_selected(v->selection, i);
            gtk_column_view_scroll_to(v->table, i, NULL, GTK_LIST_SCROLL_FOCUS, NULL);
            ui_controls(v);
            return;
        }
    }
    ui_details(v, id, v->mapped_folder, NULL);
}

void ui_map(View *v)
{
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->map), draw, v, NULL);
    GtkGesture *gesture = gtk_gesture_click_new();
    g_signal_connect(gesture, "pressed", G_CALLBACK(map_clicked), v);
    gtk_widget_add_controller(v->map, GTK_EVENT_CONTROLLER(gesture));
}

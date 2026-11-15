#include "ui.h"
#include "local.h"
#include <glib/gstdio.h>

void ui_copy_path(DsNativePath *dst, const DsNativePath *src)
{
    ds_path_clear(dst);
    if (src->raw)
        *dst = (DsNativePath){src->encoding, g_bytes_ref(src->raw)};
}

static void free_work(gpointer data)
{
    Work *w = data;
    ds_path_clear(&w->db);
    ds_path_clear(&w->target);
    ds_path_clear(&w->cache);
    ds_session_request_cancel(w->session);
    ds_session_unref(w->session);
    ds_details_clear(&w->detail);
    g_free(w->exclude);
    g_free(w->report);
    g_free(w);
}

typedef struct { uint64_t totals[5]; GString *lines; uint64_t shown; DsMetric metric; } Summary;

static DsStatus summarize_diff(const DsDiffBatch *b, void *ctx, DsError *error)
{
    (void)error;
    Summary *summary = ctx;
    summary->totals[4] = b->same_root && b->same_policy;
    const char *labels[] = {UI("새 항목"), UI("사라짐"), UI("변경"), UI("확인되지 않음")};
    for (size_t i = 0; i < b->count; ++i) {
        const DsDiff *row = &b->entries[i];
        ++summary->totals[row->kind];
        if (summary->shown >= 250 || summary->lines->len >= 512u * 1024u)
            continue;
        char *path = NULL;
        if (ds_path_display(&row->relative_path, &path, NULL) == DS_OK) {
            DsSize before = row->before_sizes[summary->metric], after = row->after_sizes[summary->metric];
            char *old_size = before.quality == DS_KNOWN ?
                g_format_size_full(before.bytes, G_FORMAT_SIZE_IEC_UNITS) : g_strdup("—");
            char *new_size = after.quality == DS_KNOWN ?
                g_format_size_full(after.bytes, G_FORMAT_SIZE_IEC_UNITS) : g_strdup("—");
            g_string_append_printf(summary->lines, "%s  |  %s → %s  |  %s\n",
                                   labels[row->kind], old_size, new_size, path);
            g_free(old_size);
            g_free(new_size);
            g_free(path);
            ++summary->shown;
        }
    }
    return DS_OK;
}

static DsStatus stable_import(Work *w, DsStore *reader, DsControl *control, DsError *error)
{
    char *dir = ds_private_cache(&w->cache, error);
    if (!dir)
        return error->status;
    char *name = g_build_filename(dir, "import.dscan", NULL);
    DsStatus status = ds_path_from_arg(name, &w->db, error);
    if (status == DS_OK)
        status = ds_snapshot_save(reader, &w->db, control, error);
    DsStore *copy = NULL;
    if (status == DS_OK)
        status = ds_snapshot_open(&w->db, &copy, control, error);
    DsStoreInfo info = {0};
    if (status == DS_OK)
        status = ds_store_info(copy, &info, error);
    if (status == DS_OK)
        w->report = g_strdup_printf("%s   ·   %" G_GUINT64_FORMAT "개%s",
            info.coverage == DS_COVERAGE_COMPLETE ? UI("저장된 결과") : UI("저장된 부분 결과"),
            info.entry_count, info.aggregation_complete ? "" : UI("   ·   집계되지 않음"));
    ds_store_info_clear(&info);
    ds_store_close(copy);
    if (status != DS_OK) {
        g_unlink(name);
        g_rmdir(dir);
    }
    g_free(name);
    g_free(dir);
    return status;
}

static void cancel_work(GCancellable *cancel, gpointer control)
{
    (void)cancel;
    ds_control_cancel(control);
}

static DsStatus run(Work *w, GCancellable *cancel, DsControl *control)
{
    if (g_cancellable_is_cancelled(cancel))
        return DS_CANCELED;
    DsError *error = &w->error;
    if (w->op == START) {
        DsScanOptions options;
        ds_scan_options_init(&options, w->profile);
        options.root = w->target;
        options.cache_path = w->cache;
        options.include_hidden = w->include_hidden;
        options.exclude_pattern = w->exclude;
        w->session = ds_session_start(&options, NULL, NULL, error);
        if (w->session && g_cancellable_is_cancelled(cancel))
            ds_session_request_cancel(w->session);
        return w->session ? DS_OK : error->status;
    }

    DsStore *reader = NULL, *after = NULL;
    DsStatus status = w->op == OPEN ? ds_snapshot_open(&w->target, &reader, control, error) :
                      ds_store_open(&w->db, w->op == AGGREGATE ? DS_STORE_WRITER : DS_STORE_READER, &reader, error);
    if (status != DS_OK)
        return status;
    switch (w->op) {
    case OPEN:
        status = stable_import(w, reader, control, error);
        break;
    case AGGREGATE:
        status = ds_finalize(reader, control, error);
        break;
    case SAVE:
        status = ds_snapshot_save(reader, &w->target, control, error);
        break;
    case EXPORT: {
        DsExportOptions options = {.format=w->format, .destination=w->target, .bom=true};
        status = ds_export(reader, &options, control, error);
        break;
    }
    case COMPARE: {
        status = ds_snapshot_open(&w->target, &after, control, error);
        if (status != DS_OK)
            break;
        Summary summary = {.lines = g_string_new(NULL), .metric = w->metric};
        status = ds_compare(reader, after, NULL, summarize_diff, &summary, control, error);
        uint64_t *count = summary.totals;
        if (status == DS_OK)
            w->report = g_strdup_printf(
                UI("새 항목 %" G_GUINT64_FORMAT "개 · 사라짐 %" G_GUINT64_FORMAT
                "개 · 변경 %" G_GUINT64_FORMAT "개 · 확인되지 않음 %" G_GUINT64_FORMAT
                "개%s\n경로 미리보기 %" G_GUINT64_FORMAT "개\n\n%s"),
                count[DS_DIFF_ADDED], count[DS_DIFF_REMOVED], count[DS_DIFF_CHANGED],
                count[DS_DIFF_UNCONFIRMED],
                count[4] ? "" : UI("\n루트 경로 또는 스캔 정책이 다릅니다."),
                summary.shown, summary.lines->str);
        g_string_free(summary.lines, true);
        break;
    }
    case DETAILS:
        status = ds_details_load(reader, w->id, w->folder, w->metric, cancel, &w->detail, error);
        break;
    case STATS:
        status = ds_statistics_report(reader, w->metric, w->group_age, cancel, &w->report, error);
        break;
    default:
        break;
    }
    ds_store_close(after);
    ds_store_close(reader);
    return status;
}

static void worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source;
    Work *w = data;
    DsControl control;
    ds_control_init(&control);
    gulong handler = g_cancellable_connect(cancel, G_CALLBACK(cancel_work), &control, NULL);
    w->status = run(w, cancel, &control);
    g_cancellable_disconnect(cancel, handler);
    ds_control_clear(&control);
    g_task_return_boolean(task, true);
}

static void apply_details(View *v, Work *w)
{
    if (w->detail.path.raw)
        ui_copy_path(&v->detail.path, &w->detail.path);
    char *display = NULL;
    if (ds_path_display(&v->detail.path, &display, NULL) == DS_OK)
        ui_label(v->path, display);
    g_free(display);
    if (!w->detail.map)
        return;
    g_clear_pointer(&v->detail.map, g_array_unref);
    v->detail.map = g_array_ref(w->detail.map);
    v->mapped_folder = w->folder;
    char *legend = g_strdup_printf(UI("트리맵 · 0 B %" G_GUINT64_FORMAT
        "개 · 미확인 %" G_GUINT64_FORMAT "개 · 기타 %" G_GUINT64_FORMAT "개"),
        w->detail.zero, w->detail.unknown, w->detail.other);
    ui_label(v->legend, legend);
    g_free(legend);
    gtk_widget_queue_draw(v->map);
}

static void done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    View *v = data;
    Work *w = g_task_get_task_data(G_TASK(result));
    bool background = w->op == DETAILS || w->op == STATS;
    if (v->closing || (background && w->generation != v->generation))
        return;
    if (!background) {
        v->busy = false;
        g_clear_object(&v->operation);
    }
    if (w->status != DS_OK) {
        if (!background && w->status != DS_CANCELED)
            ui_message(v, UI("작업을 완료하지 못했습니다"), w->error.message);
        if (w->op == STATS)
            gtk_text_buffer_set_text(v->statistics, w->error.message, -1);
        ui_controls(v);
        return;
    }
    switch (w->op) {
    case START:
        ds_session_unref(v->session);
        v->session = w->session;
        w->session = NULL;
        v->started = g_get_monotonic_time();
        v->finished = false;
        v->sequence = v->refreshed = 0;
        ds_path_clear(&v->db);
        ui_refresh(v);
        ui_tick(v);
        break;
    case OPEN:
        ds_session_unref(v->session);
        v->session = NULL;
        ui_copy_path(&v->db, &w->db);
        ui_label(v->status, w->report);
        ui_refresh(v);
        ui_details(v, 1, 1, NULL);
        break;
    case DETAILS: apply_details(v, w); break;
    case STATS: gtk_text_buffer_set_text(v->statistics, w->report, -1); break;
    case COMPARE:
        gtk_text_buffer_set_text(v->statistics, w->report, -1);
        gtk_stack_set_visible_child_name(GTK_STACK(v->stack), "statistics");
        break;
    case AGGREGATE:
        ui_refresh(v);
        ui_details(v, 1, 1, NULL);
        ui_message(v, UI("관찰된 항목 집계 완료"), UI("기존 스캔의 범위와 상태를 유지합니다."));
        break;
    case SAVE: ui_message(v, UI("스냅샷 저장 완료"), NULL); break;
    case EXPORT: ui_message(v, UI("내보내기 완료"), NULL); break;
    default: break;
    }
    ui_controls(v);
}

void ui_launch(View *v, Work *w)
{
    GCancellable **slot = w->op == DETAILS || w->op == STATS ? &v->detail_task : &v->operation;
    if (*slot) {
        g_cancellable_cancel(*slot);
        g_clear_object(slot);
    }
    *slot = g_cancellable_new();
    if (slot == &v->operation)
        v->busy = true;
    w->generation = v->generation;
    GTask *task = g_task_new(v->window, *slot, done, v);
    g_task_set_task_data(task, w, free_work);
    g_task_run_in_thread(task, worker);
    g_object_unref(task);
    ui_controls(v);
}

static void file_selected(GObject *source, GAsyncResult *result, gpointer data)
{
    GtkWindow *window = data;
    View *v = g_object_get_data(G_OBJECT(window), "disk-view");
    WorkType op = GPOINTER_TO_INT(g_object_get_data(source, "disk-action"));
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GFile *file = op == START || op == CACHE ? gtk_file_dialog_select_folder_finish(dialog, result, NULL) :
                  op == SAVE || op == EXPORT ? gtk_file_dialog_save_finish(dialog, result, NULL) :
                  gtk_file_dialog_open_finish(dialog, result, NULL);
    if (file && !v->closing && !v->busy && !ui_alive(v)) {
        char *path = g_file_get_path(file);
        Work *w = g_new0(Work, 1);
        if (path && ds_path_from_arg(path, &w->target, NULL) == DS_OK) {
            if (op == CACHE) {
                ui_copy_path(&v->cache, &w->target);
                ui_settings_save(v);
                ui_message(v, UI("캐시 위치 변경"), UI("다음 스캔부터 적용됩니다."));
                free_work(w);
            } else {
                w->op = op;
                w->metric = ui_metric(v);
                w->profile = gtk_drop_down_get_selected(v->profile) == 1 ? DS_STANDARD : DS_CONSERVATIVE;
                w->format = (DsExportFormat)gtk_drop_down_get_selected(v->format);
                w->include_hidden = !gtk_check_button_get_active(GTK_CHECK_BUTTON(v->hide));
                const char *exclude = gtk_editable_get_text(GTK_EDITABLE(v->exclude));
                w->exclude = *exclude ? g_strdup(exclude) : NULL;
                ui_copy_path(&w->db, &v->db);
                ui_copy_path(&w->cache, &v->cache);
                ui_launch(v, w);
            }
        } else {
            free_work(w);
            ui_message(v, UI("로컬 경로를 선택하세요"), NULL);
        }
        g_free(path);
    }
    g_clear_object(&file);
    g_object_unref(window);
}

void ui_file_dialog(View *v, WorkType op)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    g_object_set_data(G_OBJECT(dialog), "disk-action", GINT_TO_POINTER(op));
    if (op == START || op == CACHE) {
        gtk_file_dialog_set_title(dialog, op == START ? UI("스캔할 드라이브 또는 폴더") : UI("로컬 캐시 폴더"));
        gtk_file_dialog_select_folder(dialog, v->window, NULL, file_selected, g_object_ref(v->window));
    } else if (op == SAVE || op == EXPORT) {
        const char *extensions[] = {"csv", "json", "jsonl"};
        guint format = gtk_drop_down_get_selected(v->format);
        char *name = op == SAVE ? g_strdup("result.dscan") : g_strdup_printf("result.%s", extensions[MIN(format, 2u)]);
        gtk_file_dialog_set_initial_name(dialog, name);
        gtk_file_dialog_save(dialog, v->window, NULL, file_selected, g_object_ref(v->window));
        g_free(name);
    } else {
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, UI("DiskScope 스냅샷"));
        gtk_file_filter_add_pattern(filter, "*.dscan");
        gtk_file_dialog_set_default_filter(dialog, filter);
        g_object_unref(filter);
        gtk_file_dialog_set_title(dialog, op == COMPARE ? UI("비교할 결과 선택") : UI("결과 열기"));
        gtk_file_dialog_open(dialog, v->window, NULL, file_selected, g_object_ref(v->window));
    }
    g_object_unref(dialog);
}

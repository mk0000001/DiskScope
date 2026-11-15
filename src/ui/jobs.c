#include "ui.h"
#include "local.h"
#include "diskscope/ds_retention.h"
#include "volume_view.h"
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
    ds_recovered_free(w->recovered);
    ds_cache_lease_close(w->import_lease);
    ds_details_clear(&w->detail);
    g_free((char *)w->export_filter.filter);
    g_free((char *)w->export_filter.extension);
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
    ds_cache_prune(&w->cache, control);
    if (!ds_control_checkpoint(control))
        return DS_CANCELED;
    char *dir = ds_private_cache(&w->cache, error);
    if (!dir)
        return error->status;
    char *name = g_build_filename(dir, "import.dscan", NULL);
    w->import_lease = ds_cache_lease_open(dir, true, false, error);
    DsStatus status = w->import_lease ? ds_path_from_arg(name, &w->db, error) : error->status;
    if (status == DS_OK)
        status = ds_snapshot_save(reader, &w->db, control, error);
    DsStore *copy = NULL;
    if (status == DS_OK)
        status = ds_snapshot_open(&w->db, &copy, control, error);
    DsStoreInfo info = {0};
    if (status == DS_OK)
        status = ds_store_info(copy, &info, error);
    if (status == DS_OK) {
        ds_cache_mark_owned(dir, false);
        w->report = g_strdup_printf("%s   ·   %" G_GUINT64_FORMAT "개%s",
            info.coverage == DS_COVERAGE_COMPLETE ? UI("저장된 결과") : UI("저장된 부분 결과"),
            info.entry_count, info.aggregation_complete ? "" : UI("   ·   집계되지 않음"));
    }
    ds_store_info_clear(&info);
    ds_store_close(copy);
    if (status != DS_OK) {
        g_clear_pointer(&w->import_lease, ds_cache_lease_close);
        char *lock = g_build_filename(dir, DS_CACHE_LEASE_FILE, NULL);
        g_unlink(name);
        g_unlink(lock);
        g_free(lock);
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

static char *recovery_status(const DsStoreInfo *info)
{
    return g_strdup_printf(UI("%s · %" G_GUINT64_FORMAT "개%s"),
        info->coverage == DS_COVERAGE_COMPLETE ? UI("최근 캐시 결과") : UI("복구된 부분 결과"),
        info->entry_count, info->aggregation_complete ? "" : UI(" · 집계되지 않음"));
}

static DsStatus recover(Work *w, DsControl *control, DsError *error)
{
    ds_cache_prune(&w->cache, control);
    DsStatus status = ds_recovery_latest(&w->cache, control, &w->recovered, error);
    if (status == DS_OK && w->db.raw) {
        DsStore *reader = NULL;
        DsStoreInfo info = {0};
        if (ds_store_open(&w->db, DS_STORE_READER, &reader, NULL) == DS_OK &&
            ds_store_info(reader, &info, NULL) == DS_OK && info.entry_count > 0 &&
            (!w->recovered || info.started_at >= ds_recovered_info(w->recovered)->started_at))
            w->existing_latest = true;
        ds_store_info_clear(&info);
        ds_store_close(reader);
    }
    return status;
}

static DsStatus diagnose(Work *w, DsControl *control, DsError *error)
{
    DsStore *reader = NULL;
    DsStatus status = w->db.raw ? ds_store_open(&w->db, DS_STORE_READER, &reader, error) : DS_OK;
    if (status == DS_OK) {
        DsDiagnosticsOptions options = {
            .destination = w->target, .worker = w->has_worker_event ? &w->worker_event : NULL,
            .include_root = w->include_root,
            .gtk_major = gtk_get_major_version(), .gtk_minor = gtk_get_minor_version(),
            .gtk_micro = gtk_get_micro_version()
        };
        status = ds_diagnostics_save(reader, &options, control, error);
    }
    ds_store_close(reader);
    return status;
}

static DsStatus run(Work *w, GCancellable *cancel, DsControl *control)
{
    if (g_cancellable_is_cancelled(cancel))
        return DS_CANCELED;
    DsError *error = &w->error;
    if (w->op == RECOVER)
        return recover(w, control, error);
    if (w->op == DIAGNOSTICS)
        return diagnose(w, control, error);
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
    case AGGREGATE: {
        status = ds_finalize(reader, control, error);
        if (status == DS_OK) {
            DsStoreInfo info = {0};
            if (ds_store_info(reader, &info, NULL) == DS_OK)
                w->report = recovery_status(&info);
            ds_store_info_clear(&info);
        }
        break;
    }
    case SAVE:
        status = ds_snapshot_save(reader, &w->target, control, error);
        break;
    case EXPORT: {
        DsExportOptions options = {.format=w->format, .destination=w->target, .bom=true,
                                   .metric=w->metric, .selected_id=w->id,
                                   .view=w->export_filtered ? &w->export_filter : NULL};
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
    case VOLUME:
        w->report = ds_ui_volume_summary(reader, error);
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
    bool volume = w->op == VOLUME;
    bool background = volume || w->op == DETAILS || w->op == STATS;
    if (v->closing || (!volume && background && w->generation != v->generation) ||
        (volume && (!v->db.raw || !g_bytes_equal(v->db.raw, w->db.raw))))
        return;
    if (!background) {
        v->busy = false;
        g_clear_object(&v->operation);
    }
    if (w->status != DS_OK) {
        if (!background && !w->automatic && w->status != DS_CANCELED)
            ui_message(v, UI("작업을 완료하지 못했습니다"), w->error.message);
        if (w->op == STATS)
            gtk_text_buffer_set_text(v->statistics, w->error.message, -1);
        ui_controls(v);
        return;
    }
    switch (w->op) {
    case START:
        g_clear_pointer(&v->recovered, ds_recovered_free);
        g_clear_pointer(&v->import_lease, ds_cache_lease_close);
        ds_session_unref(v->session);
        v->session = w->session;
        w->session = NULL;
        v->started = g_get_monotonic_time();
        v->finished = false;
        v->cache_location_changed = false;
        v->sequence = v->refreshed = 0;
        ds_path_clear(&v->db);
        ui_volume_load(v);
        ui_refresh(v);
        ui_tick(v);
        break;
    case OPEN:
        g_clear_pointer(&v->recovered, ds_recovered_free);
        g_clear_pointer(&v->import_lease, ds_cache_lease_close);
        v->import_lease = g_steal_pointer(&w->import_lease);
        ds_session_unref(v->session);
        v->session = NULL;
        ui_copy_path(&v->db, &w->db);
        ui_volume_load(v);
        ui_label(v->status, w->report);
        ui_refresh(v);
        ui_details(v, 1, 1, NULL);
        break;
    case RECOVER: {
        if (w->existing_latest || (v->recovered && !v->cache_location_changed &&
            !ds_recovered_newer(w->recovered, v->recovered))) {
            if (!w->automatic)
                ui_message(v, UI("가장 최근 캐시 결과를 보고 있습니다"), NULL);
            break;
        }
        if (!w->recovered) {
            if (!w->automatic)
                ui_message(v, UI("열 수 있는 최근 캐시가 없습니다"), UI("실행 중인 스캔은 제외됩니다."));
            break;
        }
        ds_session_unref(v->session);
        v->session = NULL;
        g_clear_pointer(&v->import_lease, ds_cache_lease_close);
        g_clear_pointer(&v->recovered, ds_recovered_free);
        v->recovered = g_steal_pointer(&w->recovered);
        v->cache_location_changed = false;
        ui_copy_path(&v->db, ds_recovered_path(v->recovered));
        ui_volume_load(v);
        const DsStoreInfo *info = ds_recovered_info(v->recovered);
        v->recovered_aggregated = info->aggregation_complete;
        char *status = recovery_status(info);
        ui_label(v->status, status);
        g_free(status);
        ui_refresh(v);
        ui_details(v, 1, 1, NULL);
        break;
    }
    case DETAILS: apply_details(v, w); break;
    case STATS: gtk_text_buffer_set_text(v->statistics, w->report, -1); break;
    case VOLUME:
        ui_label(v->volume_label, w->report ? w->report : UI("볼륨 정보 없음"));
        break;
    case COMPARE:
        gtk_text_buffer_set_text(v->statistics, w->report, -1);
        gtk_stack_set_visible_child_name(GTK_STACK(v->stack), "statistics");
        break;
    case AGGREGATE:
        if (v->recovered) {
            v->recovered_aggregated = true;
            if (w->report)
                ui_label(v->status, w->report);
        }
        ui_refresh(v);
        ui_details(v, 1, 1, NULL);
        ui_message(v, UI("관찰된 항목 집계 완료"), UI("기존 스캔의 범위와 상태를 유지합니다."));
        break;
    case SAVE: ui_message(v, UI("스냅샷 저장 완료"), NULL); break;
    case EXPORT: ui_message(v, UI("내보내기 완료"), NULL); break;
    case DIAGNOSTICS: ui_message(v, UI("진단 보고서 저장 완료"), NULL); break;
    default: break;
    }
    ui_controls(v);
}

void ui_launch(View *v, Work *w)
{
    GCancellable **slot = w->op == VOLUME ? &v->volume_task :
        w->op == DETAILS || w->op == STATS ? &v->detail_task : &v->operation;
    if (*slot) {
        g_cancellable_cancel(*slot);
        g_clear_object(slot);
    }
    *slot = g_cancellable_new();
    if (slot == &v->operation) {
        v->busy = true;
        v->operation_type = w->op;
    }
    w->generation = v->generation;
    GTask *task = g_task_new(v->window, *slot, done, v);
    g_task_set_task_data(task, w, free_work);
    g_task_run_in_thread(task, worker);
    g_object_unref(task);
    ui_controls(v);
}

void ui_volume_load(View *v)
{
    if (v->volume_task)
        g_cancellable_cancel(v->volume_task);
    ui_label(v->volume_label, "");
    if (!v->db.raw)
        return;
    Work *work = g_new0(Work, 1);
    work->op = VOLUME;
    ui_copy_path(&work->db, &v->db);
    ui_launch(v, work);
}

void ui_recover(View *v, bool automatic)
{
    if (v->closing || v->busy || ui_alive(v) || (automatic && v->db.raw))
        return;
    Work *work = g_new0(Work, 1);
    work->op = RECOVER;
    work->automatic = automatic;
    ui_copy_path(&work->cache, &v->cache);
    if (v->session && !v->cache_location_changed)
        ui_copy_path(&work->db, &v->db);
    ui_launch(v, work);
}

static void file_selected(GObject *source, GAsyncResult *result, gpointer data)
{
    GtkWindow *window = data;
    View *v = g_object_get_data(G_OBJECT(window), "disk-view");
    WorkType op = GPOINTER_TO_INT(g_object_get_data(source, "disk-action"));
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    Work *pending = g_object_steal_data(source, "disk-save-work");
    GFile *file = op == START || op == CACHE ? gtk_file_dialog_select_folder_finish(dialog, result, NULL) :
                  op == SAVE || op == EXPORT || op == DIAGNOSTICS ?
                  gtk_file_dialog_save_finish(dialog, result, NULL) :
                  gtk_file_dialog_open_finish(dialog, result, NULL);
    if (file && !v->closing && !v->busy && (op == DIAGNOSTICS || !ui_alive(v))) {
        char *path = g_file_get_path(file);
        Work *w = pending ? g_steal_pointer(&pending) : g_new0(Work, 1);
        if (path && ds_path_from_arg(path, &w->target, NULL) == DS_OK) {
            if (op == CACHE) {
                ui_copy_path(&v->cache, &w->target);
                v->cache_location_changed = true;
                ui_settings_save(v);
                ui_message(v, UI("캐시 위치 변경"), UI("다음 스캔부터 적용됩니다."));
                free_work(w);
            } else if (op == DIAGNOSTICS) {
                if (v->session && ((!w->db.raw && !v->db.raw) ||
                    (w->db.raw && v->db.raw && g_bytes_equal(w->db.raw, v->db.raw)))) {
                    ds_session_event(v->session, &w->worker_event);
                    w->has_worker_event = true;
                }
                ui_launch(v, w);
            } else {
                w->op = op;
                if (op != EXPORT)
                    w->metric = ui_metric(v);
                w->profile = gtk_drop_down_get_selected(v->profile) == 1 ? DS_STANDARD : DS_CONSERVATIVE;
                w->include_hidden = !gtk_check_button_get_active(GTK_CHECK_BUTTON(v->hide));
                const char *exclude = gtk_editable_get_text(GTK_EDITABLE(v->exclude));
                w->exclude = *exclude ? g_strdup(exclude) : NULL;
                if (op != EXPORT)
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
    if (pending)
        free_work(pending);
    g_clear_object(&file);
    g_object_unref(window);
}

void ui_diagnostics_dialog(View *v, bool include_root)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    Work *work = g_new0(Work, 1);
    work->op = DIAGNOSTICS;
    work->include_root = include_root;
    ui_copy_path(&work->db, &v->db);
    g_object_set_data(G_OBJECT(dialog), "disk-action", GINT_TO_POINTER(DIAGNOSTICS));
    g_object_set_data_full(G_OBJECT(dialog), "disk-save-work", work, free_work);
    gtk_file_dialog_set_title(dialog, UI("진단 보고서 저장"));
    gtk_file_dialog_set_initial_name(dialog, "diskscope-diagnostics.json");
    gtk_file_dialog_save(dialog, v->window, NULL, file_selected, g_object_ref(v->window));
    g_object_unref(dialog);
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
        if (op == EXPORT) {
            UiExportScope scope = (UiExportScope)gtk_drop_down_get_selected(v->export_scope);
            Work *work = g_new0(Work, 1);
            work->format = (DsExportFormat)format;
            work->metric = ui_metric(v);
            ui_copy_path(&work->db, &v->db);
            if (scope == UI_EXPORT_SELECTION)
                work->id = v->export_selection;
            else if (scope == UI_EXPORT_FILTER) {
                bool tree;
                work->export_filtered = true;
                work->export_filter = ui_result_query(v, &tree);
                work->export_filter.filter = g_strdup(work->export_filter.filter);
                work->export_filter.extension = g_strdup(work->export_filter.extension);
            }
            g_object_set_data_full(G_OBJECT(dialog), "disk-save-work", work, free_work);
            gtk_file_dialog_set_title(dialog, scope == UI_EXPORT_SELECTION ? UI("선택 항목 내보내기") :
                scope == UI_EXPORT_FILTER ? UI("필터 일치 항목 내보내기") : UI("전체 결과 내보내기"));
        }
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

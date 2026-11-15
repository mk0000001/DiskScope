#include "ui.h"
#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static bool can_retry_admin(const View *v)
{
    return v->detail.path.raw && v->export_selection == v->selected &&
           v->selected_reason == DS_REASON_PERMISSION && v->selected_os_code == ERROR_ACCESS_DENIED &&
           (v->selected_kind == DS_FILE || v->selected_kind == DS_DIRECTORY);
}

static char *powershell_path(const DsNativePath *path)
{
    if (!path->raw || path->encoding != DS_PATH_UTF16LE)
        return NULL;
    gsize size = 0;
    const guchar *data = g_bytes_get_data(path->raw, &size);
    g_autofree char *encoded = g_base64_encode(data, size);
    /* Keep native path bytes outside PowerShell syntax, including quote
     * characters in a filename obtained from an imported snapshot. */
    return g_strdup_printf("([Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('%s')))", encoded);
}

static void retry_admin(View *v)
{
    if (!can_retry_admin(v))
        return;
    g_autofree char *native = ds_path_arg(&v->detail.path, NULL);
    const char *path = native && g_str_has_prefix(native, "\\\\?\\") ? native + 4 : native;
    if (!path || strlen(path) < 4 || !g_ascii_isalpha(path[0]) || path[1] != ':' || path[2] != '\\' ||
        strchr(path + 2, ':')) {
        ui_message(v, UI("로컬 파일이나 폴더를 선택하세요"), NULL);
        return;
    }
    g_autofree char *command = NULL;
    if (v->selected_kind == DS_DIRECTORY) {
        g_autofree char *target = powershell_path(&v->detail.path);
        WCHAR module[32768];
        DWORD units = GetModuleFileNameW(NULL, module, G_N_ELEMENTS(module));
        g_autofree char *image = units && units < G_N_ELEMENTS(module) ?
            g_utf16_to_utf8((const gunichar2 *)module, units, NULL, NULL, NULL) : NULL;
        g_autofree char *folder = image ? g_path_get_dirname(image) : NULL;
        g_autofree char *cli = folder ? g_build_filename(folder, "diskscope-cli.exe", NULL) : NULL;
        DsNativePath executable = {0};
        if (ds_path_from_arg(cli, &executable, NULL) != DS_OK) {
            ui_message(v, UI("DiskScope CLI 경로를 확인할 수 없습니다"), NULL);
            return;
        }
        g_autofree char *tool = powershell_path(&executable);
        ds_path_clear(&executable);
        command = g_strdup_printf(
            "$diskScopeResult = [IO.Path]::Combine([IO.Path]::GetTempPath(), "
            "'diskscope-retry-' + [Guid]::NewGuid().ToString('N') + '.dscan'); "
            "& %s scan --root %s --output $diskScopeResult; "
            "if (Test-Path -LiteralPath $diskScopeResult) { $diskScopeResult }", tool, target);
    } else {
        g_autofree char *extended = g_strconcat("\\\\?\\", path, NULL);
        DsNativePath literal = {0};
        if (ds_path_from_arg(extended, &literal, NULL) != DS_OK)
            return;
        g_autofree char *target = powershell_path(&literal);
        ds_path_clear(&literal);
        command = g_strdup_printf(
            "Microsoft.PowerShell.Management\\Get-Item -LiteralPath %s -Force | "
            "Microsoft.PowerShell.Utility\\Select-Object FullName,Length,Attributes,LastWriteTime", target);
    }
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(v->window)), command);
    ui_message(v, UI("선택 항목의 재확인 명령을 복사했습니다"),
        v->selected_kind == DS_DIRECTORY ?
        UI("PowerShell을 관리자 권한으로 열고 붙여넣으세요. 출력된 .dscan 경로를 결과 열기에서 선택하세요.") :
        UI("PowerShell을 관리자 권한으로 열고 붙여넣으면 선택한 파일의 메타데이터를 확인할 수 있습니다."));
}
#endif

void ui_label(GtkWidget *widget, const char *text)
{
    gtk_label_set_text(GTK_LABEL(widget), text ? text : "");
}

void ui_message(View *v, const char *title, const char *detail)
{
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title);
    if (detail && *detail)
        gtk_alert_dialog_set_detail(dialog, detail);
    gtk_alert_dialog_show(dialog, v->window);
    g_object_unref(dialog);
}

bool ui_alive(View *v)
{
    if (!v->session)
        return false;
    DsSessionEvent event;
    ds_session_event(v->session, &event);
    return !event.exited;
}

void ui_controls(View *v)
{
    bool running = ui_alive(v);
    bool result = v->db.raw && !running && !v->busy;
    DsSessionEvent event = {0};
    if (v->session)
        ds_session_event(v->session, &event);
    gtk_widget_set_sensitive(v->start, !running && !v->busy);
    gtk_widget_set_sensitive(v->open, !running && !v->busy);
    gtk_widget_set_sensitive(v->pause, running && event.stage == DS_RUNNING);
    gtk_widget_set_sensitive(v->resume, running && (event.stage == DS_PAUSE_REQUESTED ||
                                                     event.stage == DS_PAUSED));
    gtk_widget_set_sensitive(v->cancel, (running && event.stage < DS_CANCEL_REQUESTED) ||
                                       v->operation != NULL);
    gtk_widget_set_sensitive(v->save, result);
    guint export_scope = gtk_drop_down_get_selected(v->export_scope);
    bool filtered = false;
    if (result && export_scope == UI_EXPORT_FILTER && v->mode != EXTENSIONS && v->mode != DATES) {
        bool tree;
        ui_result_query(v, &tree);
        filtered = !tree;
    }
    gtk_widget_set_sensitive(v->export, result && (export_scope == UI_EXPORT_ALL ||
        (export_scope == UI_EXPORT_SELECTION && v->export_selection > 0) || filtered));
    gtk_widget_set_sensitive(v->compare, result);
    bool partial_recovery = v->recovered &&
        !v->recovered_aggregated && ds_recovered_info(v->recovered)->coverage == DS_COVERAGE_PARTIAL;
    gtk_widget_set_sensitive(v->aggregate, result && (partial_recovery ||
                             (v->session && event.result != DS_OK)));
    gtk_widget_set_sensitive(v->recover, !running && !v->busy);
    gtk_widget_set_sensitive(v->diagnostics, !v->busy && (!running || v->db.raw));
    gtk_widget_set_sensitive(v->diagnostics_root, v->db.raw != NULL);
    gtk_widget_set_tooltip_text(v->cancel, v->operation && v->operation_type == DIAGNOSTICS ?
        UI("진단 보고서 저장 취소") : NULL);
    gtk_widget_set_sensitive(v->copy, v->detail.path.raw != NULL);
    gtk_widget_set_sensitive(v->reveal, v->detail.path.raw != NULL);
    gtk_widget_set_sensitive(v->rescan, v->detail.path.raw && v->selected_kind == DS_DIRECTORY &&
                                        !running && !v->busy);
#ifdef G_OS_WIN32
    gtk_widget_set_sensitive(v->retry_admin, can_retry_admin(v));
#endif
}

static const char *stage_name(const DsSessionEvent *e)
{
    switch (e->stage) {
    case DS_PREPARING: return UI("작업자 시작 중");
    case DS_RUNNING: return e->stalled ? UI("스캔 지연 중 · 부분 결과") : UI("스캔 중 · 부분 결과");
    case DS_PAUSE_REQUESTED: return UI("일시정지 요청 · 진행 중인 작업 대기");
    case DS_PAUSED: return UI("일시정지됨 · 부분 결과");
    case DS_FINALIZING: return UI("수집된 항목 집계 중");
    case DS_CANCEL_REQUESTED: return UI("취소 요청");
    case DS_CANCEL_WAITING: return UI("취소 요청 · 작업자 종료 대기 · 부분 결과 갱신 가능");
    case DS_WORKER_LOST: return UI("작업자 종료 오류 · 마지막 커밋만 표시");
    case DS_FINISHED:
        if (e->result == DS_OK) return UI("완료");
        if (e->result == DS_PARTIAL) return UI("범위 누락이 있는 부분 결과");
        if (e->result == DS_CANCELED) return UI("취소됨 · 부분 결과");
        return UI("스캔 중단 · 마지막 커밋만 표시");
    }
    return "";
}

gboolean ui_tick(gpointer data)
{
    View *v = data;
    if (!v->session || v->finished)
        return G_SOURCE_CONTINUE;
    DsSessionEvent e;
    ds_session_event(v->session, &e);
    int64_t now = g_get_monotonic_time();
    char *status = g_strdup_printf(UI("%s   ·   %" G_GUINT64_FORMAT "개   ·   %" G_GINT64_FORMAT "초"),
        stage_name(&e), e.observations, (now - v->started) / G_USEC_PER_SEC);
    ui_label(v->status, status);
    g_free(status);
    if (e.detail[0])
        gtk_widget_set_tooltip_text(v->status, e.detail);
    if (e.commit_seq && (e.exited || (e.commit_seq != v->sequence && now - v->refreshed >= G_USEC_PER_SEC))) {
        v->sequence = e.commit_seq;
        v->refreshed = now;
        if (e.exited || !v->db.raw) {
            bool first_result = !v->db.raw;
            ui_copy_path(&v->db, ds_session_db_path(v->session));
            if (first_result)
                ui_volume_load(v);
            ui_refresh(v);
            ui_details(v, 1, 1, NULL);
        } else {
            ui_live_refresh(v);
        }
    }
    if (!e.exited && v->db.raw)
        ui_live_tick(v);
    v->finished = e.exited;
    ui_controls(v);
    return G_SOURCE_CONTINUE;
}

static void clipboard(View *v)
{
    char *text = ds_path_arg(&v->detail.path, NULL);
    if (!text || !g_utf8_validate(text, -1, NULL)) {
        g_free(text);
        text = NULL;
        ds_path_display(&v->detail.path, &text, NULL);
    }
    if (text && v->detail.path.raw) {
        GValue value = G_VALUE_INIT;
        g_value_init(&value, G_TYPE_STRING);
        g_value_set_string(&value, text);
        GdkContentProvider *parts[] = {
            gdk_content_provider_new_for_value(&value),
            gdk_content_provider_new_for_bytes("application/x-diskscope-native-path", v->detail.path.raw)
        };
        GdkContentProvider *provider = gdk_content_provider_new_union(parts, 2);
        gdk_clipboard_set_content(gtk_widget_get_clipboard(GTK_WIDGET(v->window)), provider);
        g_object_unref(provider);
        g_value_unset(&value);
    }
    g_free(text);
}

static void checked_reveal(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)data;
    GFile *parent = G_FILE(source);
    GFileInfo *info = g_file_query_info_finish(parent, result, NULL);
    if (info && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        char *uri = g_file_get_uri(parent);
        g_app_info_launch_default_for_uri_async(uri, NULL, NULL, NULL, NULL);
        g_free(uri);
    }
    g_clear_object(&info);
}

static void reveal(View *v)
{
    char *path = ds_path_arg(&v->detail.path, NULL);
    if (path && g_path_is_absolute(path)) {
        GFile *file = g_file_new_for_path(path);
        GFile *parent = g_file_get_parent(file);
        if (parent) {
            g_file_query_info_async(parent, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, NULL, checked_reveal, NULL);
            g_object_unref(parent);
        }
        g_object_unref(file);
    }
    g_free(path);
}

static void action(GtkButton *button, gpointer data)
{
    View *v = data;
    const char *name = g_object_get_data(G_OBJECT(button), "action");
    if (strcmp(name, "pause") == 0)
        ds_session_request_pause(v->session);
    else if (strcmp(name, "resume") == 0)
        ds_session_request_resume(v->session);
    else if (strcmp(name, "cancel") == 0) {
        if (v->operation)
            g_cancellable_cancel(v->operation);
        if ((!v->operation || v->operation_type != DIAGNOSTICS) && ui_alive(v)) {
            ds_session_request_cancel(v->session);
            ui_tick(v);
        }
    } else if (strcmp(name, "recover") == 0) {
        ui_recover(v, false);
    } else if (strcmp(name, "refresh") == 0) {
        ui_refresh(v);
        ui_details(v, 1, 1, NULL);
    } else if (strcmp(name, "aggregate") == 0) {
        Work *work = g_new0(Work, 1);
        work->op = AGGREGATE;
        ui_copy_path(&work->db, &v->db);
        ui_launch(v, work);
    } else if (strcmp(name, "copy") == 0) {
        clipboard(v);
    } else if (strcmp(name, "reveal") == 0) {
        reveal(v);
#ifdef G_OS_WIN32
    } else if (strcmp(name, "retry-admin") == 0) {
        retry_admin(v);
#endif
    } else if (strcmp(name, "rescan") == 0) {
        Work *w = g_new0(Work, 1);
        w->op = START;
        w->include_hidden = !gtk_check_button_get_active(GTK_CHECK_BUTTON(v->hide));
        w->profile = gtk_drop_down_get_selected(v->profile) == 1 ? DS_STANDARD : DS_CONSERVATIVE;
        ui_copy_path(&w->target, &v->detail.path);
        ui_copy_path(&w->cache, &v->cache);
        ui_launch(v, w);
    } else {
        ui_file_dialog(v, (WorkType)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "operation")));
    }
    ui_controls(v);
}

static void diagnostics_save(GtkButton *button, gpointer data)
{
    (void)button;
    View *v = data;
    bool include_root = v->db.raw && gtk_check_button_get_active(GTK_CHECK_BUTTON(v->diagnostics_root));
    gtk_popover_popdown(v->diagnostics_options);
    ui_diagnostics_dialog(v, include_root);
}

static void diagnostics_reset(GtkPopover *popover, gpointer data)
{
    (void)popover;
    View *v = data;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(v->diagnostics_root), FALSE);
}

static GtkWidget *button(View *v, GtkWidget *box, const char *text, const char *name, WorkType op)
{
    GtkWidget *widget = gtk_button_new_with_label(text);
    g_object_set_data(G_OBJECT(widget), "action", (gpointer)name);
    g_object_set_data(G_OBJECT(widget), "operation", GINT_TO_POINTER(op));
    g_signal_connect(widget, "clicked", G_CALLBACK(action), v);
    gtk_box_append(GTK_BOX(box), widget);
    return widget;
}

static void switch_mode(GtkButton *button, gpointer data)
{
    View *v = data;
    v->mode = (ViewMode)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "view-mode"));
    ui_refresh(v);
    ui_controls(v);
}

static void dropdown_changed(GObject *object, GParamSpec *param, gpointer data)
{
    (void)param;
    View *v = data;
    if (object == G_OBJECT(v->theme)) {
        guint theme = gtk_drop_down_get_selected(v->theme);
        GtkSettings *settings = gtk_settings_get_default();
        if (theme)
            g_object_set(settings, "gtk-application-prefer-dark-theme", theme == 2, NULL);
        else
            gtk_settings_reset_property(settings, "gtk-application-prefer-dark-theme");
    } else if (object == G_OBJECT(v->export_scope)) {
        ui_controls(v);
    } else if (object == G_OBJECT(v->metric) || object == G_OBJECT(v->cap)) {
        ui_refresh(v);
        ui_details(v, 1, 1, NULL);
        ui_controls(v);
    }
    if (object == G_OBJECT(v->theme) || object == G_OBJECT(v->profile) || object == G_OBJECT(v->metric))
        ui_settings_save(v);
}

static void settings_changed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    ui_settings_save(data);
}

static GtkDropDown *dropdown(View *v, GtkWidget *box, const char *const *values)
{
    GtkWidget *widget = gtk_drop_down_new_from_strings(values);
    gtk_box_append(GTK_BOX(box), widget);
    g_signal_connect(widget, "notify::selected", G_CALLBACK(dropdown_changed), v);
    return GTK_DROP_DOWN(widget);
}

static gboolean apply_filter(gpointer data)
{
    View *v = data;
    v->debounce = 0;
    ui_refresh(v);
    ui_controls(v);
    return G_SOURCE_REMOVE;
}

static void filter_changed(GtkEditable *entry, gpointer data)
{
    (void)entry;
    View *v = data;
    if (v->debounce)
        g_source_remove(v->debounce);
    v->debounce = g_timeout_add(200, apply_filter, v);
}

static GtkWidget *filter(View *v, GtkWidget *box, const char *text)
{
    GtkWidget *field = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(field), text);
    gtk_editable_set_width_chars(GTK_EDITABLE(field), 9);
    gtk_box_append(GTK_BOX(box), field);
    g_signal_connect(field, "changed", G_CALLBACK(filter_changed), v);
    return field;
}

static GtkWidget *scroll(GtkWidget *child)
{
    GtkWidget *widget = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(widget), child);
    return widget;
}

static void dispose_view(gpointer ptr)
{
    View *v = ptr;
    ui_live_clear(v);
    if (v->debounce) g_source_remove(v->debounce);
    if (v->ticker) g_source_remove(v->ticker);
    g_clear_object(&v->detail_task);
    g_clear_object(&v->volume_task);
    g_clear_object(&v->operation);
    g_clear_object(&v->selection);
    g_clear_object(&v->base);
    ds_session_unref(v->session);
    ds_recovered_free(v->recovered);
    ds_cache_lease_close(v->import_lease);
    ds_path_clear(&v->cache);
    ds_path_clear(&v->db);
    ds_details_clear(&v->detail);
    ds_treemap_free(v->layout);
    g_free(v);
}

static gboolean close_window(GtkWindow *window, gpointer data)
{
    (void)window;
    View *v = data;
    v->closing = true;
    if (v->debounce) {
        g_source_remove(v->debounce);
        v->debounce = 0;
    }
    if (v->ticker) {
        g_source_remove(v->ticker);
        v->ticker = 0;
    }
    if (v->detail_task) g_cancellable_cancel(v->detail_task);
    if (v->volume_task) g_cancellable_cancel(v->volume_task);
    if (v->operation) g_cancellable_cancel(v->operation);
    ds_session_request_cancel(v->session);
    return FALSE;
}

static void toolbar(View *v, GtkWidget *root)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(root), bar);
    v->start = button(v, bar, UI("드라이브·폴더 스캔"), "file", START);
    gtk_widget_add_css_class(v->start, "suggested-action");
    v->pause = button(v, bar, UI("일시정지"), "pause", 0);
    v->resume = button(v, bar, UI("계속"), "resume", 0);
    v->cancel = button(v, bar, UI("취소"), "cancel", 0);
    v->open = button(v, bar, UI("결과 열기"), "file", OPEN);
    v->save = button(v, bar, UI("저장"), "file", SAVE);
    v->compare = button(v, bar, UI("비교"), "file", COMPARE);
    v->format = dropdown(v, bar, (const char *const[]){"CSV", "JSON", "JSONL", NULL});
    v->export_scope = dropdown(v, bar, (const char *const[]){
        UI("전체"), UI("선택 항목"), UI("필터 일치 전체"), NULL});
    gtk_widget_set_tooltip_text(GTK_WIDGET(v->export_scope), UI("내보낼 결과 범위"));
    v->export = button(v, bar, UI("내보내기"), "file", EXPORT);
    v->theme = dropdown(v, bar, (const char *const[]){UI("시스템 테마"), UI("밝게"), UI("어둡게"), NULL});

    GtkWidget *filters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(root), filters);
    v->metric = dropdown(v, filters, (const char *const[]){UI("논리 크기"), UI("OS 보고 할당량"), UI("중복 제외 할당량"), NULL});
    v->search = filter(v, filters, UI("이름·경로 검색"));
    gtk_widget_set_hexpand(v->search, TRUE);
    v->extension = filter(v, filters, UI("확장자"));
    v->minimum = filter(v, filters, UI("최소 바이트"));
    v->maximum = filter(v, filters, UI("최대 바이트"));
    v->from = filter(v, filters, UI("수정일 시작"));
    v->to = filter(v, filters, UI("수정일 끝"));
    gtk_widget_set_tooltip_text(v->from, UI("UTC 날짜 YYYY-MM-DD"));
    gtk_widget_set_tooltip_text(v->to, UI("UTC 날짜 YYYY-MM-DD"));
    v->cap = dropdown(v, filters, (const char *const[]){"Top 100", "Top 1000", NULL});
}

static GtkWidget *sidebar(View *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    const char *views[] = {UI("폴더 트리"), UI("큰 파일"), UI("큰 폴더"), UI("확장자 통계"),
                           UI("수정일 통계"), UI("오류·제외")};
    for (size_t i = 0; i < G_N_ELEMENTS(views); ++i) {
        GtkWidget *item = gtk_button_new_with_label(views[i]);
        g_object_set_data(G_OBJECT(item), "view-mode", GINT_TO_POINTER((int)i));
        g_signal_connect(item, "clicked", G_CALLBACK(switch_mode), v);
        gtk_box_append(GTK_BOX(box), item);
    }
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    v->profile = dropdown(v, box, (const char *const[]){UI("보수 모드"), UI("일반 모드"), NULL});
    v->hide = gtk_check_button_new_with_label(UI("숨김 항목 제외"));
    gtk_box_append(GTK_BOX(box), v->hide);
    v->exclude = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(v->exclude), UI("제외 이름 패턴"));
    gtk_editable_set_width_chars(GTK_EDITABLE(v->exclude), 11);
    gtk_box_append(GTK_BOX(box), v->exclude);
    g_signal_connect(v->hide, "toggled", G_CALLBACK(settings_changed), v);
    g_signal_connect(v->exclude, "activate", G_CALLBACK(settings_changed), v);
    button(v, box, UI("캐시 위치"), "file", CACHE);
    v->recover = button(v, box, UI("최근 캐시 열기"), "recover", 0);
    button(v, box, UI("결과 새로고침"), "refresh", 0);
    v->aggregate = button(v, box, UI("부분 결과 집계"), "aggregate", 0);
    v->diagnostics = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(v->diagnostics), UI("진단 내보내기"));
    gtk_box_append(GTK_BOX(box), v->diagnostics);
    v->diagnostics_options = GTK_POPOVER(gtk_popover_new());
    GtkWidget *options = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(options, 8);
    gtk_widget_set_margin_end(options, 8);
    gtk_widget_set_margin_top(options, 8);
    gtk_widget_set_margin_bottom(options, 8);
    gtk_popover_set_child(v->diagnostics_options, options);
    v->diagnostics_root = gtk_check_button_new_with_label(UI("스캔 루트 경로 포함"));
    gtk_box_append(GTK_BOX(options), v->diagnostics_root);
    GtkWidget *report = gtk_button_new_with_label(UI("진단 JSON 저장"));
    gtk_box_append(GTK_BOX(options), report);
    g_signal_connect(report, "clicked", G_CALLBACK(diagnostics_save), v);
    g_signal_connect(v->diagnostics_options, "closed", G_CALLBACK(diagnostics_reset), v);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(v->diagnostics), GTK_WIDGET(v->diagnostics_options));
    gtk_widget_set_tooltip_text(v->diagnostics, UI("기본 보고서는 경로와 파일명을 숨깁니다."));
    return box;
}

static GtkWidget *explorer(View *v)
{
    v->stack = gtk_stack_new();
    GtkWidget *vertical = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_stack_add_named(GTK_STACK(v->stack), vertical, "tree");
    v->table = GTK_COLUMN_VIEW(gtk_column_view_new(NULL));
    ui_columns(v);
    gtk_paned_set_start_child(GTK_PANED(vertical), scroll(GTK_WIDGET(v->table)));
    gtk_paned_set_position(GTK_PANED(vertical), 430);
    GtkWidget *map_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    v->legend = gtk_label_new(UI("트리맵"));
    gtk_label_set_xalign(GTK_LABEL(v->legend), 0);
    gtk_box_append(GTK_BOX(map_box), v->legend);
    v->map = gtk_drawing_area_new();
    gtk_widget_set_vexpand(v->map, TRUE);
    ui_map(v);
    gtk_box_append(GTK_BOX(map_box), v->map);
    gtk_paned_set_end_child(GTK_PANED(vertical), map_box);

    GtkWidget *statistics = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(statistics), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(statistics), GTK_WRAP_WORD_CHAR);
    v->statistics = gtk_text_view_get_buffer(GTK_TEXT_VIEW(statistics));
    gtk_stack_add_named(GTK_STACK(v->stack), scroll(statistics), "statistics");
    gtk_stack_set_visible_child_name(GTK_STACK(v->stack), "tree");
    return v->stack;
}

static GtkWidget *details(View *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(box, 172, -1);
    gtk_widget_set_margin_start(box, 7);
    v->path = gtk_label_new("");
    v->info = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(v->path), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(v->path), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(v->path), TRUE);
    gtk_label_set_xalign(GTK_LABEL(v->path), 0);
    gtk_label_set_xalign(GTK_LABEL(v->info), 0);
    gtk_box_append(GTK_BOX(box), v->path);
    gtk_box_append(GTK_BOX(box), v->info);
    v->copy = button(v, box, UI("원본 경로 복사"), "copy", 0);
    v->reveal = button(v, box, UI("상위 폴더 열기"), "reveal", 0);
    v->rescan = button(v, box, UI("이 폴더 스캔"), "rescan", 0);
#ifdef G_OS_WIN32
    v->retry_admin = button(v, box, UI("관리자 재확인 명령 복사"), "retry-admin", 0);
#endif
    GtkWidget *scope = gtk_label_new(UI("읽기 전용 · 일반 파일 기본 스트림\n링크와 선택 범위 밖은 제외"));
    gtk_label_set_wrap(GTK_LABEL(scope), TRUE);
    gtk_label_set_xalign(GTK_LABEL(scope), 0);
    gtk_box_append(GTK_BOX(box), scope);
    v->volume_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(v->volume_label), 0);
    gtk_label_set_wrap(GTK_LABEL(v->volume_label), TRUE);
    gtk_box_append(GTK_BOX(box), v->volume_label);
    return scroll(box);
}

void ds_window_activate(GtkApplication *app, gpointer data)
{
    (void)data;
    if (gtk_application_get_active_window(app)) {
        gtk_window_present(gtk_application_get_active_window(app));
        return;
    }
    View *v = g_new0(View, 1);
    v->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(v->window, "DiskScope");
    gtk_window_set_default_size(v->window, 1250, 810);
    g_object_set_data_full(G_OBJECT(v->window), "disk-view", v, dispose_view);
    g_signal_connect(v->window, "close-request", G_CALLBACK(close_window), v);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_window_set_child(v->window, root);
    gtk_widget_set_margin_start(root, 10);
    gtk_widget_set_margin_end(root, 10);
    gtk_widget_set_margin_top(root, 9);
    gtk_widget_set_margin_bottom(root, 9);
    toolbar(v, root);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    gtk_widget_set_vexpand(body, TRUE);
    gtk_box_append(GTK_BOX(root), body);
    gtk_box_append(GTK_BOX(body), sidebar(v));
    GtkWidget *split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_hexpand(split, TRUE);
    gtk_box_append(GTK_BOX(body), split);
    gtk_paned_set_start_child(GTK_PANED(split), explorer(v));
    gtk_paned_set_end_child(GTK_PANED(split), details(v));
    gtk_paned_set_position(GTK_PANED(split), 825);
    v->status = gtk_label_new(UI("스캔할 폴더를 선택하거나 결과를 여세요."));
    gtk_label_set_xalign(GTK_LABEL(v->status), 0);
    gtk_box_append(GTK_BOX(root), v->status);
    ui_controls(v);
    v->ticker = g_timeout_add(200, ui_tick, v);
    gtk_window_present(v->window);
    ui_settings_load(v);
}

void ds_window_open(GtkApplication *app, GFile **files, int count, const char *hint, gpointer user)
{
    (void)hint;
    ds_window_activate(app, user);
    GtkWindow *window = gtk_application_get_active_window(app);
    View *v = g_object_get_data(G_OBJECT(window), "disk-view");
    if (!count || !v || v->busy || ui_alive(v))
        return;
    char *path = g_file_get_path(files[0]);
    Work *w = g_new0(Work, 1);
    w->op = OPEN;
    ui_copy_path(&w->cache, &v->cache);
    if (path && ds_path_from_arg(path, &w->target, NULL) == DS_OK)
        ui_launch(v, w);
    else
        g_free(w);
    g_free(path);
}

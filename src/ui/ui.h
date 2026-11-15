#ifndef DISKSCOPE_UI_H
#define DISKSCOPE_UI_H

#include "details.h"
#include "cache_lease.h"
#include "paged_model.h"
#include "diskscope/ds_compare.h"
#include "diskscope/ds_diagnostics.h"
#include "diskscope/ds_export.h"
#include "diskscope/ds_session.h"
#include "diskscope/ds_recovery.h"
#include "diskscope/ds_snapshot.h"
#include <gtk/gtk.h>

typedef enum { TREE, TOP_FILE, TOP_DIR, EXTENSIONS, DATES, ISSUES } ViewMode;
typedef enum {
    START, CACHE, OPEN, SAVE, EXPORT, COMPARE, DETAILS, STATS, AGGREGATE, RECOVER, VOLUME,
    DIAGNOSTICS
} WorkType;
typedef enum { UI_EXPORT_ALL, UI_EXPORT_SELECTION, UI_EXPORT_FILTER } UiExportScope;

typedef struct {
    GtkWindow *window;
    GtkColumnView *table;
    GtkSingleSelection *selection;
    GListModel *base;
    GtkDropDown *metric, *theme, *profile, *cap, *format;
    GtkDropDown *export_scope;
    GtkWidget *status, *path, *info, *legend, *map, *start, *pause, *resume, *cancel;
    GtkWidget *save, *export, *compare, *open, *copy, *reveal, *rescan, *hide, *stack, *aggregate, *recover;
    GtkWidget *search, *extension, *minimum, *maximum, *from, *to, *exclude;
    GtkWidget *volume_label;
#ifdef G_OS_WIN32
    GtkWidget *retry_admin;
#endif
    GtkWidget *diagnostics, *diagnostics_root;
    GtkPopover *diagnostics_options;
    GtkTextBuffer *statistics;
    DsSession *session;
    DsRecoveredScan *recovered;
    DsCacheLease *import_lease;
    DsNativePath db, cache;
    DsDetails detail;
    GQueue live_models;
    DsTreemapLayout *layout;
    GCancellable *detail_task, *volume_task, *operation;
    guint ticker, debounce;
    int64_t sequence, refreshed, started, selected, mapped_folder;
    int64_t export_selection;
    int64_t focused_folder;
    guint generation;
    guint live_pending;
    DsKind selected_kind;
    DsReason selected_reason;
    int64_t selected_os_code;
    ViewMode mode;
    WorkType operation_type;
    bool busy, finished, closing, applying_settings, cache_location_changed, recovered_aggregated;
    bool live_tree, expand_root, preview_pending, list_pending;
} View;

typedef struct {
    WorkType op;
    DsNativePath db, target, cache;
    DsSession *session;
    DsRecoveredScan *recovered;
    DsCacheLease *import_lease;
    DsSessionEvent worker_event;
    DsDetails detail;
    DsError error;
    DsStatus status;
    DsMetric metric;
    DsProfile profile;
    DsExportFormat format;
    DsQuery export_filter;
    int64_t id, folder;
    guint generation;
    bool include_hidden, group_age, automatic, export_filtered;
    bool include_root, has_worker_event, existing_latest;
    bool preview;
    char *exclude, *report;
} Work;

#define UI(text) g_dpgettext2(NULL, "DiskScope", text)

void ui_refresh(View *);
void ui_live_clear(View *);
void ui_live_refresh(View *);
void ui_live_tick(View *);
void ui_details(View *, int64_t, int64_t, const DsRow *);
void ui_columns(View *);
void ui_map(View *);
DsQuery ui_result_query(View *, bool *tree);
void ui_volume_load(View *);
void ui_launch(View *, Work *);
void ui_file_dialog(View *, WorkType);
void ui_diagnostics_dialog(View *, bool include_root);
void ui_copy_path(DsNativePath *, const DsNativePath *);
void ui_label(GtkWidget *, const char *);
void ui_message(View *, const char *, const char *);
void ui_controls(View *);
bool ui_alive(View *);
DsMetric ui_metric(View *);
gboolean ui_tick(gpointer);
void ui_settings_load(View *);
void ui_settings_save(View *);
void ui_recover(View *, bool automatic);

#endif

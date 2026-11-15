#ifndef DISKSCOPE_UI_H
#define DISKSCOPE_UI_H

#include "details.h"
#include "paged_model.h"
#include "diskscope/ds_compare.h"
#include "diskscope/ds_export.h"
#include "diskscope/ds_session.h"
#include "diskscope/ds_snapshot.h"
#include <gtk/gtk.h>

typedef enum { TREE, TOP_FILE, TOP_DIR, EXTENSIONS, DATES, ISSUES } ViewMode;
typedef enum { START, CACHE, OPEN, SAVE, EXPORT, COMPARE, DETAILS, STATS, AGGREGATE } WorkType;

typedef struct {
    GtkWindow *window;
    GtkColumnView *table;
    GtkSingleSelection *selection;
    GListModel *base;
    GtkDropDown *metric, *theme, *profile, *cap, *format;
    GtkWidget *status, *path, *info, *legend, *map, *start, *pause, *resume, *cancel;
    GtkWidget *save, *export, *compare, *open, *copy, *reveal, *rescan, *hide, *stack, *aggregate;
    GtkWidget *search, *extension, *minimum, *maximum, *from, *to, *exclude;
    GtkTextBuffer *statistics;
    DsSession *session;
    DsNativePath db, cache;
    DsDetails detail;
    DsTreemapLayout *layout;
    GCancellable *detail_task, *operation;
    guint ticker, debounce;
    int64_t sequence, refreshed, started, selected, mapped_folder;
    guint generation;
    DsKind selected_kind;
    ViewMode mode;
    bool busy, finished, closing, applying_settings;
} View;

typedef struct {
    WorkType op;
    DsNativePath db, target, cache;
    DsSession *session;
    DsDetails detail;
    DsError error;
    DsStatus status;
    DsMetric metric;
    DsProfile profile;
    DsExportFormat format;
    int64_t id, folder;
    guint generation;
    bool include_hidden, group_age;
    char *exclude, *report;
} Work;

#define UI(text) g_dpgettext2(NULL, "DiskScope", text)

void ui_refresh(View *);
void ui_details(View *, int64_t, int64_t, const DsRow *);
void ui_columns(View *);
void ui_map(View *);
void ui_launch(View *, Work *);
void ui_file_dialog(View *, WorkType);
void ui_copy_path(DsNativePath *, const DsNativePath *);
void ui_label(GtkWidget *, const char *);
void ui_message(View *, const char *, const char *);
void ui_controls(View *);
bool ui_alive(View *);
DsMetric ui_metric(View *);
gboolean ui_tick(gpointer);
void ui_settings_load(View *);
void ui_settings_save(View *);

#endif

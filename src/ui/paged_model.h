#ifndef DISKSCOPE_PAGED_MODEL_H
#define DISKSCOPE_PAGED_MODEL_H

#include "diskscope/ds_query.h"
#include <gio/gio.h>

/* Model creation, query changes and row access all belong to the UI context.
 * The model copies the database path and the query's strings. */
GListModel *ds_paged_model_new(const DsNativePath *db, const DsQuery *query);
void ds_paged_model_set_query(GListModel *model, const DsQuery *query);
void ds_paged_model_set_cap(GListModel *model, guint cap);
void ds_paged_model_refresh(GListModel *model);
/* Recount a fixed parent's unfiltered ascending-ID query on an append-only
 * store. Cached rows survive, and new positions are populated on demand. */
void ds_paged_model_update_appended(GListModel *model);
bool ds_paged_model_busy(GListModel *model);
const DsRow *ds_ui_row_get(GObject *item);

#endif

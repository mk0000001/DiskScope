#ifndef DISKSCOPE_UI_VOLUME_VIEW_H
#define DISKSCOPE_UI_VOLUME_VIEW_H

#include "diskscope/ds_store.h"

/* Returns owned plain text for the OS volume card using only store data. */
char *ds_ui_volume_summary(DsStore *store, DsError *error);

#endif

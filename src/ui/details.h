#ifndef DS_UI_DETAILS_H
#define DS_UI_DETAILS_H
#include "diskscope/ds_query.h"
#include "diskscope/ds_treemap.h"
#include <gio/gio.h>

typedef struct {
    DsNativePath path;
    GArray *map; /* DsTreemapItem, positive weights; zero/unknown have separate counts. */
    uint64_t zero, unknown, other;
} DsDetails;

DsStatus ds_details_load(DsStore *, int64_t id, int64_t folder, DsMetric metric,
                         bool preview, GCancellable *, DsDetails *, DsError *);
void ds_details_clear(DsDetails *);
DsStatus ds_statistics_report(DsStore *, DsMetric, bool group_age, GCancellable *,
                              char **out, DsError *);
#endif

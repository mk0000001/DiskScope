#ifndef DISKSCOPE_TREEMAP_H
#define DISKSCOPE_TREEMAP_H

#include "ds_types.h"

#define DS_TREEMAP_MAX 2000u
typedef struct { double x, y, width, height; } DsRect;
typedef struct { int64_t id; DsSize size; } DsTreemapItem;
typedef struct { int64_t id; DsRect rect; } DsTreemapCell;
typedef struct {
    DsTreemapCell *cells;
    size_t count, zero_count, unknown_count, other_count;
} DsTreemapLayout;

DsStatus ds_treemap_layout(const DsTreemapItem *, size_t, const DsRect *,
                            DsTreemapLayout **, DsError *);
void ds_treemap_free(DsTreemapLayout *);
int64_t ds_treemap_hit(const DsTreemapLayout *, double x, double y);

#endif

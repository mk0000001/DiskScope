#include "diskscope/ds_treemap.h"

#include <math.h>

typedef struct { int64_t id; long double weight; } Weight;
typedef struct { size_t begin, end; DsRect rect; } Group;

DsStatus ds_treemap_layout(const DsTreemapItem *items, size_t count, const DsRect *bounds,
                            DsTreemapLayout **out, DsError *error)
{
    if (!out || !bounds || (!items && count) ||
        !isfinite(bounds->x) || !isfinite(bounds->y) ||
        !isfinite(bounds->width) || !isfinite(bounds->height) ||
        bounds->width <= 0 || bounds->height <= 0 ||
        !isfinite(bounds->x + bounds->width) || !isfinite(bounds->y + bounds->height))
        return ds_error(error, DS_INVALID, 0, "Invalid treemap bounds or input");
    DsTreemapLayout *layout = g_new0(DsTreemapLayout, 1);
    Weight *weights = g_new0(Weight, DS_TREEMAP_MAX + 1);
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t size = items[i].size.bytes;
        if (items[i].size.quality != DS_KNOWN)
            ++layout->unknown_count;
        else if (size == 0)
            ++layout->zero_count;
        else if (n < DS_TREEMAP_MAX)
            weights[n++] = (Weight){items[i].id, (long double)size};
        else {
            weights[n].weight += (long double)size;
            ++layout->other_count;
        }
    }
    n += layout->other_count > 0;
    layout->cells = g_new(DsTreemapCell, n);
    Group *stack = g_new(Group, n);
    size_t groups = 0;
    if (n)
        stack[groups++] = (Group){0, n, *bounds};
    while (groups) {
        Group group = stack[--groups];
        if (group.end - group.begin == 1) {
            layout->cells[layout->count++] =
                (DsTreemapCell){weights[group.begin].id, group.rect};
            continue;
        }
        long double total = 0, left = weights[group.begin].weight;
        for (size_t i = group.begin; i < group.end; ++i)
            total += weights[i].weight;
        size_t midpoint = group.begin + 1;
        while (midpoint + 1 < group.end && left < total * 0.5L) {
            long double next = left + weights[midpoint].weight;
            if (next >= total - left)
                break;
            left = next;
            ++midpoint;
        }
        double ratio = (double)(left / total);
        DsRect a = group.rect, b = group.rect;
        if (a.width >= a.height) {
            a.width *= ratio;
            b.x += a.width;
            b.width -= a.width;
        } else {
            a.height *= ratio;
            b.y += a.height;
            b.height -= a.height;
        }
        stack[groups++] = (Group){midpoint, group.end, b};
        stack[groups++] = (Group){group.begin, midpoint, a};
    }
    g_free(stack);
    g_free(weights);
    *out = layout;
    return DS_OK;
}

void ds_treemap_free(DsTreemapLayout *layout)
{
    if (layout) {
        g_free(layout->cells);
        g_free(layout);
    }
}

int64_t ds_treemap_hit(const DsTreemapLayout *layout, double x, double y)
{
    if (layout) {
        for (size_t i = 0; i < layout->count; ++i) {
            const DsTreemapCell *cell = &layout->cells[i];
            if (x >= cell->rect.x && x < cell->rect.x + cell->rect.width &&
                y >= cell->rect.y && y < cell->rect.y + cell->rect.height)
                return cell->id;
        }
    }
    return -1;
}

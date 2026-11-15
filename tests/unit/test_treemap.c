#include "diskscope/ds_treemap.h"
#include <math.h>

static void geometry(void)
{
    DsTreemapItem items[] = {{1,{30,DS_KNOWN}}, {2,{70,DS_KNOWN}},
                             {3,{0,DS_KNOWN}}, {4,{0,DS_UNAVAILABLE}}};
    DsRect bounds = {10,20,100,100};
    DsTreemapLayout *layout = NULL;
    g_assert_cmpint(ds_treemap_layout(items, G_N_ELEMENTS(items), &bounds, &layout, NULL), ==, DS_OK);
    g_assert_cmpuint(layout->count, ==, 2);
    g_assert_cmpuint(layout->zero_count, ==, 1);
    g_assert_cmpuint(layout->unknown_count, ==, 1);
    for (size_t i = 0; i < 2; ++i) {
        DsTreemapCell a = layout->cells[i], b = layout->cells[1-i];
        g_assert_cmpfloat(a.rect.x, >=, bounds.x);
        g_assert_cmpfloat(a.rect.y, >=, bounds.y);
        g_assert_cmpfloat(a.rect.x + a.rect.width, <=, bounds.x + bounds.width);
        g_assert_cmpfloat(a.rect.y + a.rect.height, <=, bounds.y + bounds.height);
        g_assert_true(a.rect.x + a.rect.width <= b.rect.x ||
                      b.rect.x + b.rect.width <= a.rect.x ||
                      a.rect.y + a.rect.height <= b.rect.y ||
                      b.rect.y + b.rect.height <= a.rect.y);
        g_assert_cmpfloat_with_epsilon(a.rect.width * a.rect.height,
            a.id == 1 ? 3000.0 : 7000.0, 0.00001);
        g_assert_cmpint(ds_treemap_hit(layout, a.rect.x + a.rect.width / 2,
            a.rect.y + a.rect.height / 2), ==, a.id);
    }
    ds_treemap_free(layout);
}

static void other(void)
{
    DsTreemapItem *items = g_new(DsTreemapItem, 2010);
    for (size_t i = 0; i < 2010; ++i) items[i] = (DsTreemapItem){(int64_t)i+1,{1,DS_KNOWN}};
    DsTreemapLayout *layout = NULL;
    DsRect bounds = {0,0,100,100};
    g_assert_cmpint(ds_treemap_layout(items, 2010, &bounds, &layout, NULL), ==, DS_OK);
    g_assert_cmpuint(layout->count, ==, 2001);
    g_assert_cmpuint(layout->other_count, ==, 10);
    ds_treemap_free(layout);
    g_free(items);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/geometry", geometry);
    g_test_add_func("/other_bucket", other);
    return g_test_run();
}

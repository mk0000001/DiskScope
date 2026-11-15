#include "diskscope/ds_query.h"
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    char *dir, *file;
    DsNativePath path;
    DsStore *store;
} Fixture;

static void setup(Fixture *f, const void *unused)
{
    (void)unused;
    DsNativePath root = {0};
    f->dir = g_dir_make_tmp("ds-aggregate-XXXXXX", NULL);
    g_assert_nonnull(f->dir);
    f->file = g_build_filename(f->dir, "scan.db", NULL);
    g_assert_cmpint(ds_path_from_arg(f->file, &f->path, NULL), ==, DS_OK);
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, "/fixture", 8, &root, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_open(&f->path, DS_STORE_WRITER, &f->store, NULL), ==, DS_OK);
    DsScanOptions options = {.root = root, .cache_budget = DS_CACHE_BUDGET};
    g_assert_cmpint(ds_store_set_options(f->store, &options, NULL), ==, DS_OK);
    ds_path_clear(&root);
}

static void teardown(Fixture *f, const void *unused)
{
    (void)unused;
    ds_store_close(f->store);
    ds_path_clear(&f->path);
    g_assert_cmpint(g_remove(f->file), ==, 0);
    g_assert_cmpint(g_rmdir(f->dir), ==, 0);
    g_free(f->file);
    g_free(f->dir);
}

static DsEntry make_entry(int64_t id, int64_t parent, DsKind kind, const char *name,
                          uint64_t logical, uint64_t allocated, uint8_t identity)
{
    DsEntry e = {
        .id = id, .parent_id = parent, .kind = kind, .identity_known = identity != 0,
        .logical = {logical, DS_KNOWN}, .allocated = {allocated, DS_KNOWN}, .links = 2,
    };
    e.identity[0] = identity;
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, name, strlen(name), &e.name, NULL), ==, DS_OK);
    return e;
}

static void append(Fixture *f, DsEntry *entries, size_t count)
{
    DsEntryBatch batch = {entries, count};
    DsError error = {0};
    DsStatus status = ds_store_append(f->store, &batch, &error);
    g_assert_cmpint(status, ==, DS_OK);
    for (size_t i = 0; i < count; ++i)
        ds_path_clear(&entries[i].name);
}

static DsPage *read_all(Fixture *f)
{
    DsQuery q = {.parent_id = DS_QUERY_ALL, .limit = 16};
    DsPage *page = NULL;
    g_assert_cmpint(ds_query_page(f->store, &q, &page, NULL, NULL), ==, DS_OK);
    return page;
}

static void hardlinks(Fixture *f, const void *reverse)
{
    int aid = reverse ? 3 : 2, bid = reverse ? 2 : 3;
    int afile = reverse ? 5 : 4, bfile = reverse ? 4 : 5;
    DsEntry entries[] = {
        make_entry(1, 0, DS_DIRECTORY, "", 0, 0, 0),
        make_entry(2, 1, DS_DIRECTORY, reverse ? "A0" : "A", 0, 0, 0),
        make_entry(3, 1, DS_DIRECTORY, reverse ? "A" : "A0", 0, 0, 0),
        make_entry(4, reverse ? bid : aid, DS_FILE, reverse ? "b.bin" : "a.bin", 1000, 4096, 1),
        make_entry(5, reverse ? aid : bid, DS_FILE, reverse ? "a.bin" : "b.bin", 1000, 4096, 1),
        make_entry(6, bid, DS_FILE, "c.bin", 17, 4096, 2),
    };
    append(f, entries, G_N_ELEMENTS(entries));
    g_assert_cmpint(ds_finalize(f->store, NULL, NULL), ==, DS_OK);
    DsPage *page = read_all(f);
    g_assert_cmpuint(page->count, ==, 6);
    DsRow *r = page->rows;
    g_assert_true(page->info.aggregation_complete);
    g_assert_cmpint(page->info.coverage, ==, DS_COVERAGE_PARTIAL);
    g_assert_cmpuint(r[0].metrics[DS_LOGICAL].bytes, ==, 2017);
    g_assert_cmpuint(r[0].metrics[DS_ALLOCATED].bytes, ==, 12288);
    g_assert_cmpuint(r[0].metrics[DS_UNIQUE].bytes, ==, 8192);
    g_assert_cmpuint(r[0].file_count, ==, 3);
    g_assert_cmpuint(r[0].unique_file_count, ==, 2);
    g_assert_cmpuint(r[aid - 1].metrics[DS_UNIQUE].bytes, ==, 4096);
    g_assert_cmpuint(r[bid - 1].metrics[DS_UNIQUE].bytes, ==, 4096);
    g_assert_cmpint(r[afile - 1].representative_id, ==, afile);
    g_assert_cmpint(r[bfile - 1].representative_id, ==, afile);
    g_assert_cmpuint(r[bfile - 1].metrics[DS_UNIQUE].bytes, ==, 0);
    ds_page_free(page);
}

static void unknown_and_conflicting_identity(Fixture *f, const void *unused)
{
    (void)unused;
    DsEntry entries[] = {
        make_entry(1, 0, DS_DIRECTORY, "", 0, 0, 0),
        make_entry(2, 1, DS_FILE, "unknown-id", 99, 512, 0),
        make_entry(3, 1, DS_FILE, "a", 100, 4096, 1),
        make_entry(4, 1, DS_FILE, "b", 101, 8192, 1),
        make_entry(5, 1, DS_FILE, "unknown-allocation", 100, 0, 2),
    };
    entries[4].allocated.quality = DS_UNSUPPORTED;
    append(f, entries, G_N_ELEMENTS(entries));
    g_assert_cmpint(ds_finalize(f->store, NULL, NULL), ==, DS_OK);
    DsPage *page = read_all(f);
    g_assert_cmpuint(page->rows[0].metrics[DS_ALLOCATED].bytes, ==, 12800);
    g_assert_cmpuint(page->rows[0].unknown[DS_ALLOCATED], ==, 1);
    g_assert_cmpuint(page->rows[0].unknown[DS_UNIQUE], ==, 4);
    g_assert_cmpint(page->rows[1].metrics[DS_UNIQUE].quality, ==, DS_UNAVAILABLE);
    g_assert_cmpint(page->rows[2].metrics[DS_UNIQUE].quality, ==, DS_CHANGED);
    g_assert_cmpint(page->rows[3].metrics[DS_UNIQUE].quality, ==, DS_CHANGED);
    g_assert_cmpint(page->rows[4].metrics[DS_UNIQUE].quality, ==, DS_UNSUPPORTED);
    ds_page_free(page);
}

static void overflow(Fixture *f, const void *unused)
{
    (void)unused;
    DsEntry entries[] = {
        make_entry(1, 0, DS_DIRECTORY, "", 0, 0, 0),
        make_entry(2, 1, DS_DIRECTORY, "child", 0, 0, 0),
        make_entry(3, 2, DS_FILE, "max", UINT64_MAX, UINT64_MAX, 1),
        make_entry(4, 2, DS_FILE, "plus-one", 1, 1, 2),
    };
    append(f, entries, G_N_ELEMENTS(entries));
    g_assert_cmpint(ds_finalize(f->store, NULL, NULL), ==, DS_OK);
    DsPage *page = read_all(f);
    for (size_t i = 0; i < 2; ++i)
        for (DsMetric metric = DS_LOGICAL; metric < DS_METRIC_COUNT; ++metric)
            g_assert_cmpint(page->rows[i].metrics[metric].quality, ==, DS_OVERFLOW);
    g_assert_cmpuint(page->rows[2].metrics[DS_LOGICAL].bytes, ==, UINT64_MAX);
    ds_page_free(page);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/aggregate/hardlinks", Fixture, NULL, setup, hardlinks, teardown);
    g_test_add("/aggregate/reverse-hardlinks", Fixture, GINT_TO_POINTER(1), setup, hardlinks, teardown);
    g_test_add("/aggregate/unknown-identity", Fixture, NULL, setup, unknown_and_conflicting_identity, teardown);
    g_test_add("/aggregate/overflow", Fixture, NULL, setup, overflow, teardown);
    return g_test_run();
}

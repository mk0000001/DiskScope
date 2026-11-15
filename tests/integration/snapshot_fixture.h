#ifndef DISKSCOPE_TEST_SNAPSHOT_FIXTURE_H
#define DISKSCOPE_TEST_SNAPSHOT_FIXTURE_H

#include "diskscope/ds_snapshot.h"
#include <glib/gstdio.h>

static void native_argument(const char *path, DsNativePath *out)
{
    g_assert_cmpint(ds_path_from_arg(path, out, NULL), ==, DS_OK);
}

static DsStore *scan_fixture(const DsNativePath *database, const char *name, DsReason root_reason,
                             DsCoverage coverage)
{
    DsStore *store = NULL;
    DsScanOptions options = {.cache_budget = DS_CACHE_BUDGET};
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, "/root", 5, &options.root, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_open(database, DS_STORE_WRITER, &store, NULL), ==, DS_OK);
    g_assert_cmpint(ds_store_set_options(store, &options, NULL), ==, DS_OK);
    ds_path_clear(&options.root);

    DsEntry entries[] = {
        {.id = 1, .kind = DS_DIRECTORY, .reason = root_reason},
        {.id = 2, .parent_id = 1, .kind = DS_FILE, .logical = {UINT64_C(9007199254740993), DS_KNOWN},
         .allocated = {4096, DS_KNOWN}, .links = 1, .identity_known = true},
    };
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, "", 0, &entries[0].name, NULL), ==, DS_OK);
    if (name)
        g_assert_cmpint(ds_path_new(DS_PATH_BYTES, name, strlen(name), &entries[1].name, NULL), ==, DS_OK);
    DsEntryBatch batch = {entries, name ? 2 : 1};
    g_assert_cmpint(ds_store_append(store, &batch, NULL), ==, DS_OK);
    ds_path_clear(&entries[0].name);
    ds_path_clear(&entries[1].name);
    g_assert_cmpint(ds_store_set_result(store, coverage == DS_COVERAGE_COMPLETE ? DS_OK : DS_PARTIAL,
                                       coverage, NULL), ==, DS_OK);
    g_assert_cmpint(ds_finalize(store, NULL, NULL), ==, DS_OK);
    return store;
}

#endif

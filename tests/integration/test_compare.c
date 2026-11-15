#include "snapshot_fixture.h"
#include "diskscope/ds_compare.h"

static DsStatus count_unconfirmed(const DsDiffBatch *batch, void *data, DsError *error)
{
    (void)error;
    unsigned *count = data;
    g_assert_true(batch->same_root);
    g_assert_true(batch->same_policy);
    g_assert_cmpint(batch->before_coverage, ==, DS_COVERAGE_COMPLETE);
    g_assert_cmpint(batch->after_coverage, ==, DS_COVERAGE_PARTIAL);
    for (size_t i = 0; i < batch->count; ++i) {
        const DsDiff *diff = &batch->entries[i];
        g_assert_cmpint(diff->kind, ==, DS_DIFF_UNCONFIRMED);
        g_assert_cmpint(diff->before_id, ==, 2);
        g_assert_cmpint(diff->after_id, ==, 0);
        size_t length = 0;
        const void *path = g_bytes_get_data(diff->relative_path.raw, &length);
        g_assert_cmpmem(path, length, "lost", 4);
        ++*count;
    }
    return DS_OK;
}

static void partial_does_not_imply_deletion(void)
{
    char *directory = g_dir_make_tmp("ds-compare-XXXXXX", NULL);
    char *a = g_build_filename(directory, "before.db", NULL);
    char *b = g_build_filename(directory, "after.db", NULL);
    DsNativePath lhs = {0}, rhs = {0};
    native_argument(a, &lhs);
    native_argument(b, &rhs);
    DsStore *before = scan_fixture(&lhs, "lost", DS_REASON_NONE, DS_COVERAGE_COMPLETE);
    DsStore *after = scan_fixture(&rhs, NULL, DS_REASON_PERMISSION, DS_COVERAGE_PARTIAL);
    unsigned count = 0;
    g_assert_cmpint(ds_compare(before, after, NULL, count_unconfirmed, &count, NULL, NULL), ==, DS_OK);
    g_assert_cmpuint(count, ==, 1);
    ds_store_close(before);
    ds_store_close(after);
    ds_path_clear(&lhs);
    ds_path_clear(&rhs);
    g_remove(a);
    g_remove(b);
    g_rmdir(directory);
    g_free(a);
    g_free(b);
    g_free(directory);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/compare/partial-unconfirmed", partial_does_not_imply_deletion);
    return g_test_run();
}

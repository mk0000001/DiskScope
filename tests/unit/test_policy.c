#include "diskscope/ds_scan.h"

static void profiles(void)
{
    DsScanOptions options;
    ds_scan_options_init(&options, DS_CONSERVATIVE);
    g_assert_cmpuint(options.jobs, ==, 1);
    g_assert_true(options.cache_budget == DS_CACHE_BUDGET);
    g_assert_true(options.include_hidden);
    ds_scan_options_init(&options, DS_STANDARD);
    g_assert_cmpuint(options.jobs, ==, 2);
}

static void control(void)
{
    DsControl state;
    ds_control_init(&state);
    g_assert_true(ds_control_checkpoint(&state));
    ds_control_pause(&state, true);
    ds_control_cancel(&state);
    g_assert_false(ds_control_checkpoint(&state));
    ds_control_pause(&state, false);
    g_assert_true(ds_control_canceled(&state));
    ds_control_clear(&state);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/profiles", profiles);
    g_test_add_func("/cancel_is_terminal", control);
    return g_test_run();
}

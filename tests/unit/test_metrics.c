#include "diskscope/ds_types.h"

static void overflow(void)
{
    uint64_t out = 9;
    g_assert_false(ds_u64_add(UINT64_MAX, 1, &out));
    g_assert_true(out == 9);
    g_assert_false(ds_u64_mul(UINT64_MAX / 512 + 1, 512, &out));
    g_assert_true(out == 9);
    g_assert_true(ds_u64_add(UINT64_MAX - 1, 1, &out));
    g_assert_true(out == UINT64_MAX);
    g_assert_false(ds_u64_add(0, 0, NULL));
}

static void order(void)
{
    const uint64_t values[] = {0, 1, 9007199254740993ULL, 1ULL << 63, UINT64_MAX};
    uint8_t previous[8];
    for (size_t i = 0; i < G_N_ELEMENTS(values); ++i) {
        uint8_t encoded[8];
        uint64_t decoded = 1;
        ds_u64_encode(values[i], encoded);
        g_assert_true(ds_u64_decode(encoded, sizeof encoded, &decoded));
        g_assert_true(decoded == values[i]);
        if (i) g_assert_cmpint(memcmp(previous, encoded, 8), <, 0);
        memcpy(previous, encoded, 8);
    }
    g_assert_false(ds_u64_decode(previous, 7, NULL));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/u64_add_overflow", overflow);
    g_test_add_func("/u64_blob_order", order);
    return g_test_run();
}

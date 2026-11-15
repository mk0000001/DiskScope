#include "diskscope/ds_path.h"

static void bytes(void)
{
    const uint8_t first[] = {'x', 0xff}, second[] = {'x', 0xfe};
    DsNativePath a = {0}, b = {0};
    char *display = NULL;
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, first, sizeof first, &a, NULL), ==, DS_OK);
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, second, sizeof second, &b, NULL), ==, DS_OK);
    g_assert_false(g_bytes_equal(a.raw, b.raw));
    g_assert_cmpint(ds_path_display(&a, &display, NULL), ==, DS_OK);
    g_assert_true(g_utf8_validate(display, -1, NULL));
    g_assert_cmpstr(display, ==, "x\\xFF");
    g_free(display);
    ds_path_clear(&a);
    ds_path_clear(&b);
}

static void utf16(void)
{
    const uint8_t original[] = {0x41, 0, 0x3d, 0xd8, 0x80, 0xde, 0, 0xd8};
    DsNativePath path = {0};
    g_assert_cmpint(ds_path_new(DS_PATH_UTF16LE, original, sizeof original, &path, NULL), ==, DS_OK);
    g_assert_cmpmem(g_bytes_get_data(path.raw, NULL), sizeof original, original, sizeof original);
    char *display = NULL;
    g_assert_cmpint(ds_path_display(&path, &display, NULL), ==, DS_OK);
    g_assert_cmpstr(display, ==, "A🚀\\uD800");
    g_free(display);
    ds_path_clear(&path);
    g_assert_cmpint(ds_path_new(DS_PATH_UTF16LE, "\0\0", 2, &path, NULL), ==, DS_INVALID);
    g_assert_cmpint(ds_path_new(DS_PATH_UTF16LE, original, 1, &path, NULL), ==, DS_INVALID);
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, original, DS_PATH_MAX + 1, &path, NULL), ==, DS_LIMIT);
    g_assert_false(ds_path_component_valid(DS_PATH_BYTES, "../x", 4));
    g_assert_false(ds_path_component_valid(DS_PATH_UTF16LE, ".\0.\0", 4));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/native_name_collision", bytes);
    g_test_add_func("/windows_utf16_roundtrip", utf16);
    return g_test_run();
}

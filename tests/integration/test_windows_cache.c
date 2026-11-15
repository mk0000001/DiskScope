#include "../platform/windows_fixture.h"
#include "snapshot_fixture.h"
#include "local.h"

static void cache_import_long_paths(void)
{
    WCHAR *root = fixture_directory();
    if (!root) return;

    g_autofree WCHAR *source = fixture_path(root, L"source.dscan");
    g_autofree WCHAR *active = fixture_path(root, L"active.sqlite");
    g_autofree char *source_arg = g_utf16_to_utf8((const gunichar2 *)source, -1, NULL, NULL, NULL);
    g_autofree char *active_arg = g_utf16_to_utf8((const gunichar2 *)active, -1, NULL, NULL, NULL);
    DsNativePath source_path = {0}, active_path = {0};
    native_argument(source_arg, &source_path);
    native_argument(active_arg, &active_path);
    DsStore *writer = scan_fixture(&active_path, "file", DS_REASON_NONE, DS_COVERAGE_COMPLETE);
    g_assert_cmpint(ds_snapshot_save(writer, &source_path, NULL, NULL), ==, DS_OK);
    ds_store_close(writer);

    /* Start from an ordinary drive path and exceed both legacy Win32 and
     * SQLite's default Windows VFS pathname limits. */
    g_autoptr(GPtrArray) ancestors = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ancestors, root);
    const WCHAR *deep = root;
    while (wcslen(deep) < 1130) {
        WCHAR *next = fixture_path(deep,
            L"metadata-cache-depth-with-a-normal-sized-directory-component-\uD55C\uAE00");
        g_ptr_array_add(ancestors, next);
        deep = next;
    }
    g_autofree char *base = g_utf16_to_utf8((const gunichar2 *)deep + 4, -1, NULL, NULL, NULL);
    DsNativePath requested = {0};
    native_argument(base, &requested);
    DsError error = {0};
    g_autofree char *cache = ds_private_cache(&requested, &error);
    if (!cache) g_test_message("Creating the long Windows cache failed: %s (%" G_GINT64_FORMAT ")",
                              error.message, error.os_code);
    g_assert_nonnull(cache);
    const char *ordinary_cache = g_str_has_prefix(cache, "\\\\?\\") ? cache + 4 : cache;
    g_autofree char *destination = g_build_filename(ordinary_cache, "import.dscan", NULL);
    DsNativePath imported_path = {0};
    native_argument(destination, &imported_path);

    DsStore *reader = NULL;
    g_assert_cmpint(ds_snapshot_open(&source_path, &reader, NULL, &error), ==, DS_OK);
    DsStatus status = ds_snapshot_save(reader, &imported_path, NULL, &error);
    if (status != DS_OK) g_test_message("Saving inside the long Windows cache failed: %s (%" G_GINT64_FORMAT ")",
                                       error.message, error.os_code);
    g_assert_cmpint(status, ==, DS_OK);
    ds_store_close(reader);
    g_assert_cmpint(ds_snapshot_open(&imported_path, &reader, NULL, &error), ==, DS_OK);
    DsStoreInfo info = {0};
    g_assert_cmpint(ds_store_info(reader, &info, &error), ==, DS_OK);
    g_assert_cmpint(info.coverage, ==, DS_COVERAGE_COMPLETE);
    g_assert_cmpuint(info.entry_count, ==, 2);
    ds_store_info_clear(&info);
    ds_store_close(reader);
    g_autofree char *extended_target = g_strconcat("\\\\?\\", destination, NULL);
    g_autofree WCHAR *target_wide = (WCHAR *)g_utf8_to_utf16(extended_target, -1, NULL, NULL, NULL);
    g_autofree WCHAR *cache_wide = (WCHAR *)g_utf8_to_utf16(cache, -1, NULL, NULL, NULL);
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    g_assert_true(GetFileAttributesExW(target_wide, GetFileExInfoStandard, &attributes));
    const uint64_t bytes = (uint64_t)attributes.nFileSizeHigh << 32 | attributes.nFileSizeLow;
    g_assert_cmpuint(ds_cache_usage(ordinary_cache), ==, bytes);
    g_assert_true(DeleteFileW(target_wide));
    g_assert_true(RemoveDirectoryW(cache_wide));
    g_assert_true(DeleteFileW(source));
    g_assert_true(DeleteFileW(active));
    for (size_t index = ancestors->len; index-- > 0;)
        g_assert_true(RemoveDirectoryW(g_ptr_array_index(ancestors, index)));
    ds_path_clear(&imported_path);
    ds_path_clear(&requested);
    ds_path_clear(&source_path);
    ds_path_clear(&active_path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/windows/cache/import-beyond-default-vfs-limit", cache_import_long_paths);
    return g_test_run();
}

#ifndef DISKSCOPE_WINDOWS_FIXTURE_H
#define DISKSCOPE_WINDOWS_FIXTURE_H

#include "../../src/platform/windows/win_scan.h"
#include <winioctl.h>
#include <wchar.h>

static inline WCHAR *fixture_path(const WCHAR *dir, const WCHAR *name)
{
    const size_t n = wcslen(dir) + 1 + wcslen(name) + 1;
    WCHAR *path = g_new(WCHAR, n);
    swprintf(path, n, L"%ls\\%ls", dir, name);
    return path;
}

static inline WCHAR *fixture_directory(void)
{
    const DWORD needed = GetFullPathNameW(L".", 0, NULL, NULL);
    g_assert_cmpuint(needed, >, 0);
    WCHAR *cwd = g_new(WCHAR, needed);
    g_assert_cmpuint(GetFullPathNameW(L".", needed, cwd, NULL), <, needed);
    WCHAR volume_root[] = {cwd[0], L':', L'\\', 0};
    WCHAR fs[32];
    if (cwd[1] != L':' || !GetVolumeInformationW(volume_root, NULL, 0, NULL, NULL,
                                                 NULL, fs, G_N_ELEMENTS(fs)) ||
        wcscmp(fs, L"NTFS") != 0) {
        g_test_skip("a local NTFS test working directory is required");
        g_free(cwd);
        return NULL;
    }

    WCHAR directory_name[64];
    swprintf(directory_name, G_N_ELEMENTS(directory_name), L"ds-fixture-%lu-%llu",
             (unsigned long)GetCurrentProcessId(), (unsigned long long)g_get_monotonic_time());
    WCHAR *normal = fixture_path(cwd, directory_name);
    g_free(cwd);
    const size_t n = wcslen(normal) + 5;
    WCHAR *extended = g_new(WCHAR, n);
    swprintf(extended, n, L"\\\\?\\%ls", normal);
    g_free(normal);
    g_assert_true(CreateDirectoryW(extended, NULL));
    return extended;
}

static inline void fixture_set_root(DsScanOptions *options, const WCHAR *path)
{
    ds_scan_options_init(options, DS_CONSERVATIVE);
    g_assert_cmpint(ds_path_new(DS_PATH_UTF16LE, path, wcslen(path) * sizeof(*path),
                               &options->root, NULL), ==, DS_OK);
}

static inline HANDLE fixture_open(const WCHAR *path, DWORD disposition)
{
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    g_assert_true(file != INVALID_HANDLE_VALUE);
    return file;
}

static inline void fixture_write(HANDLE file)
{
    const char data[4096] = { 'a' };
    DWORD written;
    g_assert_true(WriteFile(file, data, sizeof(data), &written, NULL));
    g_assert_cmpuint(written, ==, sizeof(data));
}

#endif

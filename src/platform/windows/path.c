#include "win_scan.h"

#include <string.h>
#include <wchar.h>

/* Includes the prefix and terminating NUL. Native limits may be lower. */
#define DS_WIN_PATH_UNITS 32767u

static bool drive_letter(WCHAR ch)
{
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

static WCHAR *copy_path(const DsNativePath *path)
{
    if (!path || path->encoding != DS_PATH_UTF16LE || !path->raw) return NULL;
    size_t bytes;
    const void *raw = g_bytes_get_data(path->raw, &bytes);
    if (!bytes || bytes % sizeof(WCHAR) || bytes > DS_PATH_MAX) return NULL;
    const size_t units = bytes / sizeof(WCHAR);
    WCHAR *copy = g_new(WCHAR, units + 2);
    memcpy(copy, raw, bytes);
    copy[units] = 0;
    if (wcslen(copy) != units) {
        g_free(copy);
        return NULL;
    }
    return copy;
}

static bool valid_drive_path(const WCHAR *path)
{
    if (!drive_letter(path[0]) || path[1] != L':' || path[2] != L'\\') return false;
    const WCHAR *component = path + 3;
    for (const WCHAR *end = component; ; ++end) {
        if (*end == L'\\' || *end == 0) {
            const size_t length = (size_t)(end - component);
            if ((*end && !length) ||
                (length == 1 && component[0] == L'.') ||
                (length == 2 && component[0] == L'.' && component[1] == L'.'))
                return false;
            if (!*end) return true;
            component = end + 1;
        } else if (wcschr(L"/:*?\"<>|", *end)) {
            return false;
        }
    }
}

WCHAR *ds_win_absolute_path(const DsNativePath *path, DWORD *code)
{
    *code = ERROR_INVALID_NAME;
    WCHAR *input = copy_path(path);
    if (!input) return NULL;

    const WCHAR prefix[] = L"\\\\?\\";
    if (wcsncmp(input, prefix, 4) != 0) {
        if ((input[0] == L'\\' || input[0] == L'/') &&
            (input[1] == L'\\' || input[1] == L'/')) {
            *code = ERROR_NOT_SUPPORTED;
            g_free(input);
            return NULL;
        }
        if (drive_letter(input[0]) && input[1] == L':' && !input[2]) {
            input[2] = L'\\';
            input[3] = 0;
        } else if (drive_letter(input[0]) && input[1] == L':' &&
                   input[2] != L'\\' && input[2] != L'/') {
            g_free(input);
            return NULL;
        }
        DWORD capacity = GetFullPathNameW(input, 0, NULL, NULL);
        if (!capacity || capacity > DS_WIN_PATH_UNITS - 4) {
            *code = capacity ? ERROR_FILENAME_EXCED_RANGE : GetLastError();
            g_free(input);
            return NULL;
        }
        WCHAR *absolute = g_new(WCHAR, (size_t)capacity + 4);
        memcpy(absolute, prefix, 4 * sizeof(WCHAR));
        const DWORD written = GetFullPathNameW(input, capacity, absolute + 4, NULL);
        g_free(input);
        if (!written || written >= capacity) {
            *code = written ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
            g_free(absolute);
            return NULL;
        }
        input = absolute;
    } else if (_wcsnicmp(input + 4, L"UNC\\", 4) == 0) {
        *code = ERROR_NOT_SUPPORTED;
        g_free(input);
        return NULL;
    }

    size_t length = wcslen(input);
    if (length >= DS_WIN_PATH_UNITS || !valid_drive_path(input + 4)) {
        if (length >= DS_WIN_PATH_UNITS) *code = ERROR_FILENAME_EXCED_RANGE;
        g_free(input);
        return NULL;
    }
    if (length > 7 && input[length - 1] == L'\\') input[length - 1] = 0;
    *code = ERROR_SUCCESS;
    return input;
}

WCHAR *ds_win_join(const WCHAR *directory, const WCHAR *leaf)
{
    const size_t dir_length = wcslen(directory);
    const size_t leaf_length = wcslen(leaf);
    const bool separator = dir_length && directory[dir_length - 1] != L'\\';
    if (dir_length + separator + leaf_length >= DS_WIN_PATH_UNITS) return NULL;
    WCHAR *path = g_new(WCHAR, dir_length + separator + leaf_length + 1);
    memcpy(path, directory, dir_length * sizeof(WCHAR));
    if (separator) path[dir_length] = L'\\';
    memcpy(path + dir_length + separator, leaf, (leaf_length + 1) * sizeof(WCHAR));
    return path;
}

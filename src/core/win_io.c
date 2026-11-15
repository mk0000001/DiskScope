#include "win_io.h"
#include "diskscope/ds_types.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

struct DsWinDir {
    HANDLE find;
    WIN32_FIND_DATAW current;
    bool first;
};

char *ds_win_extended_path(const char *path)
{
    if (!path || !*path || !g_utf8_validate(path, -1, NULL)) return NULL;
    g_autofree char *absolute = g_canonicalize_filename(path, NULL);
    for (char *next = absolute; *next; ++next)
        if (*next == '/') *next = '\\';

    if (g_str_has_prefix(absolute, "\\\\?\\")) return g_steal_pointer(&absolute);
    if (g_str_has_prefix(absolute, "\\\\.\\")) return NULL;
    if (g_str_has_prefix(absolute, "\\\\")) return g_strconcat("\\\\?\\UNC\\", absolute + 2, NULL);
    return g_strconcat("\\\\?\\", absolute, NULL);
}

DsWinDir *ds_win_dir_open(const char *folder)
{
    char *absolute = ds_win_extended_path(folder);
    if (!absolute) {
        SetLastError(ERROR_INVALID_NAME);
        return NULL;
    }
    char *pattern = g_build_filename(absolute, "*", NULL);
    g_free(absolute);
    gunichar2 *wide = g_utf8_to_utf16(pattern, -1, NULL, NULL, NULL);
    g_free(pattern);
    if (!wide) {
        SetLastError(ERROR_INVALID_NAME);
        return NULL;
    }

    /* The bundled MinGW _wopendir can silently enumerate the current working
     * directory when its fixed buffer cannot hold an extended path. */
    DsWinDir *directory = g_new0(DsWinDir, 1);
    directory->find = FindFirstFileExW((const WCHAR *)wide, FindExInfoBasic,
                                       &directory->current, FindExSearchNameMatch, NULL, 0);
    directory->first = directory->find != INVALID_HANDLE_VALUE;
    const DWORD error = directory->first ? ERROR_SUCCESS : GetLastError();
    g_free(wide);
    if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND) {
        g_free(directory);
        SetLastError(error);
        return NULL;
    }
    return directory;
}

int ds_win_dir_next(DsWinDir *directory, char **name, uint64_t *size)
{
    if (name) *name = NULL;
    while (directory->find != INVALID_HANDLE_VALUE) {
        if (directory->first) {
            directory->first = false;
        } else if (!FindNextFileW(directory->find, &directory->current)) {
            const DWORD error = GetLastError();
            FindClose(directory->find);
            directory->find = INVALID_HANDLE_VALUE;
            SetLastError(error);
            return error == ERROR_NO_MORE_FILES ? 0 : -1;
        }
        const WIN32_FIND_DATAW *entry = &directory->current;
        if (!wcscmp(entry->cFileName, L".") || !wcscmp(entry->cFileName, L"..")) continue;
        if (name) {
            *name = g_utf16_to_utf8((const gunichar2 *)entry->cFileName, -1, NULL, NULL, NULL);
            if (!*name) continue;
        }
        if (size)
            *size = entry->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? 0 :
                    (uint64_t)entry->nFileSizeHigh << 32 | entry->nFileSizeLow;
        return 1;
    }
    return 0;
}

void ds_win_dir_close(DsWinDir *directory)
{
    if (!directory) return;
    if (directory->find != INVALID_HANDLE_VALUE) FindClose(directory->find);
    g_free(directory);
}

uint64_t ds_win_cache_usage(const char *folder)
{
    DsWinDir *directory = ds_win_dir_open(folder);
    if (!directory) return UINT64_MAX;

    uint64_t total = 0, size;
    int status;
    while ((status = ds_win_dir_next(directory, NULL, &size)) > 0) {
        if (!ds_u64_add(total, size, &total)) {
            status = -1;
            break;
        }
    }
    ds_win_dir_close(directory);
    return status < 0 ? UINT64_MAX : total;
}

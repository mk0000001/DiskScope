#include "output.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#ifdef G_OS_WIN32
#include "../core/win_io.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static bool same_file(const char *a, const char *b)
{
#ifdef G_OS_WIN32
    if (g_ascii_strcasecmp(a, b) == 0)
        return true;
    wchar_t *wide_a = (wchar_t *)g_utf8_to_utf16(a, -1, NULL, NULL, NULL);
    wchar_t *wide_b = (wchar_t *)g_utf8_to_utf16(b, -1, NULL, NULL, NULL);
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE ha = wide_a ? CreateFileW(wide_a, 0, share, NULL, OPEN_EXISTING, 0, NULL) :
                         INVALID_HANDLE_VALUE;
    HANDLE hb = wide_b ? CreateFileW(wide_b, 0, share, NULL, OPEN_EXISTING, 0, NULL) :
                         INVALID_HANDLE_VALUE;
    FILE_ID_INFO sa, sb;
    bool equal = ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandleEx(ha, FileIdInfo, &sa, sizeof sa) &&
        GetFileInformationByHandleEx(hb, FileIdInfo, &sb, sizeof sb) &&
        sa.VolumeSerialNumber == sb.VolumeSerialNumber &&
        memcmp(sa.FileId.Identifier, sb.FileId.Identifier, sizeof sa.FileId.Identifier) == 0;
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    g_free(wide_a);
    g_free(wide_b);
    return equal;
#else
    GStatBuf sa, sb;
    return strcmp(a, b) == 0 ||
           (g_stat(a, &sa) == 0 && g_stat(b, &sb) == 0 &&
            sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev);
#endif
}

static bool conflicts(DsStore *store, const char *destination)
{
    if (!store)
        return false;
    const char *filename = sqlite3_db_filename(ds_store_db(store), "main");
    if (!filename || !*filename)
        return false;
    char *absolute = g_canonicalize_filename(filename, NULL);
    const char *suffixes[] = {"", "-wal", "-shm", "-journal"};
    bool conflict = false;
    for (size_t i = 0; !conflict && i < G_N_ELEMENTS(suffixes); ++i) {
        char *name = g_strconcat(absolute, suffixes[i], NULL);
        conflict = same_file(destination, name);
        g_free(name);
    }
    g_free(absolute);
    return conflict;
}

DsStatus ds_output_begin(DsStore *source, const DsNativePath *path, DsOutput *out, DsError *error)
{
    *out = (DsOutput){.fd = -1};
    char *arg = ds_path_arg(path, error);
    if (!arg)
        return DS_INVALID;
#ifdef G_OS_WIN32
    char *destination = ds_win_extended_path(arg);
#else
    char *destination = g_canonicalize_filename(arg, NULL);
#endif
    g_free(arg);
    if (!destination) return ds_error(error, DS_INVALID, 0, "Invalid output path");
    if (conflicts(source, destination)) {
        g_free(destination);
        return ds_error(error, DS_INVALID, 0, "Output would replace a source database or its journal");
    }
    char *directory = g_path_get_dirname(destination);
    char *temporary = g_build_filename(directory, ".diskscope-output-XXXXXX", NULL);
    g_free(directory);
    int fd = g_mkstemp(temporary);
    if (fd < 0) {
        int code = errno;
        g_free(temporary);
        g_free(destination);
        return ds_error(error, code == ENOSPC ? DS_LIMIT : DS_IO, code,
                        "Could not create a temporary output file");
    }
    *out = (DsOutput){temporary, destination, fd};
    return DS_OK;
}

DsStatus ds_output_commit(DsOutput *out, DsError *error)
{
    int64_t code;
    bool ok;
#ifdef G_OS_WIN32
    wchar_t *from = (wchar_t *)g_utf8_to_utf16(out->temporary, -1, NULL, NULL, NULL);
    wchar_t *to = (wchar_t *)g_utf8_to_utf16(out->destination, -1, NULL, NULL, NULL);
    ok = from && to && MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    code = GetLastError();
    g_free(from);
    g_free(to);
#else
    ok = g_rename(out->temporary, out->destination) == 0;
    code = errno;
#endif
    if (!ok)
        return ds_error(error, DS_IO, code, "Could not replace the destination with the completed output");
    g_clear_pointer(&out->temporary, g_free);
    return DS_OK;
}

void ds_output_clear(DsOutput *out)
{
    if (out->fd >= 0)
        g_close(out->fd, NULL);
    if (out->temporary)
        g_unlink(out->temporary);
    g_free(out->temporary);
    g_free(out->destination);
    *out = (DsOutput){.fd = -1};
}

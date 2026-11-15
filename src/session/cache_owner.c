#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "cache_owner.h"

#ifdef G_OS_WIN32
#include "../core/win_io.h"

bool ds_cache_file_identity(DsCacheFile file, DsCacheIdentity *identity)
{
    BY_HANDLE_FILE_INFORMATION info;
    FILE_ID_INFO id;
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                  FILE_ATTRIBUTE_DEVICE)) || info.nNumberOfLinks != 1 ||
        !GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof id))
        return false;
    identity->volume = id.VolumeSerialNumber;
    memcpy(&identity->file_low, id.FileId.Identifier, sizeof identity->file_low);
    memcpy(&identity->file_high, id.FileId.Identifier + sizeof identity->file_low, sizeof identity->file_high);
    identity->birth = (uint64_t)info.ftCreationTime.dwHighDateTime << 32 | info.ftCreationTime.dwLowDateTime;
    return true;
}

bool ds_cache_mark_owned(const char *folder, bool scan)
{
    g_autofree char *absolute = ds_win_extended_path(folder);
    if (!absolute)
        return false;
    g_autofree char *db = g_build_filename(absolute, scan ? DS_CACHE_SCAN_DB : DS_CACHE_IMPORT_DB, NULL);
    g_autofree WCHAR *wide_db = (WCHAR *)g_utf8_to_utf16(db, -1, NULL, NULL, NULL);
    HANDLE primary = wide_db ? CreateFileW(wide_db, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, NULL) : DS_CACHE_BAD_FILE;
    if (primary == DS_CACHE_BAD_FILE)
        return false;
    DsCacheIdentity identity;
    bool valid = ds_cache_file_identity(primary, &identity);
    if (valid) {
        g_autofree char *name = g_build_filename(absolute, DS_CACHE_OWNER_FILE, NULL);
        g_autofree WCHAR *wide = (WCHAR *)g_utf8_to_utf16(name, -1, NULL, NULL, NULL);
        HANDLE file = wide ? CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                          FILE_FLAG_OPEN_REPARSE_POINT, NULL) : DS_CACHE_BAD_FILE;
        valid = file != DS_CACHE_BAD_FILE;
        if (valid) {
            char text[DS_CACHE_OWNER_SIZE];
            DWORD length = (DWORD)ds_cache_owner_record(text, &identity, scan);
            DWORD written = 0;
            valid = WriteFile(file, text, length, &written, NULL) && written == length;
            valid = CloseHandle(file) && valid;
        }
    }
    CloseHandle(primary);
    return valid;
}
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

bool ds_cache_file_identity(DsCacheFile file, DsCacheIdentity *identity)
{
    struct stat info;
    if (fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1)
        return false;
    *identity = (DsCacheIdentity){.volume = info.st_dev, .file_low = info.st_ino};
#ifdef STATX_BTIME
    struct statx details;
    if (statx(file, "", AT_EMPTY_PATH, STATX_BTIME, &details) == 0 && (details.stx_mask & STATX_BTIME))
        identity->birth = (uint64_t)details.stx_btime.tv_sec * UINT64_C(1000000000) + details.stx_btime.tv_nsec;
#endif
    return true;
}

bool ds_cache_mark_owned(const char *folder, bool scan)
{
    if (!folder || !g_path_is_absolute(folder))
        return false;
    int directory = open(folder, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return false;
    int primary = openat(directory, scan ? DS_CACHE_SCAN_DB : DS_CACHE_IMPORT_DB, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    DsCacheIdentity identity;
    bool valid = primary >= 0 && ds_cache_file_identity(primary, &identity);
    if (valid) {
        int file = openat(directory, DS_CACHE_OWNER_FILE,
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        valid = file >= 0;
        if (valid) {
            char text[DS_CACHE_OWNER_SIZE];
            size_t length = ds_cache_owner_record(text, &identity, scan);
            valid = write(file, text, length) == (ssize_t)length;
            valid = close(file) == 0 && valid;
        }
    }
    if (primary >= 0) close(primary);
    close(directory);
    return valid;
}
#endif

size_t ds_cache_owner_record(char text[DS_CACHE_OWNER_SIZE], const DsCacheIdentity *identity, bool scan)
{
    return (size_t)g_snprintf(text, DS_CACHE_OWNER_SIZE,
        "DiskScope cache v1 %s %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT "\n",
        scan ? "scan" : "import", identity->volume, identity->file_low, identity->file_high, identity->birth);
}

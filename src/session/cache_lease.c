#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "cache_lease.h"

#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include "../core/win_io.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct DsCacheLease {
#ifdef G_OS_WIN32
    HANDLE file;
#else
    int file;
#endif
    int64_t created;
};

#ifdef G_OS_WIN32
static bool private_directory(const WCHAR *path)
{
    HANDLE directory = CreateFileW(path, READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (directory == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION attributes;
    HANDLE token = NULL;
    TOKEN_USER *user = NULL;
    PSECURITY_DESCRIPTOR security = NULL;
    PSID owner = NULL;
    PACL acl = NULL;
    bool valid = false;
    DWORD needed = 0;
    if (!GetFileInformationByHandle(directory, &attributes) ||
        !(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        goto done;
    GetTokenInformation(token, TokenUser, NULL, 0, &needed);
    if (!needed)
        goto done;
    user = g_malloc(needed);
    if (!GetTokenInformation(token, TokenUser, user, needed, &needed) ||
        GetSecurityInfo(directory, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, NULL, &acl, NULL, &security) != ERROR_SUCCESS ||
        !owner || !EqualSid(owner, user->User.Sid) || !acl || acl->AceCount != 1)
        goto done;
    DWORD revision;
    SECURITY_DESCRIPTOR_CONTROL flags;
    void *ace = NULL;
    if (GetSecurityDescriptorControl(security, &flags, &revision) &&
        (flags & SE_DACL_PROTECTED) && GetAce(acl, 0, &ace) &&
        ((ACE_HEADER *)ace)->AceType == ACCESS_ALLOWED_ACE_TYPE) {
        const ACCESS_ALLOWED_ACE *access = ace;
        valid = EqualSid((PSID)&access->SidStart, user->User.Sid) &&
                (access->Header.AceFlags & (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE)) ==
                (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE);
    }
done:
    if (security) LocalFree(security);
    if (token) CloseHandle(token);
    g_free(user);
    CloseHandle(directory);
    return valid;
}

static DsCacheLease *open_lease(const char *folder, const char *name,
                                bool create, bool exclusive, DsError *error)
{
    char *directory = ds_win_extended_path(folder);
    char *path = ds_win_extended_path(name);
    WCHAR *wide_dir = directory ? (WCHAR *)g_utf8_to_utf16(directory, -1, NULL, NULL, NULL) : NULL;
    WCHAR *wide_file = path ? (WCHAR *)g_utf8_to_utf16(path, -1, NULL, NULL, NULL) : NULL;
    g_free(directory);
    g_free(path);
    bool directory_ok = wide_dir && private_directory(wide_dir);
    g_free(wide_dir);
    if (!wide_file || !directory_ok) {
        g_free(wide_file);
        ds_error(error, DS_INVALID, 0, "Cache lease requires a regular private directory");
        return NULL;
    }
    HANDLE file = CreateFileW(wide_file, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, create ? CREATE_NEW : OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    DWORD code = GetLastError();
    g_free(wide_file);
    if (file == INVALID_HANDLE_VALUE) {
        ds_error(error, DS_IO, code, "Could not open the cache lease");
        return NULL;
    }
    BY_HANDLE_FILE_INFORMATION info;
    bool ok = GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                   FILE_ATTRIBUTE_DEVICE)) && info.nNumberOfLinks == 1 &&
        info.nFileSizeHigh == 0 && info.nFileSizeLow == 0;
    OVERLAPPED region = {0};
    if (ok) {
        DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
        ok = LockFileEx(file, flags, 0, 1, 0, &region);
        code = GetLastError();
    } else {
        code = ERROR_INVALID_DATA;
    }
    if (!ok) {
        CloseHandle(file);
        ds_error(error, code == ERROR_LOCK_VIOLATION ? DS_LIMIT : DS_IO, code,
                 "The cache lease could not be acquired");
        return NULL;
    }
    DsCacheLease *lease = g_new(DsCacheLease, 1);
    lease->file = file;
    uint64_t timestamp = (uint64_t)info.ftCreationTime.dwHighDateTime << 32 |
                         info.ftCreationTime.dwLowDateTime;
    lease->created = (int64_t)(timestamp / 10);
    return lease;
}
#else
static DsCacheLease *open_lease(const char *folder, const char *name,
                                bool create, bool exclusive, DsError *error)
{
    (void)name;
    int dir = open(folder, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) {
        ds_error(error, DS_IO, errno, "Could not open the cache directory");
        return NULL;
    }
    struct stat st;
    if (fstat(dir, &st) < 0 || st.st_uid != geteuid() || (st.st_mode & 0077)) {
        close(dir);
        ds_error(error, DS_INVALID, 0, "Cache lease requires a private user directory");
        return NULL;
    }
    int flags = O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
    if (create)
        flags |= O_CREAT | O_EXCL;
    int fd = openat(dir, DS_CACHE_LEASE_FILE, flags, 0600);
    int code = errno;
    close(dir);
    if (fd < 0) {
        ds_error(error, DS_IO, code, "Could not open the cache lease");
        return NULL;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        st.st_nlink != 1 || st.st_size != 0 || (st.st_mode & 0077)) {
        close(fd);
        ds_error(error, DS_INVALID, 0, "Invalid cache lease file");
        return NULL;
    }
    if (flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) < 0) {
        code = errno;
        close(fd);
        ds_error(error, code == EWOULDBLOCK ? DS_LIMIT : DS_IO, code,
                 "The cache lease could not be acquired");
        return NULL;
    }
    DsCacheLease *lease = g_new(DsCacheLease, 1);
    lease->file = fd;
    lease->created = (int64_t)st.st_mtim.tv_sec * G_USEC_PER_SEC + st.st_mtim.tv_nsec / 1000;
    return lease;
}
#endif

DsCacheLease *ds_cache_lease_open(const char *folder, bool create, bool exclusive,
                                 DsError *error)
{
    if (!folder || !g_path_is_absolute(folder)) {
        ds_error(error, DS_INVALID, 0, "Invalid cache directory");
        return NULL;
    }
    char *name = g_build_filename(folder, DS_CACHE_LEASE_FILE, NULL);
    DsCacheLease *lease = open_lease(folder, name, create, exclusive, error);
    g_free(name);
    return lease;
}

void ds_cache_lease_close(DsCacheLease *lease)
{
    if (!lease)
        return;
#ifdef G_OS_WIN32
    OVERLAPPED region = {0};
    UnlockFileEx(lease->file, 0, 1, 0, &region);
    CloseHandle(lease->file);
#else
    close(lease->file);
#endif
    g_free(lease);
}

int64_t ds_cache_lease_created(const DsCacheLease *lease)
{
    return lease->created;
}

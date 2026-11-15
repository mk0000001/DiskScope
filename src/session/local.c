#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "local.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#ifdef G_OS_WIN32
#include "../core/win_io.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#ifdef G_OS_WIN32
static bool private_mkdir(const char *name)
{
    wchar_t *path = (wchar_t *)g_utf8_to_utf16(name, -1, NULL, NULL, NULL);
    HANDLE token = NULL;
    DWORD size = 0;
    TOKEN_USER *user = NULL;
    PACL acl = NULL;
    bool ok = false;
    if (!path || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        goto done;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    if (!size)
        goto done;
    user = g_malloc(size);
    if (!GetTokenInformation(token, TokenUser, user, size, &size))
        goto done;
    EXPLICIT_ACCESSW access = {
        .grfAccessPermissions = FILE_ALL_ACCESS,
        .grfAccessMode = SET_ACCESS,
        .grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT,
        .Trustee = {.TrusteeForm = TRUSTEE_IS_SID, .TrusteeType = TRUSTEE_IS_USER,
                    .ptstrName = user->User.Sid}
    };
    SECURITY_DESCRIPTOR descriptor;
    SECURITY_ATTRIBUTES attributes = {sizeof attributes, &descriptor, FALSE};
    ok = SetEntriesInAclW(1, &access, NULL, &acl) == ERROR_SUCCESS &&
         InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
         SetSecurityDescriptorOwner(&descriptor, user->User.Sid, FALSE) &&
         SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) &&
         SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED) &&
         CreateDirectoryW(path, &attributes);
done:
    g_free(user);
    g_free(path);
    if (acl) LocalFree(acl);
    if (token) CloseHandle(token);
    return ok;
}
#else
static bool private_mkdir(const char *name)
{
    return g_mkdir(name, 0700) == 0;
}
#endif

bool ds_cache_is_local(const char *path)
{
#ifdef G_OS_WIN32
    wchar_t *wide = (wchar_t *)g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    wchar_t volume[1024];
    bool ok = wide && GetVolumePathNameW(wide, volume, G_N_ELEMENTS(volume));
    UINT type = ok ? GetDriveTypeW(volume) : DRIVE_UNKNOWN;
    g_free(wide);
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK;
#else
    struct statfs info;
    if (statfs(path, &info) == -1)
        return false;
    switch ((uint32_t)info.f_type) {
    case 0x6969:     /* NFS */
    case 0xff534d42: /* CIFS */
    case 0xfe534d42: /* SMB2 */
    case 0x5346414f: /* AFS */
    case 0x73757245: /* Coda */
    case 0x65735546: /* FUSE */
    case 0x01021997: /* 9P */
        return false;
    default:
        return true;
    }
#endif
}

bool ds_cache_has_reserve(const char *folder)
{
#ifdef G_OS_WIN32
    g_autofree char *path = ds_win_extended_path(folder);
    wchar_t *wide = path ? (wchar_t *)g_utf8_to_utf16(path, -1, NULL, NULL, NULL) : NULL;
    ULARGE_INTEGER available;
    bool enough = wide && GetDiskFreeSpaceExW(wide, &available, NULL, NULL) &&
                  available.QuadPart >= DS_CACHE_FREE_RESERVE;
    g_free(wide);
    return enough;
#else
    struct statvfs info;
    return statvfs(folder, &info) == 0 && info.f_frsize &&
           info.f_bavail >= (DS_CACHE_FREE_RESERVE + info.f_frsize - 1) / info.f_frsize;
#endif
}

char *ds_cache_base(const DsNativePath *requested, DsError *error)
{
    char *base = requested && requested->raw ? ds_path_arg(requested, error) :
                 g_build_filename(g_get_user_cache_dir(), "diskscope", NULL);
    if (!base)
        return NULL;
#ifdef G_OS_WIN32
    char *absolute = ds_win_extended_path(base);
    if (!absolute) ds_error(error, DS_INVALID, 0, "Invalid Windows cache path");
#else
    char *absolute = g_canonicalize_filename(base, NULL);
#endif
    g_free(base);
    return absolute;
}

char *ds_private_cache(const DsNativePath *requested, DsError *error)
{
    char *absolute = ds_cache_base(requested, error);
    if (!absolute) return NULL;
    if (g_mkdir_with_parents(absolute, 0700) || !ds_cache_is_local(absolute) ||
        !ds_cache_has_reserve(absolute)) {
        ds_error(error, DS_LIMIT, errno, "Choose a local cache with at least 256 MiB available");
        g_free(absolute);
        return NULL;
    }
    char *nonce = g_uuid_string_random();
    char *basename = g_strconcat("scan-", nonce, NULL);
    char *child = g_build_filename(absolute, basename, NULL);
    g_free(basename);
    g_free(absolute);
    g_free(nonce);
    if (!private_mkdir(child)) {
        ds_error(error, DS_IO, errno, "Could not create a private scan cache");
        g_free(child);
        return NULL;
    }
    return child;
}

uint64_t ds_cache_usage(const char *folder)
{
#ifdef G_OS_WIN32
    return ds_win_cache_usage(folder);
#else
    GDir *dir = g_dir_open(folder, 0, NULL);
    if (!dir)
        return UINT64_MAX;
    uint64_t size = 0;
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        GStatBuf st;
        char *path = g_build_filename(folder, name, NULL);
        int result = g_lstat(path, &st);
        g_free(path);
        if (result == 0 && st.st_size >= 0 && !ds_u64_add(size, (uint64_t)st.st_size, &size)) {
            size = UINT64_MAX;
            break;
        }
    }
    g_dir_close(dir);
    return size;
#endif
}

char *ds_worker_binary(DsError *error)
{
#ifdef G_OS_WIN32
    wchar_t image[32768];
    DWORD length = GetModuleFileNameW(NULL, image, G_N_ELEMENTS(image));
    char *executable = length && length < G_N_ELEMENTS(image) ?
                       g_utf16_to_utf8((gunichar2 *)image, length, NULL, NULL, NULL) : NULL;
    const char *worker = "diskscope-worker.exe";
#else
    char *executable = g_file_read_link("/proc/self/exe", NULL);
    const char *worker = "diskscope-worker";
#endif
    if (!executable) {
        ds_error(error, DS_IO, 0, "Cannot locate the DiskScope executable");
        return NULL;
    }
    char *directory = g_path_get_dirname(executable);
    char *path = g_build_filename(directory, worker, NULL);
    g_free(directory);
    g_free(executable);
    if (!g_path_is_absolute(path) || !g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        ds_error(error, DS_IO, 0, "Install diskscope-worker next to the controller");
        g_free(path);
        return NULL;
    }
    return path;
}

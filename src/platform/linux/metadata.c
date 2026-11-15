#define _GNU_SOURCE
#include "linux_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static DsSize allocated_bytes(uint64_t blocks)
{
    DsSize result = {.quality = DS_KNOWN};
    if (!ds_u64_mul(blocks, 512, &result.bytes)) result.quality = DS_OVERFLOW;
    return result;
}

static LinuxMetadata metadata_from_stat(const struct stat *value)
{
    return (LinuxMetadata){
        .device = value->st_dev, .inode = value->st_ino, .mode = value->st_mode,
        .links = value->st_nlink, .mtime = value->st_mtim.tv_sec, .mtime_known = true,
        .logical = value->st_size < 0 ? (DsSize){0, DS_UNAVAILABLE}
                                    : (DsSize){(uint64_t)value->st_size, DS_KNOWN},
        .allocated = value->st_blocks < 0 ? (DsSize){0, DS_UNAVAILABLE}
                                         : allocated_bytes((uint64_t)value->st_blocks)
    };
}

bool linux_metadata_from_statx(const struct statx *value, LinuxMetadata *metadata)
{
    const unsigned required = STATX_TYPE | STATX_INO | STATX_NLINK | STATX_MTIME;
    const unsigned sizes = STATX_SIZE | STATX_BLOCKS;
    if ((value->stx_mask & required) != required ||
        (S_ISREG(value->stx_mode) && (value->stx_mask & sizes) != sizes)) return false;

    *metadata = (LinuxMetadata){
        .device = makedev(value->stx_dev_major, value->stx_dev_minor),
        .inode = value->stx_ino, .mode = value->stx_mode, .links = value->stx_nlink,
        .logical = (value->stx_mask & STATX_SIZE) ? (DsSize){value->stx_size, DS_KNOWN}
                                                : (DsSize){0, DS_UNAVAILABLE},
        .allocated = (value->stx_mask & STATX_BLOCKS) ? allocated_bytes(value->stx_blocks)
                                                     : (DsSize){0, DS_UNAVAILABLE},
        .mtime = value->stx_mtime.tv_sec, .mtime_known = true
    };
#ifdef STATX_MNT_ID
    if (value->stx_mask & STATX_MNT_ID) metadata->mount_id = value->stx_mnt_id;
#endif
#ifdef STATX_ATTR_AUTOMOUNT
    metadata->placeholder = (value->stx_attributes_mask & value->stx_attributes &
                             STATX_ATTR_AUTOMOUNT) != 0;
#endif
    return true;
}

int linux_metadata_at(int directory, const char *name, int flags, LinuxMetadata *metadata)
{
    flags |= AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT;
#ifdef SYS_statx
    struct statx snapshot = {0};
    unsigned request = STATX_BASIC_STATS;
#ifdef STATX_MNT_ID
    request |= STATX_MNT_ID;
#endif
    if (syscall(SYS_statx, directory, name, flags | AT_STATX_DONT_SYNC, request, &snapshot) == 0) {
        if (linux_metadata_from_statx(&snapshot, metadata)) return 0;
    } else if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP && errno != EPERM) {
        return errno;
    }
#endif
    struct stat result;
    int status = *name ? fstatat(directory, name, &result, flags) : fstat(directory, &result);
    if (status == -1) return errno;
    *metadata = metadata_from_stat(&result);
#ifdef SYS_statx
#ifdef STATX_MNT_ID
    if ((snapshot.stx_mask & (STATX_INO | STATX_MNT_ID)) == (STATX_INO | STATX_MNT_ID) &&
        snapshot.stx_ino == metadata->inode &&
        makedev(snapshot.stx_dev_major, snapshot.stx_dev_minor) == metadata->device)
        metadata->mount_id = snapshot.stx_mnt_id;
#endif
#ifdef STATX_ATTR_AUTOMOUNT
    metadata->placeholder = (snapshot.stx_attributes_mask & snapshot.stx_attributes &
                             STATX_ATTR_AUTOMOUNT) != 0;
#endif
#endif
    return 0;
}

uint64_t linux_fd_mount_id(int fd)
{
    char path[64], line[256];
    g_snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
    FILE *file = fopen(path, "re");
    if (!file) return 0;
    uint64_t id = 0;
    while (fgets(line, sizeof line, file)) {
        if (g_str_has_prefix(line, "mnt_id:")) {
            id = g_ascii_strtoull(line + strlen("mnt_id:"), NULL, 10);
            break;
        }
    }
    fclose(file);
    return id;
}

bool linux_same_identity(const LinuxMetadata *first, const LinuxMetadata *second)
{
    return first->inode == second->inode && first->device == second->device;
}

int linux_open_directory(LinuxOpen *method, int directory, const char *name, bool stay_on_mount,
                         int access_mode)
{
    const int flags = O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | access_mode;
#ifdef SYS_openat2
    if (*method != LINUX_OPEN_AT) {
        struct open_how how = {0};
        how.flags = (uint64_t)flags;
        how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS;
        if (stay_on_mount) how.resolve |= RESOLVE_NO_XDEV;
        int fd = (int)syscall(SYS_openat2, directory, name, &how, sizeof how);
        if (fd != -1) {
            *method = LINUX_OPEN_AT2;
            return fd;
        }
        if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP && errno != EPERM)
            return -1;
    }
#else
    (void)stay_on_mount;
#endif
    *method = LINUX_OPEN_AT;
    return openat(directory, name, flags);
}

int linux_open_root(const char *root, LinuxOpen *method)
{
    int directory = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (directory == -1) return -1;
    g_auto(GStrv) components = g_strsplit(root + 1, "/", -1);
    for (guint index = 0; components[index] && *components[index]; ++index) {
        int child = linux_open_directory(method, directory, components[index], false, O_PATH);
        int saved = errno;
        close(directory);
        if (child == -1) {
            errno = saved;
            return -1;
        }
        directory = child;
    }
    return directory;
}

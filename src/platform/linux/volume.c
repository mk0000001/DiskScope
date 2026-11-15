#define _GNU_SOURCE
#include "linux_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

char *linux_canonical_path(const DsNativePath *path, DsError *error)
{
    if (!path || path->encoding != DS_PATH_BYTES) {
        ds_error(error, DS_INVALID, EINVAL, "Linux requires a native byte path");
        return NULL;
    }
    g_autofree char *argument = ds_path_arg(path, error);
    if (!argument) return NULL;
    if (argument[0] != '/') {
        ds_error(error, DS_INVALID, EINVAL, "Linux requires an absolute path");
        return NULL;
    }
    /* GLib preserves two leading slashes, which Linux treats as one. */
    const char *absolute = argument;
    while (absolute[1] == '/') ++absolute;
    return g_canonicalize_filename(absolute, "/");
}

DsStatus linux_root_open(const DsNativePath *path, LinuxRoot *root, DsError *error)
{
    *root = (LinuxRoot){.fd = -1};
    root->path = linux_canonical_path(path, error);
    if (!root->path) return error ? error->status : DS_INVALID;

    root->mounts = linux_mounts_read();
    root->mount = linux_mount_for(root->mounts, root->path, 0);
    if (linux_mount_denied(root->mount))
        return ds_error(error, DS_INVALID, ENOTSUP, "Unsupported root filesystem (%s)",
                        root->mount ? root->mount->filesystem : "mount information unavailable");

    root->fd = linux_open_root(root->path, &root->method);
    if (root->fd == -1) {
        int code = errno;
        return ds_error(error, DS_IO, code, "Cannot open root directory: %s", g_strerror(code));
    }
    int code = linux_metadata_at(root->fd, "", AT_EMPTY_PATH, &root->metadata);
    if (code != 0)
        return ds_error(error, DS_IO, code, "Cannot read root metadata: %s", g_strerror(code));
    if (!root->metadata.mount_id) root->metadata.mount_id = linux_fd_mount_id(root->fd);

    root->mount = linux_mount_for(root->mounts, root->path, root->metadata.mount_id);
    if (root->metadata.placeholder || linux_mount_denied(root->mount))
        return ds_error(error, DS_INVALID, ENOTSUP, "Unsupported root filesystem (%s)",
                        root->mount ? root->mount->filesystem : "unknown");
    if (root->metadata.mount_id && root->metadata.mount_id != root->mount->id)
        return ds_error(error, DS_IO, ESTALE, "The root mount changed while opening it");
    if (!root->metadata.mount_id && root->metadata.device != makedev(root->mount->major, root->mount->minor))
        return ds_error(error, DS_IO, ESTALE, "The root device changed while opening it");
    return DS_OK;
}

void linux_root_close(LinuxRoot *root)
{
    if (root->fd >= 0) close(root->fd);
    g_clear_pointer(&root->path, g_free);
    g_clear_pointer(&root->mounts, g_ptr_array_unref);
}

static DsSize capacity(uint64_t count, uint64_t fragment_size)
{
    DsSize result = {.quality = DS_UNAVAILABLE};
    if (fragment_size)
        result.quality = ds_u64_mul(count, fragment_size, &result.bytes) ? DS_KNOWN : DS_OVERFLOW;
    return result;
}

static int read_capacity(int fd, DsVolumeInfo *volume, DsSize *free_space)
{
    *volume = (DsVolumeInfo){.total.quality = DS_UNAVAILABLE, .used.quality = DS_UNAVAILABLE,
                             .available.quality = DS_UNAVAILABLE};
    *free_space = (DsSize){0, DS_UNAVAILABLE};
    struct statvfs stats;
    int code = fstatvfs(fd, &stats) != 0 ? errno : 0;
    volume->captured_at = g_get_real_time() / G_USEC_PER_SEC;
    if (code) {
        DsQuality quality = code == EOVERFLOW ? DS_OVERFLOW :
            (code == ENOTSUP || code == ENOSYS) ? DS_UNSUPPORTED : DS_UNAVAILABLE;
        volume->total.quality = volume->used.quality = volume->available.quality = quality;
        free_space->quality = quality;
        return code;
    }
    volume->total = capacity(stats.f_blocks, stats.f_frsize);
    if (stats.f_bfree <= stats.f_blocks) {
        *free_space = capacity(stats.f_bfree, stats.f_frsize);
        volume->used = capacity(stats.f_blocks - stats.f_bfree, stats.f_frsize);
    }
    if (stats.f_bavail <= MIN(stats.f_bfree, stats.f_blocks))
        volume->available = capacity(stats.f_bavail, stats.f_frsize);
    return 0;
}

DsVolumeInfo linux_volume_info(int fd)
{
    DsVolumeInfo volume;
    DsSize free_space;
    read_capacity(fd, &volume, &free_space);
    return volume;
}

DsStatus ds_linux_volume(const DsNativePath *path, DsLinuxVolume *volume, DsError *error)
{
    if (!volume) return ds_error(error, DS_INVALID, EINVAL, "Missing volume output");
    *volume = (DsLinuxVolume){.total.quality = DS_UNAVAILABLE, .free.quality = DS_UNAVAILABLE,
                             .available.quality = DS_UNAVAILABLE};
    LinuxRoot root;
    DsStatus status = linux_root_open(path, &root, error);
    if (status != DS_OK) {
        linux_root_close(&root);
        return status;
    }

    /* Probe the NO_XDEV variant, including when the selected root is '/'. */
    int probe = linux_open_directory(&root.method, root.fd, ".", true, O_PATH);
    if (probe >= 0) close(probe);
    g_strlcpy(volume->filesystem, root.mount->filesystem, sizeof volume->filesystem);
    volume->mount_id = root.mount->id;
    volume->identity_trusted = linux_mount_trusted_identity(root.mount);
    volume->boundary = root.method == LINUX_OPEN_AT2 && probe >= 0 ? DS_LINUX_BOUNDARY_OPENAT2
        : root.metadata.mount_id ? DS_LINUX_BOUNDARY_MOUNT_ID : DS_LINUX_BOUNDARY_MOUNTINFO;

    DsVolumeInfo reading;
    int code = read_capacity(root.fd, &reading, &volume->free);
    volume->total = reading.total;
    volume->available = reading.available;
    if (code)
        status = ds_error(error, DS_PARTIAL, code, "Cannot query volume capacity: %s", g_strerror(code));
    linux_root_close(&root);
    return status;
}

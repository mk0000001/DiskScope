#define _GNU_SOURCE
#include "linux_internal.h"

#include <errno.h>
#include <string.h>

static void mount_free(gpointer ptr)
{
    LinuxMount *mount = ptr;
    g_free(mount->root);
    g_free(mount->path);
    g_free(mount->filesystem);
    g_free(mount);
}

static bool decimal(const char *text, uint64_t *value)
{
    if (!g_ascii_isdigit(*text)) return false;
    char *end;
    errno = 0;
    *value = g_ascii_strtoull(text, &end, 10);
    return errno == 0 && *end == '\0';
}

static char *mount_unescape(const char *text)
{
    GString *decoded = g_string_sized_new(strlen(text));
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor != '\\') {
            g_string_append_c(decoded, *cursor);
            continue;
        }
        if (strncmp(cursor, "\\040", 4) == 0) g_string_append_c(decoded, ' ');
        else if (strncmp(cursor, "\\011", 4) == 0) g_string_append_c(decoded, '\t');
        else if (strncmp(cursor, "\\012", 4) == 0) g_string_append_c(decoded, '\n');
        else if (strncmp(cursor, "\\134", 4) == 0) g_string_append_c(decoded, '\\');
        else return g_string_free(decoded, TRUE);
        cursor += 3;
    }
    return g_string_free(decoded, FALSE);
}

GPtrArray *linux_mounts_parse(const char *data)
{
    GPtrArray *mounts = g_ptr_array_new_with_free_func(mount_free);
    if (!data) return mounts;
    g_auto(GStrv) lines = g_strsplit(data, "\n", -1);
    for (guint index = 0; lines[index]; ++index) {
        g_auto(GStrv) fields = g_strsplit(lines[index], " ", -1);
        guint count = g_strv_length(fields);
        if (count < 10 || fields[3][0] != '/' || fields[4][0] != '/') continue;
        guint separator = 6;
        while (separator < count && !g_str_equal(fields[separator], "-")) ++separator;
        if (separator + 4 != count || fields[separator + 1][0] == '\0') continue;
        g_auto(GStrv) device = g_strsplit(fields[2], ":", 2);
        uint64_t id, parent, major, minor;
        if (!decimal(fields[0], &id) || id == 0 || !decimal(fields[1], &parent) ||
            !device[0] || !device[1] || !decimal(device[0], &major) ||
            !decimal(device[1], &minor) || major > G_MAXUINT || minor > G_MAXUINT) continue;

        LinuxMount *mount = g_new0(LinuxMount, 1);
        mount->id = id;
        mount->parent = parent;
        mount->major = (unsigned)major;
        mount->minor = (unsigned)minor;
        mount->root = mount_unescape(fields[3]);
        mount->path = mount_unescape(fields[4]);
        mount->filesystem = g_strdup(fields[separator + 1]);
        if (mount->root && mount->path) g_ptr_array_add(mounts, mount);
        else mount_free(mount);
    }
    return mounts;
}

GPtrArray *linux_mounts_read(void)
{
    g_autofree char *data = NULL;
    if (!g_file_get_contents("/proc/self/mountinfo", &data, NULL, NULL)) return NULL;
    return linux_mounts_parse(data);
}

static bool path_contains(const char *directory, const char *path, size_t length)
{
    return strncmp(directory, path, length) == 0 &&
        (length == 1 || path[length] == '/' || path[length] == '\0');
}

const LinuxMount *linux_mount_for(const GPtrArray *mounts, const char *path, uint64_t id)
{
    if (!mounts) return NULL;
    size_t best_length = 0;
    const LinuxMount *best = NULL;
    for (guint index = 0; index < mounts->len; ++index) {
        const LinuxMount *mount = g_ptr_array_index(mounts, index);
        if (id && mount->id == id) return mount;
        size_t length = strlen(mount->path);
        if (path && length >= best_length && path_contains(mount->path, path, length)) {
            best = mount;
            best_length = length;
        }
    }
    return best;
}

bool linux_mount_at(const GPtrArray *mounts, const char *path, uint64_t root_mount_id)
{
    if (!mounts) return false;
    for (guint index = 0; index < mounts->len; ++index) {
        const LinuxMount *mount = g_ptr_array_index(mounts, index);
        if (mount->id != root_mount_id && strcmp(mount->path, path) == 0) return true;
    }
    return false;
}

static bool type_in(const char *type, const char *const types[])
{
    for (; *types; ++types) if (strcmp(type, *types) == 0) return true;
    return false;
}

bool linux_mount_trusted_identity(const LinuxMount *mount)
{
    static const char *const trusted[] = {
        "btrfs", "ext2", "ext3", "ext4", "f2fs", "jfs", "lxfs", "nilfs2", "ntfs3",
        "reiserfs", "tmpfs", "ubifs", "wslfs", "xfs", "zfs", NULL
    };
    return mount && type_in(mount->filesystem, trusted);
}

bool linux_mount_denied(const LinuxMount *mount)
{
    static const char *const supported[] = {
        "aufs", "drvfs", "erofs", "exfat", "iso9660", "msdos", "ntfs", "overlay",
        "ramfs", "squashfs", "udf", "vfat", NULL
    };
    if (!mount || (mount->path && strcmp(mount->path, "/dev") == 0)) return true;
    return !linux_mount_trusted_identity(mount) && !type_in(mount->filesystem, supported);
}

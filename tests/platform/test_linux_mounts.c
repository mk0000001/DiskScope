#define _GNU_SOURCE
#include "../../src/platform/linux/linux_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <unistd.h>

static void mountinfo_escapes(void)
{
    GPtrArray *mounts = linux_mounts_parse(
        "101 71 8:2 / / rw - ext4 /dev/test rw\n"
        "102 101 8:2 /inner\\040one /test\\040folder\\011tab\\012line\\134slash rw shared:2 - ext4 /dev/test rw\n"
        "bad malformed line\n");
    g_assert_nonnull(mounts);
    g_assert_cmpuint(mounts->len, ==, 2);
    const LinuxMount *mount = linux_mount_for(mounts, NULL, 102);
    g_assert_nonnull(mount);
    g_assert_cmpuint(mount->parent, ==, 101);
    g_assert_cmpuint(mount->major, ==, 8);
    g_assert_cmpuint(mount->minor, ==, 2);
    g_assert_cmpstr(mount->root, ==, "/inner one");
    g_assert_cmpstr(mount->path, ==, "/test folder\ttab\nline\\slash");
    g_assert_true(linux_mount_at(mounts, mount->path, 101));
    g_assert_false(linux_mount_at(mounts, mount->path, 102));
    g_ptr_array_unref(mounts);
}

static void mountinfo_classification(void)
{
    GPtrArray *mounts = linux_mounts_parse(
        "1 1 8:2 / / rw - ext4 /dev/test rw\n"
        "2 1 0:30 / /proc rw - proc proc rw\n"
        "3 1 0:31 / /dev rw - devtmpfs udev rw\n"
        "4 1 0:32 / /scratch rw - tmpfs tmpfs rw\n"
        "5 1 0:33 / /remote rw - fuse.sshfs sshfs rw\n"
        "6 1 0:34 / /overlay rw - overlay overlay rw\n");
    g_assert_true(linux_mount_denied(linux_mount_for(mounts, "/proc/self", 0)));
    g_assert_true(linux_mount_denied(linux_mount_for(mounts, "/dev", 0)));
    g_assert_false(linux_mount_denied(linux_mount_for(mounts, "/scratch", 0)));
    g_assert_true(linux_mount_trusted_identity(linux_mount_for(mounts, "/scratch", 0)));
    g_assert_false(linux_mount_denied(linux_mount_for(mounts, "/proc-present", 0)));
    g_assert_true(linux_mount_denied(linux_mount_for(mounts, "/remote/file", 0)));
    g_assert_false(linux_mount_trusted_identity(linux_mount_for(mounts, "/overlay", 0)));
    g_ptr_array_unref(mounts);
}

static void missing_statx_fields(void)
{
    struct statx snapshot = {0};
    snapshot.stx_mask = STATX_TYPE | STATX_SIZE | STATX_INO;
    snapshot.stx_mode = S_IFREG | 0600;
    snapshot.stx_size = 55;
    snapshot.stx_ino = 341;
    LinuxMetadata metadata;
    g_assert_false(linux_metadata_from_statx(&snapshot, &metadata));
    snapshot.stx_mask = STATX_BASIC_STATS;
    snapshot.stx_blocks = 4;
    g_assert_true(linux_metadata_from_statx(&snapshot, &metadata));
    g_assert_cmpuint(metadata.logical.bytes, ==, 55);
    g_assert_cmpuint(metadata.allocated.bytes, ==, 2048);
    g_assert_cmpuint(metadata.inode, ==, 341);
}

static void volume_capacity(void)
{
    char *directory = g_dir_make_tmp("diskscope-volume-XXXXXX", NULL);
    g_assert_nonnull(directory);
    struct statvfs stats;
    g_assert_cmpint(statvfs(directory, &stats), ==, 0);
    DsNativePath path = {.encoding = DS_PATH_BYTES, .raw = g_bytes_new(directory, strlen(directory))};
    DsLinuxVolume volume;
    g_assert_cmpint(ds_linux_volume(&path, &volume, NULL), ==, DS_OK);
    g_assert_cmpuint(volume.mount_id, >, 0);
    g_assert_cmpstr(volume.filesystem, !=, "");
    g_assert_cmpint(volume.total.quality, ==, DS_KNOWN);
    g_assert_cmpuint(volume.total.bytes, ==, (uint64_t)stats.f_blocks * stats.f_frsize);
    g_assert_cmpint(volume.free.quality, ==, DS_KNOWN);
    g_assert_cmpint(volume.available.quality, ==, DS_KNOWN);
    g_assert_cmpuint(volume.free.bytes, >=, volume.available.bytes);
    ds_path_clear(&path);
    g_assert_cmpint(rmdir(directory), ==, 0);
    g_free(directory);
}

typedef struct { bool found_boundary, found_content; } Boundary;

static DsStatus check_boundary(const DsEntryBatch *batch, void *context, DsError *error)
{
    (void)error;
    Boundary *boundary = context;
    for (size_t i = 0; i < batch->count; ++i) {
        const DsEntry *entry = &batch->entries[i];
        size_t length;
        const char *name = g_bytes_get_data(entry->name.raw, &length);
        if (length == 6 && memcmp(name, "nested", length) == 0)
            boundary->found_boundary = entry->kind == DS_EXCLUDED && entry->reason == DS_REASON_MOUNT;
        if (length == 7 && memcmp(name, "content", length) == 0)
            boundary->found_content = true;
    }
    return DS_OK;
}

static int bind_mount(void)
{
    char *fixture = g_dir_make_tmp("diskscope-bind-XXXXXX", NULL);
    g_assert_nonnull(fixture);
    char *source = g_build_filename(fixture, "source", NULL);
    char *root = g_build_filename(fixture, "root", NULL);
    char *nested = g_build_filename(root, "nested", NULL);
    char *content = g_build_filename(source, "content", NULL);
    g_assert_cmpint(mkdir(source, 0700), ==, 0);
    g_assert_cmpint(mkdir(root, 0700), ==, 0);
    g_assert_cmpint(mkdir(nested, 0700), ==, 0);
    g_assert_true(g_file_set_contents(content, "fixture", -1, NULL));

    int result = 0;
    if (mount(source, nested, NULL, MS_BIND, NULL) == -1) {
        fprintf(stderr, "bind mount unavailable: %s\n", g_strerror(errno));
        result = 77;
    } else {
        DsScanOptions options;
        ds_scan_options_init(&options, DS_CONSERVATIVE);
        g_assert_cmpint(ds_path_new(DS_PATH_BYTES, root, strlen(root), &options.root, NULL), ==, DS_OK);
        Boundary boundary = {0};
        DsError error = {0};
        DsControl control;
        ds_control_init(&control);
        DsStatus status = ds_walk(&options, &control, check_boundary, &boundary, &error);
        g_assert_cmpint(umount(nested), ==, 0);
        g_assert_cmpint(status, ==, DS_OK);
        g_assert_true(boundary.found_boundary);
        g_assert_false(boundary.found_content);
        ds_control_clear(&control);
        ds_path_clear(&options.root);
    }
    g_assert_cmpint(unlink(content), ==, 0);
    g_assert_cmpint(rmdir(nested), ==, 0);
    g_assert_cmpint(rmdir(root), ==, 0);
    g_assert_cmpint(rmdir(source), ==, 0);
    g_assert_cmpint(rmdir(fixture), ==, 0);
    g_free(content);
    g_free(nested);
    g_free(root);
    g_free(source);
    g_free(fixture);
    return result;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--bind") == 0) return bind_mount();
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/linux/mountinfo-escapes", mountinfo_escapes);
    g_test_add_func("/linux/mountinfo-classification", mountinfo_classification);
    g_test_add_func("/linux/statx-missing-fields", missing_statx_fields);
    g_test_add_func("/linux/volume-capacity", volume_capacity);
    return g_test_run();
}

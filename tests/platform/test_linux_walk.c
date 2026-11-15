#define _GNU_SOURCE
#include "diskscope/ds_scan.h"
#include "../../src/platform/linux/linux_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
    char *root;
    GArray *entries;
    DsScanOptions options;
    DsControl control;
    DsError error;
} Fixture;

static enum { REAL_READDIR, UNKNOWN_READDIR, ERROR_READDIR, VANISH_READDIR } enumeration;
static bool injected;
static bool replace_before_open, mount_changed, probe_parallelism;
static GMutex operation_lock;
static GCond operation_changed;
static unsigned active_operations, peak_operations;

int __real_linux_open_directory(LinuxOpen *, int, const char *, bool, int);
int __wrap_linux_open_directory(LinuxOpen *method, int parent, const char *name,
                                bool same_mount, int mode)
{
    if (mount_changed && same_mount && strcmp(name, ".") == 0) {
        mount_changed = false;
        errno = EXDEV;
        return -1;
    }
    if (replace_before_open && strcmp(name, "swap-me") == 0) {
        replace_before_open = false;
        g_assert_cmpint(renameat(parent, name, parent, "retired"), ==, 0);
        g_assert_cmpint(mkdirat(parent, name, 0700), ==, 0);
    }
    return __real_linux_open_directory(method, parent, name, same_mount, mode);
}

int __real_linux_metadata_at(int, const char *, int, LinuxMetadata *);
int __wrap_linux_metadata_at(int parent, const char *name, int flags, LinuxMetadata *metadata)
{
    if (!probe_parallelism || !g_str_has_prefix(name, "paired-"))
        return __real_linux_metadata_at(parent, name, flags, metadata);

    g_mutex_lock(&operation_lock);
    ++active_operations;
    peak_operations = MAX(active_operations, peak_operations);
    g_cond_broadcast(&operation_changed);
    gint64 deadline = g_get_monotonic_time() + 300 * 1000;
    while (peak_operations < 2 && g_cond_wait_until(&operation_changed, &operation_lock, deadline)) {}
    g_mutex_unlock(&operation_lock);
    int status = __real_linux_metadata_at(parent, name, flags, metadata);
    g_mutex_lock(&operation_lock);
    --active_operations;
    g_mutex_unlock(&operation_lock);
    return status;
}

struct dirent *__real_readdir(DIR *stream);
struct dirent *__wrap_readdir(DIR *stream)
{
    struct dirent *entry = __real_readdir(stream);
    if (entry && strcmp(entry->d_name, "marker") == 0) {
        if (enumeration == UNKNOWN_READDIR) {
            entry->d_type = DT_UNKNOWN;
            injected = true;
        }
        if (enumeration == VANISH_READDIR) {
            g_assert_cmpint(unlinkat(dirfd(stream), "marker", 0), ==, 0);
            injected = true;
        }
    }
    if (!entry && errno == 0 && enumeration == ERROR_READDIR && !injected) {
        injected = true;
        errno = EIO;
    }
    return entry;
}

static void free_entry(gpointer ptr)
{
    DsEntry *entry = ptr;
    ds_path_clear(&entry->name);
}

static void setup(Fixture *f, gconstpointer unused)
{
    (void)unused;
    f->root = g_dir_make_tmp("diskscope-linux-XXXXXX", NULL);
    g_assert_nonnull(f->root);
    f->entries = g_array_new(FALSE, FALSE, sizeof(DsEntry));
    g_array_set_clear_func(f->entries, free_entry);
    ds_scan_options_init(&f->options, DS_CONSERVATIVE);
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, f->root, strlen(f->root),
                               &f->options.root, &f->error), ==, DS_OK);
    ds_control_init(&f->control);
    enumeration = REAL_READDIR;
    injected = false;
    replace_before_open = false;
    mount_changed = false;
    probe_parallelism = false;
}

static void teardown(Fixture *f, gconstpointer unused)
{
    (void)unused;
    const char *argv[] = {"/bin/rm", "-rf", "--", f->root, NULL};
    int wait_status;
    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL, 0, NULL, NULL, NULL,
                              NULL, &wait_status, NULL));
    g_assert_true(g_spawn_check_wait_status(wait_status, NULL));
    ds_control_clear(&f->control);
    ds_path_clear(&f->options.root);
    ds_path_clear(&f->options.cache_path);
    g_array_unref(f->entries);
    g_free(f->root);
}

static DsStatus collect(const DsEntryBatch *batch, void *ptr, DsError *error)
{
    (void)error;
    Fixture *f = ptr;
    g_assert_cmpuint(batch->count, >, 0);
    g_assert_cmpuint(batch->count, <=, DS_BATCH_MAX);
    for (size_t index = 0; index < batch->count; ++index) {
        DsEntry entry = batch->entries[index];
        entry.name.raw = g_bytes_ref(entry.name.raw);
        g_assert_cmpint(entry.id, >, entry.parent_id);
        g_array_append_val(f->entries, entry);
    }
    return DS_OK;
}

static DsStatus scan(Fixture *f)
{
    DsStatus result = ds_walk(&f->options, &f->control, collect, f, &f->error);
    if (result != DS_OK) g_test_message("walk status %d, %s", result, f->error.message);
    return result;
}

static const DsEntry *find(Fixture *f, const char *name)
{
    GBytes *needle = g_bytes_new_static(name, strlen(name));
    const DsEntry *found = NULL;
    for (guint index = 0; index < f->entries->len; ++index) {
        const DsEntry *entry = &g_array_index(f->entries, DsEntry, index);
        if (g_bytes_equal(needle, entry->name.raw)) found = entry;
    }
    g_bytes_unref(needle);
    return found;
}

static int fixture_dir(Fixture *f)
{
    int fd = open(f->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    g_assert_cmpint(fd, >=, 0);
    return fd;
}

static void make_file(int dir, const char *name)
{
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 1024 * 1024 + 17), ==, 0);
    g_assert_cmpint(write(fd, "A", 1), ==, 1);
    g_assert_cmpint(close(fd), ==, 0);
}

static void metadata(Fixture *f, gconstpointer unused)
{
    (void)unused;
    const char name[] = "sparse";
    int dir = fixture_dir(f);
    make_file(dir, name);
    g_assert_cmpint(linkat(dir, name, dir, "hardlink", 0), ==, 0);
    struct stat st;
    g_assert_cmpint(fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW), ==, 0);
    g_assert_cmpint(close(dir), ==, 0);
    g_assert_cmpint(scan(f), ==, DS_OK);
    const DsEntry *file = find(f, name);
    const DsEntry *hardlink = find(f, "hardlink");
    g_assert_nonnull(file);
    g_assert_nonnull(hardlink);
    g_assert_cmpint(file->kind, ==, DS_FILE);
    g_assert_cmpint(file->logical.quality, ==, DS_KNOWN);
    g_assert_cmpuint(file->logical.bytes, ==, 1048593);
    g_assert_cmpint(file->allocated.quality, ==, DS_KNOWN);
    g_assert_cmpuint(file->allocated.bytes, ==, (uint64_t)st.st_blocks * 512);
    g_assert_true(file->identity_known);
    g_assert_true(hardlink->identity_known);
    g_assert_cmpmem(file->identity, sizeof file->identity, hardlink->identity,
                   sizeof hardlink->identity);
    g_assert_cmpuint(file->links, ==, 2);
    g_assert_true(file->mtime_known);
    g_assert_cmpint(file->mtime, ==, st.st_mtime);
}

static void preserves_raw_name(Fixture *f, gconstpointer unused)
{
    (void)unused;
    const char name[] = "raw-\xff\xfe";
    int dir = fixture_dir(f);
    make_file(dir, name);
    DIR *stream = fdopendir(dir);
    g_assert_nonnull(stream);
    bool fixture_supports_raw = false;
    struct dirent *record;
    while ((record = readdir(stream)))
        if (strcmp(record->d_name, name) == 0) fixture_supports_raw = true;
    g_assert_cmpint(closedir(stream), ==, 0);
    if (!fixture_supports_raw) {
        g_test_skip("The fixture filesystem rewrites invalid UTF-8 names in readdir");
        return;
    }
    g_assert_cmpint(scan(f), ==, DS_OK);
    const DsEntry *entry = find(f, name);
    g_assert_nonnull(entry);
    g_assert_cmpint(entry->name.encoding, ==, DS_PATH_BYTES);
    g_assert_cmpint(entry->kind, ==, DS_FILE);
}

static void skips_links_and_special(Fixture *f, gconstpointer unused)
{
    (void)unused;
    int dir = fixture_dir(f);
    g_assert_cmpint(symlinkat(".", dir, "loop"), ==, 0);
    g_assert_cmpint(symlinkat("/", dir, "outside"), ==, 0);
    g_assert_cmpint(mkfifoat(dir, "pipe", 0600), ==, 0);
    int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    g_assert_cmpint(socket_fd, >=, 0);
    struct sockaddr_un endpoint = {.sun_family = AF_UNIX};
    g_snprintf(endpoint.sun_path, sizeof endpoint.sun_path, "%s/socket", f->root);
    if (bind(socket_fd, (struct sockaddr *)&endpoint, sizeof endpoint) == -1) {
        g_test_skip(g_strerror(errno));
        close(socket_fd);
        close(dir);
        return;
    }
    g_assert_cmpint(scan(f), ==, DS_OK);
    g_assert_cmpuint(f->entries->len, ==, 5);
    g_assert_cmpint(find(f, "loop")->reason, ==, DS_REASON_LINK);
    g_assert_cmpint(find(f, "outside")->kind, ==, DS_LINK);
    g_assert_cmpint(find(f, "pipe")->kind, ==, DS_SPECIAL);
    g_assert_cmpint(find(f, "socket")->kind, ==, DS_SPECIAL);
    g_assert_cmpint(find(f, "socket")->allocated.quality, ==, DS_UNAVAILABLE);
    g_assert_cmpint(close(socket_fd), ==, 0);
    g_assert_cmpint(close(dir), ==, 0);
}

static void excludes_cache(Fixture *f, gconstpointer unused)
{
    (void)unused;
    int dir = fixture_dir(f);
    g_assert_cmpint(mkdirat(dir, "cache", 0700), ==, 0);
    make_file(dir, "cache/state-wal");
    char *cache = g_build_filename(f->root, ".", "cache", NULL);
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, cache, strlen(cache), &f->options.cache_path,
                               &f->error), ==, DS_OK);
    g_free(cache);
    g_assert_cmpint(scan(f), ==, DS_OK);
    const DsEntry *entry = find(f, "cache");
    g_assert_nonnull(entry);
    g_assert_cmpint(entry->kind, ==, DS_EXCLUDED);
    g_assert_cmpint(entry->reason, ==, DS_REASON_CACHE);
    g_assert_null(find(f, "state-wal"));
    g_assert_cmpint(close(dir), ==, 0);
}

static void excludes_root_children(Fixture *f, gconstpointer unused)
{
    (void)unused;
    int dir = fixture_dir(f);
    make_file(dir, "ignored");
    g_assert_cmpint(close(dir), ==, 0);
    f->options.exclude_paths = &f->options.root;
    f->options.exclude_count = 1;
    g_assert_cmpint(scan(f), ==, DS_OK);
    g_assert_cmpuint(f->entries->len, ==, 1);
    const DsEntry *root = find(f, "");
    g_assert_nonnull(root);
    g_assert_cmpint(root->kind, ==, DS_DIRECTORY);
    g_assert_cmpint(root->reason, ==, DS_REASON_POLICY);
}

static void excludes_cache_through_alias(Fixture *f, gconstpointer unused)
{
    (void)unused;
    int dir = fixture_dir(f);
    g_assert_cmpint(mkdirat(dir, "cache", 0700), ==, 0);
    make_file(dir, "cache/state-wal");
    g_assert_cmpint(symlinkat("cache", dir, "cache-alias"), ==, 0);
    g_autofree char *cache = g_build_filename(f->root, "cache-alias", NULL);
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, cache, strlen(cache), &f->options.cache_path,
                               &f->error), ==, DS_OK);
    g_assert_cmpint(scan(f), ==, DS_OK);
    g_assert_cmpint(find(f, "cache")->reason, ==, DS_REASON_CACHE);
    g_assert_null(find(f, "state-wal"));
    g_assert_cmpint(close(dir), ==, 0);
}

static void reports_root_mount_race(Fixture *f, gconstpointer unused)
{
    (void)unused;
    mount_changed = true;
    g_assert_cmpint(scan(f), ==, DS_PARTIAL);
    g_assert_false(mount_changed);
    const DsEntry *root = find(f, "");
    g_assert_nonnull(root);
    g_assert_cmpint(root->kind, ==, DS_DIRECTORY);
    g_assert_cmpint(root->reason, ==, DS_REASON_MOUNT);
}

static void unknown_dirent(Fixture *f, gconstpointer unused)
{
    (void)unused;
    int dir = fixture_dir(f);
    make_file(dir, "marker");
    g_assert_cmpint(close(dir), ==, 0);
    enumeration = UNKNOWN_READDIR;
    g_assert_cmpint(scan(f), ==, DS_OK);
    g_assert_true(injected);
    g_assert_cmpint(find(f, "marker")->kind, ==, DS_FILE);
}

static void distinguishes_error_from_eof(Fixture *f, gconstpointer unused)
{
    (void)unused;
    enumeration = ERROR_READDIR;
    g_assert_cmpint(scan(f), ==, DS_PARTIAL);
    g_assert_true(injected);
    const DsEntry *root = find(f, "");
    g_assert_nonnull(root);
    g_assert_cmpint(root->id, ==, 1);
    g_assert_cmpint(root->reason, ==, DS_REASON_IO);
    g_assert_cmpint(root->os_code, ==, EIO);
}

static void reports_vanished_entry(Fixture *f, gconstpointer unused)
{
    (void)unused;
    int dir = fixture_dir(f);
    make_file(dir, "marker");
    g_assert_cmpint(close(dir), ==, 0);
    enumeration = VANISH_READDIR;
    g_assert_cmpint(scan(f), ==, DS_PARTIAL);
    g_assert_true(injected);
    const DsEntry *entry = find(f, "marker");
    g_assert_nonnull(entry);
    g_assert_cmpint(entry->reason, ==, DS_REASON_CHANGED);
    g_assert_cmpint(entry->os_code, ==, ENOENT);
    g_assert_cmpint(entry->logical.quality, ==, DS_CHANGED);
}

static void deep_under_low_fd_limit(Fixture *f, gconstpointer unused)
{
    (void)unused;
    int dir = fixture_dir(f);
    for (int level = 0; level < 1000; ++level) {
        g_assert_cmpint(mkdirat(dir, "path0000", 0700), ==, 0);
        int child = openat(dir, "path0000", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        g_assert_cmpint(child, >=, 0);
        g_assert_cmpint(close(dir), ==, 0);
        dir = child;
    }
    make_file(dir, "bottom");
    g_assert_cmpint(close(dir), ==, 0);

    struct rlimit original;
    g_assert_cmpint(getrlimit(RLIMIT_NOFILE, &original), ==, 0);
    struct rlimit low = original;
    low.rlim_cur = MIN(original.rlim_cur, 12);
    g_assert_cmpint(setrlimit(RLIMIT_NOFILE, &low), ==, 0);
    DsStatus status = scan(f);
    g_assert_cmpint(setrlimit(RLIMIT_NOFILE, &original), ==, 0);
    g_assert_cmpint(status, ==, DS_OK);
    g_assert_cmpuint(f->entries->len, ==, 1002);
    const DsEntry *bottom = find(f, "bottom");
    g_assert_nonnull(bottom);
    g_assert_cmpint(bottom->parent_id, ==, 1001);
}

static void verifies_replaced_directory(Fixture *f, gconstpointer unused)
{
    (void)unused;
    int dir = fixture_dir(f);
    g_assert_cmpint(mkdirat(dir, "swap-me", 0700), ==, 0);
    make_file(dir, "swap-me/untouched");
    g_assert_cmpint(close(dir), ==, 0);
    replace_before_open = true;
    g_assert_cmpint(scan(f), ==, DS_PARTIAL);
    g_assert_false(replace_before_open);
    g_assert_cmpint(find(f, "swap-me")->reason, ==, DS_REASON_CHANGED);
    g_assert_null(find(f, "untouched"));
}

static void reports_permission_denied(Fixture *f, gconstpointer unused)
{
    (void)unused;
    int dir = fixture_dir(f);
    g_assert_cmpint(mkdirat(dir, "private", 0700), ==, 0);
    make_file(dir, "private/secret");
    g_assert_cmpint(fchmodat(dir, "private", 0000, 0), ==, 0);
    g_assert_cmpint(fchmod(dir, 0755), ==, 0);
    uid_t original = geteuid();
    if (!original && seteuid(65534) == -1) {
        g_test_skip("Unable to exercise permission-denied as a non-root user");
        g_assert_cmpint(close(dir), ==, 0);
        return;
    }
    DsStatus status = scan(f);
    g_assert_cmpint(seteuid(original), ==, 0);
    g_assert_cmpint(fchmodat(dir, "private", 0700, 0), ==, 0);
    g_assert_cmpint(close(dir), ==, 0);
    g_assert_cmpint(status, ==, DS_PARTIAL);
    g_assert_cmpint(find(f, "private")->reason, ==, DS_REASON_PERMISSION);
    g_assert_null(find(f, "secret"));
}

static void standard_overlaps_metadata(Fixture *f, gconstpointer unused)
{
    (void)unused;
    int dir = fixture_dir(f);
    make_file(dir, "paired-first");
    make_file(dir, "paired-second");
    g_assert_cmpint(close(dir), ==, 0);

    f->options.profile = DS_STANDARD;
    f->options.jobs = 2;
    probe_parallelism = true;
    peak_operations = 0;
    g_assert_cmpint(scan(f), ==, DS_OK);
    probe_parallelism = false;
    g_assert_cmpuint(peak_operations, ==, 2);
    g_assert_cmpuint(f->entries->len, ==, 3);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/linux/metadata", Fixture, NULL, setup, metadata, teardown);
    g_test_add("/linux/raw-name", Fixture, NULL, setup, preserves_raw_name, teardown);
    g_test_add("/linux/skips-links-special", Fixture, NULL, setup, skips_links_and_special, teardown);
    g_test_add("/linux/cache", Fixture, NULL, setup, excludes_cache, teardown);
    g_test_add("/linux/excluded-root", Fixture, NULL, setup, excludes_root_children, teardown);
    g_test_add("/linux/cache-alias", Fixture, NULL, setup, excludes_cache_through_alias, teardown);
    g_test_add("/linux/root-mount-race", Fixture, NULL, setup, reports_root_mount_race, teardown);
    g_test_add("/linux/unknown-dirent", Fixture, NULL, setup, unknown_dirent, teardown);
    g_test_add("/linux/readdir-eio", Fixture, NULL, setup, distinguishes_error_from_eof, teardown);
    g_test_add("/linux/vanished", Fixture, NULL, setup, reports_vanished_entry, teardown);
    g_test_add("/linux/deep-low-fd", Fixture, NULL, setup, deep_under_low_fd_limit, teardown);
    g_test_add("/linux/replaced-directory", Fixture, NULL, setup, verifies_replaced_directory, teardown);
    g_test_add("/linux/permission-denied", Fixture, NULL, setup, reports_permission_denied, teardown);
    g_test_add("/linux/standard-overlap", Fixture, NULL, setup, standard_overlaps_metadata, teardown);
    return g_test_run();
}

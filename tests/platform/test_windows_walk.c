#include "windows_fixture.h"

#include <string.h>

typedef struct {
    GHashTable *entries;
    int64_t last_id;
} Observations;

static DsStatus collect(const DsEntryBatch *batch, void *data, DsError *error)
{
    (void)error;
    Observations *observations = data;
    g_assert_cmpuint(batch->count, <=, DS_BATCH_MAX);
    for (size_t i = 0; i < batch->count; ++i) {
        const DsEntry *entry = &batch->entries[i];
        g_assert_cmpint(entry->parent_id, <, entry->id);
        g_assert_cmpint(entry->name.encoding, ==, DS_PATH_UTF16LE);
        DsEntry *record = g_new(DsEntry, 1);
        *record = *entry;
        record->name = (DsNativePath){0};
        if (!g_hash_table_contains(observations->entries, entry->name.raw)) {
            g_assert_cmpint(entry->id, >, observations->last_id);
            observations->last_id = entry->id;
        }
        g_hash_table_replace(observations->entries, g_bytes_ref(entry->name.raw), record);
    }
    return DS_OK;
}

static const DsEntry *observed(const Observations *observations, const WCHAR *name)
{
    GBytes *key = g_bytes_new_static(name, wcslen(name) * sizeof(*name));
    const DsEntry *result = g_hash_table_lookup(observations->entries, key);
    g_bytes_unref(key);
    return result;
}

static bool create_junction(const WCHAR *path, const WCHAR *target)
{
    typedef struct {
        DWORD tag;
        WORD data_length, reserved, substitute_offset, substitute_length, print_offset, print_length;
        WCHAR names[1024];
    } MountReparseData;
    g_assert_true(CreateDirectoryW(path, NULL));
    HANDLE handle = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    g_assert_true(handle != INVALID_HANDLE_VALUE);
    MountReparseData reparse = { .tag = IO_REPARSE_TAG_MOUNT_POINT };
    int substitute_chars = swprintf(reparse.names, G_N_ELEMENTS(reparse.names),
                                    L"\\??\\%ls", target + 4);
    g_assert_cmpint(substitute_chars, >, 0);
    const size_t print_chars = wcslen(target + 4);
    const size_t print_start = (size_t)substitute_chars + 1;
    g_assert_cmpuint(print_start + print_chars, <, G_N_ELEMENTS(reparse.names));
    memcpy(reparse.names + print_start, target + 4, (print_chars + 1) * sizeof(WCHAR));
    reparse.substitute_length = (WORD)(substitute_chars * sizeof(WCHAR));
    reparse.print_offset = (WORD)(print_start * sizeof(WCHAR));
    reparse.print_length = (WORD)(print_chars * sizeof(WCHAR));
    const DWORD length = (DWORD)(offsetof(MountReparseData, names) +
                                 (print_start + print_chars + 1) * sizeof(WCHAR));
    reparse.data_length = (WORD)(length - 8);
    DWORD returned;
    const BOOL success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &reparse, length,
                                         NULL, 0, &returned, NULL);
    g_assert_true(CloseHandle(handle));
    if (!success) g_assert_true(RemoveDirectoryW(path));
    return success != FALSE;
}

static void scan_local_fixture(void)
{
    WCHAR *dir = fixture_directory();
    if (!dir) return;
    WCHAR *outside = fixture_directory();
    WCHAR *foreign = fixture_path(outside, L"outside-scan.bin");
    g_assert_true(CloseHandle(fixture_open(foreign, CREATE_NEW)));

    const WCHAR long_component[] =
        L"another-directory-name-long-enough-to-ensure-the-full-path-passes-legacy-win32-path-limits-"
        L"while-the-component-itself-is-within-the-ntfs-component-length-1234567890";
    WCHAR *sub1 = fixture_path(dir, long_component);
    WCHAR *sub2 = fixture_path(sub1, long_component);
    const WCHAR unicode_name[] = L"\uD55C\uAE00-\U0001F680.bin";
    WCHAR *unicode = fixture_path(sub2, unicode_name);
    WCHAR *duplicate = fixture_path(dir, L"hardlink.bin");
    WCHAR *junction = fixture_path(dir, L"junction");
    WCHAR *hidden = fixture_path(dir, L"hidden.bin");
    WCHAR *exclude = fixture_path(dir, L"ignore.tmp");
    g_assert_cmpuint(wcslen(unicode), >, 260);
    g_assert_true(CreateDirectoryW(sub1, NULL));
    g_assert_true(CreateDirectoryW(sub2, NULL));
    HANDLE file = fixture_open(unicode, CREATE_NEW);
    fixture_write(file);
    g_assert_true(CloseHandle(file));
    g_assert_true(CreateHardLinkW(duplicate, unicode, NULL));
    const bool junction_created = create_junction(junction, outside);
    g_assert_true(CloseHandle(fixture_open(hidden, CREATE_NEW)));
    g_assert_true(SetFileAttributesW(hidden, FILE_ATTRIBUTE_HIDDEN));
    g_assert_true(CloseHandle(fixture_open(exclude, CREATE_NEW)));

    DsScanOptions options;
    fixture_set_root(&options, dir);
    options.include_hidden = false;
    options.exclude_pattern = "*.tmp";
    DsControl control;
    ds_control_init(&control);
    Observations found = {g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                               (GDestroyNotify)g_bytes_unref, g_free), 0};
    DsError error = {0};
    DsStatus status = ds_walk(&options, &control, collect, &found, &error);
    g_test_message("walk status %d: %s", status, error.message);
    g_assert_cmpint(status, ==, DS_OK);
    const DsEntry *root = observed(&found, L"");
    g_assert_nonnull(root);
    g_assert_cmpint(root->id, ==, 1);
    g_assert_cmpint(root->kind, ==, DS_DIRECTORY);
    const DsEntry *a = observed(&found, unicode_name);
    const DsEntry *b = observed(&found, L"hardlink.bin");
    g_assert_nonnull(a);
    g_assert_nonnull(b);
    g_assert_cmpint(a->kind, ==, DS_FILE);
    g_assert_cmpint(a->logical.quality, ==, DS_KNOWN);
    g_assert_cmpuint(a->logical.bytes, ==, 4096);
    g_assert_cmpint(a->allocated.quality, ==, DS_KNOWN);
    g_assert_cmpuint(a->allocated.bytes, ==, b->allocated.bytes);
    g_assert_true(a->identity_known && b->identity_known);
    g_assert_cmpmem(a->identity, sizeof(a->identity), b->identity, sizeof(b->identity));
    g_assert_cmpuint(a->links, ==, 2);
    g_assert_cmpuint(b->links, ==, 2);
    g_assert_cmpint(observed(&found, L"hidden.bin")->reason, ==, DS_REASON_POLICY);
    g_assert_cmpint(observed(&found, L"ignore.tmp")->reason, ==, DS_REASON_POLICY);
    g_assert_null(observed(&found, L"outside-scan.bin"));
    if (junction_created) {
        const DsEntry *link = observed(&found, L"junction");
        g_assert_nonnull(link);
        g_assert_cmpint(link->kind, ==, DS_LINK);
        g_assert_cmpint(link->reason, ==, DS_REASON_LINK);
        g_assert_false(link->identity_known);

        ds_path_clear(&options.root);
        fixture_set_root(&options, junction);
        g_assert_cmpint(ds_walk(&options, &control, collect, &found, &error), ==, DS_INVALID);
        g_assert_true(RemoveDirectoryW(junction));
    } else {
        g_test_message("junction setup unavailable; the junction assertions were skipped");
    }
    g_hash_table_unref(found.entries);
    ds_control_clear(&control);
    ds_path_clear(&options.root);

    g_assert_true(DeleteFileW(duplicate));
    g_assert_true(DeleteFileW(unicode));
    g_assert_true(DeleteFileW(exclude));
    g_assert_true(DeleteFileW(hidden));
    g_assert_true(RemoveDirectoryW(sub2));
    g_assert_true(RemoveDirectoryW(sub1));
    g_assert_true(RemoveDirectoryW(dir));
    g_assert_true(DeleteFileW(foreign));
    g_assert_true(RemoveDirectoryW(outside));
    g_free(exclude);
    g_free(hidden);
    g_free(junction);
    g_free(duplicate);
    g_free(unicode);
    g_free(sub2);
    g_free(sub1);
    g_free(foreign);
    g_free(outside);
    g_free(dir);
}

static void drive_root_pattern(void)
{
    DsNativePath value = {0};
    const WCHAR drive[] = L"C:";
    g_assert_cmpint(ds_path_new(DS_PATH_UTF16LE, drive, sizeof(drive) - sizeof(WCHAR),
                               &value, NULL), ==, DS_OK);
    DWORD error = ERROR_SUCCESS;
    WCHAR *absolute = ds_win_absolute_path(&value, &error);
    g_assert_nonnull(absolute);
    g_assert_cmpint(wcscmp(absolute, L"\\\\?\\C:\\"), ==, 0);
    WCHAR *pattern = ds_win_join(absolute, L"*");
    g_assert_cmpint(wcscmp(pattern, L"\\\\?\\C:\\*"), ==, 0);
    g_free(pattern);
    g_free(absolute);
    ds_path_clear(&value);
}

typedef struct {
    Observations observations;
    const WCHAR *root, *retired, *replacement_file;
    bool replaced;
} RenamedRoot;

static DsStatus replace_root_after_batch(const DsEntryBatch *batch, void *data, DsError *error)
{
    RenamedRoot *race = data;
    DsStatus status = collect(batch, &race->observations, error);
    if (status == DS_OK && !race->replaced) {
        g_assert_true(MoveFileExW(race->root, race->retired, 0));
        g_assert_true(CreateDirectoryW(race->root, NULL));
        g_assert_true(CloseHandle(fixture_open(race->replacement_file, CREATE_NEW)));
        race->replaced = true;
    }
    return status;
}

static void detect_renamed_root(void)
{
    WCHAR *dir = fixture_directory();
    if (!dir) return;
    WCHAR *root = fixture_path(dir, L"root");
    WCHAR *retired = fixture_path(dir, L"retired");
    WCHAR *replacement = fixture_path(root, L"replacement.bin");
    g_assert_true(CreateDirectoryW(root, NULL));
    for (unsigned i = 0; i < DS_BATCH_MAX + 2; ++i) {
        WCHAR leaf[24];
        swprintf(leaf, G_N_ELEMENTS(leaf), L"file-%04u.bin", i);
        WCHAR *file = fixture_path(root, leaf);
        g_assert_true(CloseHandle(fixture_open(file, CREATE_NEW)));
        g_free(file);
    }
    DsScanOptions options;
    fixture_set_root(&options, root);
    RenamedRoot race = {
        .observations.entries = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                                     (GDestroyNotify)g_bytes_unref, g_free),
        .root = root, .retired = retired, .replacement_file = replacement,
    };
    DsStatus status = ds_walk(&options, NULL, replace_root_after_batch, &race, NULL);
    const DsEntry *root_entry = observed(&race.observations, L"");
    g_assert_true(race.replaced);
    g_assert_nonnull(root_entry);
    g_assert_cmpint(status, ==, DS_PARTIAL);
    g_assert_cmpint(root_entry->reason, ==, DS_REASON_CHANGED);
    WCHAR last_leaf[24];
    swprintf(last_leaf, G_N_ELEMENTS(last_leaf), L"file-%04u.bin", DS_BATCH_MAX + 1);
    g_assert_null(observed(&race.observations, last_leaf));
    g_assert_null(observed(&race.observations, L"replacement.bin"));
    g_hash_table_unref(race.observations.entries);
    ds_path_clear(&options.root);
    for (unsigned i = 0; i < DS_BATCH_MAX + 2; ++i) {
        WCHAR leaf[24];
        swprintf(leaf, G_N_ELEMENTS(leaf), L"file-%04u.bin", i);
        WCHAR *file = fixture_path(retired, leaf);
        g_assert_true(DeleteFileW(file));
        g_free(file);
    }
    g_assert_true(DeleteFileW(replacement));
    g_assert_true(RemoveDirectoryW(root));
    g_assert_true(RemoveDirectoryW(retired));
    g_assert_true(RemoveDirectoryW(dir));
    g_free(replacement);
    g_free(retired);
    g_free(root);
    g_free(dir);
}

static void cloud_and_offline_classification(void)
{
    g_assert_cmpint(ds_win_classify(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT,
                                   0x9000301aUL), ==, DS_REASON_PLACEHOLDER);
    g_assert_cmpint(ds_win_classify(FILE_ATTRIBUTE_OFFLINE, 0), ==, DS_REASON_PLACEHOLDER);
    g_assert_cmpint(ds_win_classify(0x00040000UL, 0), ==, DS_REASON_PLACEHOLDER);
    g_assert_cmpint(ds_win_classify(0x00400000UL, 0), ==, DS_REASON_PLACEHOLDER);
    g_assert_cmpint(ds_win_classify(FILE_ATTRIBUTE_REPARSE_POINT, 0x90000042UL), ==, DS_REASON_LINK);
}

typedef struct { size_t found, expected; } FileCounts;

static DsStatus count_files(const DsEntryBatch *batch, void *data, DsError *error)
{
    FileCounts *counts = data;
    for (size_t i = 0; i < batch->count; ++i)
        if (batch->entries[i].kind == DS_FILE) ++counts->found;
    if (counts->found > counts->expected)
        return ds_error(error, DS_INVALID, 0, "A directory cursor visited a file more than once");
    return DS_OK;
}

static void resume_deep_ancestors(void)
{
    WCHAR *dir = fixture_directory();
    if (!dir) return;
    WCHAR *directories[DS_DIR_HANDLES + 6] = {dir};
    for (size_t i = 1; i < G_N_ELEMENTS(directories); ++i) {
        directories[i] = fixture_path(directories[i - 1], L"alpha");
        g_assert_true(CreateDirectoryW(directories[i], NULL));
    }
    for (size_t i = 0; i < G_N_ELEMENTS(directories); ++i) {
        WCHAR *file = fixture_path(directories[i], L"zeta.bin");
        g_assert_true(CloseHandle(fixture_open(file, CREATE_NEW)));
        g_free(file);
    }

    DsScanOptions options;
    fixture_set_root(&options, dir);
    DsError error = {0};
    FileCounts counts = {.expected = G_N_ELEMENTS(directories)};
    DsStatus status = ds_walk(&options, NULL, count_files, &counts, &error);
    g_test_message("deep walk status %d: %s", status, error.message);
    ds_path_clear(&options.root);

    for (size_t i = G_N_ELEMENTS(directories); i-- > 0;) {
        WCHAR *file = fixture_path(directories[i], L"zeta.bin");
        g_assert_true(DeleteFileW(file));
        g_assert_true(RemoveDirectoryW(directories[i]));
        g_free(file);
        g_free(directories[i]);
    }
    g_assert_cmpint(status, ==, DS_OK);
    g_assert_cmpuint(counts.found, ==, counts.expected);
}

typedef struct {
    GMutex lock;
    GCond changed;
    GThread *walker;
    DsControl *control;
    unsigned walker_operations, helper_operations, active, peak_active, pauses;
} MetadataOverlap;

static MetadataOverlap *overlap;

static HANDLE WINAPI observe_metadata_open(LPCWSTR path, DWORD access, DWORD share,
                                           LPSECURITY_ATTRIBUTES attrs, DWORD disposition,
                                           DWORD flags, HANDLE template)
{
    g_mutex_lock(&overlap->lock);
    ++overlap->active;
    overlap->peak_active = MAX(overlap->peak_active, overlap->active);
    bool walker = g_thread_self() == overlap->walker;
    unsigned *this_thread = walker ? &overlap->walker_operations : &overlap->helper_operations;
    const unsigned *other_thread = walker ? &overlap->helper_operations : &overlap->walker_operations;
    ++*this_thread;
    g_cond_broadcast(&overlap->changed);
    /* Keep the real metadata operations in flight until their calls overlap.
     * Bound the wait so a serial walker fails the assertion without hanging. */
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while (*this_thread == 1 && !*other_thread &&
           g_cond_wait_until(&overlap->changed, &overlap->lock, deadline)) {}
    if (walker) ds_control_pause(overlap->control, true);
    g_mutex_unlock(&overlap->lock);
    HANDLE file = CreateFileW(path, access, share, attrs, disposition, flags, template);
    g_assert_true(file != INVALID_HANDLE_VALUE);
    return file;
}

static BOOL WINAPI observe_metadata_close(HANDLE file)
{
    const BOOL closed = CloseHandle(file);
    g_mutex_lock(&overlap->lock);
    --overlap->active;
    g_mutex_unlock(&overlap->lock);
    return closed;
}

static void acknowledge_metadata_pause(bool paused, void *data)
{
    if (!paused) return;
    MetadataOverlap *metadata = data;
    g_mutex_lock(&metadata->lock);
    g_assert_cmpuint(metadata->active, ==, 0);
    ++metadata->pauses;
    g_mutex_unlock(&metadata->lock);
    ds_control_pause(metadata->control, false);
}

static void standard_profile_metadata_helper(void)
{
    WCHAR *dir = fixture_directory();
    if (!dir) return;
    WCHAR *first = fixture_path(dir, L"alpha.bin");
    WCHAR *second = fixture_path(dir, L"zeta.bin");
    HANDLE written_file = fixture_open(first, CREATE_NEW);
    DWORD written = 0;
    g_assert_true(WriteFile(written_file, "a", 1, &written, NULL));
    g_assert_cmpuint(written, ==, 1);
    g_assert_true(CloseHandle(written_file));
    g_assert_true(CloseHandle(fixture_open(second, CREATE_NEW)));

    DsScanOptions options;
    fixture_set_root(&options, dir);
    options.profile = DS_STANDARD;
    options.jobs = 2;
    DsWinMetadataOps ops = ds_win_metadata_ops;
    ops.open = observe_metadata_open;
    ops.close = observe_metadata_close;
    DsControl control;
    ds_control_init(&control);
    MetadataOverlap metadata = {.walker = g_thread_self(), .control = &control};
    g_mutex_init(&metadata.lock);
    g_cond_init(&metadata.changed);
    control.pause_changed = acknowledge_metadata_pause;
    control.pause_data = &metadata;
    overlap = &metadata;
    Observations found = {g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                               (GDestroyNotify)g_bytes_unref, g_free), 0};
    DsError error = {0};
    DsStatus status = ds_win_walk_with_metadata_ops(&options, &control, collect, &found, &error, &ops);
    g_test_message("Standard walk status %d: %s", status, error.message);
    ds_control_clear(&control);
    overlap = NULL;
    g_cond_clear(&metadata.changed);
    g_mutex_clear(&metadata.lock);
    ds_path_clear(&options.root);
    g_assert_true(DeleteFileW(first));
    g_assert_true(DeleteFileW(second));
    g_assert_true(RemoveDirectoryW(dir));
    g_free(first);
    g_free(second);
    g_free(dir);

    g_assert_cmpint(status, ==, DS_OK);
    g_assert_cmpuint(g_hash_table_size(found.entries), ==, 3);
    g_assert_cmpuint(metadata.peak_active, ==, 2);
    g_assert_cmpuint(metadata.helper_operations, ==, 1);
    g_assert_cmpuint(metadata.walker_operations, ==, 1);
    g_assert_cmpuint(metadata.pauses, ==, 1);
    g_assert_cmpint(observed(&found, L"alpha.bin")->allocated.quality, ==, DS_KNOWN);
    g_assert_cmpint(observed(&found, L"zeta.bin")->allocated.quality, ==, DS_KNOWN);
    g_assert_cmpuint(observed(&found, L"alpha.bin")->logical.bytes, ==, 1);
    g_assert_cmpuint(observed(&found, L"zeta.bin")->logical.bytes, ==, 0);
    g_hash_table_unref(found.entries);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/windows/walk/local-ntfs", scan_local_fixture);
    g_test_add_func("/windows/walk/drive-root-pattern", drive_root_pattern);
    g_test_add_func("/windows/walk/renamed-root", detect_renamed_root);
    g_test_add_func("/windows/walk/placeholder-attributes", cloud_and_offline_classification);
    g_test_add_func("/windows/walk/deep-ancestors", resume_deep_ancestors);
    g_test_add_func("/windows/walk/standard-metadata-helper", standard_profile_metadata_helper);
    return g_test_run();
}

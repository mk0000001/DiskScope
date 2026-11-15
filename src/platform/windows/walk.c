#include "win_scan.h"

#include <string.h>
#include <wchar.h>

typedef struct {
    DsEntry entry;
    size_t path_length;
    HANDLE find;
    DsWinNode node;
    GBytes *cursor;
    bool observed;
} Directory;

typedef struct {
    DsEntry entry;
    WCHAR *path;
    WIN32_FIND_DATAW found;
} MetadataJob;

typedef struct {
    const DsScanOptions *options;
    DsControl *control;
    DsEntrySink sink;
    void *sink_data;
    DsError *error;
    const DsWinMetadataOps *file_ops;
    GPatternSpec *pattern;
    GPtrArray *exclude_paths;
    WCHAR *cache_path;
    WCHAR *path;
    GArray *directories;
    DsWinNode root;
    size_t open_searches;
    int64_t last_id;
    int64_t batch_start;
    size_t batch_count;
    DsEntry *batch;
    GThread *helper;
    GAsyncQueue *requests, *results;
    MetadataJob job;
    bool pending;
    bool ntfs;
    bool partial;
} Walker;

static Directory *top(Walker *w)
{
    return &g_array_index(w->directories, Directory, w->directories->len - 1);
}

static void close_search(Walker *w, Directory *directory)
{
    if (directory->find == INVALID_HANDLE_VALUE) return;
    FindClose(directory->find);
    directory->find = INVALID_HANDLE_VALUE;
    --w->open_searches;
}

static void pop_directory(Walker *w)
{
    Directory *directory = top(w);
    close_search(w, directory);
    g_clear_pointer(&directory->cursor, g_bytes_unref);
    ds_path_clear(&directory->entry.name);
    g_array_set_size(w->directories, w->directories->len - 1);
    if (w->directories->len) w->path[top(w)->path_length] = 0;
}

static DsStatus flush(Walker *w)
{
    if (!w->batch_count) return DS_OK;
    const DsEntryBatch batch = {w->batch, w->batch_count};
    const DsStatus status = w->sink(&batch, w->sink_data, w->error);
    for (size_t i = 0; i < w->batch_count; ++i) ds_path_clear(&w->batch[i].name);
    w->batch_count = 0;
    return status;
}

static DsStatus emit(Walker *w, const DsEntry *entry)
{
    if (entry->reason >= DS_REASON_PERMISSION) w->partial = true;
    if (!w->batch_count) w->batch_start = g_get_monotonic_time();
    DsEntry *queued = &w->batch[w->batch_count++];
    *queued = *entry;
    queued->name.raw = g_bytes_ref(entry->name.raw);
    if (w->batch_count == DS_BATCH_MAX) return flush(w);
    return DS_OK;
}

static gpointer metadata_helper(void *data)
{
    Walker *w = data;
    for (;;) {
        void *request = g_async_queue_pop(w->requests);
        if (request == w) return NULL;
        MetadataJob *job = request;
        ds_win_measure_file(w->file_ops, job->path, &w->root, &job->found, &job->entry);
        g_async_queue_push(w->results, job);
    }
}

static void start_helper(Walker *w)
{
    if (w->options->profile != DS_STANDARD || w->options->jobs < 2) return;
    w->requests = g_async_queue_new();
    w->results = g_async_queue_new();
    w->helper = g_thread_try_new("metadata", metadata_helper, w, NULL);
}

static void clear_job(Walker *w)
{
    ds_path_clear(&w->job.entry.name);
    g_clear_pointer(&w->job.path, g_free);
    w->pending = false;
}

static void stop_helper(Walker *w)
{
    if (w->helper) {
        g_async_queue_push(w->requests, w);
        g_thread_join(w->helper);
    }
    if (w->pending) clear_job(w);
    g_clear_pointer(&w->requests, g_async_queue_unref);
    g_clear_pointer(&w->results, g_async_queue_unref);
}

static DsStatus collect_metadata(Walker *w)
{
    if (!w->pending) return DS_OK;
    const MetadataJob *job = g_async_queue_pop(w->results);
    DsStatus status = emit(w, &job->entry);
    clear_job(w);
    return status;
}

static bool boundary_requested(DsControl *control)
{
    if (!control) return false;
    g_mutex_lock(&control->lock);
    const bool requested = control->pause || control->cancel;
    g_mutex_unlock(&control->lock);
    return requested;
}

static bool same_path(const WCHAR *left, const WCHAR *right)
{
    return right && CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}

static DsReason path_policy(Walker *w, const WCHAR *path, const WIN32_FIND_DATAW *found)
{
    if (same_path(path, w->cache_path)) return DS_REASON_CACHE;
    for (guint i = 0; i < w->exclude_paths->len; ++i)
        if (same_path(path, g_ptr_array_index(w->exclude_paths, i))) return DS_REASON_POLICY;
    if (!w->options->include_hidden && (found->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
        return DS_REASON_POLICY;
    if (w->pattern) {
        char *utf8 = g_utf16_to_utf8((const gunichar2 *)found->cFileName, -1, NULL, NULL, NULL);
        const bool matched = utf8 && g_pattern_spec_match_string(w->pattern, utf8);
        g_free(utf8);
        if (matched) return DS_REASON_POLICY;
    }
    return DS_REASON_NONE;
}

static DWORD open_directory(const WCHAR *path, HANDLE *handle, DsWinNode *node)
{
    const DWORD error = ds_win_open_node(&ds_win_metadata_ops, path, true, handle, node);
    if (error) return error;
    if (ds_win_classify(node->attributes, node->tag) != DS_REASON_NONE)
        return ERROR_REPARSE_TAG_MISMATCH;
    return (node->attributes & FILE_ATTRIBUTE_DIRECTORY) ? ERROR_SUCCESS : ERROR_DIRECTORY;
}

static bool supported_filesystem(const WCHAR *fs)
{
    return wcscmp(fs, L"NTFS") == 0 || wcscmp(fs, L"ReFS") == 0 ||
           wcscmp(fs, L"FAT") == 0 || wcscmp(fs, L"FAT32") == 0 || wcscmp(fs, L"exFAT") == 0;
}

static DsStatus inspect_root(Walker *w)
{
    WCHAR drive[] = {w->path[4], L':', L'\\', 0};
    const UINT type = GetDriveTypeW(drive);
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK)
        return ds_error(w->error, DS_INVALID, ERROR_NOT_SUPPORTED, "Scan root requires a local volume");

    /* CreateFileW protects the final component. Walk the selected root's
     * prefixes explicitly so an existing ancestor junction is excluded too. */
    size_t end = 7;
    for (;;) {
        if (!ds_control_checkpoint(w->control)) return DS_CANCELED;
        WCHAR saved = w->path[end];
        w->path[end] = 0;
        HANDLE handle = INVALID_HANDLE_VALUE;
        DsWinNode node;
        DWORD error = open_directory(w->path, &handle, &node);
        w->path[end] = saved;
        if (!error && !saved) {
            DsWinVolume volume;
            error = ds_win_volume_info(handle, w->path, &volume);
            if (!error && !supported_filesystem(volume.filesystem)) error = ERROR_NOT_SUPPORTED;
            if (!error) {
                w->root = node;
                w->ntfs = wcscmp(volume.filesystem, L"NTFS") == 0;
            }
        }
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (error) {
            const bool invalid = error == ERROR_REPARSE_TAG_MISMATCH || error == ERROR_DIRECTORY ||
                                 error == ERROR_NOT_SUPPORTED;
            return ds_error(w->error, invalid ? DS_INVALID : DS_IO, error,
                            "Could not open the Windows scan root as a supported local directory");
        }
        if (!saved) return DS_OK;
        if (saved == L'\\') ++end;
        while (w->path[end] && w->path[end] != L'\\') ++end;
    }
}

static void reserve_search(Walker *w)
{
    /* Two slots are available for the directory identity checks while opening
     * a search. Suspend the oldest cursor, which resumes at its active child. */
    if (w->open_searches < DS_DIR_HANDLES - 2) return;
    for (guint i = 0; i + 1 < w->directories->len; ++i) {
        Directory *directory = &g_array_index(w->directories, Directory, i);
        if (directory->find == INVALID_HANDLE_VALUE) continue;
        close_search(w, directory);
        return;
    }
}

static DWORD compare_directory(const Walker *w, const DsWinNode *expected, const DsWinNode *actual)
{
    if (!w->ntfs && (!expected->id_known || !actual->id_known)) return ERROR_NOT_SUPPORTED;
    return ds_win_same_node(expected, actual) ? ERROR_SUCCESS : ERROR_FILE_INVALID;
}

static DWORD search_directory(Walker *w, Directory *directory, WIN32_FIND_DATAW *found)
{
    HANDLE before = INVALID_HANDLE_VALUE, after = INVALID_HANDLE_VALUE;
    DsWinNode first, second;
    DWORD error = open_directory(w->path, &before, &first);
    if (error == ERROR_FILE_NOT_FOUND) error = ERROR_PATH_NOT_FOUND;
    if (!error && !w->ntfs && !first.id_known) error = ERROR_NOT_SUPPORTED;
    if (!error && !ds_win_same_volume(&w->root, &first)) error = ERROR_NOT_SAME_DEVICE;
    if (!error && directory->observed) error = compare_directory(w, &directory->node, &first);
    if (!error && directory->cursor && directory->node.legacy_known && first.legacy_known &&
        CompareFileTime(&directory->node.legacy.ftLastWriteTime, &first.legacy.ftLastWriteTime) != 0)
        error = ERROR_FILE_INVALID;

    if (!error) {
        WCHAR *pattern = ds_win_join(w->path, L"*");
        if (pattern) {
            reserve_search(w);
            directory->find = FindFirstFileExW(pattern, FindExInfoBasic, found,
                                               FindExSearchNameMatch, NULL, 0);
            if (directory->find != INVALID_HANDLE_VALUE) ++w->open_searches;
            else error = GetLastError();
            g_free(pattern);
        } else {
            error = ERROR_FILENAME_EXCED_RANGE;
        }
    }
    if (!error || error == ERROR_FILE_NOT_FOUND) {
        DWORD recheck = open_directory(w->path, &after, &second);
        if (!recheck) recheck = compare_directory(w, &first, &second);
        if (recheck)
            error = recheck == ERROR_FILE_NOT_FOUND ? ERROR_FILE_INVALID : recheck;
        else
            directory->node = first;
    }
    if (after != INVALID_HANDLE_VALUE) CloseHandle(after);
    if (before != INVALID_HANDLE_VALUE) CloseHandle(before);
    return error;
}

static DWORD next_entry(HANDLE search, WIN32_FIND_DATAW *found)
{
    return FindNextFileW(search, found) ? ERROR_SUCCESS : GetLastError();
}

static bool matching_cursor(GBytes *cursor, const WCHAR *name)
{
    size_t size;
    const void *bytes = g_bytes_get_data(cursor, &size);
    return size == wcslen(name) * sizeof(WCHAR) && memcmp(bytes, name, size) == 0;
}

static DsStatus resume_search(Walker *w, Directory *directory, WIN32_FIND_DATAW *found, DWORD *error)
{
    while (!*error) {
        if (!ds_control_checkpoint(w->control)) return DS_CANCELED;
        if (matching_cursor(directory->cursor, found->cFileName)) {
            *error = next_entry(directory->find, found);
            return DS_OK;
        }
        *error = next_entry(directory->find, found);
    }
    if (*error == ERROR_NO_MORE_FILES || *error == ERROR_FILE_NOT_FOUND) *error = ERROR_FILE_INVALID;
    return DS_OK;
}

static DWORD check_return_path(Walker *w, Directory *directory)
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    DsWinNode current;
    DWORD error = open_directory(w->path, &handle, &current);
    if (error == ERROR_FILE_NOT_FOUND) error = ERROR_FILE_INVALID;
    if (!error) error = compare_directory(w, &directory->node, &current);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    return error;
}

static bool dot_entry(const WCHAR *name)
{
    return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
}

static DsStatus observe_child(Walker *w, Directory *directory, const WIN32_FIND_DATAW *found)
{
    if (w->last_id == INT64_MAX)
        return ds_error(w->error, DS_LIMIT, 0, "Windows scan ran out of entry identifiers");

    DsEntry entry = {.id = ++w->last_id, .parent_id = directory->entry.id};
    const size_t bytes = wcslen(found->cFileName) * sizeof(WCHAR);
    DsStatus status = ds_path_new(DS_PATH_UTF16LE, found->cFileName, bytes, &entry.name, w->error);
    if (status != DS_OK) return status;
    ds_win_find_metadata(found, &entry);
    WCHAR *child = NULL;
    if (entry.reason == DS_REASON_NONE) {
        child = ds_win_join(w->path, found->cFileName);
        if (!child) {
            entry.reason = DS_REASON_LIMIT;
            entry.os_code = ERROR_FILENAME_EXCED_RANGE;
        } else {
            entry.reason = path_policy(w, child, found);
        }
        if (entry.reason != DS_REASON_NONE) {
            entry.kind = DS_EXCLUDED;
        } else if (entry.kind == DS_DIRECTORY && w->directories->len >= DS_DEPTH_MAX) {
            entry.kind = DS_EXCLUDED;
            entry.reason = DS_REASON_LIMIT;
        } else if (entry.kind == DS_FILE) {
            if (w->helper) {
                w->job = (MetadataJob){.entry = entry, .path = child, .found = *found};
                w->pending = true;
                g_async_queue_push(w->requests, &w->job);
                return DS_OK;
            }
            ds_win_measure_file(w->file_ops, child, &w->root, found, &entry);
        }
    }

    status = emit(w, &entry);
    if (status == DS_OK && entry.kind == DS_DIRECTORY) {
        directory->cursor = g_bytes_ref(entry.name.raw);
        Directory frame = {.entry = entry, .path_length = wcslen(child), .find = INVALID_HANDLE_VALUE};
        g_array_append_val(w->directories, frame);
        g_free(w->path);
        w->path = child;
    } else {
        ds_path_clear(&entry.name);
        g_free(child);
    }
    return status;
}

static DsStatus enumerate(Walker *w)
{
    Directory root = {
        .entry = {.id = ++w->last_id, .kind = DS_DIRECTORY, .logical = {0, DS_UNAVAILABLE},
                  .allocated = {0, DS_UNAVAILABLE}},
        .path_length = wcslen(w->path), .find = INVALID_HANDLE_VALUE, .node = w->root,
        .observed = true,
    };
    DsStatus status = ds_path_new(DS_PATH_UTF16LE, NULL, 0, &root.entry.name, w->error);
    if (status != DS_OK) return status;
    g_array_append_val(w->directories, root);
    status = emit(w, &root.entry);

    while (status == DS_OK && w->directories->len) {
        /* Let a file query overlap the next cursor advancement. Drain the
         * helper before acknowledging a pause or delivering the next entry. */
        if (w->pending && boundary_requested(w->control)) {
            status = collect_metadata(w);
            if (status != DS_OK) return status;
        }
        if (!w->pending && !ds_control_checkpoint(w->control)) return DS_CANCELED;
        if (w->batch_count && g_get_monotonic_time() - w->batch_start >= DS_BATCH_MSEC * 1000) {
            status = flush(w);
            if (status != DS_OK) return status;
        }
        Directory *directory = top(w);
        WIN32_FIND_DATAW found;
        DWORD error = ERROR_SUCCESS;
        if (directory->find == INVALID_HANDLE_VALUE) {
            error = search_directory(w, directory, &found);
            directory->observed = true;
            if (directory->cursor) {
                status = resume_search(w, directory, &found, &error);
                if (status != DS_OK) return status;
            }
        } else {
            if (directory->cursor) error = check_return_path(w, directory);
            if (!error) error = next_entry(directory->find, &found);
        }
        if (w->pending) {
            status = collect_metadata(w);
            if (status != DS_OK) return status;
            if (!ds_control_checkpoint(w->control)) return DS_CANCELED;
        }
        g_clear_pointer(&directory->cursor, g_bytes_unref);
        if (error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND) {
            pop_directory(w);
        } else if (error) {
            DsReason reason = ds_win_error_reason(error, DS_REASON_IO);
            if (error == ERROR_NOT_SAME_DEVICE) reason = DS_REASON_MOUNT;
            else if (error == ERROR_FILE_INVALID || error == ERROR_DIRECTORY) reason = DS_REASON_CHANGED;
            else if (error == ERROR_NOT_SUPPORTED) reason = DS_REASON_METADATA;
            directory->entry.reason = reason;
            directory->entry.os_code = error;
            status = emit(w, &directory->entry);
            pop_directory(w);
        } else if (!dot_entry(found.cFileName)) {
            status = observe_child(w, directory, &found);
        }
    }
    return status;
}

DsStatus ds_win_walk_with_metadata_ops(const DsScanOptions *options, DsControl *control,
                                       DsEntrySink sink, void *sink_data, DsError *error,
                                       const DsWinMetadataOps *file_ops)
{
    if (!options || !sink || (options->exclude_count && !options->exclude_paths) ||
        (options->exclude_pattern && !g_utf8_validate(options->exclude_pattern, -1, NULL)))
        return ds_error(error, DS_INVALID, 0, "Invalid Windows scan options or callback");

    Walker w = {.options = options, .control = control, .sink = sink, .sink_data = sink_data,
                .error = error, .file_ops = file_ops};
    DWORD code;
    w.path = ds_win_absolute_path(&options->root, &code);
    if (!w.path)
        return ds_error(error, DS_INVALID, code, "Scan root requires an absolute Windows drive path");

    w.exclude_paths = g_ptr_array_new_with_free_func(g_free);
    w.directories = g_array_new(FALSE, FALSE, sizeof(Directory));
    w.batch = g_new(DsEntry, DS_BATCH_MAX);
    if (options->exclude_pattern) w.pattern = g_pattern_spec_new(options->exclude_pattern);

    if (options->cache_path.raw) w.cache_path = ds_win_absolute_path(&options->cache_path, &code);
    for (size_t i = 0; code == ERROR_SUCCESS && i < options->exclude_count; ++i) {
        WCHAR *excluded = ds_win_absolute_path(&options->exclude_paths[i], &code);
        if (excluded) g_ptr_array_add(w.exclude_paths, excluded);
    }
    DsStatus status;
    if (code != ERROR_SUCCESS || same_path(w.path, w.cache_path))
        status = ds_error(error, DS_INVALID, code, "Invalid Windows exclusion path or cache root");
    else
        status = inspect_root(&w);
    if (status == DS_OK) {
        start_helper(&w);
        status = enumerate(&w);
    }
    stop_helper(&w);
    if (status == DS_OK || status == DS_CANCELED) {
        const DsStatus delivered = flush(&w);
        if (delivered != DS_OK) status = delivered;
    }
    for (size_t i = 0; i < w.batch_count; ++i) ds_path_clear(&w.batch[i].name);
    while (w.directories->len) pop_directory(&w);
    g_array_unref(w.directories);
    g_ptr_array_unref(w.exclude_paths);
    g_clear_pointer(&w.pattern, g_pattern_spec_free);
    g_free(w.cache_path);
    g_free(w.batch);
    g_free(w.path);
    if (status == DS_OK && w.partial) return DS_PARTIAL;
    return status;
}

DsStatus ds_walk(const DsScanOptions *options, DsControl *control, DsEntrySink sink,
                 void *sink_data, DsError *error)
{
    return ds_win_walk_with_metadata_ops(options, control, sink, sink_data, error,
                                         &ds_win_metadata_ops);
}

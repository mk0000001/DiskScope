#define _GNU_SOURCE
#include "linux_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

typedef struct {
    bool ready;
    int code;
    LinuxMetadata value;
} Observation;

typedef struct {
    GThread *thread;
    GMutex lock;
    GCond changed;
    const char *name;
    int parent;
    bool stop;
    Observation result;
} MetadataWorker;

typedef struct {
    int64_t id, parent;
    char *name;
    size_t path_length;
    LinuxMetadata metadata;
    DIR *stream;
    long cookie;
    char *next_name;
    Observation next_observation;
    bool finished;
    int read_error;
} Directory;

typedef struct {
    LinuxRoot root;
    const DsScanOptions *options;
    DsControl *control;
    DsEntrySink sink;
    void *sink_data;
    DsError *error;
    DsStatus status;
    int first_issue;
    bool partial, trust_identity, cache_identity_known;
    LinuxMetadata cache_identity;
    char *cache_path;
    GHashTable *excluded_paths;
    GString *path;
    GArray *stack;
    unsigned stream_count, stream_limit;
    int64_t last_id, batch_start;
    DsEntry *entries;
    size_t count;
    MetadataWorker worker;
} LinuxWalk;

static gpointer metadata_worker(gpointer data)
{
    MetadataWorker *worker = data;
    g_mutex_lock(&worker->lock);
    for (;;) {
        while (!worker->name && !worker->stop) g_cond_wait(&worker->changed, &worker->lock);
        if (worker->stop) break;
        const char *name = worker->name;
        int parent = worker->parent;
        g_mutex_unlock(&worker->lock);
        Observation result = {.ready = true};
        result.code = linux_metadata_at(parent, name, 0, &result.value);
        g_mutex_lock(&worker->lock);
        worker->result = result;
        worker->name = NULL;
        g_cond_signal(&worker->changed);
    }
    g_mutex_unlock(&worker->lock);
    return NULL;
}

static bool worker_start(MetadataWorker *worker, DsError *error)
{
    g_autoptr(GError) failure = NULL;
    g_mutex_init(&worker->lock);
    g_cond_init(&worker->changed);
    worker->thread = g_thread_try_new("metadata", metadata_worker, worker, &failure);
    if (worker->thread) return true;
    g_mutex_clear(&worker->lock);
    g_cond_clear(&worker->changed);
    ds_error(error, DS_LIMIT, 0, "Cannot start the second metadata job: %s", failure->message);
    return false;
}

static void worker_stop(MetadataWorker *worker)
{
    if (!worker->thread) return;
    g_mutex_lock(&worker->lock);
    worker->stop = true;
    g_cond_signal(&worker->changed);
    g_mutex_unlock(&worker->lock);
    g_thread_join(worker->thread);
    g_mutex_clear(&worker->lock);
    g_cond_clear(&worker->changed);
}

static void worker_submit(MetadataWorker *worker, int parent, const char *name)
{
    g_mutex_lock(&worker->lock);
    worker->parent = parent;
    worker->name = name;
    g_cond_signal(&worker->changed);
    g_mutex_unlock(&worker->lock);
}

static Observation worker_take(MetadataWorker *worker)
{
    g_mutex_lock(&worker->lock);
    while (worker->name) g_cond_wait(&worker->changed, &worker->lock);
    Observation result = worker->result;
    g_mutex_unlock(&worker->lock);
    return result;
}

static Directory *frame_at(LinuxWalk *walk, size_t index)
{
    return &g_array_index(walk->stack, Directory, index);
}

static void flush(LinuxWalk *walk)
{
    if (!walk->count) return;
    if (walk->status == DS_OK || walk->status == DS_CANCELED) {
        DsEntryBatch batch = {walk->entries, walk->count};
        DsStatus status = walk->sink(&batch, walk->sink_data, walk->error);
        if (status != DS_OK) walk->status = status;
    }
    for (size_t index = 0; index < walk->count; ++index)
        ds_path_clear(&walk->entries[index].name);
    walk->count = 0;
}

static bool checkpoint(LinuxWalk *walk)
{
    if (walk->status != DS_OK) return false;
    if (walk->count && g_get_monotonic_time() - walk->batch_start >= DS_BATCH_MSEC * 1000)
        flush(walk);
    if (walk->status == DS_OK && !ds_control_checkpoint(walk->control)) walk->status = DS_CANCELED;
    return walk->status == DS_OK;
}

static void emit(LinuxWalk *walk, int64_t id, int64_t parent, const char *name, DsKind kind,
                 DsReason reason, int code, const LinuxMetadata *metadata)
{
    DsSize unknown = {.quality = reason == DS_REASON_CHANGED ? DS_CHANGED : DS_UNAVAILABLE};
    DsEntry entry = {.id = id, .parent_id = parent, .kind = kind, .reason = reason,
                     .os_code = code, .logical = unknown, .allocated = unknown};
    entry.name = (DsNativePath){DS_PATH_BYTES, g_bytes_new(name, strlen(name))};
    if (metadata) {
        entry.links = metadata->links;
        entry.mtime = metadata->mtime;
        entry.mtime_known = metadata->mtime_known;
        if (kind == DS_FILE && reason != DS_REASON_CHANGED) {
            entry.logical = metadata->logical;
            entry.allocated = metadata->allocated;
            if (walk->trust_identity && metadata->inode && metadata->links) {
                ds_u64_encode(metadata->device, entry.identity);
                ds_u64_encode(metadata->inode, entry.identity + 8);
                entry.identity_known = true;
            }
        }
    }
    if (!walk->count) walk->batch_start = g_get_monotonic_time();
    walk->entries[walk->count++] = entry;
    if (walk->count == DS_BATCH_MAX) flush(walk);
}

static DsReason errno_reason(int code)
{
    switch (code) {
        case EACCES: case EPERM: return DS_REASON_PERMISSION;
        case ENOENT: case ESTALE: case ELOOP: case ENOTDIR: return DS_REASON_CHANGED;
        case EXDEV: return DS_REASON_MOUNT;
        case EMFILE: case ENFILE: case ENAMETOOLONG: case EOVERFLOW: case ENOMEM: return DS_REASON_LIMIT;
        default: return DS_REASON_IO;
    }
}

static void note_issue(LinuxWalk *walk, int code)
{
    if (!walk->partial) walk->first_issue = code;
    walk->partial = true;
}

static void directory_issue(LinuxWalk *walk, const Directory *directory, int code)
{
    note_issue(walk, code);
    emit(walk, directory->id, directory->parent, directory->name,
         DS_DIRECTORY, errno_reason(code), code, &directory->metadata);
}

static void close_stream(LinuxWalk *walk, Directory *directory)
{
    if (!directory->stream) return;
    closedir(directory->stream);
    directory->stream = NULL;
    --walk->stream_count;
}

static bool evict_stream(LinuxWalk *walk, int protected_fd)
{
    for (size_t index = 0; index < walk->stack->len; ++index) {
        Directory *candidate = frame_at(walk, index);
        if (candidate->stream && dirfd(candidate->stream) != protected_fd) {
            close_stream(walk, candidate);
            return true;
        }
    }
    return false;
}

static bool on_root_mount(const LinuxWalk *walk, const LinuxMetadata *metadata)
{
    if (metadata->mount_id) return metadata->mount_id == walk->root.mount->id;
    return metadata->device == walk->root.metadata.device;
}

static int checked_directory(LinuxWalk *walk, int parent, const char *name,
                             const LinuxMetadata *expected, int access_mode, int *code)
{
    int child = linux_open_directory(&walk->root.method, parent, name, true, access_mode);
    if (child == -1 && errno == EMFILE && evict_stream(walk, parent))
        child = linux_open_directory(&walk->root.method, parent, name, true, access_mode);
    if (child == -1) {
        *code = errno;
        return -1;
    }

    LinuxMetadata opened;
    *code = linux_metadata_at(child, "", AT_EMPTY_PATH, &opened);
    if (*code == 0 && !opened.mount_id && walk->root.metadata.mount_id &&
        walk->root.method == LINUX_OPEN_AT)
        opened.mount_id = linux_fd_mount_id(child);
    if (*code == 0 && (!on_root_mount(walk, &opened) || opened.placeholder)) *code = EXDEV;
    if (*code == 0 && (!S_ISDIR(opened.mode) || !linux_same_identity(&opened, expected))) *code = ESTALE;
    if (*code != 0) {
        close(child);
        return -1;
    }
    return child;
}

static int adopt_stream(LinuxWalk *walk, Directory *directory, int fd)
{
    if (walk->stream_count >= walk->stream_limit) evict_stream(walk, -1);
    DIR *stream = fdopendir(fd);
    if (!stream) {
        int code = errno;
        close(fd);
        return code;
    }
    if (directory->cookie) seekdir(stream, directory->cookie);
    directory->stream = stream;
    ++walk->stream_count;
    return 0;
}

static int reopen_stream(LinuxWalk *walk, size_t depth)
{
    size_t start = depth;
    while (start > 0 && !frame_at(walk, start - 1)->stream) --start;
    int base = start == 0 ? walk->root.fd : dirfd(frame_at(walk, start - 1)->stream);
    int owned = -1;
    int code = 0;
    for (size_t index = start; index <= depth && checkpoint(walk); ++index) {
        Directory *current = frame_at(walk, index);
        const char *name = index == 0 ? "." : current->name;
        int mode = index == depth ? O_RDONLY : O_PATH;
        int child = checked_directory(walk, base, name, &current->metadata, mode, &code);
        if (owned >= 0) close(owned);
        owned = child;
        if (child < 0) return code;
        base = child;
    }
    if (owned < 0 || walk->status != DS_OK) {
        if (owned >= 0) close(owned);
        return 0;
    }
    return adopt_stream(walk, frame_at(walk, depth), owned);
}

static void pop_directory(LinuxWalk *walk)
{
    Directory *directory = frame_at(walk, walk->stack->len - 1);
    close_stream(walk, directory);
    g_free(directory->name);
    g_free(directory->next_name);
    g_array_set_size(walk->stack, walk->stack->len - 1);
    if (walk->stack->len) g_string_truncate(walk->path, frame_at(walk, walk->stack->len - 1)->path_length);
}

static DsReason exclude_reason(const LinuxWalk *walk, const char *name)
{
    if (walk->cache_path && strcmp(walk->cache_path, walk->path->str) == 0) return DS_REASON_CACHE;
    if (linux_mount_at(walk->root.mounts, walk->path->str, walk->root.mount->id)) return DS_REASON_MOUNT;
    if ((!walk->options->include_hidden && name[0] == '.') ||
        (walk->options->exclude_pattern && fnmatch(walk->options->exclude_pattern, name, 0) == 0) ||
        g_hash_table_contains(walk->excluded_paths, walk->path->str)) return DS_REASON_POLICY;
    return DS_REASON_NONE;
}

static void append_name(GString *path, const char *name)
{
    if (path->len != 1) g_string_append_c(path, '/');
    g_string_append(path, name);
}

static bool can_prefetch(LinuxWalk *walk, const char *name)
{
    if (!walk->worker.thread || !name) return false;
    size_t parent_length = walk->path->len;
    append_name(walk->path, name);
    DsReason reason = exclude_reason(walk, name);
    bool allowed = walk->path->len <= DS_PATH_MAX &&
        (reason == DS_REASON_NONE || reason == DS_REASON_POLICY);
    g_string_truncate(walk->path, parent_length);
    return allowed;
}

static bool cache_contains_root(const char *cache_path, const char *root)
{
    size_t length = strlen(cache_path);
    return strncmp(cache_path, root, length) == 0 &&
        (length == 1 || root[length] == '\0' || root[length] == '/');
}

static DsStatus configure_exclusions(LinuxWalk *walk)
{
    if (walk->options->cache_path.raw) {
        walk->cache_path = linux_canonical_path(&walk->options->cache_path, walk->error);
        if (!walk->cache_path) return DS_INVALID;
        /* Resolve the explicitly configured output location so a symlinked
         * cache base cannot conceal the actual directory from the exclusion. */
        char *resolved = realpath(walk->cache_path, NULL);
        if (resolved) {
            g_free(walk->cache_path);
            walk->cache_path = g_strdup(resolved);
            free(resolved);
        }
        if (cache_contains_root(walk->cache_path, walk->root.path))
            return ds_error(walk->error, DS_INVALID, EINVAL, "The scan root is inside the private cache");
        LinuxOpen cache_method = walk->root.method;
        int fd = linux_open_root(walk->cache_path, &cache_method);
        if (fd >= 0) {
            walk->cache_identity_known = linux_metadata_at(fd, "", AT_EMPTY_PATH, &walk->cache_identity) == 0;
            close(fd);
        }
        if (walk->cache_identity_known && linux_same_identity(&walk->root.metadata, &walk->cache_identity))
            return ds_error(walk->error, DS_INVALID, EINVAL, "The scan root is the private cache");
    }
    for (size_t index = 0; index < walk->options->exclude_count; ++index) {
        char *path = linux_canonical_path(&walk->options->exclude_paths[index], walk->error);
        if (!path) return DS_INVALID;
        g_hash_table_add(walk->excluded_paths, path);
    }
    return DS_OK;
}

static void visit(LinuxWalk *walk, Directory *parent, const char *name, Observation observation)
{
    if (walk->last_id == INT64_MAX) {
        walk->status = ds_error(walk->error, DS_LIMIT, EOVERFLOW, "Entry IDs exhausted");
        return;
    }
    int64_t id = ++walk->last_id;
    size_t parent_length = parent->path_length;
    bool prefetch = !observation.ready && can_prefetch(walk, parent->next_name);
    append_name(walk->path, name);

    DsReason reason = exclude_reason(walk, name);
    LinuxMetadata metadata;
    int code = 0;
    bool observed = false;
    if (walk->path->len > DS_PATH_MAX) {
        code = ENAMETOOLONG;
        reason = DS_REASON_LIMIT;
        note_issue(walk, code);
    } else if (reason == DS_REASON_NONE || reason == DS_REASON_POLICY) {
        int descriptor = dirfd(parent->stream);
        if (prefetch) worker_submit(&walk->worker, descriptor, parent->next_name);
        if (observation.ready) {
            code = observation.code;
            metadata = observation.value;
        } else {
            code = linux_metadata_at(descriptor, name, 0, &metadata);
        }
        /* No metadata job holds this descriptor across a pause checkpoint, a
         * batch callback or a stream eviction. */
        if (prefetch) parent->next_observation = worker_take(&walk->worker);
        observed = code == 0;
        if (reason == DS_REASON_NONE && !observed) {
            reason = errno_reason(code);
            note_issue(walk, code);
        }
        if (observed && !on_root_mount(walk, &metadata)) reason = DS_REASON_MOUNT;
        if (observed && metadata.placeholder && reason == DS_REASON_NONE) reason = DS_REASON_PLACEHOLDER;
        if (observed && walk->cache_identity_known && S_ISDIR(metadata.mode) &&
            linux_same_identity(&metadata, &walk->cache_identity)) reason = DS_REASON_CACHE;
    }

    DsKind kind = DS_EXCLUDED;
    if (reason == DS_REASON_NONE) {
        if (S_ISREG(metadata.mode)) {
            kind = DS_FILE;
            if (metadata.logical.quality != DS_KNOWN || metadata.allocated.quality != DS_KNOWN) {
                reason = DS_REASON_METADATA;
                note_issue(walk, 0);
            }
        } else if (S_ISLNK(metadata.mode)) {
            kind = DS_LINK;
            reason = DS_REASON_LINK;
        } else if (!S_ISDIR(metadata.mode)) {
            kind = DS_SPECIAL;
            reason = DS_REASON_SPECIAL;
        } else if (walk->stack->len > DS_DEPTH_MAX) {
            code = ENAMETOOLONG;
            reason = DS_REASON_LIMIT;
            note_issue(walk, code);
        } else {
            kind = DS_DIRECTORY;
            int child = checked_directory(walk, dirfd(parent->stream), name, &metadata, O_RDONLY, &code);
            if (child >= 0) {
                Directory frame = {.id = id, .parent = parent->id, .name = g_strdup(name),
                                   .path_length = walk->path->len, .metadata = metadata};
                code = adopt_stream(walk, &frame, child);
                if (code == 0) {
                    emit(walk, id, parent->id, name, kind, reason, 0, &metadata);
                    g_array_append_val(walk->stack, frame);
                    return;
                }
                g_free(frame.name);
            }
            reason = errno_reason(code);
            if (reason != DS_REASON_MOUNT) note_issue(walk, code);
            else kind = DS_EXCLUDED;
        }
    }
    emit(walk, id, parent->id, name, kind, reason, code, observed ? &metadata : NULL);
    g_string_truncate(walk->path, parent_length);
}

static char *read_next(Directory *directory)
{
    if (directory->finished) return NULL;
    for (;;) {
        errno = 0;
        struct dirent *record = readdir(directory->stream);
        if (!record) {
            directory->finished = true;
            directory->read_error = errno;
            return NULL;
        }
        if (strcmp(record->d_name, ".") == 0 || strcmp(record->d_name, "..") == 0) continue;
        char *name = g_strdup(record->d_name);
        errno = 0;
        long cookie = telldir(directory->stream);
        if (cookie == -1 && errno != 0) {
            directory->finished = true;
            directory->read_error = errno;
        } else {
            directory->cookie = cookie;
        }
        return name;
    }
}

static void enumerate(LinuxWalk *walk)
{
    Directory root = {.id = 1, .name = g_strdup(""), .path_length = walk->path->len,
                      .metadata = walk->root.metadata};
    walk->last_id = 1;
    if (g_hash_table_contains(walk->excluded_paths, walk->root.path)) {
        emit(walk, 1, 0, "", DS_DIRECTORY, DS_REASON_POLICY, 0, &root.metadata);
        g_free(root.name);
        return;
    }
    emit(walk, 1, 0, "", DS_DIRECTORY, DS_REASON_NONE, 0, &root.metadata);
    g_array_append_val(walk->stack, root);
    while (walk->stack->len && checkpoint(walk)) {
        size_t depth = walk->stack->len - 1;
        Directory *directory = frame_at(walk, depth);
        if (directory->finished && !directory->next_name) {
            if (directory->read_error) directory_issue(walk, directory, directory->read_error);
            pop_directory(walk);
            continue;
        }
        if (!directory->stream) {
            int code = reopen_stream(walk, depth);
            if (walk->status != DS_OK) break;
            if (code != 0) {
                directory_issue(walk, directory, code);
                pop_directory(walk);
                continue;
            }
        }

        if (!directory->next_name) directory->next_name = read_next(directory);
        g_autofree char *name = g_steal_pointer(&directory->next_name);
        if (!name) continue;
        Observation observation = directory->next_observation;
        directory->next_observation.ready = false;
        directory->next_name = read_next(directory);
        if (checkpoint(walk)) visit(walk, directory, name, observation);
    }
}

static unsigned stream_limit(void)
{
    struct rlimit limit;
    unsigned streams = DS_DIR_HANDLES - 4;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < DS_DIR_HANDLES + 4)
        streams = limit.rlim_cur <= 8 ? 1 : (unsigned)limit.rlim_cur - 8;
    return streams;
}

DsStatus ds_walk(const DsScanOptions *options, DsControl *control, DsEntrySink sink, void *context,
                 DsError *error)
{
    if (!options || !sink || (options->exclude_count && !options->exclude_paths))
        return ds_error(error, DS_INVALID, EINVAL, "Incomplete scan options or entry sink");
    if (!ds_control_checkpoint(control)) return ds_error(error, DS_CANCELED, 0, "Scan canceled");

    LinuxWalk walk = {.options = options, .control = control, .sink = sink, .sink_data = context,
                      .error = error, .stream_limit = stream_limit()};
    walk.status = linux_root_open(&options->root, &walk.root, error);
    if (walk.status == DS_OK && checkpoint(&walk)) {
        walk.trust_identity = linux_mount_trusted_identity(walk.root.mount);
        walk.path = g_string_new(walk.root.path);
        walk.excluded_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        walk.status = configure_exclusions(&walk);
        if (walk.status == DS_OK && options->profile == DS_STANDARD && options->jobs >= 2 &&
            !worker_start(&walk.worker, error)) walk.status = DS_LIMIT;
        if (walk.status == DS_OK && checkpoint(&walk)) {
            walk.stack = g_array_new(FALSE, FALSE, sizeof(Directory));
            walk.entries = g_new(DsEntry, DS_BATCH_MAX);
            enumerate(&walk);
            flush(&walk);
            while (walk.stack->len) pop_directory(&walk);
            g_array_unref(walk.stack);
            g_free(walk.entries);
        }
        worker_stop(&walk.worker);
        g_string_free(walk.path, TRUE);
        g_hash_table_unref(walk.excluded_paths);
        g_free(walk.cache_path);
    }
    linux_root_close(&walk.root);
    if (walk.status == DS_OK && walk.partial)
        return ds_error(error, DS_PARTIAL, walk.first_issue, "Some entries could not be fully observed");
    if (walk.status == DS_CANCELED) return ds_error(error, DS_CANCELED, 0, "Scan canceled");
    return walk.status;
}

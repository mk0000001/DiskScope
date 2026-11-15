#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "diskscope/ds_retention.h"
#include "diskscope/ds_store.h"
#include "cache_lease.h"
#include "cache_owner.h"
#include "local.h"
#ifdef G_OS_WIN32
#define MAX_PATH_PINS 256u

#include <wchar.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CACHE_COUNT 10u
#define CACHE_BYTES (UINT64_C(8) * 1024 * 1024 * 1024)
#define CACHE_AGE_USEC (INT64_C(30) * 24 * 60 * 60 * G_USEC_PER_SEC)
#define MAX_CANDIDATES 4096u

enum { OWNER, LEASE, DATABASE, WAL, SHM, JOURNAL, DIAGNOSTICS, IMPORT, FILE_COUNT };
static const char *const file_names[] = {
    DS_CACHE_OWNER_FILE, DS_CACHE_LEASE_FILE, DS_CACHE_SCAN_DB, DS_CACHE_SCAN_DB "-wal",
    DS_CACHE_SCAN_DB "-shm", DS_CACHE_SCAN_DB "-journal", "diagnostics.txt", DS_CACHE_IMPORT_DB
};

typedef DsCacheFile FileHandle;
#define BAD_FILE DS_CACHE_BAD_FILE

typedef struct {
    const char *path;
    FileHandle handle;
} Directory;

typedef struct {
    const Directory *directory;
    bool deleting;
    unsigned present;
    uint64_t bytes;
    int64_t created;
    bool scan;
    DsCacheIdentity identity;
    FileHandle handles[FILE_COUNT];
} Contents;

typedef bool (*NameVisitor)(const char *, void *);

static bool cache_name(const char *name)
{
    return g_str_has_prefix(name, "scan-") && g_uuid_string_is_valid(name + 5);
}

#ifdef G_OS_WIN32
static WCHAR *wide_path(const char *path)
{
    return (WCHAR *)g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
}

static void close_file(FileHandle file)
{
    CloseHandle(file);
}

static FileHandle open_directory(const char *path, bool deleting)
{
    g_autofree WCHAR *wide = wide_path(path);
    FileHandle file = wide ? CreateFileW(wide, FILE_READ_ATTRIBUTES | (deleting ? DELETE : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL) : BAD_FILE;
    BY_HANDLE_FILE_INFORMATION info;
    if (file != BAD_FILE && (!GetFileInformationByHandle(file, &info) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
        close_file(file);
        return BAD_FILE;
    }
    return file;
}

static void unpin_directory(gpointer slot)
{
    close_file(*(FileHandle *)slot);
}

static FileHandle open_base(const char *path, GArray *pins)
{
    /* Only local DOS drive paths are accepted. Pin the entire path so no
     * component can be swapped for a junction during a path-based operation. */
    if (!g_str_has_prefix(path, "\\\\?\\") || !g_ascii_isalpha(path[4]) ||
        path[5] != ':' || path[6] != '\\')
        return BAD_FILE;
    const char *part = path + 7;
    g_autofree char *root = g_strndup(path, part - path);
    FileHandle directory = open_directory(root, false);
    if (directory == BAD_FILE)
        return BAD_FILE;
    g_array_append_val(pins, directory);
    while (*part) {
        if (pins->len >= MAX_PATH_PINS)
            return BAD_FILE;
        const char *separator = strchr(part, '\\');
        const char *end = separator ? separator : part + strlen(part);
        if (end == part)
            return BAD_FILE;
        g_autofree char *prefix = g_strndup(path, end - path);
        directory = open_directory(prefix, false);
        if (directory == BAD_FILE)
            return BAD_FILE;
        g_array_append_val(pins, directory);
        part = separator ? separator + 1 : end;
    }
    return directory;
}

static bool visit_names(const Directory *directory, NameVisitor visitor, void *user_data)
{
    g_autofree char *pattern = g_build_filename(directory->path, "*", NULL);
    g_autofree WCHAR *wide = wide_path(pattern);
    WIN32_FIND_DATAW item;
    HANDLE listing = wide ? FindFirstFileExW(wide, FindExInfoBasic, &item,
                                             FindExSearchNameMatch, NULL, 0) : BAD_FILE;
    if (listing == BAD_FILE)
        return wide && GetLastError() == ERROR_FILE_NOT_FOUND;
    bool valid = true;
    do {
        if (!wcscmp(item.cFileName, L".") || !wcscmp(item.cFileName, L".."))
            continue;
        g_autofree char *name = g_utf16_to_utf8((const gunichar2 *)item.cFileName, -1,
                                               NULL, NULL, NULL);
        valid = name && visitor(name, user_data);
    } while (valid && FindNextFileW(listing, &item));
    valid = valid && GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(listing);
    return valid;
}

static FileHandle open_child(const Directory *base, const char *path, const char *name, bool deleting)
{
    (void)base;
    (void)name;
    return open_directory(path, deleting);
}

static FileHandle open_entry(const Contents *contents, unsigned index, uint64_t *size,
                              int64_t *created)
{
    g_autofree char *path = g_build_filename(contents->directory->path, file_names[index], NULL);
    g_autofree WCHAR *wide = wide_path(path);
    DWORD access = index == OWNER ? GENERIC_READ : FILE_READ_ATTRIBUTES;
    FileHandle file = wide ? CreateFileW(wide, access | (contents->deleting ? DELETE : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, NULL) : BAD_FILE;
    if (file == BAD_FILE)
        return BAD_FILE;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                  FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_READONLY)) ||
        info.nNumberOfLinks != 1) {
        close_file(file);
        return BAD_FILE;
    }
    *size = (uint64_t)info.nFileSizeHigh << 32 | info.nFileSizeLow;
    uint64_t ticks = (uint64_t)info.ftLastWriteTime.dwHighDateTime << 32 | info.ftLastWriteTime.dwLowDateTime;
    *created = (int64_t)(ticks / 10) - INT64_C(11644473600000000);
    return file;
}

static bool read_marker(FileHandle file, char *text, size_t size)
{
    DWORD count = 0;
    return ReadFile(file, text, (DWORD)size, &count, NULL) && count == size;
}

static bool close_and_delete(FileHandle file)
{
    FILE_DISPOSITION_INFO disposition = {.DeleteFile = TRUE};
    bool deleted = SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof disposition);
    CloseHandle(file);
    return deleted;
}
#else
static void close_file(FileHandle file)
{
    close(file);
}

static FileHandle open_base(const char *path)
{
    FileHandle directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (!g_path_is_absolute(path)) {
        if (directory != BAD_FILE) close_file(directory);
        return BAD_FILE;
    }
    g_auto(GStrv) parts = g_strsplit(path + 1, "/", -1);
    for (char **part = parts; directory != BAD_FILE && *part; ++part) {
        if (!**part)
            continue;
        FileHandle next = openat(directory, *part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close_file(directory);
        directory = next;
    }
    return directory;
}

static bool visit_names(const Directory *directory, NameVisitor visitor, void *user_data)
{
    FileHandle file = openat(directory->handle, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (file == BAD_FILE)
        return false;
    DIR *listing = fdopendir(file);
    if (!listing) {
        close_file(file);
        return false;
    }
    bool valid = true;
    while (valid) {
        errno = 0;
        const struct dirent *item = readdir(listing);
        if (!item) {
            valid = errno == 0;
            break;
        }
        if (strcmp(item->d_name, ".") && strcmp(item->d_name, ".."))
            valid = visitor(item->d_name, user_data);
    }
    closedir(listing);
    return valid;
}

static bool same_mount(FileHandle parent, FileHandle child)
{
    struct stat a, b;
    if (fstat(parent, &a) != 0 || fstat(child, &b) != 0 || a.st_dev != b.st_dev)
        return false;
#ifdef STATX_MNT_ID
    struct statx parent_mount, child_mount;
    if (statx(parent, "", AT_EMPTY_PATH, STATX_MNT_ID, &parent_mount) == 0 &&
        statx(child, "", AT_EMPTY_PATH, STATX_MNT_ID, &child_mount) == 0 &&
        (parent_mount.stx_mask & child_mount.stx_mask & STATX_MNT_ID))
        return parent_mount.stx_mnt_id == child_mount.stx_mnt_id;
#endif
    return true;
}

static FileHandle open_child(const Directory *base, const char *path, const char *name, bool deleting)
{
    (void)path;
    (void)deleting;
    FileHandle file = openat(base->handle, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (file != BAD_FILE && !same_mount(base->handle, file)) {
        close_file(file);
        return BAD_FILE;
    }
    return file;
}

static FileHandle open_entry(const Contents *contents, unsigned index, uint64_t *size,
                              int64_t *created)
{
    int flags = O_NOFOLLOW | O_CLOEXEC | (index == OWNER ? O_RDONLY | O_NONBLOCK : O_PATH);
    FileHandle file = openat(contents->directory->handle, file_names[index], flags);
    if (file == BAD_FILE)
        return BAD_FILE;
    struct stat info;
    if (fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        info.st_nlink != 1 || info.st_size < 0 || !same_mount(contents->directory->handle, file)) {
        close_file(file);
        return BAD_FILE;
    }
    *size = (uint64_t)info.st_size;
    *created = (int64_t)info.st_mtim.tv_sec * G_USEC_PER_SEC + info.st_mtim.tv_nsec / 1000;
    return file;
}

static bool read_marker(FileHandle file, char *text, size_t size)
{
    return read(file, text, size) == (ssize_t)size;
}

static bool same_entry(FileHandle dir, const char *name, FileHandle file)
{
    struct stat link, handle;
    return fstatat(dir, name, &link, AT_SYMLINK_NOFOLLOW) == 0 && fstat(file, &handle) == 0 &&
           link.st_dev == handle.st_dev && link.st_ino == handle.st_ino &&
           (S_ISDIR(link.st_mode) || link.st_nlink == 1);
}
#endif

static bool inspect_entry(const char *name, void *user_data)
{
    Contents *contents = user_data;
    unsigned index = 0;
    while (index < FILE_COUNT && strcmp(name, file_names[index]))
        ++index;
    if (index == FILE_COUNT || (contents->present & (1u << index)))
        return false;
    contents->present |= 1u << index;
    if (index == LEASE || index == OWNER)
        return true;  /* Already opened and validated separately. */
    uint64_t bytes;
    int64_t created;
    FileHandle file = open_entry(contents, index, &bytes, &created);
    if (file == BAD_FILE)
        return false;
    contents->handles[index] = file;
    return ds_u64_add(contents->bytes, bytes, &contents->bytes);
}

static bool inspect(const Directory *directory, Contents *contents, bool deleting)
{
    *contents = (Contents){.directory = directory, .deleting = deleting};
    for (unsigned i = 0; i < FILE_COUNT; ++i)
        contents->handles[i] = BAD_FILE;
    char marker[DS_CACHE_OWNER_SIZE];
    const char signature[] = "DiskScope cache v1 ";
    FileHandle owner = open_entry(contents, OWNER, &contents->bytes, &contents->created);
    contents->handles[OWNER] = owner;
    size_t size = (size_t)contents->bytes;
    if (owner == BAD_FILE || contents->created <= 0 || contents->bytes >= sizeof marker ||
        size < sizeof signature || !read_marker(owner, marker, size) ||
        memcmp(marker, signature, sizeof signature - 1) ||
        !visit_names(directory, inspect_entry, contents))
        return false;
    contents->scan = (contents->present & (1u << DATABASE)) != 0;
    unsigned primary = contents->scan ? DATABASE : IMPORT;
    unsigned required = (1u << OWNER) | (1u << LEASE) | (1u << primary);
    unsigned allowed = contents->scan ? (1u << IMPORT) - 1u : required;
    if ((contents->present & required) != required || (contents->present & ~allowed) ||
        !ds_cache_file_identity(contents->handles[primary], &contents->identity))
        return false;
    char expected[DS_CACHE_OWNER_SIZE];
    return ds_cache_owner_record(expected, &contents->identity, contents->scan) == size &&
           !memcmp(marker, expected, size);
}

static void close_contents(Contents *contents)
{
    for (unsigned i = 0; i < FILE_COUNT; ++i) {
        if (contents->handles[i] != BAD_FILE)
            close_file(contents->handles[i]);
    }
}

static DsCacheLease *open_lease(const Directory *directory, bool deleting)
{
#ifdef G_OS_WIN32
    return deleting ? ds_cache_lease_open_delete(directory->path) :
                      ds_cache_lease_open(directory->path, false, true, NULL);
#else
    (void)deleting;
    return ds_cache_lease_open_at(directory->handle, false, true, NULL);
#endif
}

static int64_t recoverable_start(const Directory *directory)
{
    g_autofree char *name = g_build_filename(directory->path, file_names[DATABASE], NULL);
    DsNativePath path = {0};
    DsStore *store = NULL;
    DsStoreInfo info = {0};
    bool recoverable = ds_path_from_arg(name, &path, NULL) == DS_OK &&
        ds_store_open(&path, DS_STORE_READER, &store, NULL) == DS_OK &&
        ds_store_info(store, &info, NULL) == DS_OK && info.root.raw && g_bytes_get_size(info.root.raw) &&
        info.entry_count > 0 && info.commit_seq > 0 && info.started_at > 0 &&
        info.run_status >= DS_RUN_RUNNING && info.run_status <= DS_PROTOCOL &&
        (info.coverage == DS_COVERAGE_PARTIAL ||
         (info.coverage == DS_COVERAGE_COMPLETE && info.run_status == DS_OK));
    int64_t start = recoverable ? info.started_at : 0;
    ds_store_info_clear(&info);
    ds_store_close(store);
    ds_path_clear(&path);
    return start;
}

typedef struct {
    char *name;
    uint64_t bytes;
    int64_t created;
    DsCacheIdentity identity;
} Candidate;

typedef struct {
    Directory base;
    DsControl *control;
    GArray *candidates;
    uint64_t bytes;
    const char *latest_name;
    DsCacheLease *latest_lease;
    int64_t latest_start;
} Collection;

static bool newer_recovery(const Collection *collection, int64_t started,
                           DsCacheLease *lease, const char *name)
{
    if (started <= 0)
        return false;
    if (started != collection->latest_start)
        return started > collection->latest_start;
    int64_t created = ds_cache_lease_created(lease);
    int64_t latest_created = ds_cache_lease_created(collection->latest_lease);
    return created == latest_created ? strcmp(name, collection->latest_name) > 0 : created > latest_created;
}

static bool collect_candidate(const char *name, void *user_data)
{
    Collection *collection = user_data;
    if (ds_control_canceled(collection->control))
        return false;
    if (!cache_name(name))
        return true;
    g_autofree char *path = g_build_filename(collection->base.path, name, NULL);
    Directory directory = {path, open_child(&collection->base, path, name, false)};
    if (directory.handle == BAD_FILE)
        return true;
    DsCacheLease *lease = open_lease(&directory, false);
    bool valid = true;
    if (lease) {
        Contents files;
        bool owned = inspect(&directory, &files, false);
        close_contents(&files);
        valid = !owned || collection->candidates->len < MAX_CANDIDATES;
        if (owned && valid) {
            Candidate candidate = {g_strdup(name), files.bytes, files.created, files.identity};
            g_array_append_val(collection->candidates, candidate);
            if (!ds_u64_add(collection->bytes, candidate.bytes, &collection->bytes))
                collection->bytes = UINT64_MAX;
            int64_t start = files.scan ? recoverable_start(&directory) : 0;
            if (newer_recovery(collection, start, lease, name)) {
                ds_cache_lease_close(collection->latest_lease);
                collection->latest_name = candidate.name;
                collection->latest_lease = lease;
                collection->latest_start = start;
                lease = NULL;
            }
        }
        ds_cache_lease_close(lease);
    }
    close_file(directory.handle);
    return valid;
}

static bool delete_entries(Contents *files, DsCacheLease *lease)
{
    for (unsigned i = DATABASE; i < FILE_COUNT; ++i) {
        FileHandle file = files->handles[i];
        if (file == BAD_FILE)
            continue;
#ifdef G_OS_WIN32
        files->handles[i] = BAD_FILE;
        if (!close_and_delete(file))
            return false;
#else
        FileHandle dir = files->directory->handle;
        if (!same_entry(dir, file_names[i], file) || unlinkat(dir, file_names[i], 0) != 0)
            return false;
#endif
    }
#ifdef G_OS_WIN32
    FileHandle owner = files->handles[OWNER];
    files->handles[OWNER] = BAD_FILE;
    if (ds_cache_lease_delete(lease))
        return close_and_delete(owner);
    close_file(owner);
    return false;
#else
    FileHandle dir = files->directory->handle;
    return same_entry(dir, DS_CACHE_OWNER_FILE, files->handles[OWNER]) &&
           ds_cache_lease_delete_at(lease, dir) && unlinkat(dir, DS_CACHE_OWNER_FILE, 0) == 0;
#endif
}

static bool delete_candidate(const Directory *base, const Candidate *candidate)
{
    g_autofree char *path = g_build_filename(base->path, candidate->name, NULL);
    Directory directory = {path, open_child(base, path, candidate->name, true)};
    if (directory.handle == BAD_FILE)
        return false;
    DsCacheLease *lease = open_lease(&directory, true);
    bool removed = false;
    if (lease) {
        Contents files;
        bool valid = inspect(&directory, &files, true) && files.created == candidate->created &&
                     !memcmp(&files.identity, &candidate->identity, sizeof files.identity);
#ifndef G_OS_WIN32
        valid = valid && same_entry(base->handle, candidate->name, directory.handle);
#endif
        if (valid)
            removed = delete_entries(&files, lease);
        close_contents(&files);
        ds_cache_lease_close(lease);
    }
#ifdef G_OS_WIN32
    if (removed)
        return close_and_delete(directory.handle);
#else
    if (removed)
        removed = same_entry(base->handle, candidate->name, directory.handle) &&
                  unlinkat(base->handle, candidate->name, AT_REMOVEDIR) == 0;
#endif
    close_file(directory.handle);
    return removed;
}

static void clear_candidate(gpointer data)
{
    Candidate *candidate = data;
    g_free(candidate->name);
}

static int oldest_first(gconstpointer first, gconstpointer second)
{
    const Candidate *a = first, *b = second;
    if (a->created != b->created)
        return a->created < b->created ? -1 : 1;
    return strcmp(a->name, b->name);
}

void ds_cache_prune(const DsNativePath *cache, DsControl *control)
{
    if (ds_control_canceled(control))
        return;
    g_autofree char *path = ds_cache_base(cache, NULL);
    if (!path)
        return;
#ifdef G_OS_WIN32
    g_autoptr(GArray) pins = g_array_new(FALSE, FALSE, sizeof(FileHandle));
    g_array_set_clear_func(pins, unpin_directory);
    FileHandle base = open_base(path, pins);
#else
    FileHandle base = open_base(path);
#endif
    if (base == BAD_FILE)
        return;
    g_autoptr(GArray) candidates = g_array_new(FALSE, FALSE, sizeof(Candidate));
    g_array_set_clear_func(candidates, clear_candidate);
    Collection collection = {.base = {path, base}, .control = control, .candidates = candidates};
    if (ds_cache_is_local(path) && visit_names(&collection.base, collect_candidate, &collection)) {
        g_array_sort(candidates, oldest_first);
        unsigned count = candidates->len;
        int64_t cutoff = g_get_real_time() - CACHE_AGE_USEC;
        for (guint i = 0; i < candidates->len && !ds_control_canceled(control); ++i) {
            const Candidate *candidate = &g_array_index(candidates, Candidate, i);
            if (count <= CACHE_COUNT && collection.bytes <= CACHE_BYTES && candidate->created >= cutoff)
                break;
            if (candidate->name != collection.latest_name && delete_candidate(&collection.base, candidate)) {
                --count;
                collection.bytes -= candidate->bytes;
            }
        }
    }
    ds_cache_lease_close(collection.latest_lease);
#ifndef G_OS_WIN32
    close_file(base);
#endif
}

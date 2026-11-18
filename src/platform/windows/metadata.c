#include "win_scan.h"

#include <stddef.h>
#include <string.h>
#include <winternl.h>

typedef NTSTATUS (NTAPI *QueryInformationFile)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                               FILE_INFORMATION_CLASS);
typedef NTSTATUS (NTAPI *OpenRelativeFile)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                           PIO_STATUS_BLOCK, ULONG, ULONG);

static INIT_ONCE metadata_init = INIT_ONCE_STATIC_INIT;
static QueryInformationFile query_information_file;
static OpenRelativeFile open_relative_file;

static BOOL CALLBACK resolve_native_metadata(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once;
    (void)parameter;
    (void)context;
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    if (!module) return TRUE;
    FARPROC symbol = GetProcAddress(module, "NtQueryInformationFile");
    _Static_assert(sizeof(query_information_file) == sizeof(symbol), "Windows function pointer size");
    memcpy(&query_information_file, &symbol, sizeof(query_information_file));
    symbol = GetProcAddress(module, "NtOpenFile");
    _Static_assert(sizeof(open_relative_file) == sizeof(symbol), "Windows function pointer size");
    memcpy(&open_relative_file, &symbol, sizeof(open_relative_file));
    return TRUE;
}

static bool query_file_all(HANDLE handle, FILE_BASIC_INFO *basic, FILE_STANDARD_INFO *standard,
                           uint64_t *file_id)
{
    /* FileAllInformation includes a variable length path after the fixed fields.
     * The separate queries handle paths that do not fit and unsupported results. */
    struct {
        FILE_ALL_INFORMATION info;
        WCHAR extra_name[512];
    } response;
    const size_t name_offset = offsetof(FILE_ALL_INFORMATION, NameInformation) +
                               offsetof(FILE_NAME_INFORMATION, FileName);
    IO_STATUS_BLOCK io = {0};
    if (query_information_file(handle, &io, &response, sizeof(response), FileAllInformation) != 0 ||
        io.Information < name_offset || io.Information > sizeof(response) ||
        response.info.NameInformation.FileNameLength > io.Information - name_offset ||
        response.info.NameInformation.FileNameLength % sizeof(WCHAR))
        return false;

    _Static_assert(sizeof(*basic) == sizeof(response.info.BasicInformation), "FileBasicInformation size");
    _Static_assert(sizeof(*standard) == sizeof(response.info.StandardInformation), "FileStandardInformation size");
    memcpy(basic, &response.info.BasicInformation, sizeof(*basic));
    memcpy(standard, &response.info.StandardInformation, sizeof(*standard));
    if (file_id) *file_id = (uint64_t)response.info.InternalInformation.IndexNumber.QuadPart;
    return true;
}

static bool query_basic_standard(HANDLE handle, FILE_BASIC_INFO *basic, FILE_STANDARD_INFO *standard)
{
    return InitOnceExecuteOnce(&metadata_init, resolve_native_metadata, NULL, NULL) &&
           query_information_file && query_file_all(handle, basic, standard, NULL);
}

const DsWinMetadataOps ds_win_metadata_ops = {
    .open = CreateFileW, .query = GetFileInformationByHandleEx, .close = CloseHandle,
    .query_basic_standard = query_basic_standard,
};

DsReason ds_win_classify(DWORD attributes, DWORD tag)
{
    /* RECALL_ON_OPEN uses the EA bit value outside directory enumeration. Treat
     * ambiguity conservatively, including attributes obtained from a handle. */
    const DWORD offline = FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                          FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if (attributes & offline) return DS_REASON_PLACEHOLDER;
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return DS_REASON_NONE;

    /* CLOUD/CLOUD_1..F, plus the legacy placeholder and storage-sync tags. */
    if ((tag & ~(DWORD)IO_REPARSE_TAG_CLOUD_MASK) == (DWORD)IO_REPARSE_TAG_CLOUD ||
        tag == (DWORD)IO_REPARSE_TAG_FILE_PLACEHOLDER || tag == (DWORD)IO_REPARSE_TAG_ONEDRIVE ||
        tag == (DWORD)IO_REPARSE_TAG_STORAGE_SYNC)
        return DS_REASON_PLACEHOLDER;
    return DS_REASON_LINK;
}

DsReason ds_win_error_reason(DWORD code, DsReason otherwise)
{
    switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return DS_REASON_PERMISSION;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_DELETE_PENDING:
    case ERROR_REPARSE_TAG_MISMATCH:
        return DS_REASON_CHANGED;
    case ERROR_FILENAME_EXCED_RANGE:
        return DS_REASON_LIMIT;
    default:
        return otherwise;
    }
}

static HANDLE open_metadata_handle(const DsWinMetadataOps *ops, const WCHAR *path, bool directory)
{
    DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL;
    if (directory) flags |= FILE_FLAG_BACKUP_SEMANTICS;
    return ops->open(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     NULL, OPEN_EXISTING, flags, NULL);
}

static void query_identity(const DsWinMetadataOps *ops, HANDLE handle, bool need_legacy, DsWinNode *node)
{
    node->id_known = ops->query(handle, FileIdInfo, &node->id, sizeof(node->id));
    if (!node->id_known) node->id_error = GetLastError();
    if (need_legacy || !node->id_known)
        node->legacy_known = GetFileInformationByHandle(handle, &node->legacy);
}

static DWORD query_node(const DsWinMetadataOps *ops, HANDLE handle, bool need_legacy, DsWinNode *node)
{
    FILE_ATTRIBUTE_TAG_INFO attrs;
    if (!ops->query(handle, FileAttributeTagInfo, &attrs, sizeof(attrs))) return GetLastError();
    node->attributes = attrs.FileAttributes;
    node->tag = attrs.ReparseTag;
    if (ds_win_classify(node->attributes, node->tag) == DS_REASON_NONE)
        query_identity(ops, handle, need_legacy, node);
    return ERROR_SUCCESS;
}

DWORD ds_win_open_node(const DsWinMetadataOps *ops, const WCHAR *path, bool directory,
                       HANDLE *handle, DsWinNode *node)
{
    *node = (DsWinNode){0};
    *handle = open_metadata_handle(ops, path, directory);
    if (*handle == INVALID_HANDLE_VALUE) return GetLastError();
    const DWORD error = query_node(ops, *handle, true, node);
    if (error) {
        ops->close(*handle);
        *handle = INVALID_HANDLE_VALUE;
    }
    return error;
}

bool ds_win_same_volume(const DsWinNode *a, const DsWinNode *b)
{
    if (a->id_known && b->id_known) return a->id.VolumeSerialNumber == b->id.VolumeSerialNumber;
    return a->legacy_known && b->legacy_known &&
           a->legacy.dwVolumeSerialNumber == b->legacy.dwVolumeSerialNumber;
}

bool ds_win_same_node(const DsWinNode *a, const DsWinNode *b)
{
    if (!ds_win_same_volume(a, b)) return false;
    if (a->id_known && b->id_known)
        return memcmp(a->id.FileId.Identifier, b->id.FileId.Identifier, 16) == 0;

    /* Legacy IDs are a directory race check. They are never used to deduplicate
     * files or to override a mismatch between FILE_ID_128 identifiers. */
    return a->legacy_known && b->legacy_known &&
           (a->legacy.nFileIndexHigh || a->legacy.nFileIndexLow) &&
           a->legacy.nFileIndexHigh == b->legacy.nFileIndexHigh &&
           a->legacy.nFileIndexLow == b->legacy.nFileIndexLow;
}

static uint64_t filetime_ticks(FILETIME time)
{
    return (uint64_t)time.dwHighDateTime << 32 | time.dwLowDateTime;
}

static void set_mtime(DsEntry *entry, uint64_t ticks)
{
    const uint64_t epoch = UINT64_C(116444736000000000);
    const uint64_t rate = UINT64_C(10000000);
    entry->mtime_known = ticks != 0;
    entry->mtime = ticks >= epoch ? (int64_t)((ticks - epoch) / rate) :
                   -(int64_t)((epoch - ticks + rate - 1) / rate);
}

void ds_win_find_metadata(const WIN32_FIND_DATAW *found, DsEntry *entry)
{
    const DWORD attrs = found->dwFileAttributes;
    entry->reason = ds_win_classify(attrs, found->dwReserved0);
    if (entry->reason == DS_REASON_LINK) entry->kind = DS_LINK;
    else if (entry->reason == DS_REASON_PLACEHOLDER) entry->kind = DS_EXCLUDED;
    else entry->kind = attrs & FILE_ATTRIBUTE_DIRECTORY ? DS_DIRECTORY : DS_FILE;
    entry->allocated = (DsSize){0, DS_UNAVAILABLE};
    entry->logical = (DsSize){0, DS_UNAVAILABLE};
    if (entry->kind == DS_FILE)
        entry->logical = (DsSize){(uint64_t)found->nFileSizeHigh << 32 | found->nFileSizeLow, DS_KNOWN};
    set_mtime(entry, filetime_ticks(found->ftLastWriteTime));
}

static DsQuality metadata_quality(DWORD code)
{
    switch (code) {
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_FUNCTION:
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        return DS_UNSUPPORTED;
    default:
        return DS_UNAVAILABLE;
    }
}

static void issue(DsEntry *entry, DWORD code, DsReason otherwise)
{
    entry->os_code = code;
    entry->reason = ds_win_error_reason(code, otherwise);
    entry->allocated = (DsSize){0, metadata_quality(code)};
    if (entry->reason == DS_REASON_CHANGED) {
        entry->logical = (DsSize){0, DS_CHANGED};
        entry->allocated.quality = DS_CHANGED;
    }
}

static void query_allocation(const DsWinMetadataOps *ops, HANDLE handle,
                             const FILE_STANDARD_INFO *standard, const DsWinNode *node,
                             DsEntry *entry)
{
    LONGLONG allocated = standard->AllocationSize.QuadPart;
    if (node->attributes & (FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED)) {
        FILE_COMPRESSION_INFO compression;
        if (!ops->query(handle, FileCompressionInfo, &compression, sizeof(compression))) {
            issue(entry, GetLastError(), DS_REASON_METADATA);
            return;
        }
        allocated = compression.CompressedFileSize.QuadPart;
    }
    if (allocated < 0) {
        issue(entry, ERROR_INVALID_DATA, DS_REASON_METADATA);
        return;
    }
    entry->allocated = (DsSize){(uint64_t)allocated, DS_KNOWN};
}

static bool ordinary_file_attributes(DWORD attributes)
{
    const DWORD ordinary = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                           FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY |
                           FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    return attributes && !(attributes & ~ordinary);
}

static bool metadata_changed(const WIN32_FIND_DATAW *found, const DsEntry *entry,
                             const FILE_BASIC_INFO *basic, const FILE_STANDARD_INFO *standard)
{
    return standard->DeletePending || standard->Directory || standard->EndOfFile.QuadPart < 0 ||
           entry->logical.bytes != (uint64_t)standard->EndOfFile.QuadPart ||
           filetime_ticks(found->ftLastWriteTime) != (uint64_t)basic->LastWriteTime.QuadPart;
}

typedef struct {
    FILE_BASIC_INFO basic;
    FILE_STANDARD_INFO standard;
} FileMetadata;

static void measure_open_file(const DsWinMetadataOps *ops, HANDLE handle, const DsWinNode *root,
                              const WIN32_FIND_DATAW *found, DsEntry *entry,
                              const FileMetadata *combined)
{
    DsWinNode node = {0};
    const bool need_legacy = root && !root->id_known;
    DWORD error = ERROR_SUCCESS;
    if (combined) {
        node.attributes = combined->basic.FileAttributes;
        query_identity(ops, handle, need_legacy, &node);
    } else {
        error = query_node(ops, handle, need_legacy, &node);
    }
    if (error) {
        issue(entry, error, DS_REASON_METADATA);
        return;
    }

    const DsReason classification = ds_win_classify(node.attributes, node.tag);
    const DWORD type_attributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_COMPRESSED |
                                  FILE_ATTRIBUTE_SPARSE_FILE;
    if (classification != DS_REASON_NONE ||
        ((node.attributes ^ found->dwFileAttributes) & type_attributes)) {
        if (classification != DS_REASON_NONE)
            entry->kind = classification == DS_REASON_LINK ? DS_LINK : DS_EXCLUDED;
        issue(entry, ERROR_REPARSE_TAG_MISMATCH, DS_REASON_CHANGED);
        return;
    }
    if (root && !ds_win_same_volume(root, &node)) {
        entry->kind = DS_EXCLUDED;
        issue(entry, ERROR_NOT_SAME_DEVICE, DS_REASON_MOUNT);
        entry->logical = (DsSize){0, DS_UNAVAILABLE};
        return;
    }

    FileMetadata queried;
    if (!combined) {
        if (!ops->query(handle, FileStandardInfo, &queried.standard, sizeof(queried.standard)) ||
            !ops->query(handle, FileBasicInfo, &queried.basic, sizeof(queried.basic))) {
            issue(entry, GetLastError(), DS_REASON_METADATA);
            return;
        }
        combined = &queried;
    }
    if (metadata_changed(found, entry, &combined->basic, &combined->standard)) {
        issue(entry, ERROR_FILE_INVALID, DS_REASON_CHANGED);
        return;
    }

    entry->links = combined->standard.NumberOfLinks;
    entry->identity_known = node.id_known;
    if (node.id_known) {
        ds_u64_encode(node.id.VolumeSerialNumber, entry->identity);
        memcpy(entry->identity + 8, node.id.FileId.Identifier, 16);
    } else {
        entry->os_code = node.id_error;
        entry->reason = DS_REASON_METADATA;
    }
    query_allocation(ops, handle, &combined->standard, &node, entry);
}

bool ds_win_prepare_ntfs_metadata(void)
{
    return InitOnceExecuteOnce(&metadata_init, resolve_native_metadata, NULL, NULL) &&
           query_information_file && open_relative_file;
}

bool ds_win_measure_ntfs_file(HANDLE directory, const DsWinNode *root,
                              const WIN32_FIND_DATAW *found, DsEntry *entry)
{
    if (!ordinary_file_attributes(found->dwFileAttributes))
        return false;

    /* Open exactly one native component. An absolute name, stream suffix or
     * parent component would invalidate the verified directory's volume. */
    const WCHAR *leaf = found->cFileName;
    size_t units = 0;
    for (; units < G_N_ELEMENTS(found->cFileName) && leaf[units]; ++units)
        if (leaf[units] == L'\\' || leaf[units] == L'/' || leaf[units] == L':') return false;
    if (!units || units == G_N_ELEMENTS(found->cFileName) ||
        (leaf[0] == L'.' && (units == 1 || (units == 2 && leaf[1] == L'.'))))
        return false;
    const USHORT bytes = (USHORT)(units * sizeof(WCHAR));
    UNICODE_STRING name = {.Length = bytes, .MaximumLength = bytes, .Buffer = (WCHAR *)leaf};
    OBJECT_ATTRIBUTES object;
    InitializeObjectAttributes(&object, &name, 0, directory, NULL);
    HANDLE handle = INVALID_HANDLE_VALUE;
    IO_STATUS_BLOCK io = {0};
    const ULONG flags = FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE |
                        FILE_OPEN_REPARSE_POINT | FILE_OPEN_NO_RECALL;
    if (open_relative_file(&handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &object, &io,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, flags) != 0)
        return false;

    FileMetadata info;
    uint64_t file_id;
    const bool queried = query_file_all(handle, &info.basic, &info.standard, &file_id) &&
                         ordinary_file_attributes(info.basic.FileAttributes);
    const bool changed = queried && metadata_changed(found, entry, &info.basic, &info.standard);
    if (!queried || (!changed && (info.standard.NumberOfLinks != 1 ||
                                  info.standard.AllocationSize.QuadPart < 0 ||
                                  !file_id || file_id == UINT64_MAX))) {
        /* An incomplete combined query or a full-ID check can use the open
         * object. Resolving its path again could pick up a replacement. */
        measure_open_file(&ds_win_metadata_ops, handle, root, found, entry, queried ? &info : NULL);
        CloseHandle(handle);
        return true;
    }
    CloseHandle(handle);
    if (changed) {
        issue(entry, ERROR_FILE_INVALID, DS_REASON_CHANGED);
        return true;
    }

    /* MS-FSA Appendix A, notes 48 and 49: NTFS FileId64 is the low half
     * of FileId128, and the high half is zero. The parent supplies its volume. */
    file_id = GUINT64_TO_LE(file_id);
    ds_u64_encode(root->id.VolumeSerialNumber, entry->identity);
    memcpy(entry->identity + 8, &file_id, sizeof(file_id));
    memset(entry->identity + 16, 0, 8);
    entry->identity_known = true;
    entry->links = info.standard.NumberOfLinks;
    entry->allocated = (DsSize){(uint64_t)info.standard.AllocationSize.QuadPart, DS_KNOWN};
    return true;
}

void ds_win_measure_file(const DsWinMetadataOps *ops, const WCHAR *path,
                         const DsWinNode *root, const WIN32_FIND_DATAW *found,
                         DsEntry *entry)
{
    HANDLE handle = open_metadata_handle(ops, path, false);
    if (handle == INVALID_HANDLE_VALUE) {
        issue(entry, GetLastError(), DS_REASON_METADATA);
        return;
    }

    FileMetadata info;
    /* The old query path supplies the tag and handles special allocation.
     * Use it whenever either metadata observation has exceptional attributes. */
    const bool combined = ops->query_basic_standard &&
                          ordinary_file_attributes(found->dwFileAttributes) &&
                          ops->query_basic_standard(handle, &info.basic, &info.standard) &&
                          ordinary_file_attributes(info.basic.FileAttributes);
    measure_open_file(ops, handle, root, found, entry, combined ? &info : NULL);
    ops->close(handle);
}

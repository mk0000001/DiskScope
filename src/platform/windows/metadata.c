#include "win_scan.h"

#include <string.h>

const DsWinMetadataOps ds_win_metadata_ops = {CreateFileW, GetFileInformationByHandleEx, CloseHandle};

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

static DWORD open_node(const DsWinMetadataOps *ops, const WCHAR *path, bool directory,
                       bool need_legacy, HANDLE *handle, DsWinNode *node)
{
    *node = (DsWinNode){0};
    DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL;
    if (directory) flags |= FILE_FLAG_BACKUP_SEMANTICS;
    *handle = ops->open(path, FILE_READ_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, flags, NULL);
    if (*handle == INVALID_HANDLE_VALUE) return GetLastError();

    FILE_ATTRIBUTE_TAG_INFO attrs;
    if (!ops->query(*handle, FileAttributeTagInfo, &attrs, sizeof(attrs))) {
        const DWORD error = GetLastError();
        ops->close(*handle);
        *handle = INVALID_HANDLE_VALUE;
        return error;
    }
    node->attributes = attrs.FileAttributes;
    node->tag = attrs.ReparseTag;
    if (ds_win_classify(node->attributes, node->tag) != DS_REASON_NONE) return ERROR_SUCCESS;

    node->id_known = ops->query(*handle, FileIdInfo, &node->id, sizeof(node->id));
    if (!node->id_known) node->id_error = GetLastError();
    if (need_legacy || !node->id_known)
        node->legacy_known = GetFileInformationByHandle(*handle, &node->legacy);
    return ERROR_SUCCESS;
}

DWORD ds_win_open_node(const DsWinMetadataOps *ops, const WCHAR *path, bool directory,
                       HANDLE *handle, DsWinNode *node)
{
    return open_node(ops, path, directory, true, handle, node);
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

void ds_win_measure_file(const DsWinMetadataOps *ops, const WCHAR *path,
                         const DsWinNode *root, const WIN32_FIND_DATAW *found,
                         DsEntry *entry)
{
    DsWinNode node;
    HANDLE handle;
    const DWORD error = open_node(ops, path, false, root && !root->id_known, &handle, &node);
    if (error) {
        issue(entry, error, DS_REASON_METADATA);
        return;
    }

    const DsReason classification = ds_win_classify(node.attributes, node.tag);
    const DWORD type_attributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_COMPRESSED |
                                  FILE_ATTRIBUTE_SPARSE_FILE;
    FILE_STANDARD_INFO standard;
    FILE_BASIC_INFO basic;
    if (classification != DS_REASON_NONE ||
        ((node.attributes ^ found->dwFileAttributes) & type_attributes)) {
        if (classification != DS_REASON_NONE)
            entry->kind = classification == DS_REASON_LINK ? DS_LINK : DS_EXCLUDED;
        issue(entry, ERROR_REPARSE_TAG_MISMATCH, DS_REASON_CHANGED);
    } else if (root && !ds_win_same_volume(root, &node)) {
        entry->kind = DS_EXCLUDED;
        issue(entry, ERROR_NOT_SAME_DEVICE, DS_REASON_MOUNT);
        entry->logical = (DsSize){0, DS_UNAVAILABLE};
    } else if (!ops->query(handle, FileStandardInfo, &standard, sizeof(standard)) ||
               !ops->query(handle, FileBasicInfo, &basic, sizeof(basic))) {
        issue(entry, GetLastError(), DS_REASON_METADATA);
    } else if (standard.DeletePending || standard.Directory || standard.EndOfFile.QuadPart < 0 ||
               entry->logical.bytes != (uint64_t)standard.EndOfFile.QuadPart ||
               filetime_ticks(found->ftLastWriteTime) != (uint64_t)basic.LastWriteTime.QuadPart) {
        issue(entry, ERROR_FILE_INVALID, DS_REASON_CHANGED);
    } else {
        entry->links = standard.NumberOfLinks;
        entry->identity_known = node.id_known;
        if (node.id_known) {
            ds_u64_encode(node.id.VolumeSerialNumber, entry->identity);
            memcpy(entry->identity + 8, node.id.FileId.Identifier, 16);
        } else {
            entry->os_code = node.id_error;
            entry->reason = DS_REASON_METADATA;
        }
        query_allocation(ops, handle, &standard, &node, entry);
    }
    ops->close(handle);
}

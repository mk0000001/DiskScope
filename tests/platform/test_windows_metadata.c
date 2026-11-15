#include "windows_fixture.h"

#include <string.h>

static WIN32_FIND_DATAW read_find_data(const WCHAR *path)
{
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileExW(path, FindExInfoBasic, &found, FindExSearchNameMatch, NULL, 0);
    g_assert_true(find != INVALID_HANDLE_VALUE);
    g_assert_true(FindClose(find));
    return found;
}

static void measure_file(DWORD fsctl, DWORD required_attribute)
{
    WCHAR *dir = fixture_directory();
    if (!dir) return;
    WCHAR *path = fixture_path(dir, L"allocation.bin");
    HANDLE write_handle = fixture_open(path, CREATE_NEW);
    if (fsctl) {
        USHORT format = COMPRESSION_FORMAT_DEFAULT;
        DWORD received;
        const bool compressed = fsctl == FSCTL_SET_COMPRESSION;
        if (!DeviceIoControl(write_handle, fsctl, compressed ? &format : NULL,
                             compressed ? sizeof(format) : 0, NULL, 0, &received, NULL)) {
            g_assert_true(CloseHandle(write_handle));
            g_assert_true(DeleteFileW(path));
            g_assert_true(RemoveDirectoryW(dir));
            g_free(path);
            g_free(dir);
            g_test_skip("the NTFS test directory does not support the requested stream format");
            return;
        }
    }
    fixture_write(write_handle);
    LARGE_INTEGER end = { .QuadPart = 65536 };
    g_assert_true(SetFilePointerEx(write_handle, end, NULL, FILE_BEGIN));
    g_assert_true(SetEndOfFile(write_handle));
    g_assert_true(CloseHandle(write_handle));

    HANDLE read_handle = fixture_open(path, OPEN_EXISTING);
    FILE_STANDARD_INFO standard;
    FILE_ID_INFO identity;
    g_assert_true(GetFileInformationByHandleEx(read_handle, FileStandardInfo, &standard, sizeof(standard)));
    g_assert_true(GetFileInformationByHandleEx(read_handle, FileIdInfo, &identity, sizeof(identity)));
    uint64_t expected_allocated = (uint64_t)standard.AllocationSize.QuadPart;
    if (fsctl) {
        FILE_COMPRESSION_INFO compression;
        g_assert_true(GetFileInformationByHandleEx(read_handle, FileCompressionInfo,
                                                   &compression, sizeof(compression)));
        expected_allocated = (uint64_t)compression.CompressedFileSize.QuadPart;
    }
    g_assert_true(CloseHandle(read_handle));

    const WIN32_FIND_DATAW found = read_find_data(path);
    g_assert_cmpuint(found.dwFileAttributes & required_attribute, ==, required_attribute);
    DsEntry entry = {0};
    ds_win_find_metadata(&found, &entry);
    ds_win_measure_file(&ds_win_metadata_ops, path, NULL, &found, &entry);
    g_assert_cmpint(entry.kind, ==, DS_FILE);
    g_assert_cmpint(entry.reason, ==, DS_REASON_NONE);
    g_assert_cmpint(entry.logical.quality, ==, DS_KNOWN);
    g_assert_cmpuint(entry.logical.bytes, ==, 65536);
    g_assert_cmpuint(entry.logical.bytes, ==, (uint64_t)standard.EndOfFile.QuadPart);
    g_assert_cmpint(entry.allocated.quality, ==, DS_KNOWN);
    g_assert_cmpuint(entry.allocated.bytes, ==, expected_allocated);
    g_assert_cmpuint(entry.links, ==, standard.NumberOfLinks);
    g_assert_true(entry.identity_known);
    uint8_t expected_id[24];
    ds_u64_encode(identity.VolumeSerialNumber, expected_id);
    memcpy(expected_id + 8, identity.FileId.Identifier, 16);
    g_assert_cmpmem(entry.identity, sizeof(entry.identity), expected_id, sizeof(expected_id));

    g_assert_true(DeleteFileW(path));
    g_assert_true(RemoveDirectoryW(dir));
    g_free(path);
    g_free(dir);
}

static void standard_file(void) { measure_file(0, 0); }
static void sparse_file(void) { measure_file(FSCTL_SET_SPARSE, FILE_ATTRIBUTE_SPARSE_FILE); }
static void compressed_file(void) { measure_file(FSCTL_SET_COMPRESSION, FILE_ATTRIBUTE_COMPRESSED); }

static HANDLE WINAPI sharing_violation(LPCWSTR path, DWORD access, DWORD share,
                                       LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                       DWORD flags, HANDLE template)
{
    (void)path;
    (void)security;
    (void)template;
    g_assert_true(access == 0 || access == FILE_READ_ATTRIBUTES);
    g_assert_cmpuint(share, ==, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    g_assert_cmpuint(disposition, ==, OPEN_EXISTING);
    g_assert_cmpuint(flags & FILE_FLAG_OPEN_REPARSE_POINT, !=, 0);
    SetLastError(ERROR_SHARING_VIOLATION);
    return INVALID_HANDLE_VALUE;
}

static void metadata_open_failure(void)
{
    DsWinMetadataOps ops = ds_win_metadata_ops;
    ops.open = sharing_violation;
    WIN32_FIND_DATAW found = { .dwFileAttributes = FILE_ATTRIBUTE_NORMAL, .nFileSizeLow = 123 };
    DsEntry entry = {0};
    ds_win_find_metadata(&found, &entry);
    ds_win_measure_file(&ops, L"metadata.bin", NULL, &found, &entry);
    g_assert_cmpint(entry.logical.quality, ==, DS_KNOWN);
    g_assert_cmpuint(entry.logical.bytes, ==, 123);
    g_assert_cmpint(entry.allocated.quality, ==, DS_UNAVAILABLE);
    g_assert_false(entry.identity_known);
    g_assert_cmpint(entry.reason, ==, DS_REASON_PERMISSION);
    g_assert_cmpint(entry.os_code, ==, ERROR_SHARING_VIOLATION);
}

static BOOL WINAPI query_without_optional_classes(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS cls,
                                                   LPVOID output, DWORD length)
{
    if (cls == FileIdInfo || cls == FileCompressionInfo) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    return GetFileInformationByHandleEx(handle, cls, output, length);
}

static void absent_optional_queries(void)
{
    WCHAR *dir = fixture_directory();
    if (!dir) return;
    WCHAR *path = fixture_path(dir, L"unavailable.bin");
    HANDLE file = fixture_open(path, CREATE_NEW);
    DWORD received;
    g_assert_true(DeviceIoControl(file, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &received, NULL));
    fixture_write(file);
    g_assert_true(CloseHandle(file));

    const WIN32_FIND_DATAW found = read_find_data(path);
    DsWinMetadataOps ops = ds_win_metadata_ops;
    ops.query = query_without_optional_classes;
    DsEntry entry = {0};
    ds_win_find_metadata(&found, &entry);
    ds_win_measure_file(&ops, path, NULL, &found, &entry);
    g_assert_cmpint(entry.logical.quality, ==, DS_KNOWN);
    g_assert_cmpuint(entry.logical.bytes, ==, 4096);
    g_assert_cmpint(entry.allocated.quality, ==, DS_UNSUPPORTED);
    g_assert_false(entry.identity_known);
    g_assert_cmpint(entry.reason, ==, DS_REASON_METADATA);
    g_assert_cmpint(entry.os_code, ==, ERROR_NOT_SUPPORTED);
    g_assert_true(DeleteFileW(path));
    g_assert_true(RemoveDirectoryW(dir));
    g_free(path);
    g_free(dir);
}

static BOOL WINAPI query_after_reparse_change(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS cls,
                                              LPVOID output, DWORD length)
{
    g_assert_cmpint(cls, ==, FileAttributeTagInfo);
    if (!GetFileInformationByHandleEx(handle, cls, output, length)) return FALSE;
    FILE_ATTRIBUTE_TAG_INFO *tag = output;
    tag->FileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    tag->ReparseTag = IO_REPARSE_TAG_SYMLINK;
    return TRUE;
}

static void reparse_replacement_rechecked(void)
{
    WCHAR *dir = fixture_directory();
    if (!dir) return;
    WCHAR *path = fixture_path(dir, L"replace.bin");
    g_assert_true(CloseHandle(fixture_open(path, CREATE_NEW)));
    const WIN32_FIND_DATAW found = read_find_data(path);
    DsWinMetadataOps ops = ds_win_metadata_ops;
    ops.query = query_after_reparse_change;
    DsEntry entry = {0};
    ds_win_find_metadata(&found, &entry);
    ds_win_measure_file(&ops, path, NULL, &found, &entry);
    g_assert_cmpint(entry.kind, ==, DS_LINK);
    g_assert_cmpint(entry.reason, ==, DS_REASON_CHANGED);
    g_assert_cmpint(entry.allocated.quality, ==, DS_CHANGED);
    g_assert_false(entry.identity_known);
    g_assert_true(DeleteFileW(path));
    g_assert_true(RemoveDirectoryW(dir));
    g_free(path);
    g_free(dir);
}

static void volume_free_values(void)
{
    WCHAR *dir = fixture_directory();
    if (!dir) return;
    HANDLE handle = CreateFileW(dir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                NULL);
    g_assert_true(handle != INVALID_HANDLE_VALUE);
    ULARGE_INTEGER available, caller_total, free_space;
    g_assert_true(GetDiskFreeSpaceExW(dir, &available, &caller_total, &free_space));
    g_assert_cmpuint(available.QuadPart, <=, free_space.QuadPart);
    DsVolumeInfo volume = ds_win_volume_info(handle);
    g_assert_cmpint(volume.captured_at, >, 0);
    g_assert_cmpint(volume.available.quality, ==, DS_KNOWN);
    g_assert_cmpuint(volume.available.bytes, <=, caller_total.QuadPart);
    if (volume.total.quality == DS_KNOWN) {
        g_assert_cmpuint(caller_total.QuadPart, <=, volume.total.bytes);
        g_assert_cmpint(volume.used.quality, ==, DS_KNOWN);
        g_assert_cmpuint(volume.used.bytes, <=, volume.total.bytes);
    }
    g_assert_true(CloseHandle(handle));
    g_assert_true(RemoveDirectoryW(dir));
    g_free(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/windows/metadata/standard", standard_file);
    g_test_add_func("/windows/metadata/sparse", sparse_file);
    g_test_add_func("/windows/metadata/compressed", compressed_file);
    g_test_add_func("/windows/metadata/sharing-violation", metadata_open_failure);
    g_test_add_func("/windows/metadata/unsupported-queries", absent_optional_queries);
    g_test_add_func("/windows/metadata/reparse-change", reparse_replacement_rechecked);
    g_test_add_func("/windows/metadata/volume", volume_free_values);
    return g_test_run();
}

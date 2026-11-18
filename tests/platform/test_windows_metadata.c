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

static BOOL WINAPI query_identity_only(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS cls,
                                       LPVOID output, DWORD length)
{
    g_assert_cmpint(cls, ==, FileIdInfo);
    return GetFileInformationByHandleEx(handle, cls, output, length);
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
    if (!fsctl) {
        FILE_BASIC_INFO basic, combined_basic;
        FILE_STANDARD_INFO combined_standard;
        g_assert_true(GetFileInformationByHandleEx(read_handle, FileBasicInfo, &basic, sizeof(basic)));
        g_assert_nonnull(ds_win_metadata_ops.query_basic_standard);
        g_assert_true(ds_win_metadata_ops.query_basic_standard(read_handle, &combined_basic,
                                                              &combined_standard));
        g_assert_cmpuint(combined_basic.FileAttributes, ==, basic.FileAttributes);
        g_assert_cmpint(combined_basic.LastWriteTime.QuadPart, ==, basic.LastWriteTime.QuadPart);
        g_assert_cmpint(combined_standard.AllocationSize.QuadPart, ==, standard.AllocationSize.QuadPart);
        g_assert_cmpint(combined_standard.EndOfFile.QuadPart, ==, standard.EndOfFile.QuadPart);
        g_assert_cmpuint(combined_standard.NumberOfLinks, ==, standard.NumberOfLinks);
        g_assert_cmpint(combined_standard.DeletePending, ==, standard.DeletePending);
        g_assert_cmpint(combined_standard.Directory, ==, standard.Directory);
    } else {
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
    DsWinMetadataOps ops = ds_win_metadata_ops;
    if (!fsctl) ops.query = query_identity_only;
    ds_win_measure_file(&ops, path, NULL, &found, &entry);
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

static void parent_relative_metadata(void)
{
    WCHAR *dir = fixture_directory();
    if (!dir) return;
    WCHAR *parent = fixture_path(dir, L"parent");
    WCHAR *moved = fixture_path(dir, L"moved");
    const WCHAR raw_name[] = {L'r', 0xd800, L'w', 0};
    WCHAR *file = fixture_path(parent, raw_name);
    g_assert_true(CreateDirectoryW(parent, NULL));
    HANDLE writable = fixture_open(file, CREATE_NEW);
    fixture_write(writable);
    g_assert_true(CloseHandle(writable));
    WIN32_FIND_DATAW found = read_find_data(file);
    g_assert_cmpmem(found.cFileName, sizeof(raw_name), raw_name, sizeof(raw_name));

    HANDLE directory;
    DsWinNode node;
    g_assert_cmpuint(ds_win_open_node(&ds_win_metadata_ops, parent, true, &directory, &node),
                     ==, ERROR_SUCCESS);
    g_assert_true(node.id_known);
    g_assert_true(ds_win_prepare_ntfs_metadata());
    DsEntry expected = {0};
    ds_win_find_metadata(&found, &expected);
    ds_win_measure_file(&ds_win_metadata_ops, file, &node, &found, &expected);
    g_assert_true(expected.identity_known);

    /* The held directory still names the observed file after its old path
     * has been replaced by a different ordinary file. */
    g_assert_true(MoveFileExW(parent, moved, 0));
    g_assert_true(CreateDirectoryW(parent, NULL));
    g_assert_true(CloseHandle(fixture_open(file, CREATE_NEW)));
    DsEntry relative = {0};
    ds_win_find_metadata(&found, &relative);
    g_assert_true(ds_win_measure_ntfs_file(directory, &node, &found, &relative));
    g_assert_cmpint(relative.reason, ==, DS_REASON_NONE);
    g_assert_cmpint(relative.logical.quality, ==, DS_KNOWN);
    g_assert_cmpuint(relative.logical.bytes, ==, expected.logical.bytes);
    g_assert_cmpint(relative.allocated.quality, ==, DS_KNOWN);
    g_assert_cmpuint(relative.allocated.bytes, ==, expected.allocated.bytes);
    g_assert_cmpuint(relative.links, ==, expected.links);
    g_assert_true(relative.identity_known);
    g_assert_cmpmem(relative.identity, sizeof(relative.identity), expected.identity, sizeof(expected.identity));

    WCHAR *moved_file = fixture_path(moved, raw_name);
    WCHAR *hardlink = fixture_path(moved, L"alias.bin");
    g_assert_true(CreateHardLinkW(hardlink, moved_file, NULL));
    relative = (DsEntry){0};
    ds_win_find_metadata(&found, &relative);
    g_assert_true(ds_win_measure_ntfs_file(directory, &node, &found, &relative));
    g_assert_cmpint(relative.reason, ==, DS_REASON_NONE);
    g_assert_cmpuint(relative.allocated.bytes, ==, expected.allocated.bytes);
    g_assert_cmpuint(relative.links, ==, 2);
    g_assert_true(relative.identity_known);
    g_assert_cmpmem(relative.identity, sizeof(relative.identity), expected.identity, sizeof(expected.identity));
    g_assert_true(CloseHandle(directory));
    g_assert_true(DeleteFileW(hardlink));
    g_assert_true(DeleteFileW(moved_file));
    g_assert_true(DeleteFileW(file));
    g_assert_true(RemoveDirectoryW(moved));
    g_assert_true(RemoveDirectoryW(parent));
    g_assert_true(RemoveDirectoryW(dir));
    g_free(hardlink);
    g_free(moved_file);
    g_free(file);
    g_free(moved);
    g_free(parent);
    g_free(dir);
}

static void long_parent_relative_metadata(void)
{
    WCHAR *dir = fixture_directory();
    if (!dir) return;
    WCHAR *parents[8] = {dir};
    WCHAR component[101];
    wmemset(component, L'x', G_N_ELEMENTS(component) - 1);
    component[G_N_ELEMENTS(component) - 1] = 0;
    for (size_t i = 1; i < G_N_ELEMENTS(parents); ++i) {
        parents[i] = fixture_path(parents[i - 1], component);
        g_assert_true(CreateDirectoryW(parents[i], NULL));
    }
    const WCHAR *parent_path = parents[G_N_ELEMENTS(parents) - 1];
    WCHAR *path = fixture_path(parent_path, L"long-parent.bin");
    HANDLE file = fixture_open(path, CREATE_NEW);
    fixture_write(file);
    g_assert_true(CloseHandle(file));
    const WIN32_FIND_DATAW found = read_find_data(path);
    HANDLE parent;
    DsWinNode node;
    g_assert_cmpuint(ds_win_open_node(&ds_win_metadata_ops, parent_path, true, &parent, &node),
                     ==, ERROR_SUCCESS);
    g_assert_true(ds_win_prepare_ntfs_metadata());
    DsEntry reference = {0}, relative = {0};
    ds_win_find_metadata(&found, &reference);
    ds_win_find_metadata(&found, &relative);
    ds_win_measure_file(&ds_win_metadata_ops, path, &node, &found, &reference);
    g_assert_true(ds_win_measure_ntfs_file(parent, &node, &found, &relative));
    g_assert_true(CloseHandle(parent));
    g_assert_cmpint(relative.reason, ==, DS_REASON_NONE);
    g_assert_cmpint(relative.allocated.quality, ==, DS_KNOWN);
    g_assert_cmpuint(relative.allocated.bytes, ==, reference.allocated.bytes);
    g_assert_cmpuint(relative.logical.bytes, ==, reference.logical.bytes);
    g_assert_cmpuint(relative.links, ==, reference.links);
    g_assert_true(relative.identity_known);
    g_assert_cmpmem(relative.identity, sizeof(relative.identity), reference.identity,
                    sizeof(reference.identity));
    g_assert_true(DeleteFileW(path));
    g_free(path);
    for (size_t i = G_N_ELEMENTS(parents); i-- > 0;) {
        g_assert_true(RemoveDirectoryW(parents[i]));
        g_free(parents[i]);
    }
}

static HANDLE WINAPI sharing_violation(LPCWSTR path, DWORD access, DWORD share,
                                       LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                       DWORD flags, HANDLE template)
{
    (void)path;
    (void)security;
    (void)template;
    g_assert_cmpuint(access, ==, FILE_READ_ATTRIBUTES);
    g_assert_cmpuint(share, ==, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    g_assert_cmpuint(disposition, ==, OPEN_EXISTING);
    g_assert_cmpuint(flags, ==, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL);
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
    ops.query_basic_standard = NULL;
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

static bool query_combined_after_reparse_change(HANDLE handle, FILE_BASIC_INFO *basic,
                                               FILE_STANDARD_INFO *standard)
{
    if (!ds_win_metadata_ops.query_basic_standard(handle, basic, standard)) return false;
    basic->FileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    return true;
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
    ops.query_basic_standard = query_combined_after_reparse_change;
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
    g_test_add_func("/windows/metadata/parent-relative", parent_relative_metadata);
    g_test_add_func("/windows/metadata/long-parent-relative", long_parent_relative_metadata);
    g_test_add_func("/windows/metadata/sparse", sparse_file);
    g_test_add_func("/windows/metadata/compressed", compressed_file);
    g_test_add_func("/windows/metadata/sharing-violation", metadata_open_failure);
    g_test_add_func("/windows/metadata/unsupported-queries", absent_optional_queries);
    g_test_add_func("/windows/metadata/reparse-change", reparse_replacement_rechecked);
    g_test_add_func("/windows/metadata/volume", volume_free_values);
    return g_test_run();
}

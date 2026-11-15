#ifndef DISKSCOPE_WIN_SCAN_H
#define DISKSCOPE_WIN_SCAN_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include "diskscope/ds_scan.h"

typedef struct {
    HANDLE (WINAPI *open)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    BOOL (WINAPI *query)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
    BOOL (WINAPI *close)(HANDLE);
} DsWinMetadataOps;

typedef struct {
    DWORD attributes, tag;
    FILE_ID_INFO id;
    bool id_known;
    DWORD id_error;
    BY_HANDLE_FILE_INFORMATION legacy;
    bool legacy_known;
} DsWinNode;

typedef struct {
    WCHAR filesystem[32];
    DWORD flags;
    DsSize total, free, available, caller_total;
    DWORD space_error;
} DsWinVolume;

extern const DsWinMetadataOps ds_win_metadata_ops;

WCHAR *ds_win_absolute_path(const DsNativePath *path, DWORD *code);
WCHAR *ds_win_join(const WCHAR *directory, const WCHAR *leaf);
DWORD ds_win_open_node(const DsWinMetadataOps *ops, const WCHAR *path, bool directory,
                       HANDLE *handle, DsWinNode *node);
DsReason ds_win_classify(DWORD attributes, DWORD tag);
DsReason ds_win_error_reason(DWORD code, DsReason otherwise);
void ds_win_find_metadata(const WIN32_FIND_DATAW *found, DsEntry *entry);
void ds_win_measure_file(const DsWinMetadataOps *ops, const WCHAR *path,
                         const DsWinNode *root, const WIN32_FIND_DATAW *found,
                         DsEntry *entry);
bool ds_win_same_volume(const DsWinNode *a, const DsWinNode *b);
bool ds_win_same_node(const DsWinNode *a, const DsWinNode *b);
DWORD ds_win_volume_info(HANDLE root, const WCHAR *absolute_path, DsWinVolume *volume);

#endif

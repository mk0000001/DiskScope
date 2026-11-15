#include "win_scan.h"

#include <string.h>
#include <wchar.h>

static WCHAR *volume_path(HANDLE root)
{
    /* Query the volume named by the validated handle. Re-resolving the
     * selected path could follow an ancestor replaced since validation. */
    const DWORD flags = VOLUME_NAME_GUID | FILE_NAME_OPENED;
    DWORD capacity = GetFinalPathNameByHandleW(root, NULL, 0, flags);
    if (!capacity || capacity > DS_PATH_MAX / sizeof(WCHAR))
        return NULL;
    WCHAR *path = g_new(WCHAR, capacity);
    DWORD length = GetFinalPathNameByHandleW(root, path, capacity, flags);
    const WCHAR prefix[] = L"\\\\?\\Volume{";
    WCHAR *tail = length && length < capacity &&
                  wcsncmp(path, prefix, G_N_ELEMENTS(prefix) - 1) == 0
        ? wcschr(path + G_N_ELEMENTS(prefix) - 1, L'}') : NULL;
    if (!tail || tail[1] != L'\\') {
        g_free(path);
        return NULL;
    }
    tail[2] = 0;
    return path;
}

static DsSize allocation_bytes(uint64_t units, uint64_t unit_size)
{
    DsSize result = {0, DS_KNOWN};
    if (!ds_u64_mul(units, unit_size, &result.bytes))
        result.quality = DS_OVERFLOW;
    return result;
}

static void modern_space(const WCHAR *path, DsVolumeInfo *volume)
{
    /* GetDiskFreeSpaceExW reports a caller total, which quotas can reduce.
     * The newer API reports volume-wide total and used bytes independently. */
    typedef HRESULT (WINAPI *GetDiskSpaceInfo)(LPCWSTR, DISK_SPACE_INFORMATION *);
    FARPROC symbol = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetDiskSpaceInformationW");
    if (!symbol) {
        volume->total.quality = volume->used.quality = DS_UNSUPPORTED;
        return;
    }
    GetDiskSpaceInfo get_disk_space;
    _Static_assert(sizeof(get_disk_space) == sizeof(symbol), "Windows function pointer size");
    memcpy(&get_disk_space, &symbol, sizeof(get_disk_space));
    DISK_SPACE_INFORMATION info;
    if (FAILED(get_disk_space(path, &info)) || !info.SectorsPerAllocationUnit || !info.BytesPerSector)
        return;

    uint64_t unit_size = (uint64_t)info.SectorsPerAllocationUnit * info.BytesPerSector;
    volume->total = allocation_bytes(info.ActualTotalAllocationUnits, unit_size);
    volume->used = info.UsedAllocationUnits <= info.ActualTotalAllocationUnits
        ? allocation_bytes(info.UsedAllocationUnits, unit_size) : (DsSize){0, DS_CHANGED};
    volume->available = info.CallerAvailableAllocationUnits <= info.ActualTotalAllocationUnits
        ? allocation_bytes(info.CallerAvailableAllocationUnits, unit_size) : (DsSize){0, DS_CHANGED};
}

DsVolumeInfo ds_win_volume_info(HANDLE root)
{
    DsVolumeInfo volume = {.total.quality = DS_UNAVAILABLE, .used.quality = DS_UNAVAILABLE,
                           .available.quality = DS_UNAVAILABLE};
    g_autofree WCHAR *path = volume_path(root);
    if (path) {
        modern_space(path, &volume);
        if (volume.available.quality == DS_UNAVAILABLE) {
            ULARGE_INTEGER available;
            if (GetDiskFreeSpaceExW(path, &available, NULL, NULL))
                volume.available = (DsSize){available.QuadPart, DS_KNOWN};
        }
    }
    volume.captured_at = g_get_real_time() / G_USEC_PER_SEC;
    return volume;
}

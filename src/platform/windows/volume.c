#include "win_scan.h"

#include <string.h>

DWORD ds_win_volume_info(HANDLE root, const WCHAR *absolute_path, DsWinVolume *volume)
{
    *volume = (DsWinVolume){
        .total = {0, DS_UNSUPPORTED},
        .free = {0, DS_UNAVAILABLE},
        .available = {0, DS_UNAVAILABLE},
        .caller_total = {0, DS_UNAVAILABLE},
    };
    if (!GetVolumeInformationByHandleW(root, NULL, 0, NULL, NULL, &volume->flags,
                                       volume->filesystem, G_N_ELEMENTS(volume->filesystem)))
        return GetLastError();

    ULARGE_INTEGER available, caller_total, free;
    if (GetDiskFreeSpaceExW(absolute_path, &available, &caller_total, &free)) {
        volume->available = (DsSize){available.QuadPart, DS_KNOWN};
        volume->caller_total = (DsSize){caller_total.QuadPart, DS_KNOWN};
        volume->free = (DsSize){free.QuadPart, DS_KNOWN};
    } else {
        volume->space_error = GetLastError();
    }

    /* The total returned by GetDiskFreeSpaceExW can be reduced by the user's
     * quota. The optional newer API exposes the unqualified volume total. */
    typedef HRESULT (WINAPI *GetDiskSpaceInfo)(LPCWSTR, DISK_SPACE_INFORMATION *);
    FARPROC symbol = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetDiskSpaceInformationW");
    if (symbol) {
        GetDiskSpaceInfo get_disk_space;
        _Static_assert(sizeof(get_disk_space) == sizeof(symbol), "Windows function pointer size");
        memcpy(&get_disk_space, &symbol, sizeof(get_disk_space));
        WCHAR drive[] = {absolute_path[4], L':', L'\\', 0};
        DISK_SPACE_INFORMATION info;
        uint64_t unit_size, total;
        if (FAILED(get_disk_space(drive, &info)) || !info.SectorsPerAllocationUnit ||
            !info.BytesPerSector) {
            volume->total.quality = DS_UNAVAILABLE;
        } else if (!ds_u64_mul(info.SectorsPerAllocationUnit, info.BytesPerSector, &unit_size) ||
                   !ds_u64_mul(info.ActualTotalAllocationUnits, unit_size, &total)) {
            volume->total.quality = DS_OVERFLOW;
        } else {
            volume->total = (DsSize){total, DS_KNOWN};
        }
    }
    return ERROR_SUCCESS;
}

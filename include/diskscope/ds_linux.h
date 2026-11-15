#ifndef DISKSCOPE_LINUX_H
#define DISKSCOPE_LINUX_H

#include "ds_scan.h"

typedef enum {
    DS_LINUX_BOUNDARY_OPENAT2,
    DS_LINUX_BOUNDARY_MOUNT_ID,
    DS_LINUX_BOUNDARY_MOUNTINFO
} DsLinuxBoundary;

typedef struct {
    char filesystem[64];
    uint64_t mount_id;
    DsLinuxBoundary boundary;
    bool identity_trusted;
    DsSize total, free, available;
} DsLinuxVolume;

/* Reads volume-level metadata using the same root and mount policy as ds_walk.
 * MOUNTINFO identifies an initial path snapshot plus device checks. */
DsStatus ds_linux_volume(const DsNativePath *root, DsLinuxVolume *volume, DsError *error);

#endif

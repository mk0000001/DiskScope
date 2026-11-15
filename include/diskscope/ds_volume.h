#ifndef DISKSCOPE_VOLUME_H
#define DISKSCOPE_VOLUME_H

#include "ds_types.h"

/* One reading for the filesystem that contains the scan root. The available
 * bytes are for the scan caller. A zero timestamp means no reading was saved. */
typedef struct {
    DsSize total, used, available;
    int64_t captured_at; /* UTC Unix seconds. */
} DsVolumeInfo;

typedef DsStatus (*DsVolumeSink)(const DsVolumeInfo *, void *, DsError *);

#endif

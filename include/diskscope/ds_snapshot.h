#ifndef DISKSCOPE_SNAPSHOT_H
#define DISKSCOPE_SNAPSHOT_H

#include "ds_store.h"

DsStatus ds_snapshot_save(DsStore *reader, const DsNativePath *destination,
                          DsControl *control, DsError *error);
/* Open an untrusted standalone snapshot after checking its format. */
DsStatus ds_snapshot_open(const DsNativePath *path, DsStore **reader,
                          DsControl *control, DsError *error);

#endif

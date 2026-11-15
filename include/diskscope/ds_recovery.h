#ifndef DISKSCOPE_RECOVERY_H
#define DISKSCOPE_RECOVERY_H

#include "ds_store.h"

typedef struct DsRecoveredScan DsRecoveredScan;

/* Searches one local cache base for the latest abandoned or completed scan
 * with committed observations. DS_OK with a NULL result means no eligible scan.
 * A recovered scan holds an exclusive cache lease until it is freed. */
DsStatus ds_recovery_latest(const DsNativePath *cache, DsControl *control,
                            DsRecoveredScan **result, DsError *error);
const DsNativePath *ds_recovered_path(const DsRecoveredScan *scan);
const DsStoreInfo *ds_recovered_info(const DsRecoveredScan *scan);
bool ds_recovered_newer(const DsRecoveredScan *candidate, const DsRecoveredScan *current);
void ds_recovered_free(DsRecoveredScan *scan);

#endif

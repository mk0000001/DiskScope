#ifndef DISKSCOPE_CACHE_LEASE_H
#define DISKSCOPE_CACHE_LEASE_H

#include "diskscope/ds_types.h"

#define DS_CACHE_LEASE_FILE "scan.lock"
typedef struct DsCacheLease DsCacheLease;

/* Collection roles share the lease. Recovery exclusively acquires an
 * existing lease. Initial creation is exclusive at the filesystem level.
 * Every acquisition is nonblocking. */
DsCacheLease *ds_cache_lease_open(const char *folder, bool create, bool exclusive,
                                 DsError *error);
void ds_cache_lease_close(DsCacheLease *lease);
int64_t ds_cache_lease_created(const DsCacheLease *lease);

#endif

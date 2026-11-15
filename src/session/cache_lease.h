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
#ifdef G_OS_WIN32
DsCacheLease *ds_cache_lease_open_delete(const char *folder);
bool ds_cache_lease_delete(DsCacheLease *lease);
#else
DsCacheLease *ds_cache_lease_open_at(int dir, bool create, bool exclusive, DsError *error);
bool ds_cache_lease_delete_at(DsCacheLease *lease, int dir);
#endif
void ds_cache_lease_close(DsCacheLease *lease);
int64_t ds_cache_lease_created(const DsCacheLease *lease);

#endif

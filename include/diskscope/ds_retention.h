#ifndef DISKSCOPE_RETENTION_H
#define DISKSCOPE_RETENTION_H

#include "ds_scan.h"

#define DS_CACHE_OWNER_FILE "cache-owner.v1"

/* Mark a newly created private cache while holding its lease and once its
 * primary database exists. Marking is optional for using the cache and
 * required for automatic removal. */
bool ds_cache_mark_owned(const char *folder, bool scan);

/* Best-effort cleanup of one local cache base. NULL selects the default.
 * May block on filesystem I/O. Pass a control to cancel between caches. */
void ds_cache_prune(const DsNativePath *cache, DsControl *control);

#endif

#ifndef DS_LOCAL_H
#define DS_LOCAL_H

#include "diskscope/ds_scan.h"

char *ds_private_cache(const DsNativePath *requested, DsError *error);
char *ds_cache_base(const DsNativePath *requested, DsError *error);
bool ds_cache_is_local(const char *path);
char *ds_worker_binary(DsError *error);
uint64_t ds_cache_usage(const char *folder);
bool ds_cache_has_reserve(const char *folder);

#endif

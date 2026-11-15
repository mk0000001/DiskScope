#ifndef DISKSCOPE_STATISTICS_H
#define DISKSCOPE_STATISTICS_H
#include "ds_types.h"
#define DS_AGE_GROUPS 7u
size_t ds_age_group(int64_t observation_time, bool time_known, int64_t mtime);
char *ds_extension(const char *escaped_name);
#endif

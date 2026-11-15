#ifndef DISKSCOPE_WIN_IO_H
#define DISKSCOPE_WIN_IO_H

#include <stdint.h>

/* Returns an owned, absolute UTF-8 path for Windows filesystem calls. */
char *ds_win_extended_path(const char *path);

typedef struct DsWinDir DsWinDir;
/* NULL and -1 expose the underlying error through GetLastError(). */
DsWinDir *ds_win_dir_open(const char *folder);
/* The optional name is owned by the caller. Returns 1 for an entry, 0 at EOF,
 * or -1 on an enumeration error. The size is zero for directories. */
int ds_win_dir_next(DsWinDir *directory, char **name, uint64_t *size);
void ds_win_dir_close(DsWinDir *directory);
uint64_t ds_win_cache_usage(const char *folder);

#endif

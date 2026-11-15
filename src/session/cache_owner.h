#ifndef DISKSCOPE_CACHE_OWNER_H
#define DISKSCOPE_CACHE_OWNER_H

#include "diskscope/ds_retention.h"
#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE DsCacheFile;
#define DS_CACHE_BAD_FILE INVALID_HANDLE_VALUE
#else
typedef int DsCacheFile;
#define DS_CACHE_BAD_FILE (-1)
#endif

#define DS_CACHE_SCAN_DB "scan.sqlite"
#define DS_CACHE_IMPORT_DB "import.dscan"
#define DS_CACHE_OWNER_SIZE 128

typedef struct {
    uint64_t volume, file_low, file_high, birth;
} DsCacheIdentity;

bool ds_cache_file_identity(DsCacheFile file, DsCacheIdentity *identity);
size_t ds_cache_owner_record(char text[DS_CACHE_OWNER_SIZE], const DsCacheIdentity *identity, bool scan);

#endif

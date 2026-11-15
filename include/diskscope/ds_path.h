#ifndef DISKSCOPE_PATH_H
#define DISKSCOPE_PATH_H

#include "ds_types.h"

#define DS_PATH_MAX (256u * 1024u)

typedef enum { DS_PATH_BYTES = 1, DS_PATH_UTF16LE = 2 } DsEncoding;
typedef struct { DsEncoding encoding; GBytes *raw; } DsNativePath;

/* Outputs own their data. Initialize paths to {0} and clear them after use. */
DsStatus ds_path_new(DsEncoding encoding, const void *bytes, size_t length,
                     DsNativePath *out, DsError *error);
DsStatus ds_path_from_arg(const char *arg, DsNativePath *out, DsError *error);
DsStatus ds_path_display(const DsNativePath *path, char **out_utf8, DsError *error);
/* Returns OS filename bytes on Linux and UTF-8 for GLib filename APIs on Windows.
 * Ill-formed UTF-16 is preserved by ds_path_new, but cannot pass this conversion. */
char *ds_path_arg(const DsNativePath *path, DsError *error);
bool ds_path_component_valid(DsEncoding encoding, const void *bytes, size_t length);
void ds_path_clear(DsNativePath *path);

#endif

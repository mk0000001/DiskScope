#ifndef DS_OUTPUT_H
#define DS_OUTPUT_H

#include "diskscope/ds_store.h"

typedef struct {
    char *temporary;
    char *destination;
    int fd;
} DsOutput;

DsStatus ds_output_begin(DsStore *source, const DsNativePath *path, DsOutput *out, DsError *error);
DsStatus ds_output_commit(DsOutput *out, DsError *error);
void ds_output_clear(DsOutput *out);

#endif

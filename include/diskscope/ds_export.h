#ifndef DISKSCOPE_EXPORT_H
#define DISKSCOPE_EXPORT_H

#include "ds_query.h"

typedef enum { DS_EXPORT_CSV, DS_EXPORT_JSON, DS_EXPORT_JSONL } DsExportFormat;

typedef struct {
    DsExportFormat format;
    DsNativePath destination; /* Borrowed path to the output file. */
    bool bom; /* Optional UTF-8 BOM for CSV. */
} DsExportOptions;

DsStatus ds_export(DsStore *reader, const DsExportOptions *options,
                   DsControl *control, DsError *error);

#endif

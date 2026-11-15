#ifndef DISKSCOPE_EXPORT_INTERNAL_H
#define DISKSCOPE_EXPORT_INTERNAL_H

#include "diskscope/ds_export.h"
#include "../store/output.h"

void ds_csv_field(GString *line, const char *text, bool protect_formula);
void ds_json_string(GString *line, const char *text);
void ds_json_path(GString *line, const DsNativePath *path, const char *display);
void ds_json_size(GString *line, DsSize size, uint64_t unknown);
const char *ds_export_encoding(DsEncoding encoding);
DsStatus ds_output_write(DsOutput *output, const char *data, size_t length, DsError *error);
DsStatus ds_output_finish(DsOutput *output, DsControl *control, DsError *error);

#endif

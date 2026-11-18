#ifndef DISKSCOPE_WORKER_BATCHES_H
#define DISKSCOPE_WORKER_BATCHES_H

#include "diskscope/ds_scan.h"

typedef struct DsBatchWriter DsBatchWriter;

/* A single producer may submit borrowed batches. The sink runs in order on
 * one consumer and must remain valid until finish joins it. */
DsBatchWriter *ds_batch_writer_new(DsEntrySink sink, void *data, DsControl *control);
DsStatus ds_batch_writer_enqueue(const DsEntryBatch *batch, void *writer, DsError *error);
DsStatus ds_batch_writer_drain(DsBatchWriter *writer, DsError *error);
DsStatus ds_batch_writer_finish(DsBatchWriter *writer, DsError *error);

#endif

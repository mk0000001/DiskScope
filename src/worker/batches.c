#include "batches.h"

/* Include the batch currently in the sink in both ownership limits. */
enum { MAX_BUFFERED_BATCHES = 8, NAME_OVERHEAD = 64 };

typedef struct {
    size_t count, bytes;
    DsEntry entries[];
} Batch;

struct DsBatchWriter {
    DsEntrySink sink;
    void *data;
    DsControl *control;
    GThread *thread;
    GMutex lock;
    GCond changed;
    GQueue pending;
    size_t bytes;
    unsigned buffered;
    bool closing;
    DsStatus status;
    DsError error;
};

static void free_batch(Batch *batch)
{
    for (size_t i = 0; i < batch->count; ++i)
        ds_path_clear(&batch->entries[i].name);
    g_free(batch);
}

static DsStatus status_locked(const DsBatchWriter *writer, DsError *error)
{
    if (writer->status != DS_OK && error)
        *error = writer->error;
    return writer->status;
}

static gpointer consume(gpointer data)
{
    DsBatchWriter *writer = data;
    g_mutex_lock(&writer->lock);
    for (;;) {
        while (g_queue_is_empty(&writer->pending) && !writer->closing)
            g_cond_wait(&writer->changed, &writer->lock);
        Batch *batch = g_queue_pop_head(&writer->pending);
        if (!batch)
            break;
        g_mutex_unlock(&writer->lock);
        DsError error = {0};
        const DsEntryBatch entries = {batch->entries, batch->count};
        DsStatus status = writer->sink(&entries, writer->data, &error);
        size_t bytes = batch->bytes;
        free_batch(batch);
        g_mutex_lock(&writer->lock);
        writer->bytes -= bytes;
        --writer->buffered;
        if (status != DS_OK) {
            writer->status = status;
            writer->error = error;
            g_queue_clear_full(&writer->pending, (GDestroyNotify)free_batch);
            writer->bytes = 0;
            writer->buffered = 0;
        }
        g_cond_broadcast(&writer->changed);
        if (status != DS_OK)
            break;
    }
    bool failed = writer->status != DS_OK;
    g_mutex_unlock(&writer->lock);
    if (failed)
        ds_control_cancel(writer->control);
    return NULL;
}

DsBatchWriter *ds_batch_writer_new(DsEntrySink sink, void *data, DsControl *control)
{
    DsBatchWriter *writer = g_new0(DsBatchWriter, 1);
    writer->sink = sink;
    writer->data = data;
    writer->control = control;
    g_mutex_init(&writer->lock);
    g_cond_init(&writer->changed);
    writer->thread = g_thread_try_new("ds-cache-writer", consume, writer, NULL);
    if (!writer->thread) {
        g_cond_clear(&writer->changed);
        g_mutex_clear(&writer->lock);
        g_free(writer);
        return NULL;
    }
    return writer;
}

DsStatus ds_batch_writer_enqueue(const DsEntryBatch *batch, void *data, DsError *error)
{
    DsBatchWriter *writer = data;
    size_t storage = sizeof(Batch) + batch->count * sizeof(DsEntry);
    size_t bytes = storage;
    for (size_t i = 0; i < batch->count; ++i)
        bytes += g_bytes_get_size(batch->entries[i].name.raw) + NAME_OVERHEAD;
    if (bytes > DS_QUEUE_BYTES) {
        /* The walker can borrow a large batch without retaining it. */
        DsStatus status = ds_batch_writer_drain(writer, error);
        return status == DS_OK ? writer->sink(batch, writer->data, error) : status;
    }

    g_mutex_lock(&writer->lock);
    while (writer->status == DS_OK && (writer->buffered == MAX_BUFFERED_BATCHES ||
           bytes > DS_QUEUE_BYTES - writer->bytes))
        g_cond_wait(&writer->changed, &writer->lock);
    DsStatus status = status_locked(writer, error);
    if (status == DS_OK) {
        Batch *copy = g_malloc(storage);
        *copy = (Batch){.count = batch->count, .bytes = bytes};
        for (size_t i = 0; i < batch->count; ++i) {
            copy->entries[i] = batch->entries[i];
            copy->entries[i].name.raw = g_bytes_ref(copy->entries[i].name.raw);
        }
        g_queue_push_tail(&writer->pending, copy);
        writer->bytes += bytes;
        ++writer->buffered;
        g_cond_signal(&writer->changed);
    }
    g_mutex_unlock(&writer->lock);
    return status;
}

DsStatus ds_batch_writer_drain(DsBatchWriter *writer, DsError *error)
{
    if (!writer)
        return DS_OK;
    g_mutex_lock(&writer->lock);
    while (writer->buffered)
        g_cond_wait(&writer->changed, &writer->lock);
    DsStatus status = status_locked(writer, error);
    g_mutex_unlock(&writer->lock);
    return status;
}

DsStatus ds_batch_writer_finish(DsBatchWriter *writer, DsError *error)
{
    if (!writer)
        return DS_OK;
    g_mutex_lock(&writer->lock);
    writer->closing = true;
    g_cond_signal(&writer->changed);
    g_mutex_unlock(&writer->lock);
    g_thread_join(writer->thread);
    DsStatus status = status_locked(writer, error);
    g_cond_clear(&writer->changed);
    g_mutex_clear(&writer->lock);
    g_free(writer);
    return status;
}

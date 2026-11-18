#include "batches.h"

typedef struct {
    GMutex lock;
    GCond changed;
    bool entered, released, fail_second;
    unsigned calls;
} Sink;

static DsStatus sink(const DsEntryBatch *batch, void *data, DsError *error)
{
    Sink *state = data;
    size_t size = 0;
    const void *name = g_bytes_get_data(batch->entries[0].name.raw, &size);
    g_assert_cmpuint(batch->count, ==, 1);
    g_assert_cmpmem(name, size, "fixture", 7);
    g_mutex_lock(&state->lock);
    unsigned n = ++state->calls;
    g_assert_cmpint(batch->entries[0].id, ==, n);
    if (n == 1) {
        state->entered = true;
        g_cond_signal(&state->changed);
        while (!state->released)
            g_cond_wait(&state->changed, &state->lock);
    }
    g_mutex_unlock(&state->lock);
    return n == 2 && state->fail_second ? ds_error(error, DS_LIMIT, 12, "fixture full") : DS_OK;
}

static void copy_and_drain(gconstpointer data)
{
    Sink state = {.fail_second = GPOINTER_TO_INT(data) != 0};
    g_mutex_init(&state.lock);
    g_cond_init(&state.changed);
    DsControl control;
    ds_control_init(&control);
    DsBatchWriter *writer = ds_batch_writer_new(sink, &state, &control);
    g_assert_nonnull(writer);
    DsEntry entry = {.id = 1};
    g_assert_cmpint(ds_path_new(DS_PATH_BYTES, "fixture", 7, &entry.name, NULL), ==, DS_OK);
    DsEntryBatch batch = {&entry, 1};
    g_assert_cmpint(ds_batch_writer_enqueue(&batch, writer, NULL), ==, DS_OK);
    g_mutex_lock(&state.lock);
    while (!state.entered)
        g_cond_wait(&state.changed, &state.lock);
    g_mutex_unlock(&state.lock);
    entry.id = 2;
    g_assert_cmpint(ds_batch_writer_enqueue(&batch, writer, NULL), ==, DS_OK);
    ds_path_clear(&entry.name);
    g_mutex_lock(&state.lock);
    state.released = true;
    g_cond_signal(&state.changed);
    g_mutex_unlock(&state.lock);
    DsStatus expected = state.fail_second ? DS_LIMIT : DS_OK;
    DsError error = {0};
    g_assert_cmpint(ds_batch_writer_drain(writer, &error), ==, expected);
    g_assert_cmpuint(state.calls, ==, 2);
    g_assert_cmpint(ds_batch_writer_finish(writer, &error), ==, expected);
    g_assert_cmpint(ds_control_canceled(&control), ==, state.fail_second);
    if (state.fail_second)
        g_assert_cmpstr(error.message, ==, "fixture full");
    ds_control_clear(&control);
    g_cond_clear(&state.changed);
    g_mutex_clear(&state.lock);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_data_func("/batches/copy-and-drain", GINT_TO_POINTER(0), copy_and_drain);
    g_test_add_data_func("/batches/reports-writer-failure", GINT_TO_POINTER(1), copy_and_drain);
    return g_test_run();
}

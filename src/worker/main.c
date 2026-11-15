#include "diskscope/ds_session.h"
#include "diskscope/ds_store.h"
#include "local.h"
#include "cache_lease.h"
#include "protocol.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <sys/resource.h>
#endif

typedef struct {
    DsControl control;
    GMutex output;
    GMutex heartbeat;
    GCond stopped;
    bool stop_heartbeat;
    DsStage stage;
    char id[37];
    int64_t sequence;
    DsStore *store;
    char *cache;
    DsCacheLease *lease;
    uint64_t cache_budget;
    int64_t commit_seq, observations, reported_at, reported_seq;
} Worker;

static bool send(Worker *worker, DsMessageType type, JsonObject *object)
{
    g_mutex_lock(&worker->output);
    GBytes *frame = ds_message_frame(type, worker->id, ++worker->sequence, object, NULL);
    bool ok = false;
    if (frame) {
        size_t bytes;
        const void *data = g_bytes_get_data(frame, &bytes);
        ok = fwrite(data, 1, bytes, stdout) == bytes && fflush(stdout) == 0;
        g_bytes_unref(frame);
    }
    g_mutex_unlock(&worker->output);
    if (!ok)
        ds_control_cancel(&worker->control);
    return ok;
}

static void send_stage(Worker *worker, DsStage stage)
{
    g_mutex_lock(&worker->heartbeat);
    worker->stage = stage;
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "stage", stage);
    send(worker, DS_MSG_STATUS, data);
    json_object_unref(data);
    g_mutex_unlock(&worker->heartbeat);
}

static gpointer beat(gpointer data)
{
    Worker *worker = data;
    g_mutex_lock(&worker->heartbeat);
    while (!worker->stop_heartbeat) {
        int64_t until = g_get_monotonic_time() + 500 * G_TIME_SPAN_MILLISECOND;
        if (!g_cond_wait_until(&worker->stopped, &worker->heartbeat, until) &&
            !worker->stop_heartbeat) {
            JsonObject *payload = json_object_new();
            json_object_set_int_member(payload, "stage", worker->stage);
            send(worker, DS_MSG_STATUS, payload);
            json_object_unref(payload);
        }
    }
    g_mutex_unlock(&worker->heartbeat);
    return NULL;
}

static void pause_callback(bool paused, void *data)
{
    send_stage(data, paused ? DS_PAUSED : DS_RUNNING);
}

static bool receive(const char *id, int64_t sequence, DsMessage *out)
{
    uint8_t header[4];
    size_t count = fread(header, 1, sizeof header, stdin), length = 0;
    if (count != sizeof header || !ds_frame_length(header, &length))
        return false;
    uint8_t *body = g_malloc(length);
    bool ok = fread(body, 1, length, stdin) == length &&
              ds_message_parse(body, length, id, sequence, out, NULL) == DS_OK;
    g_free(body);
    return ok;
}

static gpointer read_control(gpointer data)
{
    Worker *worker = data;
    for (int64_t seq = 2; seq < INT64_MAX; ++seq) {
        DsMessage message = {0};
        if (!receive(worker->id, seq, &message))
            break;
        DsMessageType type = message.type;
        bool empty = !json_object_get_size(message.payload);
        ds_message_clear(&message);
        if (!empty || (type != DS_MSG_PAUSE && type != DS_MSG_RESUME))
            break;  /* CANCEL, interrupted pipe or wrong direction. */
        ds_control_pause(&worker->control, type == DS_MSG_PAUSE);
    }
    ds_control_cancel(&worker->control);
    return NULL;
}

static void committed(Worker *worker)
{
    if (worker->reported_seq == worker->commit_seq)
        return;
    worker->reported_seq = worker->commit_seq;
    JsonObject *data = json_object_new();
    json_object_set_int_member(data, "commit_seq", worker->commit_seq);
    json_object_set_int_member(data, "observations", worker->observations);
    send(worker, DS_MSG_COMMITTED, data);
    json_object_unref(data);
    worker->reported_at = g_get_monotonic_time();
}

static bool capacity(Worker *worker)
{
    return ds_cache_usage(worker->cache) < worker->cache_budget - DS_CACHE_END_RESERVE &&
           ds_cache_has_reserve(worker->cache);
}

static DsStatus append(const DsEntryBatch *batch, void *data, DsError *error)
{
    Worker *worker = data;
    /* The first commit makes the root visible immediately. */
    if (!worker->commit_seq || g_get_monotonic_time() - worker->reported_at >=
                              DS_BATCH_MSEC * G_TIME_SPAN_MILLISECOND) {
        if (!capacity(worker))
            return ds_error(error, DS_LIMIT, 0, "Scan cache reserve reached");
        if (worker->commit_seq)
            committed(worker);
    }
    DsStatus result = ds_store_append(worker->store, batch, error);
    if (result == DS_OK) {
        ++worker->commit_seq;
        for (size_t i = 0; i < batch->count; ++i)
            worker->observations = MAX(worker->observations, batch->entries[i].id);
        if (worker->commit_seq == 1)
            committed(worker);
    }
    return result;
}

static void lower_priority(void)
{
#ifdef G_OS_WIN32
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#else
    setpriority(PRIO_PROCESS, 0, 5);
#endif
}

static DsStatus capture_volume(const DsVolumeInfo *volume, void *data, DsError *error)
{
    Worker *worker = data;
    if (ds_control_canceled(&worker->control))
        return DS_CANCELED;
    return ds_store_set_volume(worker->store, volume, error);
}

static DsStatus process(Worker *worker, DsScanOptions *options, DsError *error)
{
    worker->cache = ds_path_arg(&options->cache_path, error);
    if (!worker->cache)
        return DS_INVALID;
    if (!g_path_is_absolute(worker->cache) || !capacity(worker))
        return ds_error(error, DS_LIMIT, 0, "Insufficient space in the private cache");

    worker->lease = ds_cache_lease_open(worker->cache, false, false, error);
    if (!worker->lease)
        return error->status;
    char *database = g_build_filename(worker->cache, "scan.sqlite", NULL);
    if (g_file_test(database, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK)) {
        g_free(database);
        return ds_error(error, DS_INVALID, 0, "A scan worker requires an unused cache");
    }
    DsNativePath path = {0};
    DsStatus status = ds_path_from_arg(database, &path, error);
    if (status == DS_OK)
        status = ds_store_open(&path, DS_STORE_WRITER, &worker->store, error);
    ds_path_clear(&path);
    g_free(database);
    if (status != DS_OK)
        return status;

    status = ds_store_set_options(worker->store, options, error);
    if (status != DS_OK)
        return status;
    if (options->profile == DS_CONSERVATIVE)
        lower_priority();
    send_stage(worker, DS_RUNNING);
    /* Ignore all DiskScope artifacts under the configured cache base. */
    DsScanOptions walk = *options;
    walk.cache_path = (DsNativePath){0};
    walk.volume_sink = capture_volume;
    walk.volume_data = worker;
    char *cache_base = g_path_get_dirname(worker->cache);
    status = ds_path_from_arg(cache_base, &walk.cache_path, error);
    g_free(cache_base);
    if (status == DS_OK)
        status = ds_walk(&walk, &worker->control, append, worker, error);
    ds_path_clear(&walk.cache_path);
    if (ds_control_canceled(&worker->control) && (status == DS_OK || status == DS_PARTIAL))
        status = DS_CANCELED;
    if (worker->commit_seq)
        committed(worker);

    if (worker->commit_seq && (status == DS_OK || status == DS_PARTIAL)) {
        if (capacity(worker)) {
            send_stage(worker, DS_FINALIZING);
            DsCoverage coverage = status == DS_OK ? DS_COVERAGE_COMPLETE : DS_COVERAGE_PARTIAL;
            DsStatus finalized = ds_finalize_result(worker->store, status, coverage, &worker->control, error);
            if (finalized == DS_OK)
                return status;
            status = finalized;
        } else {
            status = ds_error(error, DS_LIMIT, 0, "Scan cache reserve prevented final aggregation");
        }
    }
    DsError terminal = {0};
    DsStatus recorded = ds_store_set_result(worker->store, status, DS_COVERAGE_PARTIAL, &terminal);
    if (recorded != DS_OK) {
        *error = terminal;
        return recorded;
    }
    return status;
}

static bool options_from_start(JsonObject *payload, DsScanOptions *options,
                               DsNativePath paths[16], DsError *error)
{
    int64_t profile, budget;
    bool hidden;
    if (!ds_ipc_json_int(payload, "profile", &profile) ||
        (profile != DS_STANDARD && profile != DS_CONSERVATIVE) ||
        !ds_ipc_json_int(payload, "cache_budget", &budget) || budget < 0 ||
        (uint64_t)budget < 2 * DS_CACHE_END_RESERVE ||
        !ds_ipc_json_bool(payload, "include_hidden", &hidden))
        return false;
    ds_scan_options_init(options, (DsProfile)profile);
    options->cache_budget = (uint64_t)budget;
    options->include_hidden = hidden;
    options->exclude_pattern = ds_ipc_json_string(payload, "exclude_pattern");
    if (options->exclude_pattern && strlen(options->exclude_pattern) > 1024)
        return false;
    JsonNode *excluded = json_object_get_member(payload, "exclude");
    if (excluded) {
        if (!JSON_NODE_HOLDS_ARRAY(excluded))
            return false;
        JsonArray *array = json_node_get_array(excluded);
        unsigned count = json_array_get_length(array);
        if (count > 16)
            return false;
        options->exclude_paths = paths;
        for (unsigned i = 0; i < count; ++i) {
            JsonNode *entry = json_array_get_element(array, i);
            if (!JSON_NODE_HOLDS_OBJECT(entry) ||
                !ds_ipc_json_path(json_node_get_object(entry), "path", &paths[i], error))
                return false;
            ++options->exclude_count;
        }
    }
    return ds_ipc_json_path(payload, "root", &options->root, error) &&
           ds_ipc_json_path(payload, "cache", &options->cache_path, error);
}

int main(void)
{
#ifdef G_OS_WIN32
    SetConsoleCtrlHandler(NULL, TRUE);
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#else
    signal(SIGINT, SIG_IGN);
#endif
    DsMessage start = {0};
    if (!receive(NULL, 1, &start) || start.type != DS_MSG_START)
        return 5;
    /* The input thread remains alive until process exit if stdin stays open.
     * It uses static state for the lifetime of this single-scan process. */
    static Worker worker;
    g_mutex_init(&worker.output);
    g_mutex_init(&worker.heartbeat);
    g_cond_init(&worker.stopped);
    ds_control_init(&worker.control);
    worker.control.pause_changed = pause_callback;
    worker.control.pause_data = &worker;
    g_strlcpy(worker.id, start.scan_id, sizeof worker.id);
    DsScanOptions options = {0};
    DsNativePath exclusions[16] = {{0}};
    DsError error = {0};
    if (!options_from_start(start.payload, &options, exclusions, &error))
        return 5;
    worker.cache_budget = options.cache_budget;
    if (!send(&worker, DS_MSG_HELLO, NULL))
        return 5;
    g_thread_unref(g_thread_new("ds-worker-commands", read_control, &worker));
    worker.stage = DS_RUNNING;
    GThread *heartbeat = g_thread_new("ds-worker-heartbeat", beat, &worker);
    DsStatus result = process(&worker, &options, &error);
    if (worker.store)
        ds_store_close(worker.store);
    g_mutex_lock(&worker.heartbeat);
    worker.stop_heartbeat = true;
    g_cond_signal(&worker.stopped);
    g_mutex_unlock(&worker.heartbeat);
    g_thread_join(heartbeat);
    JsonObject *finished = json_object_new();
    json_object_set_int_member(finished, "result", result);
    if (error.message[0]) {
        char *detail = g_utf8_make_valid(error.message, -1);
        if (strlen(detail) < sizeof error.message)
            json_object_set_string_member(finished, "detail", detail);
        g_free(detail);
    }
    bool ok = send(&worker, DS_MSG_FINISHED, finished);
    json_object_unref(finished);
    ds_message_clear(&start);
    ds_path_clear(&options.root);
    ds_path_clear(&options.cache_path);
    for (size_t i = 0; i < options.exclude_count; ++i)
        ds_path_clear(&exclusions[i]);
    if (worker.cache && ds_cache_usage(worker.cache) < worker.cache_budget && ds_cache_has_reserve(worker.cache)) {
        char *log_path = g_build_filename(worker.cache, "diagnostics.txt", NULL);
        char *summary = g_strdup_printf(
            "schema=1\nresult=%d\nobservations=%" G_GINT64_FORMAT "\ncommits=%" G_GINT64_FORMAT
            "\nos_code=%" G_GINT64_FORMAT "\n", result, worker.observations,
            worker.commit_seq, error.os_code);
        g_file_set_contents_full(log_path, summary, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
        g_free(summary);
        g_free(log_path);
    }
    /* The OS releases the worker cache lease at process exit. */
    g_free(worker.cache);
    /* stdout was flushed. A blocked command read must not hold stdio shutdown. */
    _Exit(ok ? 0 : 5);
}

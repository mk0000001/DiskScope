#include "diskscope/ds_session.h"
#include "diskscope/ds_store.h"
#include "local.h"
#include "cache_lease.h"
#include "protocol.h"

#include <gio/gio.h>

struct DsSession {
    gatomicrefcount references;
    GMutex lock;
    GCond changed;
    GSubprocess *child;
    DsNativePath db;
    DsCacheLease *lease;
    char *id;
    DsSessionEvent event;
    DsSessionCallback callback;
    void *user_data;
    bool stopping, paused, cancel, pause_changed;
    int64_t progress_at, cancellation_at;
    JsonObject *start;
};

static gint active_scan;

static DsSession *session_ref(DsSession *session)
{
    g_atomic_ref_count_inc(&session->references);
    return session;
}

void ds_session_unref(DsSession *session)
{
    if (!session || !g_atomic_ref_count_dec(&session->references))
        return;
    g_object_unref(session->child);
    ds_cache_lease_close(session->lease);
    ds_path_clear(&session->db);
    g_free(session->id);
    json_object_unref(session->start);
    g_cond_clear(&session->changed);
    g_mutex_clear(&session->lock);
    g_free(session);
}

void ds_session_event(DsSession *session, DsSessionEvent *event)
{
    g_mutex_lock(&session->lock);
    *event = session->event;
    if (!event->exited) {
        int64_t now = g_get_monotonic_time();
        if (session->cancel && now - session->cancellation_at >= 2 * G_TIME_SPAN_SECOND)
            event->stage = DS_CANCEL_WAITING;
        event->stalled = now - session->progress_at >= 10 * G_TIME_SPAN_SECOND;
    }
    g_mutex_unlock(&session->lock);
}

const DsNativePath *ds_session_db_path(const DsSession *session)
{
    return &session->db;
}

static void notify(DsSession *session)
{
    if (session->callback) {
        DsSessionEvent event;
        ds_session_event(session, &event);
        session->callback(&event, session->user_data);
    }
}

static void request(DsSession *session, DsMessageType command)
{
    if (!session)
        return;
    g_mutex_lock(&session->lock);
    if (!session->stopping && !session->cancel && !session->event.exited) {
        if (command == DS_MSG_CANCEL) {
            session->cancel = true;
            session->cancellation_at = g_get_monotonic_time();
            session->event.stage = DS_CANCEL_REQUESTED;
        } else if (session->event.stage != DS_FINALIZING) {
            session->paused = command == DS_MSG_PAUSE;
            session->pause_changed = true;
            if (session->paused)
                session->event.stage = DS_PAUSE_REQUESTED;
        }
        g_cond_signal(&session->changed);
    }
    g_mutex_unlock(&session->lock);
}

void ds_session_request_pause(DsSession *s) { request(s, DS_MSG_PAUSE); }
void ds_session_request_resume(DsSession *s) { request(s, DS_MSG_RESUME); }
void ds_session_request_cancel(DsSession *s) { request(s, DS_MSG_CANCEL); }

static bool send_message(DsSession *s, DsMessageType type, int64_t seq, JsonObject *obj)
{
    GOutputStream *pipe = g_subprocess_get_stdin_pipe(s->child);
    GBytes *frame = ds_message_frame(type, s->id, seq, obj, NULL);
    if (!frame)
        return false;
    size_t length;
    const void *bytes = g_bytes_get_data(frame, &length);
    bool ok = g_output_stream_write_all(pipe, bytes, length, NULL, NULL, NULL);
    g_bytes_unref(frame);
    return ok;
}

static gpointer send_commands(gpointer ptr)
{
    DsSession *s = ptr;
    int64_t seq = 1;
    bool ok = send_message(s, DS_MSG_START, seq, s->start);
    while (ok) {
        g_mutex_lock(&s->lock);
        while (!s->stopping && !s->cancel && !s->pause_changed)
            g_cond_wait(&s->changed, &s->lock);
        bool end = s->stopping;
        DsMessageType cmd = s->cancel ? DS_MSG_CANCEL : s->paused ? DS_MSG_PAUSE : DS_MSG_RESUME;
        s->pause_changed = false;
        g_mutex_unlock(&s->lock);
        if (end)
            break;
        ok = send_message(s, cmd, ++seq, NULL);
        if (cmd == DS_MSG_CANCEL)
            break;
    }
    g_output_stream_close(g_subprocess_get_stdin_pipe(s->child), NULL, NULL);
    ds_session_unref(s);
    return NULL;
}

static bool handle_event(DsSession *s, const DsMessage *message, bool *hello, bool *finished)
{
    int64_t stage, seq, total, result;
    bool valid = false;
    const char *detail = ds_ipc_json_string(message->payload, "detail");
    if (detail && strlen(detail) >= sizeof s->event.detail)
        return false;
    g_mutex_lock(&s->lock);
    if (!*hello) {
        valid = message->type == DS_MSG_HELLO;
        *hello = valid;
    } else if (!*finished) {
        switch (message->type) {
        case DS_MSG_STATUS:
            valid = ds_ipc_json_int(message->payload, "stage", &stage) &&
                    (stage == DS_RUNNING || stage == DS_PAUSED || stage == DS_FINALIZING);
            if (valid && !s->cancel) {
                DsStage next = (DsStage)stage;
                if (next == DS_RUNNING && s->paused)
                    next = DS_PAUSE_REQUESTED;
                if (s->event.stage != next)
                    s->progress_at = g_get_monotonic_time();
                s->event.stage = next;
            }
            break;
        case DS_MSG_COMMITTED:
            valid = ds_ipc_json_int(message->payload, "commit_seq", &seq) && seq > s->event.commit_seq &&
                    ds_ipc_json_int(message->payload, "observations", &total) && total >= 0 &&
                    (uint64_t)total >= s->event.observations;
            if (valid) {
                s->event.commit_seq = seq;
                s->event.observations = (uint64_t)total;
                s->progress_at = g_get_monotonic_time();
            }
            break;
        case DS_MSG_FINISHED:
            valid = ds_ipc_json_int(message->payload, "result", &result) && result >= DS_OK &&
                    result <= DS_PROTOCOL;
            if (valid) {
                *finished = true;
                s->stopping = true;
                g_cond_signal(&s->changed);
                s->event.result = (DsStatus)result;
            }
            break;
        case DS_MSG_ERROR:
            valid = detail && *detail;
            break;
        default:
            break;
        }
    }
    if (valid && detail)
        g_strlcpy(s->event.detail, detail, sizeof s->event.detail);
    g_mutex_unlock(&s->lock);
    return valid;
}

static bool reconcile_cache(DsSession *s, int result, DsError *error)
{
    DsStore *store = NULL;
    DsStoreInfo info = {0};
    if (ds_store_open(&s->db, DS_STORE_READER, &store, NULL) != DS_OK)
        return true;  /* A worker may have exited before it created the DB. */
    bool read = ds_store_info(store, &info, NULL) == DS_OK;
    ds_store_close(store);
    if (!read)
        return true;
    g_mutex_lock(&s->lock);
    s->event.commit_seq = info.commit_seq;
    s->event.observations = info.entry_count;
    g_mutex_unlock(&s->lock);
    bool ok = true;
    if (result != DS_RUN_RUNNING && info.run_status != result) {
        ok = ds_store_open(&s->db, DS_STORE_WRITER, &store, error) == DS_OK;
        if (ok) {
            ok = ds_store_set_result(store, (DsStatus)result, DS_COVERAGE_PARTIAL, error) == DS_OK;
            ds_store_close(store);
        }
    }
    ds_store_info_clear(&info);
    return ok;
}

static gpointer receive_events(gpointer ptr)
{
    DsSession *s = ptr;
    GInputStream *pipe = g_subprocess_get_stdout_pipe(s->child);
    int64_t seq = 0;
    bool hello = false, finished = false, valid = true;
    for (;;) {
        uint8_t header[4];
        size_t read = 0, length = 0;
        valid = g_input_stream_read_all(pipe, header, sizeof header, &read, NULL, NULL);
        if (!valid || read == 0)
            break;
        valid = read == sizeof header && ds_frame_length(header, &length);
        if (!valid)
            break;
        uint8_t *body = g_malloc(length);
        DsMessage message = {0};
        valid = g_input_stream_read_all(pipe, body, length, &read, NULL, NULL) &&
                read == length && seq < INT64_MAX &&
                ds_message_parse(body, length, s->id, ++seq, &message, NULL) == DS_OK &&
                handle_event(s, &message, &hello, &finished);
        ds_message_clear(&message);
        g_free(body);
        if (!valid)
            break;
        if (finished) {
            g_mutex_lock(&s->lock);
            s->stopping = true;
            g_cond_signal(&s->changed);
            g_mutex_unlock(&s->lock);
        } else {
            notify(s);
        }
    }
    if (!valid)
        g_subprocess_force_exit(s->child);
    g_mutex_lock(&s->lock);
    s->stopping = true;
    g_cond_signal(&s->changed);
    g_mutex_unlock(&s->lock);
    bool normal = g_subprocess_wait(s->child, NULL, NULL) &&
                  g_subprocess_get_successful(s->child) && valid && hello && finished;
    g_mutex_lock(&s->lock);
    bool late_cancel = normal && s->cancel &&
                       (s->event.result == DS_OK || s->event.result == DS_PARTIAL);
    g_mutex_unlock(&s->lock);
    DsError recovery = {0};
    bool reconciled = reconcile_cache(s, !normal ? DS_IO : late_cancel ? DS_CANCELED : DS_RUN_RUNNING,
                                      &recovery);
    g_clear_pointer(&s->lease, ds_cache_lease_close);
    g_mutex_lock(&s->lock);
    s->event.exited = true;
    if (!normal) {
        s->event.stage = DS_WORKER_LOST;
        s->event.result = DS_IO;
        g_strlcpy(s->event.detail, valid ? "Worker ended without a complete status" :
                   "Worker sent invalid IPC", sizeof s->event.detail);
    } else {
        s->event.stage = DS_FINISHED;
        if (late_cancel)
            s->event.result = reconciled ? DS_CANCELED : recovery.status;
        if (!reconciled)
            g_strlcpy(s->event.detail, recovery.message, sizeof s->event.detail);
    }
    g_atomic_int_set(&active_scan, 0);
    g_mutex_unlock(&s->lock);
    notify(s);
    ds_session_unref(s);
    return NULL;
}

static JsonObject *start_payload(const DsScanOptions *options, const DsNativePath *root,
                                const DsNativePath *cache)
{
    JsonObject *data = json_object_new();
    ds_ipc_json_set_path(data, "root", root);
    ds_ipc_json_set_path(data, "cache", cache);
    json_object_set_int_member(data, "profile", options->profile);
    json_object_set_int_member(data, "cache_budget", (gint64)options->cache_budget);
    json_object_set_boolean_member(data, "include_hidden", options->include_hidden);
    if (options->exclude_pattern)
        json_object_set_string_member(data, "exclude_pattern", options->exclude_pattern);
    if (options->exclude_count) {
        JsonArray *paths = json_array_new();
        for (size_t i = 0; i < options->exclude_count; ++i) {
            JsonObject *item = json_object_new();
            ds_ipc_json_set_path(item, "path", &options->exclude_paths[i]);
            json_array_add_object_element(paths, item);
        }
        json_object_set_array_member(data, "exclude", paths);
    }
    return data;
}

static char *absolute_root(const DsNativePath *path, DsError *error)
{
    char *arg = ds_path_arg(path, error);
    if (!arg)
        return NULL;
#ifdef G_OS_WIN32
    if (g_ascii_isalpha(arg[0]) && arg[1] == ':' && arg[2] == '\0') {
        char *drive = g_strconcat(arg, "\\", NULL);
        g_free(arg);
        return drive;
    }
    /* Preserve extended Win32 spelling and avoid reinterpretation by GLib. */
    if (g_str_has_prefix(arg, "\\\\?\\"))
        return arg;
#endif
    char *root = g_canonicalize_filename(arg, NULL);
    g_free(arg);
    return root;
}

DsSession *ds_session_start(const DsScanOptions *options, DsSessionCallback callback,
                            void *data, DsError *error)
{
    if (!options || options->profile > DS_STANDARD ||
        options->cache_budget < 2 * DS_CACHE_END_RESERVE || options->cache_budget > INT64_MAX ||
        options->exclude_count > 16 ||
        (options->exclude_count && !options->exclude_paths) ||
        (options->exclude_pattern && (strlen(options->exclude_pattern) > 1024 ||
          !g_utf8_validate(options->exclude_pattern, -1, NULL)))) {
        ds_error(error, DS_INVALID, 0, "Invalid scan options");
        return NULL;
    }
    if (!g_atomic_int_compare_and_exchange(&active_scan, 0, 1)) {
        ds_error(error, DS_LIMIT, 0, "A worker is still running");
        return NULL;
    }
    char *worker = ds_worker_binary(error);
    char *root = worker ? absolute_root(&options->root, error) : NULL;
    char *cache = root ? ds_private_cache(&options->cache_path, error) : NULL;
    DsCacheLease *lease = cache ? ds_cache_lease_open(cache, true, false, error) : NULL;
    char *database = lease ? g_build_filename(cache, "scan.sqlite", NULL) : NULL;
    DsNativePath native_root = {0}, native_cache = {0}, native_db = {0};
    DsSession *s = NULL;
    if (database && ds_path_from_arg(root, &native_root, error) == DS_OK &&
        ds_path_from_arg(cache, &native_cache, error) == DS_OK &&
        ds_path_from_arg(database, &native_db, error) == DS_OK) {
        GError *failure = NULL;
        GSubprocess *child = g_subprocess_new(
            G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
            G_SUBPROCESS_FLAGS_STDERR_SILENCE, &failure, worker, NULL);
        if (child) {
            s = g_new0(DsSession, 1);
            g_atomic_ref_count_init(&s->references);
            g_mutex_init(&s->lock);
            g_cond_init(&s->changed);
            s->lease = lease;
            lease = NULL;
            s->db = native_db;
            native_db = (DsNativePath){0};
            s->child = child;
            s->callback = callback;
            s->user_data = data;
            s->id = g_uuid_string_random();
            s->event = (DsSessionEvent){.stage = DS_PREPARING, .result = DS_IO};
            s->progress_at = g_get_monotonic_time();
            s->start = start_payload(options, &native_root, &native_cache);
            g_thread_unref(g_thread_new("ds-controller-write", send_commands, session_ref(s)));
            g_thread_unref(g_thread_new("ds-controller-read", receive_events, session_ref(s)));
        } else {
            ds_error(error, DS_IO, failure->code, "%s", failure->message);
            g_error_free(failure);
        }
    }
    ds_cache_lease_close(lease);
    g_free(database);
    g_free(cache);
    g_free(root);
    g_free(worker);
    ds_path_clear(&native_db);
    ds_path_clear(&native_cache);
    ds_path_clear(&native_root);
    if (!s)
        g_atomic_int_set(&active_scan, 0);
    return s;
}

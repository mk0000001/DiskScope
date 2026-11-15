#include "protocol.h"
#include "diskscope/ds_session.h"
#include <stdio.h>

#ifdef G_OS_WIN32
#include <fcntl.h>
#include <io.h>
#endif

static bool receive(const char *id, int64_t seq, DsMessage *msg)
{
    uint8_t header[4];
    size_t length;
    if (fread(header, 1, sizeof header, stdin) != sizeof header || !ds_frame_length(header, &length))
        return false;
    void *body = g_malloc(length);
    bool valid = fread(body, 1, length, stdin) == length &&
                 ds_message_parse(body, length, id, seq, msg, NULL) == DS_OK;
    g_free(body);
    return valid;
}

static void send(const char *id, int64_t seq, DsMessageType type, JsonObject *payload)
{
    GBytes *bytes = ds_message_frame(type, id, seq, payload, NULL);
    size_t size;
    const void *data = g_bytes_get_data(bytes, &size);
    g_assert_cmpuint(fwrite(data, 1, size, stdout), ==, size);
    fflush(stdout);
    g_bytes_unref(bytes);
}

static void numeric(const char *id, int64_t seq, DsMessageType type, const char *key, int64_t value)
{
    JsonObject *payload = json_object_new();
    json_object_set_int_member(payload, key, value);
    send(id, seq, type, payload);
    json_object_unref(payload);
}

int main(void)
{
#ifdef G_OS_WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    DsMessage start = {0};
    if (!receive(NULL, 1, &start) || start.type != DS_MSG_START)
        return 5;
    DsNativePath root = {0};
    if (!ds_ipc_json_path(start.payload, "root", &root, NULL))
        return 5;
    char *arg = ds_path_arg(&root, NULL);
    bool crash = arg && g_str_has_suffix(arg, "crash");
    g_free(arg);
    ds_path_clear(&root);

    const char *id = start.scan_id;
    send(id, 1, DS_MSG_HELLO, NULL);
    numeric(id, 2, DS_MSG_STATUS, "stage", DS_RUNNING);
    int64_t seq = 2;
    if (crash) {
        ds_message_clear(&start);
        return 5;
    }

    for (int64_t command = 2; command < 12; ++command) {
        DsMessage control = {0};
        if (!receive(id, command, &control))
            return 5;
        DsMessageType type = control.type;
        ds_message_clear(&control);
        if (type == DS_MSG_CANCEL) break;
        if (type != DS_MSG_RESUME && type != DS_MSG_PAUSE) return 5;
        numeric(id, ++seq, DS_MSG_STATUS, "stage", type == DS_MSG_PAUSE ? DS_PAUSED : DS_RUNNING);
    }
    /* Stand in for an in-flight system call that returns after the grace period. */
    g_usleep(2300000);
    JsonObject *committed = json_object_new();
    json_object_set_int_member(committed, "commit_seq", 1);
    json_object_set_int_member(committed, "observations", 17);
    send(id, ++seq, DS_MSG_COMMITTED, committed);
    json_object_unref(committed);
    g_usleep(500000);
    numeric(id, ++seq, DS_MSG_FINISHED, "result", DS_CANCELED);
    ds_message_clear(&start);
    return 0;
}

#ifndef DS_PROTOCOL_H
#define DS_PROTOCOL_H

#include "diskscope/ds_scan.h"
#include <json-glib/json-glib.h>

#define DS_FRAME_MAX (1024u * 1024u)

typedef enum {
    DS_MSG_HELLO, DS_MSG_START, DS_MSG_PAUSE, DS_MSG_RESUME, DS_MSG_CANCEL,
    DS_MSG_STATUS, DS_MSG_COMMITTED, DS_MSG_FINISHED, DS_MSG_ERROR, DS_MSG_COUNT
} DsMessageType;

typedef struct {
    DsMessageType type;
    int64_t sequence;
    char scan_id[37];
    JsonObject *payload;
} DsMessage;

bool ds_frame_length(const uint8_t header[4], size_t *length);
GBytes *ds_message_frame(DsMessageType type, const char *scan_id, int64_t sequence,
                         JsonObject *payload, DsError *error);
DsStatus ds_message_parse(const void *data, size_t length, const char *expected_id,
                          int64_t expected_sequence, DsMessage *out, DsError *error);
void ds_message_clear(DsMessage *message);
const char *ds_ipc_json_string(JsonObject *obj, const char *key);
bool ds_ipc_json_int(JsonObject *obj, const char *key, int64_t *value);
bool ds_ipc_json_bool(JsonObject *obj, const char *key, bool *value);
bool ds_ipc_json_path(JsonObject *obj, const char *key, DsNativePath *path, DsError *error);
void ds_ipc_json_set_path(JsonObject *obj, const char *key, const DsNativePath *path);

#endif

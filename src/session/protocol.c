#include "protocol.h"

static const char *const types[] = {
    "HELLO", "START", "PAUSE", "RESUME", "CANCEL", "STATUS", "COMMITTED", "FINISHED", "ERROR"
};

bool ds_frame_length(const uint8_t header[4], size_t *length)
{
    uint32_t size = (uint32_t)header[0] << 24 | (uint32_t)header[1] << 16 |
                    (uint32_t)header[2] << 8 | header[3];
    if (size == 0 || size > DS_FRAME_MAX)
        return false;
    *length = size;
    return true;
}

GBytes *ds_message_frame(DsMessageType type, const char *scan_id, int64_t sequence,
                         JsonObject *payload, DsError *error)
{
    if ((unsigned)type >= G_N_ELEMENTS(types) || !scan_id ||
        !g_uuid_string_is_valid(scan_id) || sequence <= 0) {
        ds_error(error, DS_PROTOCOL, 0, "Invalid outgoing IPC envelope");
        return NULL;
    }
    JsonObject *object = json_object_new();
    json_object_set_int_member(object, "protocol_version", 1);
    json_object_set_string_member(object, "scan_id", scan_id);
    json_object_set_int_member(object, "sequence", sequence);
    json_object_set_string_member(object, "type", types[type]);
    json_object_set_object_member(object, "payload", payload ? json_object_ref(payload) : json_object_new());

    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, object);
    json_generator_set_root(generator, root);
    size_t length;
    char *json = json_generator_to_data(generator, &length);
    json_node_unref(root);
    g_object_unref(generator);
    GBytes *result = NULL;
    if (length != 0 && length <= DS_FRAME_MAX) {
        uint8_t *frame = g_malloc(length + 4);
        for (size_t i = 0; i < 4; ++i)
            frame[i] = (uint8_t)(length >> ((3 - i) * 8));
        memcpy(frame + 4, json, length);
        result = g_bytes_new_take(frame, length + 4);
    } else {
        ds_error(error, DS_PROTOCOL, 0, "Outgoing IPC payload exceeds frame limit");
    }
    g_free(json);
    return result;
}

/* Inspect the bounded frame before turning it into a JSON DOM. */
static bool shape_valid(const char *json, size_t length)
{
    if (!json || !length || length > DS_FRAME_MAX ||
        !g_utf8_validate(json, (gssize)length, NULL))
        return false;
    char delimiters[16];
    unsigned depth = 0, separators = 0, tokens = 0;
    bool quoted = false;
    for (size_t i = 0; i < length; ++i) {
        char c = json[i];
        if (quoted) {
            if ((unsigned char)c < 0x20)
                return false;
            if (c == '"') {
                quoted = false;
            } else if (c == '\\') {
                if (++i == length)
                    return false;
                if (json[i] == 'u' && length - i >= 5 && memcmp(json + i + 1, "0000", 4) == 0)
                    return false;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{' || c == '[') {
            if (depth == G_N_ELEMENTS(delimiters) || ++tokens > 128)
                return false;
            delimiters[depth++] = c == '{' ? '}' : ']';
        } else if (c == '}' || c == ']') {
            if (!depth || c != delimiters[--depth])
                return false;
        } else if (c == ',') {
            if (++separators > 128)
                return false;
        } else if (!strchr(":0123456789+-.eEtruefalsn \t\r\n", c)) {
            return false;
        }
    }
    return !quoted && depth == 0;
}

static JsonNode *typed_value(JsonObject *obj, const char *key, GType type)
{
    JsonNode *n = obj ? json_object_get_member(obj, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == type ? n : NULL;
}

const char *ds_json_string(JsonObject *obj, const char *key)
{
    JsonNode *n = typed_value(obj, key, G_TYPE_STRING);
    return n ? json_node_get_string(n) : NULL;
}

bool ds_json_int(JsonObject *obj, const char *key, int64_t *value)
{
    JsonNode *n = typed_value(obj, key, G_TYPE_INT64);
    if (!n)
        return false;
    *value = json_node_get_int(n);
    return true;
}

bool ds_json_bool(JsonObject *obj, const char *key, bool *value)
{
    JsonNode *n = typed_value(obj, key, G_TYPE_BOOLEAN);
    if (!n)
        return false;
    *value = json_node_get_boolean(n);
    return true;
}

DsStatus ds_message_parse(const void *data, size_t length, const char *expected_id,
                          int64_t expected_sequence, DsMessage *out, DsError *error)
{
    if (!out || !shape_valid(data, length))
        return ds_error(error, DS_PROTOCOL, 0, "IPC JSON violates size, encoding or nesting policy");
    JsonParser *parser = json_parser_new();
    bool valid = json_parser_load_from_data(parser, data, (gssize)length, NULL);
    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = valid && root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
    int64_t version = 0, sequence = 0;
    const char *id = ds_json_string(obj, "scan_id");
    const char *type = ds_json_string(obj, "type");
    JsonNode *payload = obj ? json_object_get_member(obj, "payload") : NULL;
    DsMessageType found = DS_MSG_COUNT;
    for (unsigned i = 0; i < G_N_ELEMENTS(types); ++i)
        if (type && strcmp(types[i], type) == 0)
            found = (DsMessageType)i;
    valid = obj && json_object_get_size(obj) == 5 &&
            ds_json_int(obj, "protocol_version", &version) && version == 1 &&
            ds_json_int(obj, "sequence", &sequence) && sequence > 0 &&
            sequence == expected_sequence && id && g_uuid_string_is_valid(id) &&
            (!expected_id || strcmp(id, expected_id) == 0) &&
            found != DS_MSG_COUNT && payload && JSON_NODE_HOLDS_OBJECT(payload);
    if (valid) {
        *out = (DsMessage){.type = found, .sequence = sequence};
        g_strlcpy(out->scan_id, id, sizeof out->scan_id);
        out->payload = json_object_ref(json_node_get_object(payload));
    }
    g_object_unref(parser);
    return valid ? DS_OK : ds_error(error, DS_PROTOCOL, 0, "Invalid IPC envelope");
}

void ds_message_clear(DsMessage *message)
{
    g_clear_pointer(&message->payload, json_object_unref);
}

bool ds_json_path(JsonObject *obj, const char *key, DsNativePath *path, DsError *error)
{
    JsonNode *node = json_object_get_member(obj, key);
    JsonObject *raw = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    const char *text = ds_json_string(raw, "base64");
    int64_t encoding;
    if (!text || strlen(text) > ((DS_PATH_MAX + 2) / 3) * 4 ||
        !ds_json_int(raw, "encoding", &encoding) ||
        (encoding != DS_PATH_BYTES && encoding != DS_PATH_UTF16LE)) {
        ds_error(error, DS_PROTOCOL, 0, "Malformed IPC path");
        return false;
    }
    size_t length;
    uint8_t *data = g_base64_decode(text, &length);
    char *canonical = g_base64_encode(data, length);
    bool valid = strcmp(canonical, text) == 0 &&
                 ds_path_new((DsEncoding)encoding, data, length, path, error) == DS_OK;
    g_free(canonical);
    g_free(data);
    return valid;
}

void ds_json_set_path(JsonObject *obj, const char *key, const DsNativePath *path)
{
    size_t size;
    const uint8_t *raw = g_bytes_get_data(path->raw, &size);
    char *encoded = g_base64_encode(raw, size);
    JsonObject *value = json_object_new();
    json_object_set_int_member(value, "encoding", path->encoding);
    json_object_set_string_member(value, "base64", encoded);
    json_object_set_object_member(obj, key, value);
    g_free(encoded);
}

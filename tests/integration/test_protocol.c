#include "protocol.h"

static void framing(void)
{
    size_t length = 0;
    g_assert_true(ds_frame_length((uint8_t[]){0,16,0,0}, &length));
    g_assert_cmpuint(length, ==, DS_FRAME_MAX);
    g_assert_false(ds_frame_length((uint8_t[]){0,16,0,1}, &length));
    g_assert_false(ds_frame_length((uint8_t[]){0,0,0,0}, &length));
}

static void envelope(void)
{
    const char *id = "a4544971-23d1-4d6a-96a1-ef9f2530f030";
    DsMessage message = {0};
    GBytes *frame = ds_message_frame(DS_MSG_START, id, 1, NULL, NULL);
    g_assert_nonnull(frame);
    size_t n;
    const uint8_t *encoded = g_bytes_get_data(frame, &n);
    g_assert_cmpint(ds_message_parse(encoded + 4, n - 4, id, 1, &message, NULL), ==, DS_OK);
    g_assert_cmpint(message.type, ==, DS_MSG_START);
    ds_message_clear(&message);
    g_assert_cmpint(ds_message_parse(encoded + 4, n - 5, id, 1, &message, NULL), ==, DS_PROTOCOL);
    g_assert_cmpint(ds_message_parse(encoded + 4, n - 4, id, 2, &message, NULL), ==, DS_PROTOCOL);
    g_assert_cmpint(ds_message_parse(encoded + 4, n - 4, "wrong", 1, &message, NULL), ==, DS_PROTOCOL);
    g_bytes_unref(frame);
}

static void invalid_json(void)
{
    DsMessage message = {0};
    GString *doc = g_string_new(
        "{\"protocol_version\":1,\"scan_id\":\"a4544971-23d1-4d6a-96a1-ef9f2530f030\","
        "\"sequence\":1,\"type\":\"START\",\"payload\":{\"field\":");
    for (int i = 0; i < 15; ++i)
        g_string_append_c(doc, '[');
    g_string_append(doc, "0");
    for (int i = 0; i < 15; ++i)
        g_string_append_c(doc, ']');
    g_string_append(doc, "}}");
    g_assert_cmpint(ds_message_parse(doc->str, doc->len, NULL, 1, &message, NULL), ==, DS_PROTOCOL);
    g_string_free(doc, true);
    const char *bad = "{\"malformed\":}";
    g_assert_cmpint(ds_message_parse(bad, strlen(bad), NULL, 1, &message, NULL), ==, DS_PROTOCOL);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/header_limits", framing);
    g_test_add_func("/envelope", envelope);
    g_test_add_func("/nesting_and_invalid_json", invalid_json);
    return g_test_run();
}

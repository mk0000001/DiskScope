#include "diskscope/ds_path.h"

static unsigned unit(const uint8_t *bytes, size_t i)
{
    return (unsigned)bytes[i] | (unsigned)bytes[i + 1] << 8;
}

DsStatus ds_path_new(DsEncoding encoding, const void *bytes, size_t length,
                     DsNativePath *out, DsError *error)
{
    if (!out || (!bytes && length))
        return ds_error(error, DS_INVALID, 0, "Missing native path or output");
    if (length > DS_PATH_MAX)
        return ds_error(error, DS_LIMIT, 0, "Native path exceeds 256 KiB");
    if (encoding != DS_PATH_BYTES && encoding != DS_PATH_UTF16LE)
        return ds_error(error, DS_INVALID, 0, "Unknown native path encoding");
    if (encoding == DS_PATH_UTF16LE && length % 2)
        return ds_error(error, DS_INVALID, 0, "Incomplete UTF-16 code unit");
    if (encoding == DS_PATH_UTF16LE) {
        for (size_t i = 0; i < length; i += 2)
            if (!unit(bytes, i))
                return ds_error(error, DS_INVALID, 0, "NUL in UTF-16 path");
    } else if (length && memchr(bytes, 0, length)) {
        return ds_error(error, DS_INVALID, 0, "NUL in native path");
    }
    *out = (DsNativePath){encoding, g_bytes_new(bytes, length)};
    return DS_OK;
}

DsStatus ds_path_from_arg(const char *arg, DsNativePath *out, DsError *error)
{
    if (!arg || !*arg)
        return ds_error(error, DS_INVALID, 0, "An empty path is not a scan target");
#ifdef G_OS_WIN32
    glong units;
    gunichar2 *wide = g_utf8_to_utf16(arg, -1, NULL, &units, NULL);
    if (!wide)
        return ds_error(error, DS_INVALID, 0, "Invalid UTF-8 argument");
    for (glong i = 0; i < units; ++i)
        wide[i] = GUINT16_TO_LE(wide[i]);
    DsStatus status = ds_path_new(DS_PATH_UTF16LE, wide, (size_t)units * 2, out, error);
    g_free(wide);
    return status;
#else
    return ds_path_new(DS_PATH_BYTES, arg, strlen(arg), out, error);
#endif
}

static void display_char(GString *text, gunichar c)
{
    if (c == '\\')
        g_string_append(text, "\\\\");
    else if (g_unichar_iscntrl(c) || g_unichar_type(c) == G_UNICODE_FORMAT)
        g_string_append_printf(text, "\\u%04X", c);
    else
        g_string_append_unichar(text, c);
}

DsStatus ds_path_display(const DsNativePath *path, char **out_utf8, DsError *error)
{
    if (!out_utf8 || !path || !path->raw ||
        (path->encoding != DS_PATH_BYTES && path->encoding != DS_PATH_UTF16LE))
        return ds_error(error, DS_INVALID, 0, "Missing or invalid native path");
    size_t length;
    const uint8_t *bytes = g_bytes_get_data(path->raw, &length);
    GString *display = g_string_sized_new(length);
    if (path->encoding == DS_PATH_BYTES) {
        for (size_t i = 0; i < length;) {
            const char *start = (const char *)bytes + i;
            gunichar c = g_utf8_get_char_validated(start, (gssize)(length - i));
            if (c == (gunichar)-1 || c == (gunichar)-2) {
                g_string_append_printf(display, "\\x%02X", bytes[i++]);
            } else {
                display_char(display, c);
                i += (size_t)(g_utf8_next_char(start) - start);
            }
        }
    } else {
        for (size_t i = 0; i + 1 < length; i += 2) {
            unsigned c = unit(bytes, i);
            if (c >= 0xd800 && c <= 0xdbff && i + 3 < length) {
                unsigned low = unit(bytes, i + 2);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                    i += 2;
                }
            }
            if (c >= 0xd800 && c <= 0xdfff)
                g_string_append_printf(display, "\\u%04X", c);
            else if (c == '\\')
                g_string_append_c(display, '\\');
            else
                display_char(display, c);
        }
    }
    *out_utf8 = g_string_free(display, false);
    return DS_OK;
}

char *ds_path_arg(const DsNativePath *path, DsError *error)
{
    if (!path || !path->raw || !g_bytes_get_size(path->raw)) {
        ds_error(error, DS_INVALID, 0, "Missing or empty local path");
        return NULL;
    }
    size_t length;
    const uint8_t *bytes = g_bytes_get_data(path->raw, &length);
#ifdef G_OS_WIN32
    if (path->encoding == DS_PATH_UTF16LE) {
        gunichar2 *units = g_new(gunichar2, length / 2);
        for (size_t i = 0; i < length; i += 2)
            units[i / 2] = (gunichar2)unit(bytes, i);
        char *utf8 = g_utf16_to_utf8(units, (glong)(length / 2), NULL, NULL, NULL);
        g_free(units);
        if (!utf8)
            ds_error(error, DS_UNSUPPORTED, 0, "Path cannot be sent through a UTF-8 API");
        return utf8;
    }
#else
    if (path->encoding == DS_PATH_BYTES)
        return g_strndup((const char *)bytes, length);
#endif
    ds_error(error, DS_INVALID, 0, "Path uses another platform's encoding");
    return NULL;
}

bool ds_path_component_valid(DsEncoding encoding, const void *bytes, size_t length)
{
    const uint8_t *data = bytes;
    if (!data || !length || length > DS_PATH_MAX)
        return false;
    if (encoding == DS_PATH_UTF16LE && length % 2 == 0) {
        if (unit(data, 0) == '.' && (length == 2 || (length == 4 && unit(data, 2) == '.')))
            return false;
        for (size_t i = 0; i < length; i += 2) {
            unsigned c = unit(data, i);
            if (c == 0 || c == '/' || c == '\\' || c == ':')
                return false;
        }
        return true;
    }
    return encoding == DS_PATH_BYTES && !memchr(data, 0, length) &&
           !memchr(data, '/', length) &&
           !(data[0] == '.' && (length == 1 || (length == 2 && data[1] == '.')));
}

void ds_path_clear(DsNativePath *path)
{
    if (path) {
        g_clear_pointer(&path->raw, g_bytes_unref);
        path->encoding = 0;
    }
}

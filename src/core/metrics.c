#include "diskscope/ds_types.h"

#include <stdarg.h>

bool ds_u64_add(uint64_t a, uint64_t b, uint64_t *out)
{
    if (!out || UINT64_MAX - a < b)
        return false;
    *out = a + b;
    return true;
}

bool ds_u64_mul(uint64_t a, uint64_t b, uint64_t *out)
{
    if (!out || (b && a > UINT64_MAX / b))
        return false;
    *out = a * b;
    return true;
}

void ds_u64_encode(uint64_t value, uint8_t out[8])
{
    for (size_t i = 0; i < 8; ++i)
        out[i] = (uint8_t)(value >> ((7 - i) * 8));
}

bool ds_u64_decode(const void *bytes, size_t length, uint64_t *out)
{
    if (!bytes || length != 8 || !out)
        return false;
    const uint8_t *encoded = bytes;
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | encoded[i];
    *out = value;
    return true;
}

const char *ds_quality_name(DsQuality quality)
{
    static const char *const names[] = {
        "known", "unavailable", "unsupported", "overflow", "changed"
    };
    return (unsigned)quality < G_N_ELEMENTS(names) ? names[quality] : "invalid";
}

DsStatus ds_error(DsError *error, DsStatus status, int64_t code,
                  const char *format, ...)
{
    if (error) {
        error->status = status;
        error->os_code = code;
        va_list args;
        va_start(args, format);
        g_vsnprintf(error->message, sizeof error->message, format, args);
        va_end(args);
    }
    return status;
}

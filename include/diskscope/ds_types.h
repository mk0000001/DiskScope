#ifndef DISKSCOPE_TYPES_H
#define DISKSCOPE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <glib.h>

typedef enum {
    DS_OK, DS_PARTIAL, DS_CANCELED, DS_INVALID, DS_IO, DS_LIMIT, DS_PROTOCOL
} DsStatus;

typedef struct {
    DsStatus status;
    int64_t os_code;
    char message[512];
} DsError;

typedef enum {
    DS_KNOWN, DS_UNAVAILABLE, DS_UNSUPPORTED, DS_OVERFLOW, DS_CHANGED
} DsQuality;

typedef enum {
    DS_LOGICAL, DS_ALLOCATED, DS_UNIQUE, DS_METRIC_COUNT
} DsMetric;

typedef struct {
    uint64_t bytes;
    DsQuality quality;
} DsSize;

bool ds_u64_add(uint64_t a, uint64_t b, uint64_t *out);
bool ds_u64_mul(uint64_t a, uint64_t b, uint64_t *out);
void ds_u64_encode(uint64_t value, uint8_t out[8]);
bool ds_u64_decode(const void *bytes, size_t length, uint64_t *out);
const char *ds_quality_name(DsQuality quality);
DsStatus ds_error(DsError *error, DsStatus status, int64_t code,
                  const char *format, ...) G_GNUC_PRINTF(4, 5);

#endif

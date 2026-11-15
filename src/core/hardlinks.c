#include "../store/store_internal.h"

typedef struct {
    uint8_t identity[24];
    int64_t first, representative;
    DsNativePath path;
    DsSize seen[2];
    bool changed;
} FileObject;

static size_t component_end(const uint8_t *raw, size_t length, size_t start, size_t unit)
{
    for (size_t i = start; i < length; i += unit)
        if (unit == 1 ? raw[i] == '/' : raw[i] == '\\' && raw[i + 1] == 0) return i;
    return length;
}

static int compare_paths(const DsNativePath *lhs, const DsNativePath *rhs)
{
    size_t llen = 0, rlen = 0;
    const uint8_t *left = g_bytes_get_data(lhs->raw, &llen);
    const uint8_t *right = g_bytes_get_data(rhs->raw, &rlen);
    size_t l = 0, r = 0, unit = lhs->encoding == DS_PATH_BYTES ? 1 : 2;
    while (l < llen && r < rlen) {
        size_t le = component_end(left, llen, l, unit);
        size_t re = component_end(right, rlen, r, unit);
        size_t a = le - l, b = re - r;
        int order = memcmp(left + l, right + r, MIN(a, b));
        if (order) return order;
        if (a != b) return a < b ? -1 : 1;
        l = le + unit;
        r = re + unit;
    }
    return (l < llen) - (r < rlen);
}

static int record_object(sqlite3_stmt *stmt, const FileObject *object)
{
    if (!object->first) return SQLITE_OK;
    sqlite3_reset(stmt);
    int rc = sqlite3_bind_blob(stmt, 2, object->identity, sizeof object->identity, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 3, object->representative);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 4, object->changed);
    if (rc == SQLITE_OK) rc = ds_sql_step_done(stmt);
    return rc;
}

static int path_result(DsStatus status)
{
    switch (status) {
    case DS_OK: return SQLITE_OK;
    case DS_CANCELED: return SQLITE_INTERRUPT;
    case DS_LIMIT: return SQLITE_TOOBIG;
    case DS_IO: return SQLITE_IOERR;
    default: return SQLITE_CORRUPT;
    }
}

static int consider_path(DsStore *store, FileObject *object, int64_t id)
{
    if (!object->path.raw) {
        DsStatus status = ds_store_relative_path(store, object->first, &object->path, NULL);
        if (status != DS_OK) return path_result(status);
    }
    DsNativePath candidate = {0};
    DsStatus status = ds_store_relative_path(store, id, &candidate, NULL);
    if (status != DS_OK) return path_result(status);
    if (compare_paths(&candidate, &object->path) < 0) {
        ds_path_clear(&object->path);
        object->path = candidate;
        object->representative = id;
    } else {
        ds_path_clear(&candidate);
    }
    return SQLITE_OK;
}

static bool observe_sizes(FileObject *object, sqlite3_stmt *stmt)
{
    for (size_t i = 0; i < G_N_ELEMENTS(object->seen); ++i) {
        DsSize size;
        if (!ds_sql_read_size(stmt, 2 + 2 * (int)i, &size)) return false;
        if (size.quality == DS_CHANGED) object->changed = true;
        if (size.quality != DS_KNOWN) continue;
        DsSize *seen = &object->seen[i];
        if (seen->quality == DS_KNOWN && seen->bytes != size.bytes)
            object->changed = true;
        *seen = size;
    }
    return true;
}

int ds_hardlinks_finalize(DsStore *store, int64_t generation, DsControl *control)
{
    sqlite3_stmt *files = NULL, *insert = NULL;
    int rc = ds_sql_prepare(store, &files,
        "SELECT id,identity,logical,logical_quality,allocated,allocated_quality FROM entries"
        " WHERE kind=2 AND identity IS NOT NULL ORDER BY identity,id");
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &insert,
        "INSERT INTO file_objects(generation,identity,representative_id,changed) VALUES(?1,?2,?3,?4)");
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(insert, 1, generation);
    FileObject object = {0};
    size_t processed = 0;
    while (rc == SQLITE_OK && (rc = sqlite3_step(files)) == SQLITE_ROW) {
        if (++processed % DS_BATCH_MAX == 0) {
            if (control && !ds_control_checkpoint(control)) { rc = SQLITE_INTERRUPT; break; }
            rc = ds_store_check_budget(store, false);
            if (rc != SQLITE_OK) break;
        }
        int64_t id = sqlite3_column_int64(files, 0);
        const void *identity = sqlite3_column_blob(files, 1);
        if ((size_t)sqlite3_column_bytes(files, 1) != sizeof object.identity) {
            rc = SQLITE_CORRUPT;
            break;
        }
        if (object.first && !memcmp(object.identity, identity, sizeof object.identity)) {
            rc = consider_path(store, &object, id);
        } else {
            rc = record_object(insert, &object);
            ds_path_clear(&object.path);
            object = (FileObject){.first = id, .representative = id,
                                  .seen = {{0, DS_UNAVAILABLE}, {0, DS_UNAVAILABLE}}};
            memcpy(object.identity, identity, sizeof object.identity);
        }
        if (rc == SQLITE_OK && !observe_sizes(&object, files)) rc = SQLITE_CORRUPT;
    }
    if (rc == SQLITE_DONE) rc = record_object(insert, &object);
    ds_path_clear(&object.path);
    sqlite3_finalize(insert);
    sqlite3_finalize(files);
    return rc;
}

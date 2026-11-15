#include "store_internal.h"

static bool valid_size(DsSize size)
{
    return size.quality >= DS_KNOWN && size.quality <= DS_CHANGED &&
           (size.quality == DS_KNOWN || size.bytes == 0);
}

static bool fits_total(DsSize size, DsSize total)
{
    return size.quality != DS_KNOWN || total.quality != DS_KNOWN || size.bytes <= total.bytes;
}

static bool valid_volume(const DsVolumeInfo *volume)
{
    return volume->captured_at > 0 &&
           valid_size(volume->total) && valid_size(volume->used) && valid_size(volume->available) &&
           fits_total(volume->used, volume->total) && fits_total(volume->available, volume->total);
}

DsStatus ds_store_set_volume(DsStore *store, const DsVolumeInfo *volume, DsError *error)
{
    if (!store || store->mode != DS_STORE_WRITER || store->schema_version < 2 ||
        !volume || !valid_volume(volume))
        return ds_error(error, DS_INVALID, 0, "Invalid volume reading or store");

    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt,
        "INSERT INTO volume_info(id,captured_at,total,total_quality,used,used_quality,"
        " available,available_quality) SELECT id,?1,?2,?3,?4,?5,?6,?7 FROM scans"
        " WHERE id=1 AND commit_seq=0 AND run_status=-1 AND root_raw IS NOT NULL");
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, volume->captured_at);
    if (rc == SQLITE_OK) rc = ds_sql_bind_size(stmt, 2, volume->total);
    if (rc == SQLITE_OK) rc = ds_sql_bind_size(stmt, 4, volume->used);
    if (rc == SQLITE_OK) rc = ds_sql_bind_size(stmt, 6, volume->available);
    if (rc == SQLITE_OK) rc = ds_sql_step_done(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_OK && sqlite3_changes(store->db) != 1)
        return ds_error(error, DS_INVALID, 0, "Volume reading requires a scan with no observations");
    return ds_sql_status(store, rc, error);
}

static bool read_size(sqlite3_stmt *stmt, int col, DsSize *size)
{
    int64_t quality = sqlite3_column_int64(stmt, col + 1);
    return quality >= DS_KNOWN && quality <= DS_CHANGED && ds_sql_read_size(stmt, col, size);
}

int ds_store_read_volume(DsStore *store, DsVolumeInfo *volume)
{
    *volume = (DsVolumeInfo){
        .total.quality = DS_UNAVAILABLE, .used.quality = DS_UNAVAILABLE,
        .available.quality = DS_UNAVAILABLE
    };
    if (store->schema_version < 2) return SQLITE_OK;

    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt,
        "SELECT id,captured_at,total,total_quality,used,used_quality,available,available_quality"
        " FROM volume_info");
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        DsVolumeInfo reading = {.captured_at = sqlite3_column_int64(stmt, 1)};
        if (sqlite3_column_int64(stmt, 0) != 1 || !read_size(stmt, 2, &reading.total) ||
            !read_size(stmt, 4, &reading.used) || !read_size(stmt, 6, &reading.available) ||
            !valid_volume(&reading)) {
            rc = SQLITE_CORRUPT;
        } else {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) *volume = reading;
            if (rc == SQLITE_ROW) rc = SQLITE_CORRUPT;
        }
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

DsStatus ds_store_volume(DsStore *store, DsVolumeInfo *out, DsError *error)
{
    if (!store || !out) return ds_error(error, DS_INVALID, 0, "Missing volume store or output");
    return ds_sql_status(store, ds_store_read_volume(store, out), error);
}

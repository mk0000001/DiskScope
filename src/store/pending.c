#include "store_internal.h"

DsStatus ds_store_pending_push(DsStore *store, int64_t id, DsError *error)
{
    if (!store || store->mode != DS_STORE_WRITER || id <= 0)
        return ds_error(error, DS_INVALID, 0, "Invalid pending directory request");
    int rc = ds_store_check_budget(store, true);
    sqlite3_stmt *stmt = NULL;
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &stmt,
        "INSERT OR IGNORE INTO pending_dirs(node_id)"
        " SELECT id FROM entries WHERE id=?1 AND kind=1");
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = ds_sql_step_done(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_OK) ds_store_checkpoint(store, false);
    return ds_sql_status(store, rc, error);
}

DsStatus ds_store_pending_pop(DsStore *store, int64_t *id, DsError *error)
{
    if (!store || store->mode != DS_STORE_WRITER || !id)
        return ds_error(error, DS_INVALID, 0, "Invalid pending directory request");
    *id = 0;
    sqlite3_stmt *stmt = NULL;
    int rc = ds_sql_prepare(store, &stmt,
        "DELETE FROM pending_dirs WHERE sequence=(SELECT min(sequence) FROM pending_dirs)"
        " RETURNING node_id");
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int64_t node = sqlite3_column_int64(stmt, 0);
        rc = ds_sql_step_done(stmt);
        if (rc == SQLITE_OK) *id = node;
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_OK && *id) ds_store_checkpoint(store, false);
    return ds_sql_status(store, rc, error);
}

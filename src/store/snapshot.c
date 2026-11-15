#include "diskscope/ds_snapshot.h"
#include "output.h"
#include "store_internal.h"

#include <glib/gstdio.h>
#include <sqlite3.h>

static bool has_sidecar(const char *path)
{
    const char *const suffixes[] = {"-wal", "-shm", "-journal"};
    for (size_t i = 0; i < G_N_ELEMENTS(suffixes); ++i) {
        char *sidecar = g_strconcat(path, suffixes[i], NULL);
        bool exists = g_file_test(sidecar, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK);
        g_free(sidecar);
        if (exists)
            return true;
    }
    return false;
}

DsStatus ds_snapshot_save(DsStore *reader, const DsNativePath *destination,
                          DsControl *control, DsError *error)
{
    if (!reader)
        return ds_error(error, DS_INVALID, 0, "Missing snapshot source");
    if (ds_control_canceled(control))
        return DS_CANCELED;
    DsOutput output;
    DsStatus status = ds_output_begin(reader, destination, &output, error);
    if (status != DS_OK)
        return status;
    if (has_sidecar(output.destination)) {
        ds_output_clear(&output);
        return ds_error(error, DS_INVALID, 0, "Snapshot destination has an existing SQLite journal");
    }
    g_close(output.fd, NULL);
    output.fd = -1;
    sqlite3 *db = NULL;
    int code = sqlite3_open_v2(output.temporary, &db, SQLITE_OPEN_READWRITE, DS_FILE_VFS);
    sqlite3_backup *copy = code == SQLITE_OK ?
        sqlite3_backup_init(db, "main", ds_store_db(reader), "main") : NULL;
    if (copy) {
        int64_t deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
        while (!ds_control_canceled(control)) {
            code = sqlite3_backup_step(copy, 64);
            if (code != SQLITE_OK && code != SQLITE_BUSY && code != SQLITE_LOCKED)
                break;
            if (code == SQLITE_BUSY || code == SQLITE_LOCKED) {
                if (g_get_monotonic_time() >= deadline)
                    break;
                g_usleep(10000);
            } else {
                deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
            }
        }
        int stopped = sqlite3_backup_finish(copy);
        if (stopped != SQLITE_OK)
            code = stopped;
        if (code == SQLITE_DONE)
            code = sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
        else if (code == SQLITE_OK)
            code = SQLITE_INTERRUPT;
    } else if (code == SQLITE_OK) {
        code = sqlite3_errcode(db);
    }
    if (db)
        sqlite3_close(db);
    if (ds_control_canceled(control))
        status = DS_CANCELED;
    else if (code != SQLITE_OK)
        status = ds_error(error, code == SQLITE_FULL ? DS_LIMIT : DS_IO, code,
                          "SQLite could not complete the snapshot");
    else if (has_sidecar(output.destination))
        status = ds_error(error, DS_INVALID, 0, "Snapshot destination acquired a SQLite journal");
    else
        status = ds_output_commit(&output, error);
    ds_output_clear(&output);
    return status;
}

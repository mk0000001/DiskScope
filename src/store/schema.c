#include "store_internal.h"
#include <glib/gstdio.h>

static const char schema[] =
    "BEGIN IMMEDIATE;"
    "PRAGMA application_id=1146307407;"
    "PRAGMA user_version=1;"
    "CREATE TABLE scans("
    " id INTEGER PRIMARY KEY CHECK(id=1), root_raw BLOB, encoding INTEGER,"
    " root_display TEXT, policy TEXT, run_status INTEGER NOT NULL DEFAULT -1,"
    " coverage INTEGER NOT NULL DEFAULT 0, commit_seq INTEGER NOT NULL DEFAULT 0,"
    " active_generation INTEGER NOT NULL DEFAULT 0, entry_count INTEGER NOT NULL DEFAULT 0,"
    " started_at INTEGER, ended_at INTEGER, cache_budget INTEGER NOT NULL DEFAULT 2147483648"
    ") STRICT;"
    "INSERT INTO scans(id) VALUES(1);"
    "CREATE TABLE entries("
    " id INTEGER PRIMARY KEY,"
    " parent_id INTEGER REFERENCES entries(id),"
    " name BLOB NOT NULL, name_encoding INTEGER NOT NULL, display TEXT NOT NULL,"
    " kind INTEGER NOT NULL, reason INTEGER NOT NULL, os_code INTEGER NOT NULL,"
    " logical BLOB, logical_quality INTEGER NOT NULL,"
    " allocated BLOB, allocated_quality INTEGER NOT NULL,"
    " identity BLOB, links BLOB NOT NULL, mtime INTEGER, commit_seq INTEGER NOT NULL,"
    " CHECK((id=1 AND parent_id IS NULL) OR"
    "       (id>1 AND parent_id IS NOT NULL AND parent_id>0 AND parent_id<id)),"
    " UNIQUE(parent_id,name_encoding,name)"
    ") STRICT;"
    "CREATE INDEX entries_parent_display ON entries(parent_id,display,id);"
    "CREATE INDEX entries_file_identity ON entries(identity,id) WHERE kind=2 AND identity IS NOT NULL;"
    "CREATE TABLE generations("
    " id INTEGER PRIMARY KEY, as_of_commit_seq INTEGER NOT NULL,"
    " aggregation_complete INTEGER NOT NULL, coverage INTEGER NOT NULL,"
    " entry_count INTEGER NOT NULL"
    ") STRICT;"
    "CREATE TABLE file_objects("
    " generation INTEGER NOT NULL REFERENCES generations(id) ON DELETE CASCADE,"
    " identity BLOB NOT NULL, representative_id INTEGER NOT NULL REFERENCES entries(id),"
    " changed INTEGER NOT NULL, PRIMARY KEY(generation,identity)"
    ") STRICT, WITHOUT ROWID;"
    "CREATE TABLE aggregates("
    " generation INTEGER NOT NULL REFERENCES generations(id) ON DELETE CASCADE,"
    " node_id INTEGER NOT NULL REFERENCES entries(id),"
    " logical BLOB, logical_quality INTEGER NOT NULL, logical_unknown INTEGER NOT NULL,"
    " allocated BLOB, allocated_quality INTEGER NOT NULL, allocated_unknown INTEGER NOT NULL,"
    " unique_allocated BLOB, unique_quality INTEGER NOT NULL, unique_unknown INTEGER NOT NULL,"
    " representative_id INTEGER REFERENCES entries(id),"
    " entry_count INTEGER NOT NULL, file_count INTEGER NOT NULL, unique_file_count INTEGER NOT NULL,"
    " PRIMARY KEY(generation,node_id)"
    ") STRICT, WITHOUT ROWID;"
    "CREATE TABLE issues("
    " node_id INTEGER NOT NULL REFERENCES entries(id), operation TEXT NOT NULL,"
    " os_code INTEGER NOT NULL, reason INTEGER NOT NULL, observed_at INTEGER NOT NULL,"
    " PRIMARY KEY(node_id,operation,reason,os_code)"
    ") STRICT, WITHOUT ROWID;"
    "CREATE TABLE pending_dirs("
    " sequence INTEGER PRIMARY KEY, node_id INTEGER NOT NULL UNIQUE REFERENCES entries(id)"
    ") STRICT;"
    "COMMIT;";

DsStatus ds_sql_status(DsStore *store, int code, DsError *error)
{
    if (code == SQLITE_OK) return DS_OK;
    DsStatus status = DS_IO;
    switch (code & 0xff) {
    case SQLITE_INTERRUPT: status = DS_CANCELED; break;
    case SQLITE_FULL:
    case SQLITE_NOMEM:
    case SQLITE_TOOBIG: status = DS_LIMIT; break;
    case SQLITE_CONSTRAINT:
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
    case SQLITE_MISMATCH: status = DS_INVALID; break;
    }
    const char *detail = (sqlite3_errcode(store->db) == (code & 0xff))
        ? sqlite3_errmsg(store->db) : sqlite3_errstr(code);
    return ds_error(error, status, code, "SQLite: %s", detail);
}

int ds_sql_exec(DsStore *store, const char *sql)
{
    return sqlite3_exec(store->db, sql, NULL, NULL, NULL);
}

int ds_sql_prepare(DsStore *store, sqlite3_stmt **statement, const char *sql)
{
    return sqlite3_prepare_v2(store->db, sql, -1, statement, NULL);
}

int ds_sql_step_done(sqlite3_stmt *statement)
{
    int result = sqlite3_step(statement);
    return result == SQLITE_DONE ? SQLITE_OK : result;
}

int ds_sql_bind_u64(sqlite3_stmt *statement, int column, uint64_t value)
{
    uint8_t bytes[8];
    ds_u64_encode(value, bytes);
    return sqlite3_bind_blob(statement, column, bytes, sizeof bytes, SQLITE_TRANSIENT);
}

int ds_sql_bind_size(sqlite3_stmt *statement, int column, DsSize size)
{
    int rc = size.quality == DS_KNOWN ? ds_sql_bind_u64(statement, column, size.bytes)
                                     : sqlite3_bind_null(statement, column);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(statement, column + 1, size.quality);
    return rc;
}

bool ds_sql_read_u64(sqlite3_stmt *statement, int column, uint64_t *value)
{
    return sqlite3_column_type(statement, column) == SQLITE_BLOB &&
        ds_u64_decode(sqlite3_column_blob(statement, column),
                      (size_t)sqlite3_column_bytes(statement, column), value);
}

bool ds_sql_read_size(sqlite3_stmt *statement, int column, DsSize *size)
{
    size->bytes = 0;
    size->quality = sqlite3_column_int(statement, column + 1);
    if (size->quality == DS_KNOWN) return ds_sql_read_u64(statement, column, &size->bytes);
    return size->quality >= DS_UNAVAILABLE && size->quality <= DS_CHANGED &&
           sqlite3_column_type(statement, column) == SQLITE_NULL;
}

bool ds_sql_progress(void *control)
{
    return control && ds_control_canceled(control);
}

int ds_sql_interrupt(void *control)
{
    return ds_sql_progress(control);
}

static int scalar(DsStore *store, const char *sql, int64_t *value)
{
    sqlite3_stmt *statement = NULL;
    int rc = ds_sql_prepare(store, &statement, sql);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) {
            *value = sqlite3_column_int64(statement, 0);
            rc = SQLITE_OK;
        } else if (rc == SQLITE_DONE) {
            rc = SQLITE_CORRUPT;
        }
    }
    sqlite3_finalize(statement);
    return rc;
}

static int match_schema(DsStore *store)
{
    static const char sql[] = "SELECT type,name,tbl_name,sql FROM sqlite_schema ORDER BY type,name";
    sqlite3 *expected = NULL;
    sqlite3_stmt *actual = NULL, *canonical = NULL;
    int rc = sqlite3_open(":memory:", &expected);
    if (rc == SQLITE_OK) rc = sqlite3_exec(expected, schema, NULL, NULL, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(expected, sql, -1, &canonical, NULL);
    if (rc == SQLITE_OK) rc = ds_sql_prepare(store, &actual, sql);
    while (rc == SQLITE_OK) {
        int source = sqlite3_step(actual), reference = sqlite3_step(canonical);
        if (source != SQLITE_ROW || reference != SQLITE_ROW) {
            if (source != SQLITE_DONE || reference != SQLITE_DONE)
                rc = source != SQLITE_ROW && source != SQLITE_DONE ? source : SQLITE_NOTADB;
            break;
        }
        for (int i = 0; i < 4; ++i) {
            const char *left = (const char *)sqlite3_column_text(actual, i);
            const char *right = (const char *)sqlite3_column_text(canonical, i);
            if (g_strcmp0(left, right) || sqlite3_column_bytes(actual, i) != sqlite3_column_bytes(canonical, i)) {
                rc = SQLITE_NOTADB;
                break;
            }
        }
    }
    sqlite3_finalize(actual);
    sqlite3_finalize(canonical);
    sqlite3_close(expected);
    return rc;
}

static int open_schema(DsStore *store)
{
    int64_t application = 0, version = 0, objects = 0, budget = 0;
    int rc = scalar(store, "PRAGMA application_id", &application);
    if (rc == SQLITE_OK) rc = scalar(store, "PRAGMA user_version", &version);
    if (rc != SQLITE_OK) return rc;
    if (application == 0 && version == 0 && store->mode == DS_STORE_WRITER) {
        rc = scalar(store, "SELECT count(*) FROM sqlite_schema", &objects);
        if (rc != SQLITE_OK) return rc;
        if (objects != 0) return SQLITE_NOTADB;
        rc = ds_sql_exec(store, schema);
    } else if (application != DS_STORE_APPLICATION_ID || version != DS_STORE_SCHEMA_VERSION) {
        return SQLITE_NOTADB;
    } else {
        rc = match_schema(store);
    }
    if (rc == SQLITE_OK) rc = scalar(store, "SELECT cache_budget FROM scans WHERE id=1", &budget);
    if (rc == SQLITE_OK && budget < (128LL << 20)) rc = SQLITE_CORRUPT;
    if (rc == SQLITE_OK) store->budget = (uint64_t)budget;
    return rc;
}

static int enable_wal(DsStore *store)
{
    sqlite3_stmt *statement = NULL;
    int rc = ds_sql_prepare(store, &statement, "PRAGMA journal_mode=WAL");
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) {
            const char *mode = (const char *)sqlite3_column_text(statement, 0);
            rc = mode && g_ascii_strcasecmp(mode, "wal") == 0 ? SQLITE_OK : SQLITE_IOERR;
        } else if (rc == SQLITE_DONE) {
            rc = SQLITE_IOERR;
        }
    }
    sqlite3_finalize(statement);
    return rc;
}

DsStatus ds_store_open(const DsNativePath *path, DsStoreMode mode,
                       DsStore **out, DsError *error)
{
    if (!path || !path->raw || !out || (mode != DS_STORE_WRITER && mode != DS_STORE_READER))
        return ds_error(error, DS_INVALID, 0, "Invalid store path or mode");
    *out = NULL;
    char *filename = ds_path_arg(path, error);
    if (!filename) return error && error->status != DS_OK ? error->status : DS_INVALID;
    if (!filename[0] || g_str_equal(filename, ":memory:")) {
        g_free(filename);
        return ds_error(error, DS_INVALID, 0, "A file-backed scan store is required");
    }
    DsStore *store = g_new0(DsStore, 1);
    store->filename = filename;
    store->mode = mode;
    int flags = mode == DS_STORE_WRITER ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                                         : SQLITE_OPEN_READONLY;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    int rc = sqlite3_open_v2(filename, &store->db, flags, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_db_config(store->db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_db_config(store->db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_busy_timeout(store->db, 0);
    if (rc == SQLITE_OK) {
        sqlite3_limit(store->db, SQLITE_LIMIT_LENGTH, 8 * (int)DS_PATH_MAX);
        rc = ds_sql_exec(store, "PRAGMA foreign_keys=ON; PRAGMA temp_store=FILE;"
            "PRAGMA mmap_size=0;");
    }
    if (rc == SQLITE_OK) {
        rc = ds_sql_exec(store, mode == DS_STORE_WRITER
            ? "PRAGMA cache_size=-32768;"
            : "PRAGMA cache_size=-8192; PRAGMA query_only=ON;");
    }
    if (rc == SQLITE_OK) rc = open_schema(store);
    if (rc == SQLITE_OK && mode == DS_STORE_WRITER) {
        rc = enable_wal(store);
        if (rc == SQLITE_OK) rc = ds_sql_exec(store, "PRAGMA synchronous=NORMAL;"
            "PRAGMA wal_autocheckpoint=0; PRAGMA journal_size_limit=16777216;");
        int64_t page_size = 0;
        if (rc == SQLITE_OK) rc = scalar(store, "PRAGMA page_size", &page_size);
        if (rc == SQLITE_OK && (page_size < 512 || page_size > 65536)) rc = SQLITE_CORRUPT;
        if (rc == SQLITE_OK) store->page_size = (uint64_t)page_size;
    }
    if (rc == SQLITE_OK) rc = ds_query_register_functions(store);
    if (rc != SQLITE_OK) {
        DsStatus status = ds_sql_status(store, rc, error);
        ds_store_close(store);
        return status;
    }
    *out = store;
    return DS_OK;
}

void ds_store_close(DsStore *store)
{
    if (!store) return;
    sqlite3_close_v2(store->db);
    g_free(store->cache_directory);
    g_free(store->filename);
    g_free(store);
}

sqlite3 *ds_store_db(DsStore *store)
{
    return store ? store->db : NULL;
}

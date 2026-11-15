#ifndef DISKSCOPE_STORE_INTERNAL_H
#define DISKSCOPE_STORE_INTERNAL_H

#include "diskscope/ds_query.h"
#include <sqlite3.h>

#ifdef G_OS_WIN32
#include "../core/win_io.h"
#define DS_FILE_VFS "win32-longpath"
#else
#define DS_FILE_VFS NULL
#endif

struct DsStore {
    sqlite3 *db;
    char *filename;
    char *cache_directory;
    DsStoreMode mode;
    int schema_version;
    uint64_t budget;
    uint64_t page_size;
};

DsStatus ds_sql_status(DsStore *store, int code, DsError *error);
int ds_sql_exec(DsStore *store, const char *sql);
int ds_sql_prepare(DsStore *store, sqlite3_stmt **statement, const char *sql);
int ds_sql_step_done(sqlite3_stmt *statement);
int ds_sql_bind_size(sqlite3_stmt *statement, int column, DsSize size);
int ds_sql_bind_u64(sqlite3_stmt *statement, int column, uint64_t value);
bool ds_sql_read_size(sqlite3_stmt *statement, int column, DsSize *size);
bool ds_sql_read_u64(sqlite3_stmt *statement, int column, uint64_t *value);
int ds_store_read_volume(DsStore *store, DsVolumeInfo *volume);
bool ds_sql_progress(void *control);
int ds_sql_interrupt(void *control);
int ds_store_check_budget(DsStore *store, bool collecting);
void ds_store_checkpoint(DsStore *store);
int ds_query_register_functions(DsStore *store);
int ds_query_prepare_entry(DsStore *store, sqlite3_stmt **stmt, int64_t generation);
int ds_query_step_entry(sqlite3_stmt *stmt, int64_t id, DsRow *row);
void ds_query_clear_row(DsRow *row);
int ds_hardlinks_finalize(DsStore *store, int64_t generation, DsControl *control);

#endif

#ifndef DISKSCOPE_STORE_INTERNAL_H
#define DISKSCOPE_STORE_INTERNAL_H

#include "diskscope/ds_query.h"
#include <sqlite3.h>

struct DsStore {
    sqlite3 *db;
    char *filename;
    DsStoreMode mode;
    uint64_t budget;
};

DsStatus ds_sql_status(DsStore *store, int code, DsError *error);
int ds_sql_exec(DsStore *store, const char *sql);
int ds_sql_prepare(DsStore *store, sqlite3_stmt **statement, const char *sql);
int ds_sql_step_done(sqlite3_stmt *statement);
int ds_sql_bind_size(sqlite3_stmt *statement, int column, DsSize size);
int ds_sql_bind_u64(sqlite3_stmt *statement, int column, uint64_t value);
bool ds_sql_read_size(sqlite3_stmt *statement, int column, DsSize *size);
bool ds_sql_read_u64(sqlite3_stmt *statement, int column, uint64_t *value);
bool ds_sql_progress(void *control);
int ds_sql_interrupt(void *control);
int ds_store_check_budget(DsStore *store, bool collecting);
void ds_store_checkpoint(DsStore *store);
int ds_query_register_functions(DsStore *store);
int ds_hardlinks_finalize(DsStore *store, int64_t generation, DsControl *control);

#endif

#include "diskscope/ds_recovery.h"
#include "cache_lease.h"
#include "local.h"
#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../core/win_io.h"
#endif

struct DsRecoveredScan {
    DsNativePath db;
    DsCacheLease *lease;
    DsStoreInfo info;
};

static DsRecoveredScan *read_candidate(const char *base, const char *name)
{
    char *folder = g_build_filename(base, name, NULL);
    DsCacheLease *lease = ds_cache_lease_open(folder, false, true, NULL);
    if (!lease) {
        g_free(folder);
        return NULL;
    }

    DsRecoveredScan *result = g_new0(DsRecoveredScan, 1);
    result->lease = lease;
    char *database = g_build_filename(folder, "scan.sqlite", NULL);
    g_free(folder);
    DsStore *reader = NULL;
    bool valid = ds_path_from_arg(database, &result->db, NULL) == DS_OK &&
                 ds_store_open(&result->db, DS_STORE_READER, &reader, NULL) == DS_OK &&
                 ds_store_info(reader, &result->info, NULL) == DS_OK &&
                 result->info.root.raw && g_bytes_get_size(result->info.root.raw) &&
                 result->info.entry_count > 0 && result->info.commit_seq > 0 &&
                 result->info.started_at > 0 &&
                 result->info.run_status >= DS_RUN_RUNNING && result->info.run_status <= DS_PROTOCOL &&
                 (result->info.coverage == DS_COVERAGE_PARTIAL ||
                  (result->info.coverage == DS_COVERAGE_COMPLETE && result->info.run_status == DS_OK));
    ds_store_close(reader);
    g_free(database);
    if (!valid) {
        ds_recovered_free(result);
        return NULL;
    }
    return result;
}

static void record_interruption(DsRecoveredScan *result)
{
    if (result->info.run_status != DS_RUN_RUNNING)
        return;
    DsStore *writer = NULL;
    if (ds_store_open(&result->db, DS_STORE_WRITER, &writer, NULL) == DS_OK &&
        ds_store_set_result(writer, DS_IO, DS_COVERAGE_PARTIAL, NULL) == DS_OK) {
        result->info.run_status = DS_IO;
        result->info.ended_at = g_get_real_time() / G_USEC_PER_SEC;
    }
    /* If the cache cannot record the terminal marker, the old observation
     * rows remain readable. Its coverage is still partial. */
    ds_store_close(writer);
}

bool ds_recovered_newer(const DsRecoveredScan *candidate, const DsRecoveredScan *current)
{
    if (!candidate)
        return false;
    if (!current)
        return true;
    if (candidate->info.started_at != current->info.started_at)
        return candidate->info.started_at > current->info.started_at;
    int64_t a = ds_cache_lease_created(candidate->lease);
    int64_t b = ds_cache_lease_created(current->lease);
    return a == b ? g_bytes_compare(candidate->db.raw, current->db.raw) > 0 : a > b;
}

static void consider(const char *base, const char *name, DsRecoveredScan **best)
{
    if (!g_str_has_prefix(name, "scan-") || !g_uuid_string_is_valid(name + 5))
        return;
    DsRecoveredScan *candidate = read_candidate(base, name);
    if (ds_recovered_newer(candidate, *best)) {
        ds_recovered_free(*best);
        *best = candidate;
    } else {
        ds_recovered_free(candidate);
    }
}

DsStatus ds_recovery_latest(const DsNativePath *cache, DsControl *control,
                            DsRecoveredScan **result, DsError *error)
{
    if (!result)
        return ds_error(error, DS_INVALID, 0, "Missing recovery result");
    *result = NULL;
    if (ds_control_canceled(control))
        return DS_CANCELED;
    char *base = ds_cache_base(cache, error);
    if (!base)
        return DS_INVALID;
    DsRecoveredScan *best = NULL;
#ifdef G_OS_WIN32
    DsWinDir *directory = ds_win_dir_open(base);
    if (!directory) {
        DWORD code = GetLastError();
        g_free(base);
        return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? DS_OK :
               ds_error(error, DS_IO, code, "Could not read the cache directory");
    }
    if (!ds_cache_is_local(base)) {
        ds_win_dir_close(directory);
        g_free(base);
        return ds_error(error, DS_INVALID, 0, "Recovery requires a local cache");
    }
    int read_status = 0;
    char *name = NULL;
    while (!ds_control_canceled(control) && (read_status = ds_win_dir_next(directory, &name, NULL)) == 1) {
        consider(base, name, &best);
        g_clear_pointer(&name, g_free);
    }
    DWORD code = GetLastError();
    ds_win_dir_close(directory);
    if (read_status < 0) {
        ds_recovered_free(best);
        g_free(base);
        return ds_error(error, DS_IO, code, "Could not finish reading the cache directory");
    }
#else
    GError *directory_error = NULL;
    GDir *directory = g_dir_open(base, 0, &directory_error);
    if (!directory) {
        bool missing = g_error_matches(directory_error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
        DsStatus status = missing ? DS_OK :
            ds_error(error, DS_IO, directory_error->code, "%s", directory_error->message);
        g_error_free(directory_error);
        g_free(base);
        return status;
    }
    if (!ds_cache_is_local(base)) {
        g_dir_close(directory);
        g_free(base);
        return ds_error(error, DS_INVALID, 0, "Recovery requires a local cache");
    }

    const char *name;
    while (!ds_control_canceled(control) && (name = g_dir_read_name(directory)))
        consider(base, name, &best);
    g_dir_close(directory);
#endif
    g_free(base);
    if (ds_control_canceled(control)) {
        ds_recovered_free(best);
        return DS_CANCELED;
    }
    if (best)
        record_interruption(best);
    *result = best;
    return DS_OK;
}

const DsNativePath *ds_recovered_path(const DsRecoveredScan *scan)
{
    return scan ? &scan->db : NULL;
}

const DsStoreInfo *ds_recovered_info(const DsRecoveredScan *scan)
{
    return scan ? &scan->info : NULL;
}

void ds_recovered_free(DsRecoveredScan *scan)
{
    if (!scan)
        return;
    ds_store_info_clear(&scan->info);
    ds_path_clear(&scan->db);
    ds_cache_lease_close(scan->lease);
    g_free(scan);
}

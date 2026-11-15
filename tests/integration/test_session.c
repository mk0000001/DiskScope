#include "diskscope/ds_session.h"
#include <glib/gstdio.h>

static DsSessionEvent reach(DsSession *session, DsStage stage)
{
    int64_t deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    DsSessionEvent event;
    do {
        ds_session_event(session, &event);
        if (event.stage == stage)
            return event;
        if (event.exited)
            break;
        g_usleep(10000);
    } while (g_get_monotonic_time() < deadline);
    g_error("expected stage %d, received %d", stage, event.stage);
}

static void exercise(bool crash)
{
    GError *problem = NULL;
    char *folder = g_dir_make_tmp("diskscope-session-XXXXXX", &problem);
    g_assert_no_error(problem);
    char *root = g_build_filename(folder, crash ? "crash" : "stall", NULL);
    DsScanOptions options;
    DsError failure = {0};
    ds_scan_options_init(&options, DS_CONSERVATIVE);
    g_assert_cmpint(ds_path_from_arg(root, &options.root, NULL), ==, DS_OK);
    g_assert_cmpint(ds_path_from_arg(folder, &options.cache_path, NULL), ==, DS_OK);
    DsSession *s = ds_session_start(&options, NULL, NULL, &failure);
    if (!s) g_error("%s", failure.message);
    if (crash) {
        DsSessionEvent lost = reach(s, DS_WORKER_LOST);
        g_assert_true(lost.exited);
        g_assert_cmpint(lost.result, ==, DS_IO);
    } else {
        reach(s, DS_RUNNING);
        ds_session_request_pause(s);
        reach(s, DS_PAUSED);
        ds_session_request_resume(s);
        reach(s, DS_RUNNING);
        ds_session_request_cancel(s);
        DsSessionEvent requested = reach(s, DS_CANCEL_REQUESTED);
        g_assert_false(requested.exited);
        DsSessionEvent waiting = reach(s, DS_CANCEL_WAITING);
        g_assert_false(waiting.exited);
        DsSession *second = ds_session_start(&options, NULL, NULL, &failure);
        g_assert_null(second);
        g_assert_cmpint(failure.status, ==, DS_LIMIT);
        int64_t deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
        while (!waiting.commit_seq && !waiting.exited && g_get_monotonic_time() < deadline) {
            g_usleep(10000);
            ds_session_event(s, &waiting);
        }
        g_assert_false(waiting.exited);
        g_assert_cmpint(waiting.stage, ==, DS_CANCEL_WAITING);
        g_assert_true(waiting.observations == 17);
        DsSessionEvent finished = reach(s, DS_FINISHED);
        g_assert_true(finished.exited);
        g_assert_cmpint(finished.result, ==, DS_CANCELED);
    }
    char *db = ds_path_arg(ds_session_db_path(s), NULL);
    ds_session_unref(s);
    char *cache = g_path_get_dirname(db);
    char *lock = g_build_filename(cache, "scan.lock", NULL);
    g_unlink(lock);
    g_free(lock);
    g_rmdir(cache);
    g_rmdir(folder);
    g_free(cache);
    g_free(db);
    ds_path_clear(&options.root);
    ds_path_clear(&options.cache_path);
    g_free(root);
    g_free(folder);
}

static void paused_cancel(void) { exercise(false); }
static void worker_loss(void) { exercise(true); }

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/pause_cancel_late_commit", paused_cancel);
    g_test_add_func("/worker_loss", worker_loss);
    return g_test_run();
}

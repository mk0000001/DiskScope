#include "../../src/ui/paged_model.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

static DsNativePath fixture;
static guint fixture_count;

static void remove_fixture(const char *database, const char *directory)
{
    g_assert_cmpint(g_remove(database), ==, 0);
    /* SQLite's read-only page connections may leave WAL sidecars. */
    const char *sidecars[] = {"-wal", "-shm"};
    for (size_t i = 0; i < G_N_ELEMENTS(sidecars); ++i) {
        g_autofree char *path = g_strconcat(database, sidecars[i], NULL);
        g_assert_true(g_remove(path) == 0 || errno == ENOENT);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void populate(void)
{
    DsStore *store;
    DsError error;
    g_assert_cmpint(ds_store_open(&fixture, DS_STORE_WRITER, &store, &error), ==, DS_OK);
    g_autofree char *sql = g_strdup_printf(
        "BEGIN; WITH RECURSIVE test_ids(id) AS (VALUES(1) UNION ALL SELECT id+1 FROM test_ids WHERE id<%u)"
        " INSERT INTO entries(id,parent_id,name,name_encoding,display,kind,reason,os_code,"
        " logical,logical_quality,allocated,allocated_quality,links,commit_seq)"
        " SELECT id,CASE id WHEN 1 THEN NULL WHEN 3 THEN 2 ELSE 1 END,"
        " CAST(CASE id WHEN 1 THEN '' ELSE printf('row-%%07d',id) END AS BLOB),1,"
        " printf('row-%%07d',id),CASE WHEN id<3 THEN 1 ELSE 2 END,0,0,"
        " X'0000000000000001',0,X'0000000000000200',0,X'0000000000000001',1 FROM test_ids;"
        " UPDATE scans SET entry_count=%u,commit_seq=1; COMMIT;", fixture_count, fixture_count);
    sqlite3 *db = ds_store_db(store);
    int code = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (code != SQLITE_OK) g_test_message("Fixture SQL: %s", sqlite3_errmsg(db));
    g_assert_cmpint(code, ==, SQLITE_OK);
    ds_store_close(store);
}

static void drain(void)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
}

static void wait_count(GListModel *model, guint count)
{
    gint64 deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    while (g_list_model_get_n_items(model) != count && g_get_monotonic_time() < deadline) {
        drain();
        g_autofree char *error = NULL;
        g_object_get(model, "query-error", &error, NULL);
        if (error) g_error("Paged query failed while waiting for %u rows: %s", count, error);
        g_usleep(1000);
    }
    g_assert_cmpuint(g_list_model_get_n_items(model), ==, count);
}

static GObject *wait_row(GListModel *model, guint position)
{
    gint64 deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    do {
        drain();
        GObject *item = g_list_model_get_item(model, position);
        if (item && ds_ui_row_get(item)) return item;
        g_clear_object(&item);
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_error("Row at position %u never loaded", position);
}

typedef struct { GThread *owner; bool prefetched; } Changes;

static void observe_changes(GListModel *model, guint position, guint removed, guint added, gpointer data)
{
    (void)model;
    Changes *changes = data;
    g_assert_true(changes->owner == g_thread_self());
    if (position == DS_PAGE_MAX && removed == DS_PAGE_MAX && added == DS_PAGE_MAX)
        changes->prefetched = true;
}

static void pages_arrive_on_main_context(void)
{
    DsQuery query = {.parent_id = DS_QUERY_ALL, .sort = DS_SORT_ID};
    g_autoptr(GListModel) model = ds_paged_model_new(&fixture, &query);
    g_assert_nonnull(model);
    Changes changes = {.owner = g_thread_self()};
    g_signal_connect(model, "items-changed", G_CALLBACK(observe_changes), &changes);
    g_assert_cmpuint(g_list_model_get_n_items(model), ==, 0);
    wait_count(model, fixture_count);
    g_autoptr(GObject) first = wait_row(model, 0);
    g_assert_cmpint(ds_ui_row_get(first)->entry.id, ==, 1);

    /* Page one arrives without a row request. The one-page lookahead must
     * stop before page two, which begins with a loading placeholder. */
    gint64 deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    while (!changes.prefetched && g_get_monotonic_time() < deadline) {
        drain();
        g_usleep(1000);
    }
    g_assert_true(changes.prefetched);
    g_autoptr(GObject) prefetched = g_list_model_get_item(model, DS_PAGE_MAX);
    g_assert_nonnull(ds_ui_row_get(prefetched));
    g_autoptr(GObject) beyond_prefetch = g_list_model_get_item(model, DS_PAGE_MAX * 2);
    g_assert_nonnull(beyond_prefetch);
    g_assert_null(ds_ui_row_get(beyond_prefetch));
    for (guint index = 2; index < 8; ++index) {
        guint position = index * DS_PAGE_MAX;
        g_autoptr(GObject) far = wait_row(model, position);
        g_assert_cmpint(ds_ui_row_get(far)->entry.id, ==, position + 1);
    }
    g_assert_cmpint(ds_ui_row_get(first)->entry.id, ==, 1);
    g_autoptr(GObject) evicted = g_list_model_get_item(model, 0);
    g_assert_null(ds_ui_row_get(evicted));
    g_signal_handlers_disconnect_by_data(model, &changes);
}

static void changes_discard_previous_query(void)
{
    DsQuery old = {.parent_id = 1, .sort = DS_SORT_NAME, .descending = true};
    g_autoptr(GListModel) model = ds_paged_model_new(&fixture, &old);
    g_assert_nonnull(model);
    DsQuery current = {.parent_id = 2, .sort = DS_SORT_NAME};
    ds_paged_model_set_query(model, &current);
    wait_count(model, 1);
    g_autoptr(GObject) row = wait_row(model, 0);
    const DsRow *result = ds_ui_row_get(row);
    g_assert_cmpint(result->entry.id, ==, 3);
    g_assert_cmpint(result->entry.parent_id, ==, 2);
    g_assert_cmpstr(result->display, ==, "row-0000003");
}

static void cap_limits_rows(void)
{
    DsQuery query = {.parent_id = DS_QUERY_ALL, .sort = DS_SORT_ID, .descending = true};
    g_autoptr(GListModel) model = ds_paged_model_new(&fixture, &query);
    g_assert_nonnull(model);
    ds_paged_model_set_cap(model, 100);
    wait_count(model, 100);
    g_autoptr(GObject) top = wait_row(model, 0);
    g_assert_cmpint(ds_ui_row_get(top)->entry.id, ==, fixture_count);
    g_assert_null(g_list_model_get_item(model, 100));
    ds_paged_model_set_cap(model, 0);
    wait_count(model, fixture_count);
    g_autoptr(GObject) more = wait_row(model, 100);
    g_assert_cmpint(ds_ui_row_get(more)->entry.id, ==, fixture_count - 100);
}

static void shares_page_memory_budget(void)
{
    g_autofree char *directory = g_dir_make_tmp("diskscope-paged-weighted-XXXXXX", NULL);
    g_assert_nonnull(directory);
    g_autofree char *database = g_build_filename(directory, "test.db", NULL);
    DsNativePath path = {0};
    g_assert_cmpint(ds_path_from_arg(database, &path, NULL), ==, DS_OK);
    DsStore *store;
    g_assert_cmpint(ds_store_open(&path, DS_STORE_WRITER, &store, NULL), ==, DS_OK);
    /* One 512-row page occupies roughly 12 MiB of raw path and display text.
     * Six small lists fit locally, but cannot all fit the shared cache. */
    g_assert_cmpint(sqlite3_exec(ds_store_db(store),
        "BEGIN; WITH RECURSIVE ids(id) AS (VALUES(1) UNION ALL SELECT id+1 FROM ids WHERE id<513),"
        " rows AS (SELECT id,CASE WHEN id=1 THEN '' ELSE printf('weighted-%05d-%s',id,hex(zeroblob(6000))) END name FROM ids)"
        " INSERT INTO entries(id,parent_id,name,name_encoding,display,kind,reason,os_code,"
        " logical,logical_quality,allocated,allocated_quality,links,commit_seq)"
        " SELECT id,CASE id WHEN 1 THEN NULL ELSE 1 END,CAST(name AS BLOB),1,name,"
        " CASE id WHEN 1 THEN 1 ELSE 2 END,0,0,X'0000000000000001',0,X'0000000000000200',0,"
        " X'0000000000000001',1 FROM rows; UPDATE scans SET entry_count=513,commit_seq=1; COMMIT;",
        NULL, NULL, NULL), ==, SQLITE_OK);
    ds_store_close(store);

    DsQuery query = {.parent_id = 1, .sort = DS_SORT_ID};
    g_autoptr(GPtrArray) models = g_ptr_array_new_with_free_func(g_object_unref);
    GListModel *first = ds_paged_model_new(&path, &query);
    g_ptr_array_add(models, first);
    wait_count(first, DS_PAGE_MAX);
    g_autoptr(GObject) held = wait_row(first, 0);
    for (unsigned i = 0; i < 5; ++i) {
        GListModel *other = ds_paged_model_new(&path, &query);
        g_ptr_array_add(models, other);
        wait_count(other, DS_PAGE_MAX);
        g_autoptr(GObject) loaded = wait_row(other, 0);
        g_assert_cmpuint(strlen(ds_ui_row_get(loaded)->display), >, 12000);
    }
    g_autoptr(GObject) loading = g_list_model_get_item(first, 0);
    g_assert_null(ds_ui_row_get(loading));
    g_assert_cmpint(ds_ui_row_get(held)->entry.id, ==, 2);
    g_autoptr(GObject) reloaded = wait_row(first, 0);
    g_assert_cmpint(ds_ui_row_get(reloaded)->entry.id, ==, 2);
    g_clear_pointer(&models, g_ptr_array_unref);
    ds_path_clear(&path);
    remove_fixture(database, directory);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_autofree char *directory = g_dir_make_tmp("diskscope-paged-model-XXXXXX", NULL);
    g_assert_nonnull(directory);
    g_autofree char *database = g_build_filename(directory, "test.db", NULL);
    g_assert_cmpint(ds_path_from_arg(database, &fixture, NULL), ==, DS_OK);
    fixture_count = g_getenv("DISKSCOPE_TEST_MILLION") ? 1000000 : 8192;
    populate();
    g_test_add_func("/paged-model/page-prefetch-eviction", pages_arrive_on_main_context);
    g_test_add_func("/paged-model/generation-tree", changes_discard_previous_query);
    g_test_add_func("/paged-model/cap", cap_limits_rows);
    g_test_add_func("/paged-model/shared-cache", shares_page_memory_budget);
    int status = g_test_run();
    ds_path_clear(&fixture);
    remove_fixture(database, directory);
    return status;
}

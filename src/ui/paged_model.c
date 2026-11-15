#include "paged_model.h"
#include <sqlite3.h>

enum { CACHED_PAGES = 3, QUEUED_PAGES = 2, ROW_CACHE_BUDGET = 64 * 1024 * 1024 };

typedef struct { GObject parent; bool loaded; DsRow row; } DsUiRow;
typedef struct { GObjectClass parent; } DsUiRowClass;
G_DEFINE_TYPE(DsUiRow, ds_ui_row, G_TYPE_OBJECT)

static void ds_ui_row_finalize(GObject *object)
{
    DsUiRow *item = (DsUiRow *)object;
    ds_path_clear(&item->row.entry.name);
    g_free(item->row.display);
    G_OBJECT_CLASS(ds_ui_row_parent_class)->finalize(object);
}

static void ds_ui_row_class_init(DsUiRowClass *class)
{
    G_OBJECT_CLASS(class)->finalize = ds_ui_row_finalize;
}

static void ds_ui_row_init(DsUiRow *item) { (void)item; }

const DsRow *ds_ui_row_get(GObject *object)
{
    if (!object || !G_TYPE_CHECK_INSTANCE_TYPE(object, ds_ui_row_get_type())) return NULL;
    DsUiRow *item = (DsUiRow *)object;
    return item->loaded ? &item->row : NULL;
}

typedef struct { guint offset; bool prefetch; } PageRequest;

typedef struct {
    PageRequest request;
    bool count_only;
    guint64 generation;
    GWeakRef model;
    DsNativePath db;
    DsQuery query;
    DsControl control;
} PageLoad;

typedef struct {
    guint offset, count;
    gint64 last_access;
    DsUiRow **items;
    GList lru_link;
    gsize bytes;
} RowPage;

typedef struct { GQueue lru; gsize bytes; } RowCache;

/* Expanded lists on the GTK thread share one resident page budget. */
static GPrivate thread_cache = G_PRIVATE_INIT(g_rc_box_release);

typedef struct {
    GObject parent;
    DsNativePath db;
    DsQuery query;
    guint64 generation, total;
    guint length, cap;
    bool applying, append_pending;
    PageLoad *active;
    PageRequest requests[QUEUED_PAGES];
    size_t request_count;
    RowPage pages[CACHED_PAGES];
    RowCache *cache;
    DsUiRow *placeholder;
    char *error;
    guint failed_offset;
} DsPagedModel;

typedef struct { GObjectClass parent; } DsPagedModelClass;

static void list_model_init(GListModelInterface *interface);
G_DEFINE_TYPE_WITH_CODE(DsPagedModel, ds_paged_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, list_model_init))

static void clear_query(DsQuery *query)
{
    g_free((gpointer)query->filter);
    g_free((gpointer)query->extension);
}

static DsQuery copy_query(const DsQuery *source)
{
    DsQuery copy = *source;
    copy.filter = g_strdup(source->filter);
    copy.extension = g_strdup(source->extension);
    return copy;
}

static void clear_page(RowCache *cache, RowPage *page)
{
    if (page->bytes) {
        g_queue_unlink(&cache->lru, &page->lru_link);
        cache->bytes -= page->bytes;
    }
    for (guint i = 0; i < page->count; ++i) g_object_unref(page->items[i]);
    g_free(page->items);
    *page = (RowPage){0};
}

static RowPage *find_page(DsPagedModel *model, guint position)
{
    for (size_t i = 0; i < CACHED_PAGES; ++i) {
        RowPage *page = &model->pages[i];
        if (position >= page->offset && position - page->offset < page->count) return page;
    }
    return NULL;
}

static void add_page(DsPagedModel *model, guint offset, DsPage *result)
{
    if (!result->count) return;
    gsize bytes = sizeof(RowPage);
    for (guint i = 0; i < result->count; ++i) {
        const DsRow *row = &result->rows[i];
        /* Include row and pointer storage, display text, native bytes, and
         * an allowance for the opaque GObject and GBytes allocations. */
        bytes += sizeof(DsUiRow) + sizeof(DsUiRow *) + 64 + strlen(row->display) + 1 +
                 g_bytes_get_size(row->entry.name.raw);
    }
    if (bytes > ROW_CACHE_BUDGET) return;

    RowCache *cache = model->cache;
    RowPage *slot = offset % DS_PAGE_MAX ? find_page(model, offset - 1) : NULL;
    if (slot && slot->offset + slot->count == offset &&
        slot->bytes - sizeof(RowPage) <= ROW_CACHE_BUDGET - bytes) {
        /* Extend a partial cached page without replacing its row objects. */
        bytes -= sizeof(RowPage);
        g_queue_unlink(&cache->lru, &slot->lru_link);
    } else {
        slot = &model->pages[0];
        for (size_t i = 1; i < CACHED_PAGES; ++i)
            if (model->pages[i].last_access < slot->last_access) slot = &model->pages[i];
        clear_page(cache, slot);
        slot->offset = offset;
    }
    while (cache->bytes > ROW_CACHE_BUDGET - bytes)
        clear_page(cache, g_queue_peek_head(&cache->lru));
    guint previous = slot->count;
    slot->count += (guint)result->count;
    slot->last_access = g_get_monotonic_time();
    slot->items = g_renew(DsUiRow *, slot->items, slot->count);
    slot->bytes += bytes;
    slot->lru_link.data = slot;
    cache->bytes += bytes;
    g_queue_push_tail_link(&cache->lru, &slot->lru_link);
    for (guint i = 0; i < result->count; ++i) {
        DsUiRow *item = g_object_new(ds_ui_row_get_type(), NULL);
        item->loaded = true;
        item->row = result->rows[i];
        result->rows[i] = (DsRow){0};
        slot->items[previous + i] = item;
    }
}

static void page_load_free(gpointer data)
{
    PageLoad *load = data;
    g_weak_ref_clear(&load->model);
    ds_path_clear(&load->db);
    clear_query(&load->query);
    ds_control_clear(&load->control);
    g_free(load);
}

static void query_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source;
    (void)cancel;
    PageLoad *load = data;
    DsPage *result = NULL;
    DsError error = {0};
    DsStatus status = DS_CANCELED;
    /* SQLite readers have no busy timeout. Retry brief WAL setup/teardown
     * races here so the UI thread and writer stay independent. */
    static const unsigned retry_msec[] = {5, 10, 20, 40, 80};
    for (size_t attempt = 0; !ds_control_canceled(&load->control); ++attempt) {
        DsStore *store = NULL;
        status = ds_store_open(&load->db, DS_STORE_READER, &store, &error);
        if (status == DS_OK) status = ds_query_page(store, &load->query, &result, &load->control, &error);
        ds_store_close(store);
        int code = (int)(error.os_code & 0xff);
        if (status == DS_OK || (code != SQLITE_BUSY && code != SQLITE_LOCKED) ||
            attempt == G_N_ELEMENTS(retry_msec)) break;
        g_usleep(retry_msec[attempt] * G_TIME_SPAN_MILLISECOND);
    }
    if (status != DS_OK && ds_control_canceled(&load->control)) status = DS_CANCELED;
    if (status == DS_OK) g_task_return_pointer(task, result, (GDestroyNotify)ds_page_free);
    else g_task_return_new_error(task, G_IO_ERROR,
        status == DS_CANCELED ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED,
        "%s", *error.message ? error.message : "Result page could not be loaded");
}

static void start_next(DsPagedModel *model);

static void queue_page(DsPagedModel *model, guint offset, bool prefetch)
{
    if (find_page(model, offset) || (model->error && offset == model->failed_offset)) return;
    PageLoad *active = model->active;
    if (active && !active->count_only && active->generation == model->generation &&
        active->request.offset == offset) {
        if (!prefetch) active->request.prefetch = false;
        return;
    }
    for (size_t i = 0; i < model->request_count; ++i) {
        if (model->requests[i].offset == offset) {
            if (!prefetch) model->requests[i].prefetch = false;
            return;
        }
    }
    if (prefetch && model->request_count) return;
    if (model->request_count == QUEUED_PAGES) --model->request_count;
    model->requests[model->request_count++] = (PageRequest){offset, prefetch};
    start_next(model);
}

static guint list_length(const DsPagedModel *model)
{
    return (guint)MIN(model->total, model->cap ? model->cap : G_MAXUINT);
}

static void query_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    (void)data;
    GTask *task = G_TASK(result);
    PageLoad *load = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    DsPage *page = g_task_propagate_pointer(task, &error);
    DsPagedModel *model = g_weak_ref_get(&load->model);
    if (!model) {
        ds_page_free(page);
        return;
    }

    model->active = NULL;
    model->applying = true;
    if (load->generation == model->generation) {
        guint offset = load->request.offset;
        if (page && load->count_only) {
            guint old_length = model->length;
            model->total = MAX(model->total, page->total);
            model->length = list_length(model);
            if (model->length > old_length)
                g_list_model_items_changed(G_LIST_MODEL(model), old_length, 0, model->length - old_length);
            if (load->generation == model->generation && model->error) {
                guint retry_offset = model->failed_offset;
                g_clear_pointer(&model->error, g_free);
                if (retry_offset < model->length) queue_page(model, retry_offset, false);
                g_object_notify(G_OBJECT(model), "query-error");
            }
        } else if (page) {
            guint old_length = model->length;
            if (!load->query.omit_total) model->total = page->total;
            model->length = list_length(model);
            add_page(model, offset, page);
            if (model->length != old_length)
                g_list_model_items_changed(G_LIST_MODEL(model), MIN(old_length, model->length),
                    old_length > model->length ? old_length - model->length : 0,
                    model->length > old_length ? model->length - old_length : 0);
            else if (offset < model->length && page->count) {
                guint changed = MIN((guint)page->count, model->length - offset);
                g_list_model_items_changed(G_LIST_MODEL(model), offset, changed, changed);
            }

            uint64_t following = (uint64_t)offset + page->count;
            if (load->generation == model->generation && !load->request.prefetch && page->count &&
                following < model->length)
                queue_page(model, (guint)following, true);
        } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
                   (!load->count_only || !model->error || model->failed_offset == G_MAXUINT)) {
            /* Keep a failed page available for retry after a later recount. */
            g_free(model->error);
            model->error = g_strdup(error->message);
            model->failed_offset = load->count_only ? G_MAXUINT : offset;
            g_object_notify(G_OBJECT(model), "query-error");
        }
    }
    model->applying = false;
    ds_page_free(page);
    start_next(model);
    g_object_unref(model);
}

static void start_next(DsPagedModel *model)
{
    if (model->active || model->applying || (!model->request_count && !model->append_pending)) return;
    bool count_only = model->append_pending && (!model->request_count || model->requests[0].prefetch);
    PageRequest request = {0};
    if (count_only) {
        model->append_pending = false;
    } else {
        request = model->requests[0];
        --model->request_count;
        if (model->request_count) model->requests[0] = model->requests[1];
    }

    PageLoad *load = g_new0(PageLoad, 1);
    load->request = request;
    load->count_only = count_only;
    load->generation = model->generation;
    g_weak_ref_init(&load->model, model);
    load->db = (DsNativePath){model->db.encoding, g_bytes_ref(model->db.raw)};
    load->query = copy_query(&model->query);
    load->query.offset = request.offset;
    /* The count ignores the cursor. At the highest possible ID there are
     * no matching rows to materialize alongside it. */
    load->query.after_id = count_only ? INT64_MAX : 0;
    load->query.limit = count_only ? 1 : MIN(DS_PAGE_MAX - request.offset % DS_PAGE_MAX,
                                             G_MAXUINT - request.offset);
    load->query.omit_total = !count_only && request.offset != 0;
    ds_control_init(&load->control);
    model->active = load;

    GTask *task = g_task_new(NULL, NULL, query_done, NULL);
    g_task_set_task_data(task, load, page_load_free);
    g_task_run_in_thread(task, query_worker);
    g_object_unref(task);
}

static GType get_item_type(GListModel *list) { (void)list; return ds_ui_row_get_type(); }

static guint get_n_items(GListModel *list) { return ((DsPagedModel *)list)->length; }

static gpointer get_item(GListModel *list, guint position)
{
    DsPagedModel *model = (DsPagedModel *)list;
    if (position >= model->length) return NULL;
    RowPage *page = find_page(model, position);
    if (page) {
        page->last_access = g_get_monotonic_time();
        g_queue_unlink(&model->cache->lru, &page->lru_link);
        g_queue_push_tail_link(&model->cache->lru, &page->lru_link);
        return g_object_ref(page->items[position - page->offset]);
    }

    /* Oversized imported names may cause SQLite to return a short page.
     * Resume at the first uncached position instead of leaving a hole. */
    guint offset = position - position % DS_PAGE_MAX;
    for (RowPage *part = find_page(model, offset); part; part = find_page(model, offset))
        offset = part->offset + part->count;
    queue_page(model, offset, false);
    return g_object_ref(model->placeholder);
}

static void list_model_init(GListModelInterface *interface)
{
    interface->get_item_type = get_item_type;
    interface->get_n_items = get_n_items;
    interface->get_item = get_item;
}

void ds_paged_model_refresh(GListModel *list)
{
    DsPagedModel *model = (DsPagedModel *)list;
    guint count = model->length;
    ++model->generation;
    if (model->active) ds_control_cancel(&model->active->control);
    model->total = 0;
    model->length = 0;
    model->request_count = 0;
    model->append_pending = false;
    for (size_t i = 0; i < CACHED_PAGES; ++i) clear_page(model->cache, &model->pages[i]);
    if (model->error) {
        g_clear_pointer(&model->error, g_free);
        g_object_notify(G_OBJECT(model), "query-error");
    }
    if (count) g_list_model_items_changed(list, 0, count, 0);
    queue_page(model, 0, false);
}

void ds_paged_model_update_appended(GListModel *list)
{
    DsPagedModel *model = (DsPagedModel *)list;
    const DsQuery *query = &model->query;
    g_return_if_fail(query->sort == DS_SORT_ID && !query->descending && !query->kind &&
        (!query->filter || !*query->filter) && !query->extension && !query->issues_only &&
        !query->has_min_bytes && !query->has_max_bytes && !query->has_mtime_from && !query->has_mtime_to);
    model->append_pending = true;
    start_next(model);
}

bool ds_paged_model_busy(GListModel *list)
{
    DsPagedModel *model = (DsPagedModel *)list;
    return model->active || model->applying || model->request_count || model->append_pending;
}

void ds_paged_model_set_query(GListModel *list, const DsQuery *query)
{
    g_return_if_fail(query != NULL);
    DsPagedModel *model = (DsPagedModel *)list;
    DsQuery copy = copy_query(query);
    clear_query(&model->query);
    model->query = copy;
    ds_paged_model_refresh(list);
}

void ds_paged_model_set_cap(GListModel *list, guint cap)
{
    DsPagedModel *model = (DsPagedModel *)list;
    guint old = model->length;
    model->cap = cap;
    model->length = list_length(model);
    if (model->length > old) g_list_model_items_changed(list, old, 0, model->length - old);
    if (model->length < old) g_list_model_items_changed(list, model->length, old - model->length, 0);
}

static void ds_paged_model_get_property(GObject *object, guint property, GValue *value,
                                        GParamSpec *spec)
{
    if (property == 1) g_value_set_string(value, ((DsPagedModel *)object)->error);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property, spec);
}

static void ds_paged_model_finalize(GObject *object)
{
    DsPagedModel *model = (DsPagedModel *)object;
    if (model->active) ds_control_cancel(&model->active->control);
    for (size_t i = 0; i < CACHED_PAGES; ++i) clear_page(model->cache, &model->pages[i]);
    g_rc_box_release(model->cache);
    ds_path_clear(&model->db);
    clear_query(&model->query);
    g_object_unref(model->placeholder);
    g_free(model->error);
    G_OBJECT_CLASS(ds_paged_model_parent_class)->finalize(object);
}

static void ds_paged_model_class_init(DsPagedModelClass *class)
{
    GObjectClass *object_class = G_OBJECT_CLASS(class);
    object_class->finalize = ds_paged_model_finalize;
    object_class->get_property = ds_paged_model_get_property;
    g_object_class_install_property(object_class, 1, g_param_spec_string("query-error", "Query error",
        "Most recent result page error", NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

static void ds_paged_model_init(DsPagedModel *model)
{
    RowCache *cache = g_private_get(&thread_cache);
    if (!cache) {
        cache = g_rc_box_new0(RowCache);
        g_private_set(&thread_cache, cache);
    }
    model->cache = g_rc_box_acquire(cache);
    model->placeholder = g_object_new(ds_ui_row_get_type(), NULL);
}

GListModel *ds_paged_model_new(const DsNativePath *db, const DsQuery *query)
{
    g_return_val_if_fail(db && db->raw && query, NULL);
    DsPagedModel *model = g_object_new(ds_paged_model_get_type(), NULL);
    model->db = (DsNativePath){db->encoding, g_bytes_ref(db->raw)};
    ds_paged_model_set_query(G_LIST_MODEL(model), query);
    return G_LIST_MODEL(model);
}

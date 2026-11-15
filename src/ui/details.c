#include "details.h"
#include "diskscope/ds_statistics.h"

#include <math.h>

static DsStatus full_path(DsStore *db, int64_t id, DsNativePath *out, DsError *error)
{
    DsStoreInfo info = {0};
    DsNativePath relative = {0};
    DsStatus status = ds_store_info(db, &info, error);
    if (status == DS_OK)
        status = ds_store_relative_path(db, id, &relative, error);
    if (status == DS_OK) {
        size_t root_len = 0, rel_len = 0;
        const uint8_t *root = g_bytes_get_data(info.root.raw, &root_len);
        const uint8_t *suffix = g_bytes_get_data(relative.raw, &rel_len);
        const bool utf16 = info.root.encoding == DS_PATH_UTF16LE;
        const uint8_t separator[] = {utf16 ? '\\' : '/', 0};
        size_t unit = utf16 ? 2u : 1u;
        bool needs_sep = rel_len && root_len >= unit && ((utf16 && root[root_len - 1]) ||
                         (root[root_len - unit] != separator[0] &&
                         (!utf16 || root[root_len - unit] != '/')));
        if (root_len + (needs_sep ? unit : 0) + rel_len > DS_PATH_MAX) {
            status = ds_error(error, DS_LIMIT, 0, "Native path exceeded its storage limit");
        } else {
            GByteArray *combined = g_byte_array_sized_new((guint)(root_len + rel_len + unit));
            g_byte_array_append(combined, root, (guint)root_len);
            if (needs_sep)
                g_byte_array_append(combined, separator, (guint)unit);
            if (rel_len)
                g_byte_array_append(combined, suffix, (guint)rel_len);
            status = ds_path_new(info.root.encoding, combined->data, combined->len, out, error);
            g_byte_array_unref(combined);
        }
    }
    ds_path_clear(&relative);
    ds_store_info_clear(&info);
    return status;
}

static gint by_weight(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    const DsTreemapItem *left = a, *right = b;
    if (left->size.bytes != right->size.bytes)
        return left->size.bytes > right->size.bytes ? -1 : 1;
    return (left->id > right->id) - (left->id < right->id);
}

static DsStatus load_children(DsStore *db, int64_t folder, DsMetric metric, bool preview, GCancellable *cancel,
                              DsDetails *details, DsError *error)
{
    DsQuery query = {.parent_id=folder, .after_id=0, .limit=DS_PAGE_MAX, .omit_total=true,
                     .sort=DS_SORT_ID, .metric=metric};
    GSequence *largest = g_sequence_new(g_free);
    uint64_t other_bytes = 0;
    bool overflow = false;
    DsStatus status = DS_OK;
    for (;;) {
        if (g_cancellable_is_cancelled(cancel)) {
            status = DS_CANCELED;
            break;
        }
        DsPage *page = NULL;
        status = ds_query_page(db, &query, &page, NULL, error);
        if (status != DS_OK)
            break;
        for (size_t i = 0; i < page->count; ++i) {
            const DsRow *row = &page->rows[i];
            query.after_id = row->entry.id;
            DsSize size = row->metrics[metric];
            if (size.quality != DS_KNOWN) {
                ++details->unknown;
            } else if (!size.bytes) {
                ++details->zero;
            } else {
                DsTreemapItem item = {row->entry.id, size};
                GSequenceIter *last = g_sequence_iter_prev(g_sequence_get_end_iter(largest));
                if (g_sequence_get_length(largest) < (gint)DS_TREEMAP_MAX ||
                    by_weight(&item, g_sequence_get(last), NULL) < 0) {
                    g_sequence_insert_sorted(largest, g_memdup2(&item, sizeof item), by_weight, NULL);
                    if (g_sequence_get_length(largest) <= (gint)DS_TREEMAP_MAX)
                        continue;
                    last = g_sequence_iter_prev(g_sequence_get_end_iter(largest));
                    size = ((DsTreemapItem *)g_sequence_get(last))->size;
                    g_sequence_remove(last);
                }
                ++details->other;
                overflow |= !ds_u64_add(other_bytes, size.bytes, &other_bytes);
            }
        }
        bool done = preview || page->count == 0;
        ds_page_free(page);
        if (done)
            break;
    }
    if (status == DS_OK) {
        details->map = g_array_sized_new(false, false, sizeof(DsTreemapItem), DS_TREEMAP_MAX + 1);
        GSequenceIter *iter = g_sequence_get_begin_iter(largest);
        while (!g_sequence_iter_is_end(iter)) {
            g_array_append_vals(details->map, g_sequence_get(iter), 1);
            iter = g_sequence_iter_next(iter);
        }
        if (details->other) {
            DsTreemapItem other = {0, {other_bytes, overflow ? DS_OVERFLOW : DS_KNOWN}};
            g_array_append_val(details->map, other);
        }
    }
    g_sequence_free(largest);
    return status;
}

DsStatus ds_details_load(DsStore *db, int64_t id, int64_t folder, DsMetric metric,
                         bool preview, GCancellable *cancel, DsDetails *details, DsError *error)
{
    DsStatus status = id > 0 ? full_path(db, id, &details->path, error) : DS_OK;
    if (status == DS_OK && folder > 0)
        status = load_children(db, folder, metric, preview, cancel, details, error);
    return status;
}

void ds_details_clear(DsDetails *details)
{
    ds_path_clear(&details->path);
    g_clear_pointer(&details->map, g_array_unref);
    *details = (DsDetails){0};
}

typedef struct { uint64_t count, bytes, unknown; bool overflow; } Group;

static void group_add(Group *group, const DsRow *row, DsMetric metric)
{
    ++group->count;
    if (row->metrics[metric].quality == DS_KNOWN)
        group->overflow |= !ds_u64_add(group->bytes, row->metrics[metric].bytes, &group->bytes);
    else
        ++group->unknown;
}

static void print_bucket(GString *buffer, const char *name, const Group *group)
{
    char *size = group->overflow ? g_strdup("범위 초과") :
                 g_format_size_full(group->bytes, G_FORMAT_SIZE_IEC_UNITS);
    g_string_append_printf(buffer, "%s  |  %s  |  %" G_GUINT64_FORMAT
        "개  |  미확인 %" G_GUINT64_FORMAT "개\n", name, size, group->count, group->unknown);
    g_free(size);
}

DsStatus ds_statistics_report(DsStore *db, DsMetric metric, bool group_age, GCancellable *cancel,
                              char **out, DsError *error)
{
    const char *age_labels[] = {
        "7일 미만", "7–29일", "30–89일", "90–364일", "365일 이상", "미래 시각", "미확인"
    };
    Group ages[DS_AGE_GROUPS] = {{0}}, other = {0};
    DsStoreInfo info = {0};
    DsStatus status = ds_store_info(db, &info, error);
    int64_t created = info.started_at;
    /* Stores use Unix seconds. Accept an older microsecond-valued timestamp. */
    if (created > INT64_C(1000000000000))
        created /= G_USEC_PER_SEC;
    ds_store_info_clear(&info);
    GHashTable *extensions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    DsQuery query = {.parent_id=DS_QUERY_ALL, .kind=DS_FILE, .limit=DS_PAGE_MAX,
                     .sort=DS_SORT_ID, .metric=metric, .omit_total=true};
    while (status == DS_OK) {
        if (g_cancellable_is_cancelled(cancel)) {
            status = DS_CANCELED;
            break;
        }
        DsPage *page = NULL;
        status = ds_query_page(db, &query, &page, NULL, error);
        if (status != DS_OK)
            break;
        for (size_t i = 0; i < page->count; ++i) {
            const DsRow *row = &page->rows[i];
            query.after_id = row->entry.id;
            if (group_age) {
                group_add(&ages[ds_age_group(created, row->entry.mtime_known, row->entry.mtime)],
                          row, metric);
            } else {
                char *key = ds_extension(row->display);
                Group *found = g_hash_table_lookup(extensions, key);
                if (!found && g_hash_table_size(extensions) < 2048) {
                    found = g_new0(Group, 1);
                    g_hash_table_insert(extensions, key, found);
                } else {
                    g_free(key);
                }
                group_add(found ? found : &other, row, metric);
            }
        }
        bool done = page->count == 0;
        ds_page_free(page);
        if (done)
            break;
    }
    if (status == DS_OK) {
        GString *output = g_string_new(group_age ? "수정일 · 크기 · 항목 · 미확인\n\n" :
                                                    "확장자 · 크기 · 항목 · 미확인\n\n");
        if (group_age) {
            for (size_t i = 0; i < DS_AGE_GROUPS; ++i)
                print_bucket(output, age_labels[i], &ages[i]);
        } else {
            GList *names = g_list_sort(g_hash_table_get_keys(extensions), (GCompareFunc)g_strcmp0);
            for (GList *item = names; item; item = item->next) {
                const char *ext = item->data;
                print_bucket(output, *ext ? ext : "확장자 없음", g_hash_table_lookup(extensions, ext));
            }
            if (other.count)
                print_bucket(output, "기타", &other);
            g_list_free(names);
        }
        *out = g_string_free(output, false);
    }
    g_hash_table_unref(extensions);
    return status;
}

#include "ui.h"
#include <glib/gstdio.h>

typedef struct {
    GKeyFile *file;
    gint revision;
} Configuration;

static GMutex settings_lock;
static gint latest;

static char *settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "diskscope", "settings.ini", NULL);
}

static void clear_configuration(gpointer ptr)
{
    Configuration *c = ptr;
    g_key_file_unref(c->file);
    g_free(c);
}

static void write_file(GTask *task, gpointer object, gpointer ptr, GCancellable *cancel)
{
    (void)object;
    (void)cancel;
    Configuration *c = ptr;
    g_mutex_lock(&settings_lock);
    if (c->revision == g_atomic_int_get(&latest)) {
        char *path = settings_path();
        char *dir = g_path_get_dirname(path);
        if (g_mkdir_with_parents(dir, 0700) == 0) {
            size_t length;
            char *text = g_key_file_to_data(c->file, &length, NULL);
            g_file_set_contents_full(path, text, (gssize)length, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
            g_free(text);
        }
        g_free(dir);
        g_free(path);
    }
    g_mutex_unlock(&settings_lock);
    g_task_return_boolean(task, true);
}

void ui_settings_save(View *v)
{
    if (v->applying_settings || v->closing)
        return;
    Configuration *c = g_new0(Configuration, 1);
    c->file = g_key_file_new();
    g_key_file_set_integer(c->file, "Explorer", "Theme", (int)gtk_drop_down_get_selected(v->theme));
    g_key_file_set_integer(c->file, "Explorer", "Profile", (int)gtk_drop_down_get_selected(v->profile));
    g_key_file_set_integer(c->file, "Explorer", "Metric", (int)gtk_drop_down_get_selected(v->metric));
    g_key_file_set_boolean(c->file, "Explorer", "Hide", gtk_check_button_get_active(GTK_CHECK_BUTTON(v->hide)));
    g_key_file_set_string(c->file, "Explorer", "Exclude", gtk_editable_get_text(GTK_EDITABLE(v->exclude)));
    if (v->cache.raw) {
        size_t length;
        const uint8_t *bytes = g_bytes_get_data(v->cache.raw, &length);
        char *encoded = g_base64_encode(bytes, length);
        g_key_file_set_string(c->file, "Explorer", "Cache", encoded);
        g_key_file_set_integer(c->file, "Explorer", "Encoding", v->cache.encoding);
        g_free(encoded);
    }
    c->revision = g_atomic_int_add(&latest, 1) + 1;
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, c, clear_configuration);
    g_task_run_in_thread(task, write_file);
    g_object_unref(task);
}

static void read_file(GTask *task, gpointer object, gpointer ptr, GCancellable *cancel)
{
    (void)object;
    (void)cancel;
    Configuration *c = ptr;
    char *path = settings_path();
    GStatBuf st;
    bool found = g_stat(path, &st) == 0 && st.st_size >= 0 && st.st_size <= (int64_t)DS_PATH_MAX &&
                 g_key_file_load_from_file(c->file, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    g_task_return_boolean(task, found);
}

static void apply(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)object;
    View *v = data;
    GTask *task = G_TASK(result);
    Configuration *c = g_task_get_task_data(task);
    bool current = c->revision == g_atomic_int_get(&latest);
    if (v->closing || !current || !g_task_propagate_boolean(task, NULL))
        return;
    v->applying_settings = true;
    gtk_drop_down_set_selected(v->theme, (guint)CLAMP(g_key_file_get_integer(c->file, "Explorer", "Theme", NULL), 0, 2));
    gtk_drop_down_set_selected(v->profile, (guint)CLAMP(g_key_file_get_integer(c->file, "Explorer", "Profile", NULL), 0, 1));
    gtk_drop_down_set_selected(v->metric, (guint)CLAMP(g_key_file_get_integer(c->file, "Explorer", "Metric", NULL), 0, 2));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(v->hide), g_key_file_get_boolean(c->file, "Explorer", "Hide", NULL));
    char *excluded = g_key_file_get_string(c->file, "Explorer", "Exclude", NULL);
    gtk_editable_set_text(GTK_EDITABLE(v->exclude), excluded ? excluded : "");
    g_free(excluded);
    char *encoded = g_key_file_get_string(c->file, "Explorer", "Cache", NULL);
    if (encoded) {
        size_t length;
        uint8_t *bytes = g_base64_decode(encoded, &length);
        DsNativePath path = {0};
        int encoding = g_key_file_get_integer(c->file, "Explorer", "Encoding", NULL);
        if (length && ds_path_new((DsEncoding)encoding, bytes, length, &path, NULL) == DS_OK)
            ui_copy_path(&v->cache, &path);
        ds_path_clear(&path);
        g_free(bytes);
        g_free(encoded);
    }
    v->applying_settings = false;
}

void ui_settings_load(View *v)
{
    Configuration *c = g_new0(Configuration, 1);
    c->file = g_key_file_new();
    c->revision = g_atomic_int_get(&latest);
    GTask *task = g_task_new(v->window, NULL, apply, v);
    g_task_set_task_data(task, c, clear_configuration);
    g_task_run_in_thread(task, read_file);
    g_object_unref(task);
}

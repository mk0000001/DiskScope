#include <gtk/gtk.h>
void ds_window_activate(GtkApplication *, gpointer);
void ds_window_open(GtkApplication *, GFile **, int, const char *, gpointer);

#ifndef G_OS_WIN32
static void set_app_icon(GApplication *app, gpointer data)
{
    (void)data;
    gtk_window_set_default_icon_name(g_application_get_application_id(app));
}
#endif

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--cairo") == 0) {
            g_setenv("GSK_RENDERER", "cairo", TRUE);
            for (int j = i; j < argc; ++j)
                argv[j] = argv[j + 1];
            --argc;
            break;
        }
    }
    const char *app_id = "org.diskscope.Explorer";
#ifndef G_OS_WIN32
    /* The Wayland fallback for this non-unique app is the program name. */
    g_set_prgname(app_id);
    g_set_application_name("DiskScope");
#endif
    GtkApplication *app = gtk_application_new(app_id, G_APPLICATION_NON_UNIQUE | G_APPLICATION_HANDLES_OPEN);
#ifndef G_OS_WIN32
    g_signal_connect(app, "startup", G_CALLBACK(set_app_icon), NULL);
#endif
    g_signal_connect(app, "activate", G_CALLBACK(ds_window_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(ds_window_open), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

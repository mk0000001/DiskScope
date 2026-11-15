#include <gtk/gtk.h>
void ds_window_activate(GtkApplication *, gpointer);
void ds_window_open(GtkApplication *, GFile **, int, const char *, gpointer);

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
    GtkApplication *app = gtk_application_new("org.diskscope.Explorer", G_APPLICATION_NON_UNIQUE | G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(ds_window_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(ds_window_open), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

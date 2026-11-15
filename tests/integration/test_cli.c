#include "diskscope/ds_snapshot.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static char *program;

#ifdef G_OS_WIN32
static void require_subsystem(const char *path, WORD expected)
{
    FILE *file = g_fopen(path, "rb");
    g_assert_nonnull(file);
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS pe;
    g_assert_cmpuint(fread(&dos, sizeof dos, 1, file), ==, 1);
    g_assert_cmpuint(dos.e_magic, ==, IMAGE_DOS_SIGNATURE);
    g_assert_cmpint(fseek(file, dos.e_lfanew, SEEK_SET), ==, 0);
    g_assert_cmpuint(fread(&pe, sizeof pe, 1, file), ==, 1);
    fclose(file);
    g_assert_cmpuint(pe.Signature, ==, IMAGE_NT_SIGNATURE);
    g_assert_cmpuint(pe.OptionalHeader.Subsystem, ==, expected);
}

static void console_subsystems(void)
{
    g_autofree char *folder = g_path_get_dirname(program);
    g_autofree char *worker = g_build_filename(folder, "diskscope-worker.exe", NULL);
    require_subsystem(worker, IMAGE_SUBSYSTEM_WINDOWS_GUI);
    require_subsystem(program, IMAGE_SUBSYSTEM_WINDOWS_CUI);
}
#endif

static int run(const char *const args[])
{
    GError *error = NULL;
    GSubprocess *p = g_subprocess_newv(args, G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
        G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    g_assert_no_error(error);
    g_assert_true(g_subprocess_wait(p, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(g_subprocess_get_if_exited(p));
    int code = g_subprocess_get_exit_status(p);
    g_object_unref(p);
    return code;
}

static void usage(void)
{
    g_assert_cmpint(run((const char *[]){program, "--version", NULL}), ==, 0);
    g_assert_cmpint(run((const char *[]){program, "scan", "--root", NULL}), ==, 4);
}

static void scan(void)
{
    GError *error = NULL;
    char *fixture = g_dir_make_tmp("diskscope-cli-XXXXXX", &error);
    g_assert_no_error(error);
    char *root = g_build_filename(fixture, "source folder", NULL);
    char *file = g_build_filename(root, "-한글🚀", NULL);
    char *cache = g_build_filename(fixture, "cache", NULL);
    char *output = g_build_filename(fixture, "result.dscan", NULL);
    g_assert_cmpint(g_mkdir(root, 0700), ==, 0);
    g_assert_true(g_file_set_contents(file, "seventeen letters", 17, &error));
    g_assert_no_error(error);
    const char *args[] = {program, "scan", "--root", root, "--cache-dir", cache,
                          "--output", output, NULL};
    g_assert_cmpint(run(args), ==, 0);
    DsNativePath path = {0};
    g_assert_cmpint(ds_path_from_arg(output, &path, NULL), ==, DS_OK);
    DsStore *store = NULL;
    g_assert_cmpint(ds_snapshot_open(&path, &store, NULL, NULL), ==, DS_OK);
    DsStoreInfo info = {0};
    g_assert_cmpint(ds_store_info(store, &info, NULL), ==, DS_OK);
    g_assert_true(info.entry_count == 2);
    g_assert_true(info.aggregation_complete);
    g_assert_cmpint(info.coverage, ==, DS_COVERAGE_COMPLETE);
    DsVolumeInfo volume;
    g_assert_cmpint(ds_store_volume(store, &volume, NULL), ==, DS_OK);
    g_assert_cmpint(volume.captured_at, >, 0);
    g_assert_cmpint(volume.available.quality, ==, DS_KNOWN);
    if (volume.total.quality == DS_KNOWN) {
        g_assert_cmpuint(volume.total.bytes, >, 17);
        g_assert_cmpint(volume.used.quality, ==, DS_KNOWN);
        g_assert_cmpuint(volume.used.bytes, <=, volume.total.bytes);
    }
    ds_store_info_clear(&info);
    ds_store_close(store);
    ds_path_clear(&path);
    g_unlink(output);
    g_unlink(file);
    g_rmdir(root);
    GDir *scans = g_dir_open(cache, 0, NULL);
    if (scans) {
        const char *name;
        while ((name = g_dir_read_name(scans))) {
            char *child = g_build_filename(cache, name, NULL);
            char *database = g_build_filename(child, "scan.sqlite", NULL);
            g_unlink(database);
            char *log = g_build_filename(child, "diagnostics.txt", NULL);
            g_unlink(log);
            g_free(log);
            char *lock = g_build_filename(child, "scan.lock", NULL);
            g_unlink(lock);
            g_free(lock);
            g_rmdir(child);
            g_free(database);
            g_free(child);
        }
        g_dir_close(scans);
    }
    g_rmdir(cache);
    g_rmdir(fixture);
    g_free(output);
    g_free(cache);
    g_free(file);
    g_free(root);
    g_free(fixture);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    program = argv[1];
    argc = 1;
    argv[1] = NULL;
    g_test_init(&argc, &argv, NULL);
#ifdef G_OS_WIN32
    g_test_add_func("/windows_console_subsystems", console_subsystems);
#endif
    g_test_add_func("/usage_codes", usage);
    g_test_add_func("/real_worker_scan", scan);
    return g_test_run();
}

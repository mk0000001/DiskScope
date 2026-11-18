#include "worker_process.h"

#include <glib/gstdio.h>
#include <wchar.h>

static const WCHAR inherited_event[] = L"DISKSCOPE_LAUNCH_TEST_EVENT";
static const uint8_t payload[] = {0x00, 0x0d, 0xff};

static int run_child(const WCHAR *value)
{
    HANDLE sentinel = (HANDLE)(uintptr_t)wcstoull(value, NULL, 10);
    SetEvent(sentinel);
    HANDLE commands = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE events = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleWindow() ||
        GetFileType(commands) != FILE_TYPE_PIPE || GetFileType(events) != FILE_TYPE_PIPE ||
        GetFileType(GetStdHandle(STD_ERROR_HANDLE)) != FILE_TYPE_CHAR)
        return 10;

    uint8_t input[sizeof(payload)];
    DWORD count;
    if (!ReadFile(commands, input, sizeof(input), &count, NULL) || count != sizeof(input))
        return 11;
    uint8_t extra;
    if (ReadFile(commands, &extra, sizeof(extra), &count, NULL) || GetLastError() != ERROR_BROKEN_PIPE)
        return 12;
    if (!WriteFile(events, input, sizeof(input), &count, NULL) || count != sizeof(input))
        return 13;
    return 0;
}

static void launch_with_restricted_handles(void)
{
    WCHAR image[32768];
    DWORD length = GetModuleFileNameW(NULL, image, G_N_ELEMENTS(image));
    g_assert_cmpuint(length, >, 0);
    g_assert_cmpuint(length, <, G_N_ELEMENTS(image));
    g_autofree char *original = g_utf16_to_utf8((const gunichar2 *)image, length, NULL, NULL, NULL);
    GError *problem = NULL;
    g_autofree char *folder = g_dir_make_tmp("diskscope launch-XXXXXX", &problem);
    g_assert_no_error(problem);
    g_autofree char *child_path = g_build_filename(folder, "worker copy \uD55C\uAE00.exe", NULL);
    g_autofree char *executable = NULL;
    size_t bytes;
    g_assert_true(g_file_get_contents(original, &executable, &bytes, &problem));
    g_assert_no_error(problem);
    g_assert_true(g_file_set_contents(child_path, executable, bytes, &problem));
    g_assert_no_error(problem);

    SECURITY_ATTRIBUTES shareable = {sizeof(shareable), NULL, TRUE};
    HANDLE sentinel = CreateEventW(&shareable, TRUE, FALSE, NULL);
    g_assert_nonnull(sentinel);
    WCHAR value[32];
    swprintf(value, G_N_ELEMENTS(value), L"%llu", (unsigned long long)(uintptr_t)sentinel);
    g_assert_true(SetEnvironmentVariableW(inherited_event, value));

    DsWorkerProcess child = {0};
    DsError failure = {0};
    if (!ds_worker_process_start(&child, child_path, &failure)) g_error("%s", failure.message);
    g_assert_true(SetEnvironmentVariableW(inherited_event, NULL));
    g_assert_true(g_output_stream_write_all(child.commands, payload, sizeof(payload), NULL, NULL, NULL));
    g_assert_true(g_output_stream_close(child.commands, NULL, NULL));
    uint8_t echo[sizeof(payload)];
    size_t received;
    g_assert_true(g_input_stream_read_all(child.events, echo, sizeof(echo), &received, NULL, NULL));
    g_assert_cmpuint(received, ==, sizeof(payload));
    g_assert_cmpmem(echo, sizeof(echo), payload, sizeof(payload));
    g_assert_true(ds_worker_process_wait(&child));
    g_assert_cmpuint(WaitForSingleObject(sentinel, 0), ==, WAIT_TIMEOUT);
    ds_worker_process_clear(&child);
    g_assert_true(CloseHandle(sentinel));
    g_assert_cmpint(g_unlink(child_path), ==, 0);
    g_assert_cmpint(g_rmdir(folder), ==, 0);
}

int main(int argc, char **argv)
{
    WCHAR value[32];
    if (GetEnvironmentVariableW(inherited_event, value, G_N_ELEMENTS(value)) != 0)
        return run_child(value);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/windows/session/restricted-worker-handles", launch_with_restricted_handles);
    return g_test_run();
}

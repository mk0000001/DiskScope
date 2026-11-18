#include "worker_process.h"

#ifdef G_OS_WIN32
#include <gio/gwin32inputstream.h>
#include <gio/gwin32outputstream.h>
#include <string.h>
#include <wchar.h>

static void close_handle(HANDLE handle)
{
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
}

bool ds_worker_process_start(DsWorkerProcess *child, const char *executable, DsError *error)
{
    /* The caller has already resolved and verified the installed sibling. */
    if (!g_path_is_absolute(executable)) {
        ds_error(error, DS_IO, ERROR_BAD_PATHNAME, "Worker executable path must be absolute");
        return false;
    }
    g_autofree WCHAR *image = (WCHAR *)g_utf8_to_utf16(executable, -1, NULL, NULL, NULL);
    if (!image) {
        ds_error(error, DS_IO, ERROR_NO_UNICODE_TRANSLATION, "Invalid Windows worker executable path");
        return false;
    }
    const size_t length = wcslen(image);
    g_autofree WCHAR *command = g_new(WCHAR, length + 3);
    command[0] = L'"';
    memcpy(command + 1, image, length * sizeof(WCHAR));
    command[length + 1] = L'"';
    command[length + 2] = 0;

    HANDLE inherited[3] = {NULL};
    HANDLE commands = NULL, events = NULL;
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    STARTUPINFOEXW startup = {.StartupInfo = {.cb = sizeof(startup)}};
    PROCESS_INFORMATION process = {0};
    DWORD code;
    SIZE_T bytes = 0;
    bool initialized = false, started = false;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;

    /* Only the child ends are inheritable. The handle list also excludes
     * any unrelated inheritable handles already held by the controller. */
    if (!CreatePipe(&inherited[0], &commands, NULL, 4096) ||
        !CreatePipe(&events, &inherited[1], NULL, 4096) ||
        !SetHandleInformation(inherited[0], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ||
        !SetHandleInformation(inherited[1], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
        goto done;
    inherited[2] = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (inherited[2] == INVALID_HANDLE_VALUE) goto done;

    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    if (!bytes) goto done;
    attributes = g_malloc(bytes);
    initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != FALSE;
    if (!initialized ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof(inherited), NULL, NULL))
        goto done;
    startup.lpAttributeList = attributes;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inherited[0];
    startup.StartupInfo.hStdOutput = inherited[1];
    startup.StartupInfo.hStdError = inherited[2];
    started = CreateProcessW(image, command, NULL, NULL, TRUE,
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                             NULL, NULL, &startup.StartupInfo, &process) != FALSE;

done:
    code = started ? ERROR_SUCCESS : GetLastError();
    if (initialized) DeleteProcThreadAttributeList(attributes);
    g_free(attributes);
    close_handle(process.hThread);
    for (size_t i = 0; i < G_N_ELEMENTS(inherited); ++i) close_handle(inherited[i]);
    if (!started) {
        close_handle(commands);
        close_handle(events);
        g_autofree char *detail = g_win32_error_message(code);
        ds_error(error, DS_IO, code, "Could not start the scan worker: %s", detail);
        return false;
    }
    *child = (DsWorkerProcess){.handle = process.hProcess,
                              .commands = g_win32_output_stream_new(commands, TRUE),
                              .events = g_win32_input_stream_new(events, TRUE)};
    return true;
}

void ds_worker_process_clear(DsWorkerProcess *child)
{
    g_clear_object(&child->commands);
    g_clear_object(&child->events);
    close_handle(child->handle);
    child->handle = NULL;
}

void ds_worker_process_force_exit(DsWorkerProcess *child)
{
    TerminateProcess(child->handle, 5);
}

bool ds_worker_process_wait(DsWorkerProcess *child)
{
    DWORD code;
    return WaitForSingleObject(child->handle, INFINITE) == WAIT_OBJECT_0 &&
           GetExitCodeProcess(child->handle, &code) && code == 0;
}
#else
bool ds_worker_process_start(DsWorkerProcess *child, const char *executable, DsError *error)
{
    GError *failure = NULL;
    GSubprocess *handle = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                           G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                           G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                           &failure, executable, NULL);
    if (!handle) {
        ds_error(error, DS_IO, failure->code, "%s", failure->message);
        g_error_free(failure);
        return false;
    }
    *child = (DsWorkerProcess){.handle = handle,
                              .commands = g_subprocess_get_stdin_pipe(handle),
                              .events = g_subprocess_get_stdout_pipe(handle)};
    return true;
}

void ds_worker_process_clear(DsWorkerProcess *child)
{
    g_clear_object(&child->handle);
    child->commands = NULL;
    child->events = NULL;
}

void ds_worker_process_force_exit(DsWorkerProcess *child)
{
    g_subprocess_force_exit(child->handle);
}

bool ds_worker_process_wait(DsWorkerProcess *child)
{
    return g_subprocess_wait(child->handle, NULL, NULL) && g_subprocess_get_successful(child->handle);
}
#endif

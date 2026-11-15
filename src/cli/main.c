#include "cli.h"

#include <signal.h>
#include <stdio.h>
#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef G_OS_WIN32
static volatile LONG interrupted;
static BOOL WINAPI console_control(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
        return FALSE;
    InterlockedExchange(&interrupted, 1);
    return TRUE;
}
#else
static volatile sig_atomic_t interrupted;
static void sigint(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}
#endif

void ds_cli_interrupt(void)
{
#ifdef G_OS_WIN32
    SetConsoleCtrlHandler(console_control, TRUE);
#else
    signal(SIGINT, sigint);
#endif
}

bool ds_cli_interrupted(void)
{
#ifdef G_OS_WIN32
    return InterlockedCompareExchange(&interrupted, 0, 0) != 0;
#else
    return interrupted != 0;
#endif
}

int ds_cli_result(DsStatus status, const DsError *error)
{
    if (status != DS_OK && error && error->message[0])
        g_printerr("DiskScope: %s\n", error->message);
    switch (status) {
    case DS_OK: return 0;
    case DS_PARTIAL: return 2;
    case DS_CANCELED: return 3;
    case DS_INVALID: return 4;
    default: return 5;
    }
}

static int dispatch(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        g_print("DiskScope " DS_VERSION "\n");
        return 0;
    }
    if (argc == 1 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        g_print("DiskScope " DS_VERSION "\n"
                "  scan --root PATH --output RESULT.dscan [--profile conservative|standard]\n");
        return 0;
    }
    if (strcmp(argv[1], "scan") == 0)
        return ds_cli_scan(argc - 1, argv + 1);
    return ds_cli_result(DS_INVALID, &(DsError){.message = "Unknown command. Use --help."});
}

#ifdef G_OS_WIN32
int main(void)
{
    char **argv = g_win32_get_command_line();
    if (!argv)
        return 4;
    int code = dispatch((int)g_strv_length(argv), argv);
    g_strfreev(argv);
    return code;
}
#else
int main(int argc, char **argv)
{
    return dispatch(argc, argv);
}
#endif

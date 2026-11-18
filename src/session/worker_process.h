#ifndef DISKSCOPE_WORKER_PROCESS_H
#define DISKSCOPE_WORKER_PROCESS_H

#include "diskscope/ds_types.h"
#include <gio/gio.h>
#ifdef G_OS_WIN32
#include <windows.h>
#endif

typedef struct {
    GOutputStream *commands;
    GInputStream *events;
#ifdef G_OS_WIN32
    HANDLE handle;
#else
    GSubprocess *handle;
#endif
} DsWorkerProcess;

bool ds_worker_process_start(DsWorkerProcess *, const char *executable, DsError *);
void ds_worker_process_clear(DsWorkerProcess *);
void ds_worker_process_force_exit(DsWorkerProcess *);
bool ds_worker_process_wait(DsWorkerProcess *);

#endif

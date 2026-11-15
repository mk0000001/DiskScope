#ifndef DISKSCOPE_SESSION_H
#define DISKSCOPE_SESSION_H

#include "ds_scan.h"

typedef enum {
    DS_PREPARING, DS_RUNNING, DS_PAUSE_REQUESTED, DS_PAUSED, DS_FINALIZING,
    DS_CANCEL_REQUESTED, DS_CANCEL_WAITING, DS_FINISHED, DS_WORKER_LOST
} DsStage;

typedef struct {
    DsStage stage;
    DsStatus result;          /* Final only after worker exit. */
    int64_t commit_seq;
    uint64_t observations;
    bool exited;
    bool stalled;
    char detail[512];
} DsSessionEvent;

typedef struct DsSession DsSession;
typedef void (*DsSessionCallback)(const DsSessionEvent *, void *);

/* Cache location is a local base directory. Each session creates an exclusive
 * private child. Start and callbacks may run on background threads. Keep callback
 * data alive until the terminal callback returns. Events are borrowed. */
DsSession *ds_session_start(const DsScanOptions *, DsSessionCallback, void *, DsError *);
void ds_session_request_pause(DsSession *);
void ds_session_request_resume(DsSession *);
void ds_session_request_cancel(DsSession *);
void ds_session_event(DsSession *, DsSessionEvent *);
const DsNativePath *ds_session_db_path(const DsSession *);
void ds_session_unref(DsSession *);

#endif

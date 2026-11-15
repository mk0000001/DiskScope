#ifndef DISKSCOPE_DIAGNOSTICS_H
#define DISKSCOPE_DIAGNOSTICS_H

#include "ds_session.h"
#include "ds_store.h"

typedef struct {
    DsNativePath destination;       /* Borrowed path. */
    const DsSessionEvent *worker;   /* Optional controller state captured by the caller. */
    bool include_root;              /* Opt in to the displayed scan root. */
    unsigned gtk_major, gtk_minor, gtk_micro;
} DsDiagnosticsOptions;

/* An optional reader supplies committed scan metadata. The report never
 * includes file names, raw logs, worker error messages or cache paths. */
DsStatus ds_diagnostics_save(DsStore *reader, const DsDiagnosticsOptions *options,
                             DsControl *control, DsError *error);

#endif

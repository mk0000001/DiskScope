#include "diskscope/ds_scan.h"

void ds_scan_options_init(DsScanOptions *options, DsProfile profile)
{
    *options = (DsScanOptions){
        .profile = profile == DS_STANDARD ? DS_STANDARD : DS_CONSERVATIVE,
        .jobs = profile == DS_STANDARD ? 2 : 1,
        .cache_budget = DS_CACHE_BUDGET,
        .include_hidden = true,
    };
}

void ds_control_init(DsControl *control)
{
    *control = (DsControl){0};
    g_mutex_init(&control->lock);
    g_cond_init(&control->changed);
}

void ds_control_clear(DsControl *control)
{
    g_cond_clear(&control->changed);
    g_mutex_clear(&control->lock);
}

void ds_control_pause(DsControl *control, bool paused)
{
    g_mutex_lock(&control->lock);
    control->pause = paused && !control->cancel;
    g_cond_broadcast(&control->changed);
    g_mutex_unlock(&control->lock);
}

void ds_control_cancel(DsControl *control)
{
    g_mutex_lock(&control->lock);
    control->cancel = true;
    control->pause = false;
    g_cond_broadcast(&control->changed);
    g_mutex_unlock(&control->lock);
}

bool ds_control_canceled(DsControl *control)
{
    if (!control)
        return false;
    g_mutex_lock(&control->lock);
    bool canceled = control->cancel;
    g_mutex_unlock(&control->lock);
    return canceled;
}

bool ds_control_checkpoint(DsControl *control)
{
    if (!control)
        return true;
    g_mutex_lock(&control->lock);
    bool notified = control->pause && !control->cancel && control->pause_changed;
    if (notified) {
        g_mutex_unlock(&control->lock);
        control->pause_changed(true, control->pause_data);
        g_mutex_lock(&control->lock);
    }
    while (control->pause && !control->cancel)
        g_cond_wait(&control->changed, &control->lock);
    bool proceed = !control->cancel;
    g_mutex_unlock(&control->lock);
    if (notified && proceed)
        control->pause_changed(false, control->pause_data);
    return proceed;
}

const char *ds_reason_name(DsReason reason)
{
    static const char *const names[] = {
        "none", "link", "placeholder", "mount-boundary", "special-file", "policy",
        "private-cache", "permission", "changed", "metadata-unavailable", "limit", "io"
    };
    return (unsigned)reason < G_N_ELEMENTS(names) ? names[reason] : "invalid";
}

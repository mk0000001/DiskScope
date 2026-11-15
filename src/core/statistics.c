#include "diskscope/ds_statistics.h"

size_t ds_age_group(int64_t observation_time, bool time_known, int64_t mtime)
{
    if (!time_known)
        return 6;
    if (mtime > observation_time)
        return 5;
    uint64_t age = (uint64_t)observation_time - (uint64_t)mtime;
    const uint64_t boundaries[] = {
        7u * 86400u, 30u * 86400u, 90u * 86400u, 365u * 86400u
    };
    size_t i = 0;
    while (i < G_N_ELEMENTS(boundaries) && age >= boundaries[i])
        ++i;
    return i;
}

char *ds_extension(const char *escaped_name)
{
    const char *dot = escaped_name ? strrchr(escaped_name, '.') : NULL;
    return dot && dot != escaped_name && dot[1] ? g_ascii_strdown(dot + 1, -1) : g_strdup("");
}

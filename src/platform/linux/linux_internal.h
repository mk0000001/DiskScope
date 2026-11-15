#ifndef DISKSCOPE_LINUX_INTERNAL_H
#define DISKSCOPE_LINUX_INTERNAL_H

#include "diskscope/ds_linux.h"
#include <linux/stat.h>
#include <sys/stat.h>

typedef struct {
    uint64_t id, parent;
    unsigned major, minor;
    char *root, *path, *filesystem;
} LinuxMount;

GPtrArray *linux_mounts_parse(const char *data);
GPtrArray *linux_mounts_read(void);
const LinuxMount *linux_mount_for(const GPtrArray *, const char *path, uint64_t mount_id);
bool linux_mount_at(const GPtrArray *, const char *path, uint64_t root_mount_id);
bool linux_mount_denied(const LinuxMount *mount);
bool linux_mount_trusted_identity(const LinuxMount *mount);

typedef struct {
    uint64_t device, inode, mount_id, links;
    mode_t mode;
    DsSize logical, allocated;
    int64_t mtime;
    bool mtime_known, placeholder;
} LinuxMetadata;

bool linux_metadata_from_statx(const struct statx *, LinuxMetadata *);
int linux_metadata_at(int directory, const char *name, int flags, LinuxMetadata *);
uint64_t linux_fd_mount_id(int fd);
bool linux_same_identity(const LinuxMetadata *, const LinuxMetadata *);

typedef enum { LINUX_OPEN_UNTESTED, LINUX_OPEN_AT2, LINUX_OPEN_AT } LinuxOpen;
int linux_open_directory(LinuxOpen *, int directory, const char *name, bool stay_on_mount,
                         int access_mode);
int linux_open_root(const char *root, LinuxOpen *method);

typedef struct {
    int fd;
    LinuxOpen method;
    char *path;
    GPtrArray *mounts;
    const LinuxMount *mount;
    LinuxMetadata metadata;
} LinuxRoot;

DsStatus linux_root_open(const DsNativePath *, LinuxRoot *, DsError *);
void linux_root_close(LinuxRoot *);
char *linux_canonical_path(const DsNativePath *, DsError *);

#endif

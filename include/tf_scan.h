#ifndef TF_SCAN_H
#define TF_SCAN_H

#include <limits.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TF_SCAN_MAX_MOUNTS 8

typedef struct {
    char source[PATH_MAX];
    char mount_point[PATH_MAX];
    char fs_type[64];
} tf_mount_t;

size_t tf_find_mounts(tf_mount_t *mounts, size_t capacity);

#endif

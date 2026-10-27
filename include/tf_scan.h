#ifndef TF_SCAN_H
#define TF_SCAN_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TF_SCAN_MAX_MOUNTS 8

typedef struct {
    char source[PATH_MAX];
    char mount_point[PATH_MAX];
    char fs_type[64];
} tf_mount_t;

typedef struct {
    uint64_t complete_count;
    uint64_t incomplete_count;
    uint64_t total_bytes;
    uint64_t printed_count;
    uint64_t skipped_system_dirs;
    uint64_t error_count;
} scan_result_t;

typedef struct {
    int include_incomplete;
    int count_only;
    uint64_t print_limit;
} scan_options_t;

size_t tf_find_mounts(tf_mount_t *mounts, size_t capacity);

int tf_scan_mcap(const char *root,
                 const scan_options_t *options,
                 scan_result_t *result);

void tf_format_bytes(uint64_t bytes, char *buffer, size_t buffer_size);

#endif

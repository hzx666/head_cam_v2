#ifndef OSS_CONFIG_H
#define OSS_CONFIG_H

#include <limits.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char endpoint[256];
    char bucket[128];
    char access_key_id[256];
    char access_key_secret[256];
    char security_token[2048];
    char object_prefix[512];
    char ossutil_path[PATH_MAX];
    char ca_cert_path[PATH_MAX];
} oss_config_t;

int oss_config_load(const char *path, oss_config_t *config,
                    char *error, size_t error_size);

int oss_config_write_interactive(const char *path,
                                 const char *default_ossutil_path,
                                 char *error, size_t error_size);

#endif

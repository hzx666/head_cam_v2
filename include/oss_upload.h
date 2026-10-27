#ifndef OSS_UPLOAD_H
#define OSS_UPLOAD_H

#include "oss_config.h"

#include <stddef.h>

int oss_build_destination(const oss_config_t *config,
                          const char *file_name,
                          char *destination, size_t destination_size,
                          char *error, size_t error_size);

int oss_upload_one(const oss_config_t *config,
                   const char *local_path,
                   const char *destination,
                   char *error, size_t error_size);

#endif

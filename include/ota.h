#ifndef HEAD_CAM_OTA_H
#define HEAD_CAM_OTA_H
#include "service.h"
#include <sys/types.h>
cJSON *ego_query(void);
int ego_upgrade(const char *path, void (*event)(const cJSON *, void *), void *context);
int ota_compare_versions(const char *current, const char *target, int *comparison);
int operation_lock(int exclusive);
typedef int (*ota_download_progress)(uint64_t bytes, void *context);
int ota_download(const char *url, const char *path, uint64_t expected,
                 ota_download_progress progress, void *context);
#endif

#ifndef HEAD_CAM_MONITOR_H
#define HEAD_CAM_MONITOR_H
#include "service.h"
#include <signal.h>
extern volatile sig_atomic_t monitor_stop;
cJSON *monitor_scan(const cJSON *config);
int monitor_connect(const cJSON *config,const char *topic,const char *will);
int monitor_publish(int fd,const char *topic,const char *payload,int retain);
int monitor_ping(int fd);
void monitor_disconnect(int fd);
#endif

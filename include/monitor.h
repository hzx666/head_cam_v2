#ifndef HEAD_CAM_MONITOR_H
#define HEAD_CAM_MONITOR_H
#include "service.h"
#include <signal.h>
#include <stddef.h>
extern volatile sig_atomic_t monitor_stop;
cJSON *monitor_scan(const cJSON *config);
int monitor_connect(const cJSON *config,const char *topic,const char *will);
int monitor_subscribe(int fd,const char *downlink_req,const char *downlink_resp);
int monitor_publish(int fd,const char *topic,const char *payload,int retain);
int monitor_publish_qos1(int fd,const char *topic,const char *payload,unsigned packet_id,int dup);
int monitor_ping(int fd);
/* 0 收到一包，1 超时，-1 失败。成功时 *body 由调用方 free。 */
int monitor_read(int fd,int timeout_ms,unsigned char *type,unsigned char **body,size_t *len);
int monitor_accept_publish(int fd,unsigned char header,unsigned char *body,size_t len,
    char *topic,size_t topic_cap,const unsigned char **payload,size_t *payload_len);
void monitor_disconnect(int fd);
#endif

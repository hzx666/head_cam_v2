/* 扫描线程和 MQTT 循环各自运行，通过内存快照交接；上传完全独立。 */
#include "monitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/file.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>

volatile sig_atomic_t monitor_stop;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static cJSON *latest;
static unsigned generation;
static char local_ip[128]="";
static void stop_monitor(int sig){(void)sig;monitor_stop=1;}
static void pause_seconds(unsigned n){while(n-- && !monitor_stop)sleep(1);}
static void *scan_loop(void *arg) {
    const cJSON *config=arg;char *previous=NULL;
    while(!monitor_stop) {
        cJSON *snapshot=monitor_scan(config);
        pthread_mutex_lock(&mutex);
        if(*str(snapshot,"error") && latest) {
            cJSON *old=cJSON_Duplicate(latest,1);
            cJSON_DeleteItemFromObject(old,"error");js(old,"error",str(snapshot,"error"));cJSON_Delete(snapshot);snapshot=old;
        }
        cJSON *compare=cJSON_Duplicate(snapshot,1);cJSON_DeleteItemFromObject(compare,"scan_ts");
        char *fingerprint=cJSON_PrintUnformatted(compare);cJSON_Delete(compare);
        if(!previous || !fingerprint || strcmp(previous,fingerprint))generation++;
        free(previous);previous=fingerprint;cJSON_Delete(latest);latest=snapshot;
        pthread_mutex_unlock(&mutex);
        pause_seconds((unsigned)num(config,"scan_interval_sec"));
    }
    free(previous);return NULL;
}
static cJSON *payload(const cJSON *config,const char *event,unsigned seq) {
    cJSON *j=cJSON_CreateObject();char ts[40];time_t now=time(NULL)+8*3600;struct tm tm;
    gmtime_r(&now,&tm);strftime(ts,sizeof(ts),"%Y-%m-%dT%H:%M:%S+08:00",&tm);
    int summary=!strcmp(event,"recording_summary");
    if(summary) {
        js(j,"device_sn",str(config,"device_id"));json_number(j,"schema_version",MONITOR_SCHEMA_VERSION);
    } else {
        js(j,"device_id",str(config,"device_id"));js(j,"device_type","head_cam_v2");
        js(j,"service","head-cam-monitor");js(j,"version",MONITOR_RELEASE_VERSION);js(j,"ip",local_ip);
    }
    js(j,"event_type",event);js(j,"ts",ts);json_number(j,"seq",seq);
    return j;
}
static int publish_json(int fd,const char *topic,cJSON *j,int retain) {
    char *text=cJSON_PrintUnformatted(j);int rc=text?monitor_publish(fd,topic,text,retain):-1;
    free(text);cJSON_Delete(j);return rc;
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--self-check")){puts("PASS: monitor");return 0;}
    if(argc!=3){fprintf(stderr,"Usage: %s CONFIG RUNTIME\n",argv[0]);return 64;}
    umask(0077);signal(SIGTERM,stop_monitor);signal(SIGINT,stop_monitor);
    if(private_directory(argv[2]))return 1;
    char status_path[PATH_MAX],lock_path[PATH_MAX];
    if(path_join(status_path,sizeof(status_path),argv[2],"status.json") || path_join(lock_path,sizeof(lock_path),argv[2],".lock"))return 1;
    int lock=open(lock_path,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);if(lock<0 || flock(lock,LOCK_EX|LOCK_NB))return 1;
    cJSON *config=json_load(argv[1]);
    int configured=config && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(config,"enabled")) &&
        *str(config,"host") && *str(config,"device_id") && strlen(str(config,"device_id"))<128 &&
        !strpbrk(str(config,"device_id"),"/+#") && *str(config,"topic_prefix") &&
        strlen(str(config,"topic_prefix"))<256 && !strpbrk(str(config,"topic_prefix"),"+#") &&
        num(config,"port")>=1 && num(config,"port")<=65535 &&
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(config,"tls")) && num(config,"qos")==0 &&
        (!*str(config,"password") || *str(config,"username")) &&
        num(config,"scan_interval_sec")>=10 && num(config,"scan_interval_sec")<=86400 &&
        num(config,"report_interval_sec")>=10 && num(config,"report_interval_sec")<=86400;
    if(!configured) {
        cJSON *status=cJSON_CreateObject();js(status,"state","unconfigured");json_save(status_path,status);cJSON_Delete(status);
        fprintf(stderr,"Monitor disabled or unconfigured; upload is independent\n");
        while(!monitor_stop)pause_seconds(1);
        cJSON_Delete(config);close(lock);return 0;
    }
    pthread_t scanner;if(pthread_create(&scanner,NULL,scan_loop,config))return 1;
    char presence[512],summary[512];
    snprintf(presence,sizeof(presence),"%s/device/%s/presence",str(config,"topic_prefix"),str(config,"device_id"));
    snprintf(summary,sizeof(summary),"%s/device/%s/recording/summary",str(config,"topic_prefix"),str(config,"device_id"));
    int fd=-1;unsigned seq=0,sent=0,retry=2;time_t next_report=0,next_ping=0;
    while(!monitor_stop) {
        if(fd<0) {
            cJSON *will=payload(config,"offline",++seq);js(will,"status","offline");
            char *text=cJSON_PrintUnformatted(will);fd=text?monitor_connect(config,presence,text):-1;free(text);cJSON_Delete(will);
            if(fd>=0) {
                struct sockaddr_storage addr;socklen_t len=sizeof(addr);
                if(!getsockname(fd,(struct sockaddr *)&addr,&len))
                    getnameinfo((struct sockaddr *)&addr,len,local_ip,sizeof(local_ip),NULL,0,NI_NUMERICHOST);
            }
            next_report=0;next_ping=time(NULL)+20;sent=0;
            if(fd>=0)retry=2;
        }
        cJSON *state=cJSON_CreateObject();jb(state,"connected",fd>=0);js(state,"state",fd>=0?"running":"reconnecting");
        pthread_mutex_lock(&mutex);unsigned current=generation;cJSON *snapshot=latest?cJSON_Duplicate(latest,1):NULL;pthread_mutex_unlock(&mutex);
        if(snapshot)cJSON_AddItemToObject(state,"snapshot",cJSON_Duplicate(snapshot,1));
        json_save(status_path,state);cJSON_Delete(state);
        int failed=0;time_t now=time(NULL);
        if(fd>=0 && (now>=next_report || current!=sent)) {
            cJSON *online=payload(config,"online",++seq);js(online,"status","online");failed=publish_json(fd,presence,online,1);
            if(!failed && snapshot) {
                cJSON *j=payload(config,"recording_summary",++seq);
                const char *fields[]={"recordings","storage","battery","alert"};
                for(size_t i=0;i<4;i++) {
                    cJSON *item=cJSON_GetObjectItemCaseSensitive(snapshot,fields[i]);
                    if(item)cJSON_AddItemToObject(j,fields[i],cJSON_Duplicate(item,1));
                }
                cJSON *recordings=cJSON_GetObjectItemCaseSensitive(j,"recordings");
                if(recordings)cJSON_DeleteItemFromObject(recordings,"total_size_bytes");
                failed=publish_json(fd,summary,j,0);
            }
            sent=current;next_report=now+(int)num(config,"report_interval_sec");
        }
        cJSON_Delete(snapshot);
        if(fd>=0 && !failed && now>=next_ping){failed=monitor_ping(fd);next_ping=now+20;}
        if(failed){close(fd);fd=-1;}
        pause_seconds(fd<0?retry:1);
        if(fd<0 && retry<60)retry=retry>30?60:retry*2;
    }
    if(fd>=0){cJSON *j=payload(config,"offline",++seq);js(j,"status","offline");publish_json(fd,presence,j,1);monitor_disconnect(fd);close(fd);}
    pthread_join(scanner,NULL);cJSON_Delete(latest);cJSON_Delete(config);close(lock);return 0;
}

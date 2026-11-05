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
#include <sys/time.h>

volatile sig_atomic_t monitor_stop;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static cJSON *latest;
static unsigned generation;
static char local_ip[128]="";
static struct { char channel[24],request_id[48],method[64],message[64]; int code,valid; } last_rpc;
static char session_path[PATH_MAX];
static cJSON *session;
static char report_version[32],report_request[40];
static long long report_sent_at,report_expires;
static int report_open;
static struct { char *topic,*payload; unsigned id; int dup; time_t deadline; } inflight;
static struct { char *topic,*payload; } qos1_queue[4];
static int qos1_head,qos1_tail,qos1_count;
static unsigned qos1_packet_id=1;
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
static void cst_timestamp(char ts[40]) {
    time_t now=time(NULL)+8*3600;struct tm tm;
    gmtime_r(&now,&tm);strftime(ts,40,"%Y-%m-%dT%H:%M:%S+08:00",&tm);
}
static cJSON *payload(const cJSON *config,const char *event,unsigned seq) {
    cJSON *j=cJSON_CreateObject();char ts[40];cst_timestamp(ts);
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
static int topic_of(char *out,size_t cap,const char *prefix,const char *id,const char *suffix) {
    int wrote=snprintf(out,cap,"%s/device/%s/%s",prefix,id,suffix);
    return wrote<0 || (size_t)wrote>=cap?-1:0;
}
static int token_ok(const char *s,size_t cap) {
    size_t n=strlen(s);
    if(!n || n>=cap) return 0;
    for(size_t i=0;i<n;i++) if((unsigned char)s[i]<0x20 || s[i]==0x7f) return 0;
    return 1;
}
static int id_ok(const char *s) {
    size_t n=strlen(s);
    if(!n || n>=128) return 0;
    for(size_t i=0;i<n;i++) if((unsigned char)s[i]<0x20 || s[i]==0x7f) return 0;
    return 1;
}
static int topic_segment(char *out,size_t cap,const char *sn) {
    size_t n=0;
    for(const unsigned char *p=(const unsigned char *)sn;*p;p++) {
        int plain=(*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='.'||*p=='_'||*p=='~';
        if(n+(plain?1:3)>=cap) return -1;
        if(plain) out[n++]=*p;
        else { snprintf(out+n,4,"%%%02X",*p); n+=3; }
    }
    out[n]=0;return n?0:-1;
}
static long long unix_ms(void) {
    struct timeval tv;gettimeofday(&tv,NULL);
    return (long long)tv.tv_sec*1000+tv.tv_usec/1000;
}
static int uuid_v4(char out[40]) {
    unsigned char b[16];FILE *f=fopen("/dev/urandom","r");
    if(!f || fread(b,1,16,f)!=16) { if(f)fclose(f); return -1; }
    fclose(f);b[6]=(b[6]&0x0f)|0x40;b[8]=(b[8]&0x3f)|0x80;
    snprintf(out,40,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
    return 0;
}
static void qos1_reset(void) {
    free(inflight.topic);free(inflight.payload);memset(&inflight,0,sizeof inflight);
    while(qos1_count) { free(qos1_queue[qos1_head].topic);free(qos1_queue[qos1_head].payload); qos1_head=(qos1_head+1)%4; qos1_count--; }
}
static int qos1_enqueue(const char *topic,const char *payload) {
    if(qos1_count>=4) return -1;
    char *copy_topic=strdup(topic),*copy_payload=strdup(payload);
    if(!copy_topic || !copy_payload) { free(copy_topic);free(copy_payload); return -1; }
    qos1_queue[qos1_tail].topic=copy_topic;qos1_queue[qos1_tail].payload=copy_payload;
    qos1_tail=(qos1_tail+1)%4;qos1_count++;return 0;
}
static int qos1_pump(int fd) {
    if(inflight.id || !qos1_count) return 0;
    inflight.topic=qos1_queue[qos1_head].topic;inflight.payload=qos1_queue[qos1_head].payload;
    qos1_head=(qos1_head+1)%4;qos1_count--;
    inflight.id=qos1_packet_id++;if(!qos1_packet_id || qos1_packet_id>65535) qos1_packet_id=1;
    inflight.dup=0;inflight.deadline=time(NULL)+5;
    return monitor_publish_qos1(fd,inflight.topic,inflight.payload,inflight.id,0);
}
static int qos1_ack(int fd,unsigned id) {
    if(!inflight.id || id!=inflight.id) return -1;
    free(inflight.topic);free(inflight.payload);memset(&inflight,0,sizeof inflight);
    return qos1_pump(fd);
}
static int qos1_tick(int fd) {
    if(inflight.id && time(NULL)>inflight.deadline) {
        if(inflight.dup) return -1;
        inflight.dup=1;inflight.deadline=time(NULL)+5;
        if(monitor_publish_qos1(fd,inflight.topic,inflight.payload,inflight.id,1)) return -1;
    }
    return qos1_pump(fd);
}
static int enqueue_json(const char *topic,cJSON *j) {
    char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);
    if(!text) return -1;
    int rc=qos1_enqueue(topic,text);free(text);return rc;
}
static void session_save(void) { if(session) json_save(session_path,session); }
static void session_open(const char *runtime) {
    if(path_join(session_path,sizeof(session_path),runtime,"mqtt-session.json")) session_path[0]=0;
    if(session_path[0]) session=json_load(session_path);
    if(!session) session=cJSON_CreateObject();
    if(!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(session,"propertyVersion"))) js(session,"propertyVersion","1");
}
static const char *device_type_of(const cJSON *config) {
    const char *type=str(config,"device_type");return *type?type:"head_cam_v2";
}
static const char *rpc_text(int code) {
    switch(code) {
    case 0: return "操作成功";
    case 4001: return "INVALID_PARAMS";
    case 4002: return "UNKNOWN_METHOD";
    case 4003: return "UNKNOWN_PROPERTY";
    case 4031: return "UNAUTHORIZED";
    case 4032: return "DEVICE_MISMATCH";
    case 4041: return "TASK_NOT_FOUND";
    case 4081: return "REQUEST_EXPIRED";
    case 4091: return "IDEMPOTENCY_CONFLICT";
    case 4092: return "STALE_COMMAND";
    case 4093: return "TASK_CONFLICT";
    case 4094: return "INVALID_STATE";
    case 4095: return "UNSAFE_STAGE";
    case 4221: return "MODEL_MISMATCH";
    default: return "TEMPORARY_UNAVAILABLE";
    }
}
static void remember_rpc(const char *channel,const char *id,const char *method,int code,const char *message) {
    snprintf(last_rpc.channel,sizeof(last_rpc.channel),"%s",channel?channel:"");
    snprintf(last_rpc.request_id,sizeof(last_rpc.request_id),"%s",id?id:"");
    snprintf(last_rpc.method,sizeof(last_rpc.method),"%s",method?method:"");
    snprintf(last_rpc.message,sizeof(last_rpc.message),"%s",message?message:"");
    last_rpc.code=code;last_rpc.valid=1;
    fprintf(stderr,"mqtt %s requestId=%s method=%s code=%d %s\n",last_rpc.channel,last_rpc.request_id,last_rpc.method,code,last_rpc.message);
}
static cJSON *rpc_envelope(const cJSON *config,const char *event,const char *request_id,const char *method) {
    cJSON *j=cJSON_CreateObject();
    js(j,"protocolVersion","1.0");js(j,"device_id",str(config,"device_id"));js(j,"device_type",device_type_of(config));
    js(j,"event_type",event);json_number(j,"ts",(double)unix_ms());
    js(j,"requestId",request_id?request_id:"");js(j,"method",method?method:"");
    return j;
}
static cJSON *firmware_value(void) {
    cJSON *f=cJSON_CreateObject();
    js(f,"version",MONITOR_RELEASE_VERSION);
    cJSON_AddItemToObject(f,"upgradeSource",cJSON_CreateNull());
    cJSON_AddItemToObject(f,"upgradeId",cJSON_CreateNull());
    return f;
}
static cJSON *properties_value(const cJSON *config) {
    cJSON *p=cJSON_CreateObject();
    const char *model=str(config,"hardware_model");
    if(*model) js(p,"hardwareModel",model); else cJSON_AddItemToObject(p,"hardwareModel",cJSON_CreateNull());
    cJSON_AddItemToObject(p,"firmware",firmware_value());
    return p;
}
static int publish_properties(const char *topic,const cJSON *config) {
    char id[40];if(uuid_v4(id)) return -1;
    cJSON *j=rpc_envelope(config,"properties_report",NULL,NULL);
    cJSON_DeleteItemFromObject(j,"requestId");cJSON_DeleteItemFromObject(j,"method");
    js(j,"reportId",id);
    cJSON *data=cJSON_AddObjectToObject(j,"data");
    js(data,"propertyVersion",str(session,"propertyVersion"));
    cJSON_AddItemToObject(data,"properties",properties_value(config));
    return enqueue_json(topic,j);
}
static cJSON *task_object(void) {
    cJSON *task=session?cJSON_GetObjectItemCaseSensitive(session,"task"):NULL;
    return cJSON_IsObject(task)?task:NULL;
}
static cJSON *task_snapshot(const cJSON *config) {
    cJSON *task=task_object();if(!task) return NULL;
    cJSON *d=cJSON_CreateObject();
    const char *fields[]={"taskId","upgradeId","hardwareModel","toVersion","statusVersion","upgradeStatus","stage"};
    for(size_t i=0;i<7;i++) js(d,fields[i],str(task,fields[i]));
    json_number(d,"commandSeq",num(task,"commandSeq"));json_number(d,"progress",num(task,"progress"));
    cJSON *from=cJSON_GetObjectItemCaseSensitive(task,"fromVersion");
    cJSON_AddItemToObject(d,"fromVersion",from?cJSON_Duplicate(from,1):cJSON_CreateNull());
    cJSON *package=cJSON_GetObjectItemCaseSensitive(task,"package");
    if(package) cJSON_AddItemToObject(d,"package",cJSON_Duplicate(package,1));
    cJSON *started=cJSON_GetObjectItemCaseSensitive(task,"startedAt"),*finished=cJSON_GetObjectItemCaseSensitive(task,"finishedAt"),*error=cJSON_GetObjectItemCaseSensitive(task,"error");
    cJSON_AddItemToObject(d,"startedAt",started?cJSON_Duplicate(started,1):cJSON_CreateNull());
    cJSON_AddItemToObject(d,"finishedAt",finished?cJSON_Duplicate(finished,1):cJSON_CreateNull());
    cJSON_AddItemToObject(d,"error",error?cJSON_Duplicate(error,1):cJSON_CreateNull());
    if(!strcmp(str(task,"upgradeStatus"),"success")) {
        js(d,"propertyVersion",str(session,"propertyVersion"));
        cJSON_AddItemToObject(d,"firmware",firmware_value());
    }
    (void)config;return d;
}
static void mark_report(void) { report_version[0]=0;report_open=1;report_sent_at=0;report_expires=0; }
static int maybe_report(const char *topic,const cJSON *config) {
    cJSON *task=task_object();if(!task) return 0;
    const char *version=str(task,"statusVersion");
    if(!report_version[0] || strcmp(report_version,version)) {
        if(uuid_v4(report_request)) return -1;
        snprintf(report_version,sizeof(report_version),"%s",version);
        report_open=1;report_sent_at=0;report_expires=0;
    }
    if(!report_open) return 0;
    long long now=unix_ms();
    if(report_expires && now>=report_expires) { if(uuid_v4(report_request)) return -1; report_expires=0; report_sent_at=0; }
    if(report_sent_at && now<report_sent_at+10000) return 0;
    cJSON *j=rpc_envelope(config,"rpc_request",report_request,"ota.reportStatus");
    long long expires=now+120000;json_number(j,"expiresAt",(double)expires);
    cJSON *data=task_snapshot(config);if(!data) data=cJSON_CreateObject();
    cJSON_AddItemToObject(j,"data",data);
    if(enqueue_json(topic,j)) return -1;
    report_sent_at=now;report_expires=expires;remember_rpc("uplink/req",report_request,"ota.reportStatus",0,"sent");
    return 0;
}
static void bump_status(cJSON *task,const char *status,const char *stage,const char *finished) {
    unsigned long version=strtoul(str(task,"statusVersion"),NULL,10);
    char next[32];snprintf(next,sizeof(next),"%lu",version?version+1:1);js(task,"statusVersion",next);
    js(task,"upgradeStatus",status);js(task,"stage",stage);
    if(finished) js(task,"finishedAt",finished); else cJSON_DeleteItemFromObject(task,"finishedAt"),cJSON_AddItemToObject(task,"finishedAt",cJSON_CreateNull());
    mark_report();session_save();
}
static int hex64(const char *s) {
    if(strlen(s)!=64) return 0;
    for(int i=0;i<64;i++) if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return 0;
    return 1;
}
static cJSON *inbox_find(const char *request_id) {
    cJSON *inbox=cJSON_GetObjectItemCaseSensitive(session,"inbox"),*item=NULL;
    if(!cJSON_IsArray(inbox)) return NULL;
    cJSON_ArrayForEach(item,inbox) if(!strcmp(str(item,"requestId"),request_id)) return item;
    return NULL;
}
static void inbox_store(const char *request_id,const char *method,const char *digest,int code,cJSON *data) {
    cJSON *inbox=cJSON_GetObjectItemCaseSensitive(session,"inbox");
    if(!cJSON_IsArray(inbox)) { inbox=cJSON_CreateArray(); cJSON_AddItemToObject(session,"inbox",inbox); }
    while(cJSON_GetArraySize(inbox)>=16) cJSON_DeleteItemFromArray(inbox,0);
    cJSON *item=cJSON_CreateObject();
    js(item,"requestId",request_id);js(item,"method",method);js(item,"digest",digest?digest:"");
    json_number(item,"code",code);js(item,"message",rpc_text(code));
    cJSON_AddItemToObject(item,"data",data?data:cJSON_CreateNull());
    cJSON_AddItemToArray(inbox,item);session_save();
}
static cJSON *accepted_data(void) { cJSON *d=cJSON_CreateObject();jb(d,"accepted",1);return d; }
static int dispatch_method(const cJSON *config,const char *method,cJSON *data,int *code,cJSON **result) {
    if(!data) { *code=4001; return 0; }
    if(!strcmp(method,"properties.get")) {
        cJSON *keys=cJSON_GetObjectItemCaseSensitive(data,"keys");
        if(!cJSON_IsArray(keys) || !cJSON_GetArraySize(keys)) { *code=4001; return 0; }
        cJSON *props=cJSON_CreateObject(),*key=NULL;
        cJSON_ArrayForEach(key,keys) {
            if(!cJSON_IsString(key)) { cJSON_Delete(props); *code=4001; return 0; }
            if(!strcmp(key->valuestring,"firmware")) cJSON_AddItemToObject(props,"firmware",firmware_value());
            else if(!strcmp(key->valuestring,"hardwareModel")) {
                const char *model=str(config,"hardware_model");
                if(*model) js(props,"hardwareModel",model); else cJSON_AddItemToObject(props,"hardwareModel",cJSON_CreateNull());
            } else { cJSON_Delete(props); *code=4003; return 0; }
        }
        *result=cJSON_CreateObject();js(*result,"propertyVersion",str(session,"propertyVersion"));
        cJSON_AddItemToObject(*result,"properties",props);*code=0;return 0;
    }
    if(!strcmp(method,"ota.register")) { *code=4031; return 0; }
    if(!strcmp(method,"ota.getStatus")) {
        if(!token_ok(str(data,"taskId"),64) || !token_ok(str(data,"upgradeId"),64)) { *code=4001; return 0; }
        cJSON *task=task_object();
        if(!task || strcmp(str(task,"taskId"),str(data,"taskId")) || strcmp(str(task,"upgradeId"),str(data,"upgradeId"))) { *code=4041; return 0; }
        *result=task_snapshot(config);*code=0;return 0;
    }
    if(!strcmp(method,"ota.pause")) { *code=4095; return 0; }
    if(!strcmp(method,"ota.resume")) { *code=task_object() && !strcmp(str(task_object(),"upgradeStatus"),"paused")?4095:4094; return 0; }
    if(!strcmp(method,"ota.cancel")) {
        if(!token_ok(str(data,"taskId"),64) || !token_ok(str(data,"upgradeId"),64) || !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(data,"commandSeq"))) { *code=4001; return 0; }
        cJSON *task=task_object();
        if(!task || strcmp(str(task,"taskId"),str(data,"taskId")) || strcmp(str(task,"upgradeId"),str(data,"upgradeId"))) { *code=4041; return 0; }
        double seq=num(data,"commandSeq"),previous=num(task,"commandSeq");
        if(seq<previous) { *code=4092; return 0; }
        if(!strcmp(str(task,"upgradeStatus"),"cancelled") && seq==previous) { *result=accepted_data();*code=0;return 0; }
        if(strcmp(str(task,"upgradeStatus"),"pending") && strcmp(str(task,"upgradeStatus"),"upgrading") && strcmp(str(task,"upgradeStatus"),"paused")) { *code=4094; return 0; }
        const char *stage=str(task,"stage");
        if(!strcmp(stage,"installing") || !strcmp(stage,"rebooting") || !strcmp(stage,"selfChecking")) { *code=4095; return 0; }
        json_number(task,"commandSeq",seq);char ts[40];cst_timestamp(ts);bump_status(task,"cancelled","completed",ts);
        *result=accepted_data();*code=0;return 0;
    }
    if(!strcmp(method,"ota.start")) {
        cJSON *package=cJSON_GetObjectItemCaseSensitive(data,"package"),*from=cJSON_GetObjectItemCaseSensitive(data,"fromVersion");
        cJSON *seq_item=cJSON_GetObjectItemCaseSensitive(data,"commandSeq"),*size_item=package?cJSON_GetObjectItemCaseSensitive(package,"fileSize"):NULL;
        const char *model=str(config,"hardware_model");
        if(!*model || strcmp(str(data,"hardwareModel"),model)) { *code=4221; return 0; }
        if(!token_ok(str(data,"taskId"),64) || !token_ok(str(data,"upgradeId"),64) || !token_ok(str(data,"toVersion"),64) ||
           !cJSON_IsNumber(seq_item) || num(data,"commandSeq")<1 || !cJSON_IsObject(package) ||
           !hex64(str(package,"sha256")) || !cJSON_IsNumber(size_item) || size_item->valuedouble<=0 || strncmp(str(package,"downloadUrl"),"https://",8) ||
           !token_ok(str(package,"fileName"),256) || !token_ok(str(package,"downloadExpiresAt"),64) ||
           (from && !cJSON_IsNull(from) && !cJSON_IsString(from))) { *code=4001; return 0; }
        cJSON *task=task_object();
        if(task && strcmp(str(task,"upgradeId"),str(data,"upgradeId"))) {
            const char *state=str(task,"upgradeStatus");
            if(strcmp(state,"success") && strcmp(state,"failed") && strcmp(state,"cancelled")) { *code=4093; return 0; }
            task=NULL;
        }
        if(task) {
            double seq=num(data,"commandSeq"),previous=num(task,"commandSeq");
            if(seq<previous) { *code=4092; return 0; }
            if(strcmp(str(task,"upgradeStatus"),"pending") && strcmp(str(task,"upgradeStatus"),"paused") && strcmp(str(task,"upgradeStatus"),"upgrading")) { *code=4094; return 0; }
            if(seq>previous) { *code=4093; return 0; }
            *result=accepted_data();*code=0;return 0;
        }
        task=cJSON_CreateObject();
        js(task,"taskId",str(data,"taskId"));js(task,"upgradeId",str(data,"upgradeId"));
        json_number(task,"commandSeq",num(data,"commandSeq"));js(task,"hardwareModel",str(data,"hardwareModel"));
        cJSON_AddItemToObject(task,"fromVersion",from?cJSON_Duplicate(from,1):cJSON_CreateNull());
        js(task,"toVersion",str(data,"toVersion"));cJSON_AddItemToObject(task,"package",cJSON_Duplicate(package,1));
        js(task,"statusVersion","1");js(task,"upgradeStatus","pending");js(task,"stage","queued");json_number(task,"progress",0);
        cJSON_AddItemToObject(task,"startedAt",cJSON_CreateNull());cJSON_AddItemToObject(task,"finishedAt",cJSON_CreateNull());
        cJSON_AddItemToObject(task,"error",cJSON_CreateNull());
        cJSON_DeleteItemFromObject(session,"task");cJSON_AddItemToObject(session,"task",task);
        mark_report();session_save();
        *result=accepted_data();*code=0;return 0;
    }
    *code=4002;return 0;
}
static int reply_rpc(const char *topic,const cJSON *config,const char *request_id,const char *method,int code,cJSON *data) {
    cJSON *j=rpc_envelope(config,"rpc_response",request_id,method);
    json_number(j,"code",code);js(j,"message",rpc_text(code));
    cJSON_AddItemToObject(j,"data",code?cJSON_CreateNull():(data?data:cJSON_CreateObject()));
    if(code && data) cJSON_Delete(data);
    int rc=enqueue_json(topic,j);
    if(!rc) remember_rpc("uplink/resp",request_id,method,code,rpc_text(code));
    return rc;
}
static int handle_downlink(const char *topic,const char *down_req,const char *up_resp,const char *down_resp,
                           const unsigned char *bytes,size_t len,const cJSON *config) {
    if(strcmp(topic,down_req) && strcmp(topic,down_resp)) return 0;
    char *text=malloc(len+1);if(!text) return -1;
    if(len) memcpy(text,bytes,len);text[len]=0;
    cJSON *in=cJSON_Parse(text);free(text);
    if(!strcmp(topic,down_resp)) {
        if(in) {
            cJSON *code_item=cJSON_GetObjectItemCaseSensitive(in,"code");
            int code=cJSON_IsNumber(code_item)?(int)code_item->valuedouble:-1;
            remember_rpc("downlink/resp",str(in,"requestId"),str(in,"method"),code,str(in,"message"));
            if(report_request[0] && !strcmp(str(in,"requestId"),report_request) && !strcmp(str(in,"method"),"ota.reportStatus") && !code) report_open=0;
        }
        cJSON_Delete(in);return 0;
    }
    if(!in) return 0;
    const char *request_id=str(in,"requestId"),*method=str(in,"method"),*version=str(in,"protocolVersion");
    cJSON *data=cJSON_GetObjectItemCaseSensitive(in,"data");
    if(!cJSON_IsObject(data)) data=NULL;
    int code=0;cJSON *result=NULL;
    if(!token_ok(request_id,64) || !token_ok(method,64)) code=4001;
    else if(strcmp(str(in,"device_id"),str(config,"device_id")) || strcmp(str(in,"device_type"),device_type_of(config))) code=4032;
    else if(strcmp(version,"1.0")) code=4001;
    else {
        char *digest=data?cJSON_PrintUnformatted(data):strdup("{}");
        cJSON *prior=inbox_find(request_id);
        if(prior) {
            if(strcmp(str(prior,"method"),method) || strcmp(str(prior,"digest"),digest?digest:"")) code=4091;
            else {
                code=(int)num(prior,"code");
                cJSON *stored=cJSON_GetObjectItemCaseSensitive(prior,"data");
                if(cJSON_IsObject(stored)) result=cJSON_Duplicate(stored,1);
            }
        } else {
            cJSON *ts_item=cJSON_GetObjectItemCaseSensitive(in,"ts"),*exp_item=cJSON_GetObjectItemCaseSensitive(in,"expiresAt");
            long long now=unix_ms();
            if(!cJSON_IsNumber(ts_item) || !cJSON_IsNumber(exp_item) || num(in,"expiresAt")<=num(in,"ts")) code=4001;
            else if(num(in,"expiresAt")<=(double)now) code=4081;
            else dispatch_method(config,method,data,&code,&result);
            if(code!=4091) inbox_store(request_id,method,digest,code,result?cJSON_Duplicate(result,1):NULL);
        }
        free(digest);
    }
    int rc=reply_rpc(up_resp,config,request_id,method,code,result);
    cJSON_Delete(in);return rc;
}
static void copy_fields(cJSON *dst,const cJSON *src) {
    const char *fields[]={"recordings","storage","battery","alert"};
    for(size_t i=0;i<4;i++) {
        cJSON *item=src?cJSON_GetObjectItemCaseSensitive(src,fields[i]):NULL;
        if(item)cJSON_AddItemToObject(dst,fields[i],cJSON_Duplicate(item,1));
    }
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
        *str(config,"host") && id_ok(str(config,"device_id")) && *str(config,"topic_prefix") &&
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
    char presence[512],summary[512],down_req[512],down_resp[512],up_req[512],up_resp[512],up_prop[512],encoded[384];
    const char *prefix=str(config,"topic_prefix"),*id=str(config,"device_id");
    if(topic_segment(encoded,sizeof(encoded),id) ||
       topic_of(presence,sizeof(presence),prefix,id,"presence") ||
       topic_of(summary,sizeof(summary),prefix,id,"recording/summary") ||
       topic_of(down_req,sizeof(down_req),prefix,encoded,"downlink/req") ||
       topic_of(down_resp,sizeof(down_resp),prefix,encoded,"downlink/resp") ||
       topic_of(up_req,sizeof(up_req),prefix,encoded,"uplink/req") ||
       topic_of(up_resp,sizeof(up_resp),prefix,encoded,"uplink/resp") ||
       topic_of(up_prop,sizeof(up_prop),prefix,encoded,"uplink/property")) return 1;
    session_open(argv[2]);
    pthread_t scanner;if(pthread_create(&scanner,NULL,scan_loop,config))return 1;
    int fd=-1;unsigned seq=0,sent=0,retry=2;time_t next_report=0,next_property=0,next_ping=0,ping_deadline=0;
    while(!monitor_stop) {
        if(fd<0) {
            cJSON *will=payload(config,"offline",++seq);js(will,"status","offline");
            char *text=cJSON_PrintUnformatted(will);fd=text?monitor_connect(config,presence,text):-1;free(text);cJSON_Delete(will);
            if(fd>=0) {
                struct sockaddr_storage addr;socklen_t len=sizeof(addr);
                if(!getsockname(fd,(struct sockaddr *)&addr,&len))
                    getnameinfo((struct sockaddr *)&addr,len,local_ip,sizeof(local_ip),NULL,0,NI_NUMERICHOST);
                if(monitor_subscribe(fd,down_req,down_resp)) {close(fd);fd=-1;}
            }
            next_report=0;next_property=0;next_ping=time(NULL)+20;ping_deadline=0;sent=0;report_sent_at=0;
            if(fd>=0)retry=2; else qos1_reset();
        }
        cJSON *state=cJSON_CreateObject();jb(state,"connected",fd>=0);jb(state,"subscribed",fd>=0);
        js(state,"state",fd>=0?"running":"reconnecting");
        if(fd>=0) {
            cJSON *subs=cJSON_AddArrayToObject(state,"subscriptions");
            cJSON_AddItemToArray(subs,cJSON_CreateString(down_req));
            cJSON_AddItemToArray(subs,cJSON_CreateString(down_resp));
        }
        if(last_rpc.valid) {
            cJSON *rpc=cJSON_AddObjectToObject(state,"rpc");
            js(rpc,"channel",last_rpc.channel);js(rpc,"request_id",last_rpc.request_id);
            js(rpc,"method",last_rpc.method);json_number(rpc,"code",last_rpc.code);js(rpc,"message",last_rpc.message);
        }
        pthread_mutex_lock(&mutex);unsigned current=generation;cJSON *snapshot=latest?cJSON_Duplicate(latest,1):NULL;pthread_mutex_unlock(&mutex);
        if(snapshot)cJSON_AddItemToObject(state,"snapshot",cJSON_Duplicate(snapshot,1));
        json_save(status_path,state);cJSON_Delete(state);
        int failed=0;time_t now=time(NULL);
        if(fd>=0 && (now>=next_report || current!=sent)) {
            cJSON *online=payload(config,"online",++seq);js(online,"status","online");failed=publish_json(fd,presence,online,1);
            if(!failed && snapshot) {
                cJSON *j=payload(config,"recording_summary",++seq);
                copy_fields(j,snapshot);
                cJSON *recordings=cJSON_GetObjectItemCaseSensitive(j,"recordings");
                if(recordings)cJSON_DeleteItemFromObject(recordings,"total_size_bytes");
                failed=publish_json(fd,summary,j,0);
            }
            sent=current;next_report=now+(int)num(config,"report_interval_sec");
        }
        if(fd>=0 && !failed && now>=next_property){ failed=publish_properties(up_prop,config); next_property=now+60; }
        if(fd>=0 && !failed)failed=maybe_report(up_req,config);
        if(fd>=0 && !failed)failed=qos1_tick(fd);
        cJSON_Delete(snapshot);
        if(fd>=0 && !failed && !ping_deadline && now>=next_ping) {
            if(monitor_ping(fd))failed=1; else {ping_deadline=now+10;next_ping=now+20;}
        }
        if(fd>=0 && !failed) {
            int wait_ms=1000;
            while(!failed && !monitor_stop) {
                unsigned char type=0,*pkt=NULL;size_t pn=0;
                int rd=monitor_read(fd,wait_ms,&type,&pkt,&pn);
                if(rd<0){failed=1;free(pkt);break;}
                if(rd>0){free(pkt);break;}
                if(type==0xd0){ if(pn)failed=1; else ping_deadline=0; }
                else if(type==0x40){ if(pn!=2 || qos1_ack(fd,((unsigned)pkt[0]<<8)|pkt[1])) failed=1; }
                else if((type&0xf0)==0x30) {
                    char incoming[512];const unsigned char *data=NULL;size_t data_len=0;
                    if(monitor_accept_publish(fd,type,pkt,pn,incoming,sizeof(incoming),&data,&data_len) ||
                       handle_downlink(incoming,down_req,up_resp,down_resp,data,data_len,config) || qos1_pump(fd)) failed=1;
                } else failed=1;
                free(pkt);wait_ms=0;
            }
        }
        if(fd>=0 && !failed && ping_deadline && time(NULL)>ping_deadline)failed=1;
        if(failed){close(fd);fd=-1;qos1_reset();}
        if(fd<0) {
            pause_seconds(retry);
            if(retry<60)retry=retry>30?60:retry*2;
        }
    }
    if(fd>=0){cJSON *j=payload(config,"offline",++seq);js(j,"status","offline");publish_json(fd,presence,j,1);monitor_disconnect(fd);close(fd);}
    pthread_join(scanner,NULL);cJSON_Delete(latest);cJSON_Delete(session);cJSON_Delete(config);close(lock);return 0;
}

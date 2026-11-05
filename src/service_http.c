#include "service.h"
#include "tf_scan.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>

static volatile sig_atomic_t stopping;
static void stop_signal(int sig) { (void)sig; stopping=1; }
typedef struct {
    task_manager *manager; const char *token;
    pthread_mutex_t mutex; pthread_cond_t done; unsigned clients;
} http_server;
typedef struct { http_server *server; int fd; } connection;
static cJSON *error_json(const char *error) { cJSON *j=cJSON_CreateObject(); jb(j,"accepted",0); js(j,"error",error); return j; }
static void respond(int fd,int code,cJSON *body) {
    char *text=cJSON_PrintUnformatted(body); if(!text) return;
    char header[512];
    int n=snprintf(header,sizeof(header),"HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",code,code<400?"OK":"Error",strlen(text));
    if(!write_all(fd,header,(size_t)n)) write_all(fd,text,strlen(text));
    free(text);
}
static int decode(const char *in,char *out,size_t cap) {
    size_t used=0;
    while(*in) {
        unsigned char c=(unsigned char)*in++;
        if(c=='%') {
            if(!in[0] || !in[1] || !isxdigit((unsigned char)in[0]) || !isxdigit((unsigned char)in[1])) return -1;
            char hex[3]={in[0],in[1],0}; c=(unsigned char)strtoul(hex,NULL,16); in+=2;
        } else if(c=='+') c=' ';
        if(!c || c<32 || used+1>=cap) return -1;
        out[used++]=(char)c;
    }
    out[used]=0; return 0;
}
static int sort_names(const void *a,const void *b) { return strcmp(*(char*const*)a,*(char*const*)b); }
typedef struct {
    cJSON *result,*live;
    char task_id[33],live_id[33];
    double uploaded_at;
} listed_history;

static void copy_field(cJSON *entry,const char *key,const cJSON *source,const char *field) {
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(source,field);
    cJSON_AddItemToObject(entry,key,value?cJSON_Duplicate(value,1):cJSON_CreateNull());
}
static void nullable_text(cJSON *entry,const char *key,const char *value) {
    if(*value) js(entry,key,value); else cJSON_AddNullToObject(entry,key);
}
static double recorded_ms(const char *name,const head_identity *id) {
    char date[16];
    snprintf(date,sizeof(date),"%.4s%.2s%.2s_",id->date,id->date+5,id->date+8);
    const char *part=strstr(name,date);
    if(!part) return 0;
    part+=strlen(date);
    if(strspn(part,"0123456789")!=6 || part[6]!='_' || strspn(part+7,"0123456789")!=3 ||
       (part[10]!='_' && part[10]!='.')) return 0;
    struct tm tm={0};
    if(!strptime(id->date,"%Y-%m-%d",&tm) || !strptime(part,"%H%M%S",&tm) || tm.tm_sec>=60) return 0;
    return ((double)timegm(&tm)-8*3600)*1000+atoi(part+7);
}
static void history_snapshot(task_manager *manager,const char *path,char **names,size_t count,listed_history *history) {
    pthread_mutex_lock(&manager->mutex);
    for(task *t=manager->first;t;t=t->next) {
        if(!path_inside(path,str(t->json,"path"))) continue;
        const cJSON *item,*results=cJSON_GetObjectItemCaseSensitive(t->json,"results");
        const char *task_id=str(t->json,"task_id");
        cJSON_ArrayForEach(item,results) {
            const char *full=str(item,"path"),*slash=strrchr(full,'/');
            if(!slash || (slash==full ? strcmp(path,"/") : (strlen(path)!=(size_t)(slash-full) || strncmp(full,path,(size_t)(slash-full))))) continue;
            const char *name=slash+1;
            char **slot=bsearch(&name,names,count,sizeof(*names),sort_names);
            if(!slot || !*str(item,"fileId")) continue;
            listed_history *h=&history[slot-names];
            if(strcmp(str(h->result,"fileId"),str(item,"fileId"))) h->uploaded_at=0;
            if(num(item,"uploadedAt")>0) h->uploaded_at=num(item,"uploadedAt");
            cJSON_Delete(h->result); h->result=cJSON_Duplicate(item,1);
            snprintf(h->task_id,sizeof(h->task_id),"%s",task_id);
        }
        if(strcmp(str(t->json,"status"),"uploading")) continue;
        const cJSON *current=cJSON_GetObjectItemCaseSensitive(t->json,"current_file");
        const char *full=str(current,"path"),*slash=strrchr(full,'/');
        if(!slash || (slash==full ? strcmp(path,"/") : (strlen(path)!=(size_t)(slash-full) || strncmp(full,path,(size_t)(slash-full))))) continue;
        const char *name=slash+1,*phase=str(current,"phase");
        char **slot=bsearch(&name,names,count,sizeof(*names),sort_names);
        if(!slot || !*str(current,"fileId") || !strcmp(phase,"uploaded") || !strcmp(phase,"skipped") ||
           !strcmp(phase,"failed") || !strcmp(phase,"empty")) continue;
        listed_history *h=&history[slot-names];
        cJSON_Delete(h->live); h->live=cJSON_Duplicate(current,1);
        snprintf(h->live_id,sizeof(h->live_id),"%s",task_id);
    }
    pthread_mutex_unlock(&manager->mutex);
}
static cJSON *listed_file(const char *name,const char *full,const struct stat *st,
                          const char *filter,const listed_history *h) {
    char id[67]; file_identity(full,st,id);
    const cJSON *result=!strcmp(id,str(h->result,"fileId"))?h->result:NULL;
    const cJSON *live=!strcmp(id,str(h->live,"fileId"))?h->live:NULL;
    int uploaded=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result,"upload_verified"));
    if(*filter && uploaded!=(!strcmp(filter,"UPLOADED"))) return NULL;
    cJSON *entry=cJSON_CreateObject();
    js(entry,"fileId",id); js(entry,"name",name); js(entry,"path",full);
    json_number(entry,"size",(double)st->st_size);
    json_number(entry,"modifiedAt",(double)st->st_mtim.tv_sec*1000+st->st_mtim.tv_nsec/1000000);
    head_identity identity; char error[ERROR_SIZE];
    int valid=!head_parse(name,&identity,error);
    double recorded=valid?recorded_ms(name,&identity):0;
    if(recorded) json_number(entry,"recordedAt",recorded); else cJSON_AddNullToObject(entry,"recordedAt");
    js(entry,"sealState","SEALED");
    const char *failure=str(result,"error");
    js(entry,"uploadState",uploaded?"UPLOADED":live?"UPLOADING":*failure?"FAILED":"NOT_UPLOADED");
    json_number(entry,"uploadedBytes",uploaded?(double)st->st_size:num(live,"bytes_uploaded"));
    json_number(entry,"progressPercent",uploaded?100:num(live,"progress_percent"));
    nullable_text(entry,"taskId",live?h->live_id:result?h->task_id:"");
    copy_field(entry,"sha256",result,"sha256"); copy_field(entry,"remoteSha256",result,"remote_sha256");
    copy_field(entry,"updatedAt",result,"updatedAt");
    if(uploaded && h->uploaded_at>0) json_number(entry,"uploadedAt",h->uploaded_at); else cJSON_AddNullToObject(entry,"uploadedAt");
    if(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(result,"duration")))
        json_number(entry,"durationMs",num(result,"duration")*1000);
    else cJSON_AddNullToObject(entry,"durationMs");
    const char *uri=str(result,"ossPath"),*key=!strncmp(uri,"oss://",6)?strchr(uri+6,'/'):NULL;
    nullable_text(entry,"objectKey",key?key+1:"");
    nullable_text(entry,"errorCode",*failure?"UPLOAD_FAILED":""); nullable_text(entry,"errorMessage",failure);
    const char *blocked=live?"UPLOAD_IN_PROGRESS":!valid?"INVALID_FILENAME":!st->st_size?"EMPTY_FILE":"";
    jb(entry,"canUpload",!*blocked); nullable_text(entry,"blockedReason",blocked);
    return entry;
}
static cJSON *browse(task_manager *manager,const char *query,int *status,int file_mode) {
    const char *filter="";
    char raw[PATH_MAX]={0}; unsigned long offset=0; int path_seen=0,offset_seen=0;
    if(query && *query) {
        char *copy=strdup(query),*state=NULL;
        for(char *part=strtok_r(copy,"&",&state);part;part=strtok_r(NULL,"&",&state)) {
            if(!strncmp(part,"path=",5) && !path_seen++) {
                if(decode(part+5,raw,sizeof(raw))) { free(copy); *status=400; return error_json("Invalid path encoding"); }
            } else if(!strncmp(part,"offset=",7) && !offset_seen++) {
                char *end=NULL; errno=0; offset=strtoul(part+7,&end,10);
                if(errno || !part[7] || *end || offset>MAX_FILES) { free(copy); *status=400; return error_json("Invalid offset"); }
             } else if(file_mode && !*filter && !strcmp(part,"upload_state=UPLOADED")) filter="UPLOADED";
            else if(file_mode && !*filter && !strcmp(part,"upload_state=NOT_UPLOADED")) filter="NOT_UPLOADED";
            else { free(copy); *status=400; return error_json("Invalid or duplicate query parameter"); }
        }
        free(copy);
    }
    cJSON *out=cJSON_CreateObject(),*entries=cJSON_AddArrayToObject(out,file_mode?"files":"directories");
    *status=200;
    if(!raw[0]) {
        if(file_mode) { *status=400; cJSON_Delete(out); return error_json("path is required for file listing"); }
        js(out,"path",""); cJSON_AddNullToObject(out,"parent"); jb(out,"selectable",0);
        if(settings.allowed_root[0]) {
            cJSON *entry=cJSON_CreateObject(); js(entry,"name",settings.allowed_root); js(entry,"path",settings.allowed_root); cJSON_AddItemToArray(entries,entry);
        } else {
            tf_mount_t mounts[TF_SCAN_MAX_MOUNTS]; size_t count=tf_find_mounts(mounts,TF_SCAN_MAX_MOUNTS);
            for(size_t i=0;i<count;i++) {
                char resolved[PATH_MAX]; if(!realpath(mounts[i].mount_point,resolved) || path_inside(resolved,settings.runtime)) continue;
                cJSON *entry=cJSON_CreateObject(); js(entry,"name",mounts[i].mount_point); js(entry,"path",resolved); cJSON_AddItemToArray(entries,entry);
            }
        }
        json_number(out,"total",cJSON_GetArraySize(entries)); cJSON_AddNullToObject(out,"next_offset");
        if(!cJSON_GetArraySize(entries)) js(out,"message","No TF card detected; mount the card or configure --allowed-root");
        return out;
    }
    char path[PATH_MAX]; struct stat st;
    if(raw[0]!='/' || !realpath(raw,path) || stat(path,&st) || !S_ISDIR(st.st_mode)) { *status=400; goto invalid; }
    if((settings.allowed_root[0] && !path_inside(path,settings.allowed_root)) || path_inside(path,settings.runtime)) { *status=403; goto invalid; }
    DIR *dir=opendir(path); if(!dir) { *status=403; goto invalid; }
    char **names=NULL; size_t count=0,capacity=0,files=0; int bad=0;
    while(1) {
        errno=0; struct dirent *e=readdir(dir); if(!e) { if(errno) bad=1; break; }
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char full[PATH_MAX]; if(path_join(full,sizeof(full),path,e->d_name) || lstat(full,&st)) continue;
        if(S_ISLNK(st.st_mode) || path_inside(full,settings.runtime)) continue;
        if(S_ISREG(st.st_mode)) {
            size_t n=strlen(e->d_name); if(n>=5 && !strcasecmp(e->d_name+n-5,".mcap")) files++;
        }
        if(file_mode) {
            size_t n=strlen(e->d_name);
            if(!S_ISREG(st.st_mode) || n<5 || strcasecmp(e->d_name+n-5,".mcap")) continue;
        } else if(!S_ISDIR(st.st_mode)) continue;
        if(count==MAX_FILES) { bad=1; break; }
        if(count==capacity) {
            size_t cap=capacity?capacity*2:64; char **more=realloc(names,cap*sizeof(*more));
            if(!more) { bad=1; break; } names=more; capacity=cap;
        }
        names[count]=strdup(e->d_name); if(!names[count]) { bad=1; break; } count++;
    }
    closedir(dir);
    if(!bad) {
        if(count) qsort(names,count,sizeof(*names),sort_names);
        js(out,"path",path); jb(out,"selectable",1); json_number(out,"mcap_files",(double)files); json_number(out,"total",(double)count);
        char parent[PATH_MAX]; snprintf(parent,sizeof(parent),"%s",path); char *slash=strrchr(parent,'/');
        if(slash==parent) parent[1]=0; else if(slash) *slash=0;
        if(!strcmp(path,"/") || (settings.allowed_root[0] && !strcmp(path,settings.allowed_root))) cJSON_AddNullToObject(out,"parent");
        else js(out,"parent",parent);
        if(file_mode) {
            listed_history *history=count?calloc(count,sizeof(*history)):NULL;
            if(count && !history) { bad=1; goto cleanup; }
            if(count) history_snapshot(manager,path,names,count,history);
            size_t matched=0; double total_bytes=0;
            for(size_t i=0;i<count;i++) {
                char full[PATH_MAX];
                if(path_join(full,sizeof(full),path,names[i]) || lstat(full,&st) || !S_ISREG(st.st_mode)) continue;
                cJSON *entry=listed_file(names[i],full,&st,filter,&history[i]);
                if(!entry) continue;
                total_bytes+=(double)st.st_size;
                if(matched>=offset && matched<offset+200) cJSON_AddItemToArray(entries,entry);
                else cJSON_Delete(entry);
                matched++;
            }
            for(size_t i=0;i<count;i++) { cJSON_Delete(history[i].result); cJSON_Delete(history[i].live); }
            free(history);
            json_number(out,"total",(double)matched); json_number(out,"total_bytes",total_bytes);
            json_number(out,"snapshot_at",epoch_ms());
            if(offset+200<matched) json_number(out,"next_offset",(double)(offset+200));
            else cJSON_AddNullToObject(out,"next_offset");
            const char *config=getenv("HEAD_CAM_MONITOR_CONFIG");
            cJSON *monitor=config && *config?json_load(config):NULL;
            nullable_text(out,"device_sn",str(monitor,"device_id")); cJSON_Delete(monitor);
            goto cleanup;
        }
        size_t end=offset+200; if(end>count) end=count;
        for(size_t i=offset;i<end;i++) {
            char full[PATH_MAX]; path_join(full,sizeof(full),path,names[i]);
            cJSON *entry=cJSON_CreateObject(); js(entry,"name",names[i]); js(entry,"path",full); cJSON_AddItemToArray(entries,entry);
            if(file_mode && !lstat(full,&st)) json_number(entry,"size",(double)st.st_size);
        }
        if(end<count) json_number(out,"next_offset",(double)end); else cJSON_AddNullToObject(out,"next_offset");
    }
cleanup:
    for(size_t i=0;i<count;i++) free(names[i]);
    free(names);
    if(bad) { *status=500; goto invalid; }
    return out;
invalid:
    cJSON_Delete(out); return error_json("Cannot browse directory: invalid, restricted, unreadable or too large");
}
static void handle(connection *c) {
    int fd=c->fd,code=400; cJSON *out=NULL;
    char buffer[32769]; size_t received=0,head_size=0;
    while(received<16384) {
        ssize_t n=recv(fd,buffer+received,16384-received,0);
        if(n<=0) { code=408; out=error_json("Request header timeout or closed connection"); goto done; }
        received+=(size_t)n; buffer[received]=0;
        char *end=strstr(buffer,"\r\n\r\n"); if(end) { head_size=(size_t)(end-buffer)+4; break; }
    }
    if(!head_size) { code=431; out=error_json("Request headers too large"); goto done; }
    if(memchr(buffer,0,head_size)) { out=error_json("Invalid HTTP header"); goto done; }
    char header[16385]; memcpy(header,buffer,head_size); header[head_size]=0;
    char *line_end=strstr(header,"\r\n"); if(!line_end) goto malformed; *line_end=0;
    char method[16],target[8192],version[16],extra;
    if(sscanf(header,"%15s %8191s %15s %c",method,target,version,&extra)!=3 || (strcmp(version,"HTTP/1.1") && strcmp(version,"HTTP/1.0"))) goto malformed;
    size_t content_length=0; int seen_length=0,seen_auth=0,seen_type=0,json_type=0;
    const char *auth="";
    for(char *line=line_end+2;*line && strncmp(line,"\r\n",2);) {
        char *end=strstr(line,"\r\n"); if(!end) goto malformed; *end=0;
        char *colon=strchr(line,':'); if(!colon || line[0]==' ' || line[0]=='\t') goto malformed;
        *colon=0; char *value=colon+1; while(*value==' ' || *value=='\t') value++;
        size_t len=strlen(value); while(len && (value[len-1]==' ' || value[len-1]=='\t')) value[--len]=0;
        if(!strcasecmp(line,"Content-Length")) {
            if(seen_length++ || !*value || strspn(value,"0123456789")!=strlen(value)) goto malformed;
            char *tail; errno=0; unsigned long size=strtoul(value,&tail,10);
            if(errno || *tail || size>16384) { code=413; out=error_json("Request body exceeds 16384 bytes"); goto done; }
            content_length=(size_t)size;
        } else if(!strcasecmp(line,"Transfer-Encoding")) goto malformed;
        else if(!strcasecmp(line,"Authorization")) { if(seen_auth++) goto malformed; auth=value; }
        else if(!strcasecmp(line,"Content-Type")) {
            if(seen_type++) goto malformed;
            json_type=!strncasecmp(value,"application/json",16) && (!value[16] || value[16]==';');
        } else if(!strcasecmp(line,"Expect")) { code=417; out=error_json("Expect is unsupported"); goto done; }
        line=end+2;
    }
    if(c->server->token && *c->server->token) {
        size_t n=strlen(c->server->token);
        if(strlen(auth)!=n+7 || strncmp(auth,"Bearer ",7) || CRYPTO_memcmp(auth+7,c->server->token,n)) { code=401; out=error_json("Valid Bearer token required"); goto done; }
    }
    char *query=strchr(target,'?'); if(query) *query++=0;
    if(!strcmp(method,"GET")) {
        if(!strcmp(target,"/health")) {
            out=cJSON_CreateObject(); js(out,"status","ok"); js(out,"version",SERVICE_VERSION); code=200;
        } else if(!strcmp(target,"/directories")) out=browse(c->server->manager,query,&code,0);
        else if(!strcmp(target,"/files")) out=browse(c->server->manager,query,&code,1);
        else if(!strncmp(target,"/upload/",8)) out=task_get(c->server->manager,target+8,&code);
        else { code=404; out=error_json("Endpoint not found"); }
    } else if(!strcmp(method,"POST") && !strcmp(target,"/upload")) {
        if(!json_type) { code=415; out=error_json("Content-Type must be application/json"); goto done; }
        if(!seen_length || !content_length) goto malformed;
        size_t need=head_size+content_length;
        while(received<need) {
            ssize_t n=recv(fd,buffer+received,need-received,0);
            if(n<=0) { code=408; out=error_json("Request body timeout or closed connection"); goto done; }
            received+=(size_t)n;
        }
        buffer[need]=0;
        if(memchr(buffer+head_size,0,content_length) || strstr(buffer+head_size,"\\u0000")) goto malformed;
        cJSON *body=json_parse(buffer+head_size,content_length+1);
        cJSON *files=cJSON_GetObjectItemCaseSensitive(body,"files");
        cJSON *remove_local=cJSON_GetObjectItemCaseSensitive(body,"delete_after_upload");
        if((remove_local && !cJSON_IsBool(remove_local)) || !cJSON_IsObject(body) || cJSON_GetArraySize(body)!=(1+(files?1:0)+(remove_local?1:0)) || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(body,"path"))) {
            cJSON_Delete(body); out=error_json("JSON body must contain path and optionally files and boolean delete_after_upload"); goto done;
        }
        out=task_submit_files(c->server->manager,str(body,"path"),files,cJSON_IsTrue(remove_local),&code); cJSON_Delete(body);
    } else { code=404; out=error_json("Endpoint not found"); }
    goto done;
malformed:
    out=error_json("Malformed HTTP request");
done:
    if(out) { respond(fd,code,out); cJSON_Delete(out); }
}
static void *client_thread(void *data) {
    connection *c=data; handle(c); close(c->fd);
    pthread_mutex_lock(&c->server->mutex); c->server->clients--; pthread_cond_signal(&c->server->done); pthread_mutex_unlock(&c->server->mutex);
    free(c); return NULL;
}
int http_serve(task_manager *manager,const char *host,unsigned port,const char *token) {
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0); if(fd<0) return -1;
    int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    if(inet_pton(AF_INET,host,&addr.sin_addr)!=1 || bind(fd,(struct sockaddr*)&addr,sizeof(addr)) || listen(fd,32)) { close(fd); return -1; }
    struct sigaction sa={0}; sa.sa_handler=stop_signal; sigemptyset(&sa.sa_mask); sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);
    signal(SIGPIPE,SIG_IGN);
    http_server server={.manager=manager,.token=token,.mutex=PTHREAD_MUTEX_INITIALIZER,.done=PTHREAD_COND_INITIALIZER};
    fprintf(stdout,"head-cam-upload %s listening on %s:%u\n",SERVICE_VERSION,host,port); fflush(stdout);
    while(!stopping) {
        struct pollfd p={.fd=fd,.events=POLLIN}; if(poll(&p,1,250)<=0) continue;
        int client=accept4(fd,NULL,NULL,SOCK_CLOEXEC); if(client<0) continue;
        struct timeval timeout={.tv_sec=5}; setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)); setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        pthread_mutex_lock(&server.mutex);
        if(server.clients>=16) {
            pthread_mutex_unlock(&server.mutex); cJSON *busy=error_json("Too many concurrent requests"); respond(client,503,busy); cJSON_Delete(busy); close(client); continue;
        }
        server.clients++; pthread_mutex_unlock(&server.mutex);
        connection *c=malloc(sizeof(*c)); pthread_t thread;
        if(c) { c->server=&server; c->fd=client; }
        if(!c || pthread_create(&thread,NULL,client_thread,c)) {
            free(c); close(client); pthread_mutex_lock(&server.mutex); server.clients--; pthread_mutex_unlock(&server.mutex);
        } else pthread_detach(thread);
    }
    close(fd);
    pthread_mutex_lock(&manager->mutex); manager->closing=1; pthread_mutex_unlock(&manager->mutex);
    pthread_mutex_lock(&server.mutex); while(server.clients) pthread_cond_wait(&server.done,&server.mutex); pthread_mutex_unlock(&server.mutex);
    pthread_mutex_destroy(&server.mutex); pthread_cond_destroy(&server.done);
    return 0;
}

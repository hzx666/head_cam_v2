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
static cJSON *browse(const char *query,int *status,int file_mode) {
    char raw[PATH_MAX]={0}; unsigned long offset=0; int path_seen=0,offset_seen=0;
    if(query && *query) {
        char *copy=strdup(query),*state=NULL;
        for(char *part=strtok_r(copy,"&",&state);part;part=strtok_r(NULL,"&",&state)) {
            if(!strncmp(part,"path=",5) && !path_seen++) {
                if(decode(part+5,raw,sizeof(raw))) { free(copy); *status=400; return error_json("Invalid path encoding"); }
            } else if(!strncmp(part,"offset=",7) && !offset_seen++) {
                char *end=NULL; errno=0; offset=strtoul(part+7,&end,10);
                if(errno || !part[7] || *end || offset>MAX_FILES) { free(copy); *status=400; return error_json("Invalid offset"); }
            } else { free(copy); *status=400; return error_json("Only path and offset query parameters are supported"); }
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
        size_t end=offset+200; if(end>count) end=count;
        for(size_t i=offset;i<end;i++) {
            char full[PATH_MAX]; path_join(full,sizeof(full),path,names[i]);
            cJSON *entry=cJSON_CreateObject(); js(entry,"name",names[i]); js(entry,"path",full); cJSON_AddItemToArray(entries,entry);
            if(file_mode && !lstat(full,&st)) json_number(entry,"size",(double)st.st_size);
        }
        if(end<count) json_number(out,"next_offset",(double)end); else cJSON_AddNullToObject(out,"next_offset");
    }
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
        } else if(!strcmp(target,"/directories")) out=browse(query,&code,0);
        else if(!strcmp(target,"/files")) out=browse(query,&code,1);
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
        if(!cJSON_IsObject(body) || cJSON_GetArraySize(body)!=(files?2:1) || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(body,"path"))) {
            cJSON_Delete(body); out=error_json("JSON body must contain path and optionally files"); goto done;
        }
        out=task_submit_files(c->server->manager,str(body,"path"),files,&code); cJSON_Delete(body);
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

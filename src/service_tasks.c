#include "service.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static int active(const char *s) { return !strcmp(s,"queued") || !strcmp(s,"scanning") || !strcmp(s,"uploading"); }
static int id_valid(const char *s) { return strlen(s)==32 && strspn(s,"0123456789abcdef")==32; }
static int save(task_manager *m,task *t) {
    char path[PATH_MAX],name[64],now[32]; timestamp(now); js(t->json,"updated_at",now);
    snprintf(name,sizeof(name),"tasks/%s.json",str(t->json,"task_id"));
    if(path_join(path,sizeof(path),settings.runtime,name) || json_save(path,t->json)) {
        m->storage_failed=1; js(t->json,"status","failed"); js(t->json,"error","Cannot persist task state; service is refusing new tasks");
        fprintf(stderr,"Task state storage failed\n"); return -1;
    }
    char log_path[PATH_MAX];
    if(!path_join(log_path,sizeof(log_path),settings.runtime,"logs/service.log")) {
        int fd=open(log_path,O_WRONLY|O_APPEND|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd>=0) {
            dprintf(fd,"%s task=%s status=%s processed=%.0f total=%.0f\n",now,str(t->json,"task_id"),str(t->json,"status"),num(t->json,"processed"),num(t->json,"total"));
            close(fd);
        }
    }
    return 0;
}
static cJSON *failure(const char *message) { cJSON *j=cJSON_CreateObject(); jb(j,"accepted",0); js(j,"error",message); return j; }
cJSON *task_get(task_manager *m,const char *id,int *status) {
    pthread_mutex_lock(&m->mutex); cJSON *out=NULL;
    for(task *t=m->first;t;t=t->next) if(!strcmp(str(t->json,"task_id"),id)) { out=cJSON_Duplicate(t->json,1); break; }
    pthread_mutex_unlock(&m->mutex);
    *status=out?200:404; return out?out:failure("Task not found");
}
cJSON *task_submit_files(task_manager *m,const char *raw,const cJSON *files,int delete_after_upload,int *status) {
    char path[PATH_MAX]; struct stat st;
    *status=400;
    if(!raw || raw[0]!='/' || strlen(raw)>=PATH_MAX || !realpath(raw,path)) return failure("path must be an existing absolute device directory");
    if(stat(path,&st) || !S_ISDIR(st.st_mode)) return failure("path must be a directory");
    if(settings.allowed_root[0] && !path_inside(path,settings.allowed_root)) { *status=403; return failure("path is outside allowed root"); }
    if(path_inside(path,settings.runtime)) return failure("Service runtime directory cannot be uploaded");
    DIR *dir=opendir(path);
    if(!dir) { *status=403; return failure("Directory is not readable"); } closedir(dir);
    cJSON *normalized=NULL;
    if(files) {
        if(!cJSON_IsArray(files) || cJSON_GetArraySize(files)<1 || cJSON_GetArraySize(files)>100) return failure("files must contain 1 to 100 file names");
        normalized=cJSON_CreateArray();
        const cJSON *entry;
        cJSON_ArrayForEach(entry,files) {
            const char *name=cJSON_IsString(entry)?entry->valuestring:NULL;
            char full[PATH_MAX],resolved[PATH_MAX]; size_t n=name?strlen(name):0;
            if(!name || n<5 || strcasecmp(name+n-5,".mcap") || strchr(name,'/') || strchr(name,'\\') ||
               path_join(full,sizeof(full),path,name) || lstat(full,&st) || !S_ISREG(st.st_mode) ||
               !realpath(full,resolved) || strcmp(full,resolved) || access(full,R_OK)) {
                cJSON_Delete(normalized); return failure("files must name readable regular .mcap files directly inside path (no symlinks)");
            }
            const cJSON *prior;
            cJSON_ArrayForEach(prior,normalized) if(!strcmp(prior->valuestring,name)) {
                cJSON_Delete(normalized); return failure("Duplicate file name in files");
            }
            cJSON_AddItemToArray(normalized,cJSON_CreateString(name));
        }
        /* Canonical order makes retry detection independent of selection order. */
        for(cJSON *a=normalized->child;a;a=a->next) for(cJSON *b=a->next;b;b=b->next)
            if(strcmp(a->valuestring,b->valuestring)>0) { char *v=a->valuestring; a->valuestring=b->valuestring; b->valuestring=v; }
    }
    pthread_mutex_lock(&m->mutex);
    cJSON *out=NULL; task *selected=NULL; int duplicate=0,count=0;
    if(m->closing || m->storage_failed) { *status=503; out=failure("Service is stopping or task storage is unavailable"); goto done; }
    for(task *t=m->first;t;t=t->next) if(active(str(t->json,"status"))) {
        count++;
        const char *previous=str(t->json,"path");
        if(!strcmp(path,previous)) {
            const cJSON *old=cJSON_GetObjectItemCaseSensitive(t->json,"files");
            if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t->json,"delete_after_upload"))==delete_after_upload &&
               ((!old && !normalized) || (old && normalized && cJSON_Compare(old,normalized,1)))) { selected=t; duplicate=1; break; }
            *status=409; out=failure("Directory overlaps an active upload task"); goto done;
        }
        if(path_inside(path,previous) || path_inside(previous,path)) { *status=409; out=failure("Directory overlaps an active upload task"); goto done; }
    }
    if(!selected) {
        if(count>=100) { *status=503; out=failure("Upload queue is full"); goto done; }
        unsigned char bytes[16]; char id[33],now[32];
        if(RAND_bytes(bytes,sizeof(bytes))!=1) { *status=500; out=failure("Cannot create task ID"); goto done; }
        for(int i=0;i<16;i++) snprintf(id+i*2,3,"%02x",bytes[i]);
        selected=calloc(1,sizeof(*selected));
        if(!selected) { *status=503; out=failure("Insufficient memory"); goto done; }
        selected->json=cJSON_CreateObject(); timestamp(now);
        js(selected->json,"task_id",id); js(selected->json,"path",path); js(selected->json,"status","queued");
        js(selected->json,"phase","queued"); jb(selected->json,"bytes_total_known",0);
        json_number(selected->json,"bytes_total",0); json_number(selected->json,"bytes_done",0);
        json_number(selected->json,"speed_bytes_per_second",0);
        json_number(selected->json,"bytes_uploaded",0); json_number(selected->json,"progress_percent",0);
        jb(selected->json,"delete_after_upload",delete_after_upload);
        js(selected->json,"mode",normalized?"files":"directory");
        if(normalized) cJSON_AddItemToObject(selected->json,"files",cJSON_Duplicate(normalized,1));
        js(selected->json,"created_at",now); js(selected->json,"updated_at",now);
        const char *counters[]={"total","processed","uploaded","skipped","empty","failed","ignored"};
        for(size_t i=0;i<7;i++) json_number(selected->json,counters[i],0);
        cJSON_AddArrayToObject(selected->json,"results");
        if(save(m,selected)) { cJSON_Delete(selected->json); free(selected); *status=503; out=failure("Cannot persist task"); goto done; }
        if(m->last) m->last->next=selected; else m->first=selected;
        m->last=selected; pthread_cond_signal(&m->changed);
    }
    out=cJSON_CreateObject(); jb(out,"delete_after_upload",delete_after_upload); jb(out,"accepted",1); jb(out,"duplicate",duplicate);
    js(out,"task_id",str(selected->json,"task_id")); js(out,"status",str(selected->json,"status"));
    char url[64]; snprintf(url,sizeof(url),"/upload/%s",str(selected->json,"task_id")); js(out,"status_url",url);
    *status=duplicate?200:202;
done:
    cJSON_Delete(normalized);
    pthread_mutex_unlock(&m->mutex); return out;
}

typedef struct { char *path,*dest; char error[ERROR_SIZE]; int conflict; uint64_t size; } plan;
typedef struct { plan *items; size_t count,cap,ignored; } plans;
static int ignored_dir(const char *name) {
    const char *names[]={".Spotlight-V100",".Trashes",".fseventsd","System Volume Information","$RECYCLE.BIN"};
    for(size_t i=0;i<5;i++) if(!strcasecmp(name,names[i])) return 1;
    return 0;
}
static int suffix(const char *s,const char *end) { size_t n=strlen(s),e=strlen(end); return n>=e && !strcasecmp(s+n-e,end); }
static int scan(plans *p,const char *root,unsigned depth,char *error) {
    if(depth>128) { snprintf(error,ERROR_SIZE,"Maximum directory depth exceeded"); return -1; }
    struct stat st;
    char resolved[PATH_MAX];
    if(lstat(root,&st) || !S_ISDIR(st.st_mode) || !realpath(root,resolved) || strcmp(root,resolved)) { snprintf(error,ERROR_SIZE,"Directory disappeared or became a symlink"); return -1; }
    DIR *dir=opendir(root);
    if(!dir) { snprintf(error,ERROR_SIZE,"Cannot read scan directory"); return -1; }
    int rc=0; struct dirent *entry;
    while(1) {
        errno=0; entry=readdir(dir);
        if(!entry) { if(errno) { snprintf(error,ERROR_SIZE,"Directory read failed"); rc=-1; } break; }
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char path[PATH_MAX];
        if(path_join(path,sizeof(path),root,entry->d_name) || lstat(path,&st)) { snprintf(error,ERROR_SIZE,"File disappeared or path too long during scan"); rc=-1; break; }
        if(S_ISLNK(st.st_mode) || path_inside(path,settings.runtime)) { p->ignored++; continue; }
        if(S_ISDIR(st.st_mode)) {
            if(ignored_dir(entry->d_name)) { p->ignored++; continue; }
            if(scan(p,path,depth+1,error)) { rc=-1; break; } continue;
        }
        int incomplete=suffix(entry->d_name,".mcap.incomplete");
        if(!S_ISREG(st.st_mode) || (!incomplete && !suffix(entry->d_name,".mcap"))) { p->ignored++; continue; }
        if(incomplete) {
            char complete[PATH_MAX]; snprintf(complete,sizeof(complete),"%s",path); complete[strlen(complete)-11]=0;
            struct stat other;
            if(!lstat(complete,&other) && S_ISREG(other.st_mode)) { p->ignored++; continue; }
        }
        if(p->count>=MAX_FILES) { snprintf(error,ERROR_SIZE,"Directory exceeds 10000 MCAP files; select a smaller directory"); rc=-1; break; }
        if(p->count==p->cap) {
            size_t cap=p->cap?p->cap*2:64; plan *items=realloc(p->items,cap*sizeof(plan));
            if(!items) { snprintf(error,ERROR_SIZE,"Insufficient memory scanning directory"); rc=-1; break; }
            p->items=items; p->cap=cap;
        }
        plan *item=&p->items[p->count++]; memset(item,0,sizeof(*item)); item->path=strdup(path);
        if(!item->path) { snprintf(error,ERROR_SIZE,"Insufficient memory"); rc=-1; break; }
        char dest[PATH_MAX+1024];
        if(incomplete) snprintf(item->error,sizeof(item->error),"MCAP is incomplete; upload and automatic repair disabled");
        else if(!destination_for(entry->d_name,dest,sizeof(dest),item->error)) {
            item->dest=strdup(dest);
            if(!item->dest) { snprintf(error,ERROR_SIZE,"Insufficient memory"); rc=-1; break; }
        }
    }
    closedir(dir); return rc;
}
static int by_path(const void *a,const void *b) { return strcmp(((const plan*)a)->path,((const plan*)b)->path); }
static int by_dest(const void *a,const void *b) {
    const plan *x=*(plan*const*)a,*y=*(plan*const*)b; return strcmp(x->dest?x->dest:"",y->dest?y->dest:"");
}
typedef struct {
    task_manager *manager; task *current; const char *path;
    uint64_t total, settled, transferred, current_bytes, size;
    double last_log;
} task_progress;
/* Frequent progress stays in memory; file boundaries persist the snapshot.
   This avoids rewriting an ever-growing task JSON on every network event. */
static void update_progress(void *context,const char *phase,uint64_t bytes,uint64_t total,double speed) {
    task_progress *p=context;
    if(bytes>p->size) bytes=p->size;
    p->current_bytes=bytes;
    pthread_mutex_lock(&p->manager->mutex);
    cJSON *j=p->current->json,*current=cJSON_GetObjectItemCaseSensitive(j,"current_file");
    if(!current) { current=cJSON_CreateObject(); cJSON_AddItemToObject(j,"current_file",current); }
    if(strcmp(phase,"transferring")) speed=0;
    json_number(j,"speed_bytes_per_second",speed);
    json_number(current,"speed_bytes_per_second",speed);
    js(j,"phase",phase); js(current,"path",p->path); js(current,"file",strrchr(p->path,'/')+1);
    if(!strcmp(phase,"preparing") || !cJSON_GetObjectItemCaseSensitive(current,"fileId")) {
        struct stat st; char id[67];
        cJSON_DeleteItemFromObjectCaseSensitive(current,"fileId");
        if(!lstat(p->path,&st) && S_ISREG(st.st_mode)) { file_identity(p->path,&st,id); js(current,"fileId",id); }
    }
    js(current,"phase",phase); json_number(current,"bytes_total",(double)p->size);
    json_number(current,"bytes_uploaded",(double)bytes);
    double percent=p->size?100.0*(double)bytes/(double)p->size:0;
    if(percent>=100) percent=99.9;
    json_number(current,"progress_percent",percent);
    uint64_t done=p->settled+bytes; if(done>p->total) done=p->total;
    json_number(j,"bytes_total",(double)p->total); json_number(j,"bytes_done",(double)done);
    json_number(j,"bytes_uploaded",(double)(p->transferred+bytes));
    percent=p->total?100.0*(double)done/(double)p->total:0;
    json_number(j,"progress_percent",percent>=100?99.9:percent);
    char now[32]; timestamp(now); js(j,"updated_at",now);
    if(!strcmp(phase,"transferring")) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
        double now_mono=(double)ts.tv_sec+(double)ts.tv_nsec/1e9;
        if(now_mono-p->last_log>=1 && speed>0) {
            p->last_log=now_mono;
            char line[256],log_path[PATH_MAX];
            int n=snprintf(line,sizeof(line),"%s task=%s phase=transferring bytes=%llu/%llu speed=%.2f KiB/s speed_bytes_per_second=%.2f\n",
                now,str(j,"task_id"),(unsigned long long)bytes,(unsigned long long)p->size,speed/1024,speed);
            if(n>0 && (size_t)n<sizeof(line)) {
                fputs(line,stderr);
                if(!path_join(log_path,sizeof(log_path),settings.runtime,"logs/service.log")) {
                    int fd=open(log_path,O_WRONLY|O_APPEND|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
                    if(fd>=0) { (void)write_all(fd,line,(size_t)n); close(fd); }
                }
            }
        }
    }
    pthread_mutex_unlock(&p->manager->mutex);
    (void)total;
}
static void execute(task_manager *m,task *t) {
    char error[ERROR_SIZE]={0},root[PATH_MAX],now[32]; plans p={0};
    task_progress progress={.manager=m,.current=t};
    pthread_mutex_lock(&m->mutex);
    snprintf(root,sizeof(root),"%s",str(t->json,"path")); js(t->json,"status","scanning"); timestamp(now); js(t->json,"started_at",now);
    js(t->json,"phase","scanning");
    int rc=save(m,t); pthread_mutex_unlock(&m->mutex);
    if(rc) goto cleanup;
    const cJSON *files=cJSON_GetObjectItemCaseSensitive(t->json,"files");
    if(!files) rc=scan(&p,root,0,error);
    else {
        p.cap=(size_t)cJSON_GetArraySize(files); p.items=calloc(p.cap,sizeof(plan));
        if(!p.items) { rc=-1; snprintf(error,sizeof(error),"Insufficient memory"); }
        else {
            const cJSON *entry;
            cJSON_ArrayForEach(entry,files) {
                char full[PATH_MAX],dest[PATH_MAX+1024];
                if(!cJSON_IsString(entry) || strchr(entry->valuestring,'/') || strchr(entry->valuestring,'\\') ||
                   path_join(full,sizeof(full),root,entry->valuestring)) { rc=-1; snprintf(error,sizeof(error),"Invalid selected file"); break; }
                plan *item=&p.items[p.count++]; item->path=strdup(full);
                if(!item->path) { rc=-1; snprintf(error,sizeof(error),"Insufficient memory"); break; }
                if(!destination_for(entry->valuestring,dest,sizeof(dest),item->error)) {
                    item->dest=strdup(dest);
                    if(!item->dest) { rc=-1; snprintf(error,sizeof(error),"Insufficient memory"); break; }
                }
            }
        }
    }
    if(!rc && p.count) {
        for(size_t i=0;i<p.count;i++) {
            struct stat st;
            if(!lstat(p.items[i].path,&st) && S_ISREG(st.st_mode) && st.st_size>0) p.items[i].size=(uint64_t)st.st_size;
            progress.total+=p.items[i].size;
        }
        qsort(p.items,p.count,sizeof(plan),by_path);
        plan **sorted=malloc(p.count*sizeof(*sorted));
        if(!sorted) { rc=-1; snprintf(error,sizeof(error),"Insufficient memory"); }
        else {
            for(size_t i=0;i<p.count;i++) sorted[i]=&p.items[i];
            qsort(sorted,p.count,sizeof(*sorted),by_dest);
            for(size_t i=1;i<p.count;i++) if(sorted[i]->dest && sorted[i-1]->dest && !strcmp(sorted[i]->dest,sorted[i-1]->dest)) sorted[i]->conflict=sorted[i-1]->conflict=1;
            free(sorted);
        }
    }
    pthread_mutex_lock(&m->mutex);
    json_number(t->json,"total",(double)p.count); json_number(t->json,"ignored",(double)p.ignored);
    json_number(t->json,"bytes_total",(double)progress.total);
    jb(t->json,"bytes_total_known",!rc);
    json_number(t->json,"bytes_done",0); json_number(t->json,"bytes_uploaded",0);
    json_number(t->json,"progress_percent",0);
    js(t->json,"status",rc?"failed":p.count?"uploading":"no_data");
    if(rc) js(t->json,"error",error);
    int stored=save(m,t); pthread_mutex_unlock(&m->mutex);
    if(rc || stored || !p.count) goto finish;
    for(size_t i=0;i<p.count;i++) {
        plan *item=&p.items[i]; cJSON *result;
        progress.path=item->path; progress.size=item->size; progress.current_bytes=0;
        progress.last_log=0;
        update_progress(&progress,"preparing",0,item->size,0);
        if(item->conflict) snprintf(item->error,sizeof(item->error),"Multiple local files map to the same OSS object; none uploaded");
        if(item->error[0]) {
            result=cJSON_CreateObject(); js(result,"path",item->path); js(result,"file",strrchr(item->path,'/')+1);
            js(result,"status","failed"); jb(result,"ok",0); js(result,"error",item->error);
        } else { error[0]=0; result=process_file_progress(item->path,item->dest,cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t->json,"delete_after_upload")),error,update_progress,&progress); }
        pthread_mutex_lock(&m->mutex);
        if(!cJSON_GetObjectItemCaseSensitive(result,"local_deleted")) jb(result,"local_deleted",0);
        json_number(result,"updatedAt",epoch_ms());
        const char *status=str(result,"status");
        progress.transferred+=progress.current_bytes;
        if(!strcmp(status,"uploaded") || !strcmp(status,"skipped") || !strcmp(status,"empty")) progress.settled+=item->size;
        else progress.settled+=progress.current_bytes;
        json_number(t->json,"bytes_done",(double)progress.settled);
        json_number(t->json,"bytes_uploaded",(double)progress.transferred);
        cJSON *current=cJSON_GetObjectItemCaseSensitive(t->json,"current_file");
        json_number(t->json,"speed_bytes_per_second",0);
        json_number(current,"speed_bytes_per_second",0);
        js(current,"phase",status);
        if(strcmp(status,"failed")) json_number(current,"progress_percent",100);
        double percent=progress.total?100.0*(double)progress.settled/(double)progress.total:0;
        json_number(t->json,"progress_percent",percent>=100?99.9:percent);
        json_number(t->json,status,num(t->json,status)+1); json_number(t->json,"processed",num(t->json,"processed")+1);
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(t->json,"results"),result);
        rc=save(m,t); pthread_mutex_unlock(&m->mutex);
        if(rc) break;
    }
    pthread_mutex_lock(&m->mutex);
    if(!m->storage_failed) {
        double failed=num(t->json,"failed"),ok=num(t->json,"uploaded")+num(t->json,"skipped");
        js(t->json,"status",failed?(ok?"partial_failed":"failed"):"completed");
    }
    pthread_mutex_unlock(&m->mutex);
finish:
    pthread_mutex_lock(&m->mutex);
    json_number(t->json,"speed_bytes_per_second",0);
    js(t->json,"phase",str(t->json,"status"));
    if(!strcmp(str(t->json,"status"),"completed")) json_number(t->json,"progress_percent",100);
    timestamp(now); js(t->json,"finished_at",now); save(m,t); pthread_mutex_unlock(&m->mutex);
cleanup:
    for(size_t i=0;i<p.count;i++) { free(p.items[i].path); free(p.items[i].dest); } free(p.items);
}
static void *worker(void *data) {
    task_manager *m=data;
    while(1) {
        pthread_mutex_lock(&m->mutex); task *next=NULL;
        for(task *t=m->first;t;t=t->next) if(!strcmp(str(t->json,"status"),"queued")) { next=t; break; }
        if(!next || m->storage_failed) {
            if(m->closing) { pthread_mutex_unlock(&m->mutex); break; }
            pthread_cond_wait(&m->changed,&m->mutex); pthread_mutex_unlock(&m->mutex); continue;
        }
        pthread_mutex_unlock(&m->mutex); execute(m,next);
    }
    return NULL;
}
int tasks_start(task_manager *m,char *error) {
    memset(m,0,sizeof(*m)); pthread_mutex_init(&m->mutex,NULL); pthread_cond_init(&m->changed,NULL);
    char folder[PATH_MAX];
    if(path_join(folder,sizeof(folder),settings.runtime,"tasks") || private_directory(folder)) goto bad;
    DIR *dir=opendir(folder); if(!dir) goto bad;
    struct dirent *entry;
    while((entry=readdir(dir))) {
        size_t n=strlen(entry->d_name);
        if(n!=37 || strcmp(entry->d_name+32,".json")) continue;
        char path[PATH_MAX],id[33]; memcpy(id,entry->d_name,32); id[32]=0;
        if(!id_valid(id) || path_join(path,sizeof(path),folder,entry->d_name)) { closedir(dir); goto bad; }
        cJSON *j=json_load(path);
        if(!j || strcmp(str(j,"task_id"),id) || str(j,"path")[0]!='/' || !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(j,"results"))) { cJSON_Delete(j); closedir(dir); goto bad; }
        task *t=calloc(1,sizeof(*t)); if(!t) { cJSON_Delete(j); closedir(dir); goto bad; } t->json=j;
        if(m->last) m->last->next=t; else m->first=t; m->last=t;
        if(active(str(j,"status"))) {
            char now[32]; timestamp(now); js(j,"status","interrupted"); js(j,"finished_at",now); js(j,"error","Service stopped before completion; resubmit path to retry");
            js(j,"phase","interrupted");
            json_number(j,"speed_bytes_per_second",0);
            cJSON *current=cJSON_GetObjectItemCaseSensitive(j,"current_file");
            if(current) json_number(current,"speed_bytes_per_second",0);
            if(save(m,t)) { closedir(dir); goto bad; }
        }
    }
    closedir(dir);
    if(pthread_create(&m->worker,NULL,worker,m)) goto bad;
    return 0;
bad:
    snprintf(error,ERROR_SIZE,"Cannot initialize task storage (check runtime permissions and task JSON)"); return -1;
}
void tasks_close(task_manager *m) {
    pthread_mutex_lock(&m->mutex); m->closing=1; pthread_cond_broadcast(&m->changed); pthread_mutex_unlock(&m->mutex);
    pthread_join(m->worker,NULL);
    task *t=m->first; while(t) { task *next=t->next; cJSON_Delete(t->json); free(t); t=next; }
    pthread_mutex_destroy(&m->mutex); pthread_cond_destroy(&m->changed);
}

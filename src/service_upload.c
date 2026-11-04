#include "service.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int destination_for(const char *name,char *out,size_t cap,char *error) {
    head_identity id;
    if(head_parse(name,&id,error)) return -1;
    int n=snprintf(out,cap,"oss://%s/original_data/ego_human_data/head_cam/%s/%s_device/%s",settings.bucket,id.session,id.device,name);
    if(n<0 || (size_t)n>=cap) { snprintf(error,ERROR_SIZE,"OSS path too long"); return -1; }
    return 0;
}
static char *trim(char *s) {
    while(isspace((unsigned char)*s)) s++;
    size_t n=strlen(s); while(n && isspace((unsigned char)s[n-1])) s[--n]=0;
    return s;
}
static int digest_valid(const char *s) {
    if(strlen(s)!=64) return 0;
    for(int i=0;i<64;i++) if(!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}
/* Return 1: exists, 0: confirmed NoSuchKey, -1: any other error. */
static int remote_stat(const char *dest,cJSON **headers,char *error) {
    const char *args[]={"stat",dest,NULL}; char *text=NULL;
    int rc=oss_run(args,&text,error);
    if(rc) {
        int missing=text && (strstr(text,"ErrorCode=NoSuchKey") || strstr(text,"ErrorCode: NoSuchKey") || strstr(text,"Code: NoSuchKey"));
        free(text); return missing ? 0 : -1;
    }
    cJSON *j=cJSON_CreateObject(); char *state=NULL;
    for(char *line=strtok_r(text,"\n",&state);line;line=strtok_r(NULL,"\n",&state)) {
        char *sep=strchr(line,':'); if(!sep) continue;
        *sep=0; char *key=trim(line),*value=trim(sep+1);
        for(char *k=key;*k;k++) *k=(char)tolower((unsigned char)*k);
        js(j,key,value);
    }
    free(text);
    const char *etag=str(j,"etag");
    if(!*etag || !*str(j,"last-modified")) { cJSON_Delete(j); snprintf(error,ERROR_SIZE,"OSS stat response lacks Etag/Last-Modified; refusing upload"); return -1; }
    char cleaned[256]; snprintf(cleaned,sizeof(cleaned),"%s",etag);
    size_t n=strlen(cleaned);
    if(n>=2 && cleaned[0]=='"' && cleaned[n-1]=='"') { cleaned[n-1]=0; memmove(cleaned,cleaned+1,n-1); }
    js(j,"etag",cleaned);
    *headers=j; return 1;
}
static int iso_modified(const char *input,char out[32]) {
    struct tm tm={0};
    char *end=strptime(input,"%Y-%m-%d %H:%M:%S %z",&tm);
    if(!end) end=strptime(input,"%a, %d %b %Y %H:%M:%S GMT",&tm);
    if(!end) end=strptime(input,"%Y-%m-%dT%H:%M:%SZ",&tm);
    if(!end) return -1;
    long offset=tm.tm_gmtoff;
    time_t value=timegm(&tm)-offset;
    gmtime_r(&value,&tm); strftime(out,32,"%Y-%m-%dT%H:%M:%S.000Z",&tm); return 0;
}
static int metadata(const char *dest,const char *name,const head_identity *id,
    const struct stat *st,double duration,const char *sha,const cJSON *remote,char *error) {
    char relative[PATH_MAX],device_dir[160],modified[32];
    snprintf(device_dir,sizeof(device_dir),"%s_device",id->device);
    if(snprintf(relative,sizeof(relative),"%s/%s/%s",id->session,device_dir,name)>=(int)sizeof(relative) ||
       iso_modified(str(remote,"last-modified"),modified)) { snprintf(error,ERROR_SIZE,"Invalid remote Last-Modified or metadata path"); return -1; }
    cJSON *j=cJSON_CreateObject();
    js(j,"ossPath",dest); js(j,"rootPrefix","original_data"); js(j,"dataDomain","ego_human_data"); js(j,"dataKind","head_cam_v2");
    js(j,"captureDate",id->date); js(j,"deviceNo",id->device); js(j,"deviceDir",device_dir); js(j,"sessionName",id->session);
    js(j,"relativePath",relative); js(j,"objectRole","data"); js(j,"fileFormat","mcap");
    char size_text[32],duration_text[64];
    snprintf(size_text,sizeof(size_text),"%lld",(long long)st->st_size);
    snprintf(duration_text,sizeof(duration_text),"%.3f",duration);
    js(j,"fileSize",size_text); js(j,"duration",duration_text); js(j,"etag",str(remote,"etag"));
    js(j,"storageClass","Standard"); js(j,"lastModified",modified); js(j,"ossMetadata","{}"); js(j,"sha256",sha);
    char headers[16384]; size_t used=0; int rc=-1;
    for(cJSON *item=j->child;item;item=item->next) {
        if(strpbrk(item->valuestring,"#\r\n")) { snprintf(error,ERROR_SIZE,"Unsafe metadata value"); goto done; }
        int n=snprintf(headers+used,sizeof(headers)-used,"%sx-oss-meta-%s:%s",used?"#":"",item->string,item->valuestring);
        if(n<0 || (size_t)n>=sizeof(headers)-used) { snprintf(error,ERROR_SIZE,"Metadata too long"); goto done; }
        used+=(size_t)n;
    }
    const char *args[]={"set-meta",dest,headers,"--update","--force",NULL};
    rc=oss_run(args,NULL,error);
    if(rc) snprintf(error,ERROR_SIZE,"File exists on OSS but metadata update failed; resubmit directory to retry");
done:
    cJSON_Delete(j); return rc;
}
cJSON *process_file_progress(const char *path,const char *destination,char *error,
                            upload_progress_fn progress,void *context) {
    const char *name=strrchr(path,'/'); name=name?name+1:path;
    cJSON *result=cJSON_CreateObject(),*remote=NULL;
    js(result,"path",path); js(result,"file",name); js(result,"status","failed"); jb(result,"ok",0);
    head_identity id; int fd=-1,exists; struct stat initial,after;
    char resolved[PATH_MAX],sha[65],after_sha[65]; double duration=0;
    if(head_parse(name,&id,error)) goto done;
    if(!realpath(path,resolved) || strcmp(resolved,path) ||
       (settings.allowed_root[0] && !path_inside(resolved,settings.allowed_root))) { snprintf(error,ERROR_SIZE,"File path changed or contains a symlink"); goto done; }
    fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0 || fstat(fd,&initial) || !S_ISREG(initial.st_mode)) { snprintf(error,ERROR_SIZE,"File is missing, unreadable or not regular"); goto done; }
    char fd_path[128]; snprintf(fd_path,sizeof(fd_path),"/proc/self/fd/%d",fd);
    if(!realpath(fd_path,resolved) || strcmp(resolved,path)) { snprintf(error,ERROR_SIZE,"File location changed while opening"); goto done; }
    json_number(result,"size",(double)initial.st_size);
    if(progress) progress(context,"preparing",0,(uint64_t)initial.st_size);
    if(!initial.st_size) { js(result,"status","empty"); jb(result,"ok",1); goto done; }
    if(mcap_duration(fd,&duration,error) || sha256_fd(fd,sha) || fstat(fd,&after) || !same_file(&initial,&after)) {
        if(!error[0]) snprintf(error,ERROR_SIZE,"File changed while parsing/checksumming");
        goto done;
    }
    js(result,"ossPath",destination); json_number(result,"duration",duration); js(result,"sha256",sha);
    cJSON_AddNullToObject(result,"remote_sha256"); cJSON_AddNullToObject(result,"same_content");
    js(result,"checksum_status","unknown"); jb(result,"already_exists",0);
    if(progress) progress(context,"checking_remote",0,(uint64_t)initial.st_size);
    exists=remote_stat(destination,&remote,error);
    if(exists<0) goto done;
    jb(result,"already_exists",exists);
    if(exists) {
        const char *old=str(remote,"x-oss-meta-sha256");
        if(!digest_valid(old)) {
            js(result,"status","skipped"); jb(result,"ok",1);
            js(result,"checksum_message","Existing object has no valid SHA256; content unknown, object left unchanged");
            error[0]=0; goto done;
        }
        char lower[65]; for(int i=0;i<65;i++) lower[i]=(char)tolower((unsigned char)old[i]);
        js(result,"remote_sha256",lower); jb(result,"same_content",!strcmp(lower,sha));
        js(result,"checksum_status",strcmp(lower,sha)?"mismatch":"match");
        if(strcmp(lower,sha)) { snprintf(error,ERROR_SIZE,"Existing OSS object has different SHA256; not overwritten"); goto done; }
    } else {
        char source[128],meta[128],checkpoint[PATH_MAX];
        /* Pin the inode: ossutil follows this fd path, not a replaceable directory entry. */
        snprintf(source,sizeof(source),"/proc/%ld/fd/%d",(long)getpid(),fd);
        snprintf(meta,sizeof(meta),"x-oss-meta-sha256:%s",sha);
        if(path_join(checkpoint,sizeof(checkpoint),settings.runtime,"checkpoints")) goto done;
        const char *args[]={"cp",source,destination,"--force","--meta",meta,"--checkpoint-dir",checkpoint,"--parallel=1","--jobs=1",NULL};
        if(progress) progress(context,"transferring",0,(uint64_t)initial.st_size);
        if(oss_run_progress(args,NULL,error,progress,context,(uint64_t)initial.st_size)) goto done;
        if(progress) progress(context,"verifying",(uint64_t)initial.st_size,(uint64_t)initial.st_size);
        cJSON_Delete(remote); remote=NULL;
        if(remote_stat(destination,&remote,error)!=1) { snprintf(error,ERROR_SIZE,"Cannot verify OSS object after upload"); goto done; }
        const char *saved=str(remote,"x-oss-meta-sha256");
        js(result,"remote_sha256",saved);
        if(strcmp(saved,sha)) { snprintf(error,ERROR_SIZE,"Remote SHA256 metadata missing or changed after upload"); goto done; }
    }
    if(progress) progress(context,"verifying",exists?0:(uint64_t)initial.st_size,(uint64_t)initial.st_size);
    if(sha256_fd(fd,after_sha) || fstat(fd,&after) || !same_file(&initial,&after) || strcmp(sha,after_sha)) {
        snprintf(error,ERROR_SIZE,"Local file changed during upload/check; remote content cannot be confirmed");
        cJSON_DeleteItemFromObjectCaseSensitive(result,"same_content"); cJSON_AddNullToObject(result,"same_content");
        js(result,"checksum_status","unknown"); goto done;
    }
    js(result,"remote_sha256",sha); jb(result,"same_content",1); js(result,"checksum_status","match");
    if(progress) progress(context,"metadata",exists?0:(uint64_t)initial.st_size,(uint64_t)initial.st_size);
    if(metadata(destination,name,&id,&initial,duration,sha,remote,error)) goto done;
    js(result,"status",exists?"skipped":"uploaded"); jb(result,"ok",1); error[0]=0;
done:
    if(error[0]) js(result,"error",error);
    if(fd>=0) close(fd);
    cJSON_Delete(remote); return result;
}

#include "service.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static void cleanup_sessions(void) {
    DIR *dir=opendir(settings.runtime); if(!dir) return;
    struct dirent *e;
    while((e=readdir(dir))) {
        if(strncmp(e->d_name,"session-",8)) continue;
        char path[PATH_MAX],file[PATH_MAX]; struct stat st;
        if(path_join(path,sizeof(path),settings.runtime,e->d_name) || lstat(path,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid()) continue;
        const char *names[]={"ossutil","credentials","ca.crt"};
        for(size_t i=0;i<3;i++) if(!path_join(file,sizeof(file),path,names[i])) unlink(file);
        rmdir(path);
    }
    closedir(dir);
}
static void usage(const char *name) {
    printf("head-cam-upload %s\nUsage: %s [--host IPv4] [--port N] [--runtime-dir PATH] [--allowed-root PATH]\n"
        "       %s --self-check\nDefault: 0.0.0.0:38127, runtime/ beside executable.\n"
        "HEAD_CAM_UPLOAD_TOKEN optionally requires Authorization: Bearer <token>.\n",SERVICE_VERSION,name,name);
}
int main(int argc,char **argv) {
    umask(0077);
    /* Only built-in AES/SHA algorithms are used; do not load device-specific
       OpenSSL config/provider modules into the statically linked application. */
    if(!OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG,NULL)) return 1;
    const char *host="0.0.0.0",*runtime=NULL,*root=NULL,*config=NULL,*ossutil=NULL;
    unsigned port=38127; int self_check=0;
    settings.process_timeout=1800;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) { usage(argv[0]); return 0; }
        if(!strcmp(argv[i],"--self-check")) { self_check=1; continue; }
        if(i+1>=argc) { usage(argv[0]); return 64; }
        const char *option=argv[i],*value=argv[++i];
        if(!strcmp(option,"--host")) host=value;
        else if(!strcmp(option,"--runtime-dir")) runtime=value;
        else if(!strcmp(option,"--allowed-root")) root=value;
        else if(!strcmp(option,"--port")) {
            char *end; errno=0; unsigned long parsed=strtoul(value,&end,10);
            if(errno || !*value || *end || parsed<1 || parsed>65535) return 64;
            port=(unsigned)parsed;
        }
#ifdef SERVICE_TESTING
        else if(!strcmp(option,"--test-config")) { config=value; settings.offline=1; }
        else if(!strcmp(option,"--test-ossutil")) ossutil=value;
        else if(!strcmp(option,"--test-timeout")) settings.process_timeout=(unsigned)strtoul(value,NULL,10);
#endif
        else { usage(argv[0]); return 64; }
    }
    char exe[PATH_MAX],path[PATH_MAX],error[ERROR_SIZE]={0};
    ssize_t len=readlink("/proc/self/exe",exe,sizeof(exe)-1);
    if(len<0 || len>=(ssize_t)sizeof(exe)-1) { fprintf(stderr,"Cannot locate executable\n"); return 1; }
    exe[len]=0; char *slash=strrchr(exe,'/'); if(!slash) return 1; *slash=0;
    if(self_check) {
        snprintf(path,sizeof(path),"/tmp/head-cam-selfcheck-XXXXXX");
        if(!mkdtemp(path)) return 1;
    } else if(runtime) {
        if(strlen(runtime)>=sizeof(path)) return 64;
        snprintf(path,sizeof(path),"%s",runtime);
    } else if(path_join(path,sizeof(path),exe,"runtime")) return 1;
    if(private_directory(path) || !realpath(path,settings.runtime)) { fprintf(stderr,"Runtime must be a writable private directory (parent must exist)\n"); return 1; }
    if(root) {
        struct stat st;
        if(!realpath(root,settings.allowed_root) || stat(settings.allowed_root,&st) || !S_ISDIR(st.st_mode)) { fprintf(stderr,"Invalid allowed root\n"); return 64; }
    }
    char lock_path[PATH_MAX]; path_join(lock_path,sizeof(lock_path),settings.runtime,".lock");
    int lock=open(lock_path,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lock<0 || flock(lock,LOCK_EX|LOCK_NB)) { fprintf(stderr,"Runtime is already locked by another service or inaccessible\n"); if(lock>=0) close(lock); return 1; }
    cleanup_sessions();
    int rc=1;
    if(bundle_open(config,ossutil,error)) goto done;
    if(oss_self_check(error)) goto done;
    if(self_check) { printf("PASS: C service, embedded credentials, ossutil; no OSS requests made.\n"); rc=0; goto done; }
    if(chdir(settings.runtime)) { snprintf(error,sizeof(error),"Cannot enter runtime directory"); goto done; }
    if(path_join(path,sizeof(path),settings.runtime,"logs") || private_directory(path)) { snprintf(error,sizeof(error),"Cannot create log directory"); goto done; }
    char log_path[PATH_MAX]; path_join(log_path,sizeof(log_path),path,"service.log");
    int log_fd=open(log_path,O_WRONLY|O_APPEND|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if(log_fd<0) { snprintf(error,sizeof(error),"Cannot create service log"); goto done; }
    char now[32]; timestamp(now); dprintf(log_fd,"%s service %s started\n",now,SERVICE_VERSION); close(log_fd);
    task_manager manager;
    if(tasks_start(&manager,error)) goto done;
    rc=http_serve(&manager,host,port,getenv("HEAD_CAM_UPLOAD_TOKEN"));
    if(rc) snprintf(error,sizeof(error),"Cannot listen on requested IPv4 address/port");
    tasks_close(&manager);
done:
    if(error[0]) fprintf(stderr,"%s\n",error);
    bundle_close(); close(lock);
    if(self_check) { unlink(lock_path); rmdir(settings.runtime); }
    return rc?1:0;
}

/* 单文件多模式入口：每个业务仍通过 exec 启动独立进程。 */
#include "service.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
int upload_main(int,char **);
int monitor_main(int,char **);
int supervisor_main(int,char **);

static int tools(int argc,char **argv,int alias) {
#ifdef SERVICE_BUSYBOX
    extern const unsigned char embedded_busybox_start[],embedded_busybox_end[];
    char directory[PATH_MAX],path[PATH_MAX],lockpath[PATH_MAX];
    snprintf(directory,sizeof(directory),"/tmp/head-cam-tools-%lu-%s",(unsigned long)geteuid(),BB_CACHE_TAG);
    if(private_directory(directory) || path_join(path,sizeof(path),directory,"busybox") ||
       path_join(lockpath,sizeof(lockpath),directory,".lock"))return 1;
    int lock=open(lockpath,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(lock<0 || flock(lock,LOCK_EX)){if(lock>=0)close(lock);return 1;}
    struct stat st;size_t size=(size_t)(embedded_busybox_end-embedded_busybox_start);
    if(lstat(path,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (size_t)st.st_size!=size) {
        char temporary[PATH_MAX];if(path_join(temporary,sizeof(temporary),directory,"busybox.new")){close(lock);return 1;}
        int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0700);
        if(fd<0){close(lock);return 1;}
        int failed=write_all(fd,embedded_busybox_start,size) || fsync(fd);close(fd);
        if(failed || rename(temporary,path)){close(lock);return 1;}
    }
    flock(lock,LOCK_UN);close(lock);
    char **args=calloc((size_t)argc+2,sizeof(char *));if(!args)return 1;
    args[0]=alias?argv[0]:"busybox";
    for(int i=1;i<argc;i++)args[i]=argv[i];
    execv(path,args);perror("embedded busybox");free(args);return 1;
#else
    (void)argc;(void)argv;(void)alias;
    fprintf(stderr,"Native test build has no embedded BusyBox\n");return 1;
#endif
}

int main(int argc,char **argv) {
    const char *name=strrchr(argv[0],'/');name=name?name+1:argv[0];
    if(!strcmp(name,"head-cam-upload"))return upload_main(argc,argv);
    if(!strcmp(name,"head-cam-monitor"))return monitor_main(argc,argv);
    if(strcmp(name,"head-cam-service"))return tools(argc,argv,1);
    if(argc>1) {
        if(!strcmp(argv[1],"upload"))return upload_main(argc-1,argv+1);
        if(!strcmp(argv[1],"monitor"))return monitor_main(argc-1,argv+1);
        if(!strcmp(argv[1],"supervise"))return supervisor_main(argc-1,argv+1);
        if(!strcmp(argv[1],"--flock") || !strcmp(argv[1],"--rename"))return supervisor_main(argc,argv);
#ifdef SERVICE_TESTING
        if(!strcmp(argv[1],"--self-check"))return supervisor_main(argc,argv);
#endif
        if(!strcmp(argv[1],"--self-check") || !strncmp(argv[1],"--update-",9))return upload_main(argc,argv);
        if(argv[1][0]!='-')return tools(argc,argv,0);
    }
    if(argc>1 && (!strcmp(argv[1],"--help") || !strcmp(argv[1],"-h"))) {
        puts("head-cam-service [upload OPTIONS | monitor CONFIG RUNTIME | supervise LOCK UPLOAD_ENTRY MONITOR_ENTRY]\nNo arguments: supervise components using monitor.json beside binary.");return 0;
    }
    if(argc!=1)return upload_main(argc,argv);
    char exe[PATH_MAX],home[PATH_MAX],runtime[PATH_MAX],logs[PATH_MAX],lock[PATH_MAX];
    ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return 1;exe[n]=0;
    const char *packed=getenv("HEAD_CAM_PACKED_EXE");
    const char *origin=packed && *packed=='/' ? packed : exe;
    if(strlen(origin)>=sizeof(home))return 1;
    strcpy(home,origin);char *slash=strrchr(home,'/');if(!slash)return 1;*slash=0;
    const char *configured=getenv("HEAD_CAM_HOME");if(configured){if(strlen(configured)>=sizeof(home))return 1;strcpy(home,configured);}
    if(setenv("HEAD_CAM_HOME",home,1) || path_join(runtime,sizeof(runtime),home,"runtime") || private_directory(runtime) ||
       path_join(logs,sizeof(logs),home,"logs") || private_directory(logs) || path_join(lock,sizeof(lock),runtime,"supervisor.lock"))return 1;
    char *args[]={argv[0],lock,exe,NULL};return supervisor_main(3,args);
}

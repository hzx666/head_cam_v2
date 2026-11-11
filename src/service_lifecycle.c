#include "service.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#include "lifecycle_data.h"

/* Helpers are private implementation resources, not deployment files. */
int lifecycle_main(const char *command) {
    char exe[PATH_MAX],home[PATH_MAX],cache[PATH_MAX],path[PATH_MAX],lockpath[PATH_MAX];
    ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
    if(n<0 || n>=(ssize_t)sizeof(exe)-1)return 1;
    exe[n]=0;
    const char *packed=getenv("HEAD_CAM_PACKED_EXE");
    if(packed && *packed=='/' && strlen(packed)<sizeof(exe))strcpy(exe,packed);
    strcpy(home,exe);char *slash=strrchr(home,'/');if(!slash)return 1;*slash=0;
    const char *configured=getenv("HEAD_CAM_HOME");
    if(configured) {if(strlen(configured)>=sizeof(home))return 1;strcpy(home,configured);}
    if(setenv("HEAD_CAM_HOME",home,1))return 1;
    char active[PATH_MAX],resolved[PATH_MAX],origin[PATH_MAX];
    if(strcmp(command,"install") && !path_join(active,sizeof(active),home,"current/head-cam-service") &&
       realpath(active,resolved) && realpath(exe,origin) && strcmp(resolved,origin)) {
        execl(resolved,"head-cam-service",command,(char *)NULL);
        perror("Cannot execute active service");return 1;
    }
    if(setenv("HEAD_CAM_BUSYBOX",exe,1) ||
       setenv("HEAD_CAM_LOCK_HELPER",exe,1))return 1;
    /* Key by the actual executable cache directory, so versions never overwrite
       helpers used by another running version. */
    char core[PATH_MAX];n=readlink("/proc/self/exe",core,sizeof(core)-1);
    if(n<0 || n>=(ssize_t)sizeof(core)-1)return 1;
    core[n]=0;char hash[65];int fd=open(core,O_RDONLY|O_CLOEXEC);
    if(fd<0)return 1;
    int failed=sha256_fd(fd,hash);close(fd);if(failed)return 1;
    snprintf(cache,sizeof(cache),"/tmp/head-cam-lifecycle-%lu-%s",(unsigned long)geteuid(),hash);
    if(private_directory(cache) || path_join(lockpath,sizeof(lockpath),cache,".lock"))return 1;
    int lock=open(lockpath,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(lock<0)return 1;
    if(flock(lock,LOCK_EX)){close(lock);return 1;}
    const char *names[]={"boot_autostart.sh","manual_dhcp.sh"};
    const char *data[]={embedded_boot,embedded_dhcp};
    for(size_t i=0;i<2;i++) {
        if(path_join(path,sizeof(path),cache,names[i])){close(lock);return 1;}
        char temporary[PATH_MAX];int length=snprintf(temporary,sizeof(temporary),"%s.new",path);
        if(length<0 || (size_t)length>=sizeof(temporary)){close(lock);return 1;}
        fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0){close(lock);return 1;}
        failed=write_all(fd,data[i],strlen(data[i]));close(fd);
        if(failed || rename(temporary,path)){close(lock);return 1;}
    }
    close(lock);
    if(setenv("HEAD_CAM_INTERNAL_DIR",cache,1))return 1;
    if(!strcmp(command,"dhcp")) {
        if(path_join(path,sizeof(path),cache,"manual_dhcp.sh") || setenv("HEAD_CAM_TOOL",exe,1))return 1;
        const char *iface=getenv("HEAD_CAM_NETWORK_IFACE");
        execl("/bin/sh","sh",path,iface?iface:"wlan0",(char *)NULL);
        return 1;
    }
    if(path_join(path,sizeof(path),cache,"boot_autostart.sh"))return 1;
    execl("/bin/sh","sh",path,command,(char *)NULL);
    perror("Cannot start service manager");return 1;
}

cJSON *monitor_defaults(void) {
    cJSON *config=cJSON_Parse(embedded_monitor);
    if(!config)return NULL;
    /* Temporary identity until firmware exposes the SN interface. */
    js(config,"device_sn",str(config,"device_id"));
    return config;
}

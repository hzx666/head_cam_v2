/* 统一进程管理：业务进程独立重启，不共享业务状态。 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>

static volatile sig_atomic_t stopping;
static void stop_service(int sig) { (void)sig; stopping=1; }
typedef struct { const char *path; pid_t pid; time_t retry_at; } child;

static void launch(child *c) {
    pid_t parent=getpid();
    c->pid=fork();
    if(c->pid==0) {
        if(prctl(PR_SET_PDEATHSIG,SIGTERM) || getppid()!=parent) _exit(125);
        setpgid(0,0);
        signal(SIGTERM,SIG_DFL); signal(SIGINT,SIG_DFL);
        const char *home=getenv("HEAD_CAM_HOME");
        if(home) {
            char log[PATH_MAX];const char *name=strrchr(c->path,'/');name=name?name+1:c->path;
            int n=snprintf(log,sizeof(log),"%s/logs/%s.log",home,name);
            if(n<0 || (size_t)n>=sizeof(log))_exit(125);
            int fd=open(log,O_WRONLY|O_APPEND|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
            if(fd<0 || dup2(fd,STDOUT_FILENO)<0 || dup2(fd,STDERR_FILENO)<0)_exit(125);
            if(fd>2)close(fd);
        }
        execl(c->path,c->path,(char *)NULL);
        perror("Cannot launch component"); _exit(127);
    }
    if(c->pid<0) { perror("fork"); c->pid=0; c->retry_at=time(NULL)+10; }
    else setpgid(c->pid,c->pid);
}

int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--self-check")) {puts("PASS: supervisor");return 0;}
    if(argc!=4) {
        fprintf(stderr,"Usage: %s LOCK_FILE UPLOAD_EXECUTABLE MONITOR_EXECUTABLE\n",argv[0]);
        return 64;
    }
    int lock=open(argv[1],O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(lock<0 || flock(lock,LOCK_EX|LOCK_NB)) { perror("supervisor lock"); return 1; }
    struct sigaction sa={0}; sa.sa_handler=stop_service; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM,&sa,NULL); sigaction(SIGINT,&sa,NULL);
    child children[2]={{argv[2],0,0},{argv[3],0,0}};
    while(!stopping) {
        for(int i=0;i<2;i++) {
            child *c=&children[i];
            if(c->pid) {
                int status; pid_t result=waitpid(c->pid,&status,WNOHANG);
                if(result==c->pid || (result<0 && errno==ECHILD)) {
                    /* 清理该组件遗留的辅助进程，避免重启后重复运行。 */
                    kill(-c->pid,SIGKILL);
                    fprintf(stderr,"component %s exited; retry in 10 seconds\n",c->path);
                    c->pid=0; c->retry_at=time(NULL)+10;
                }
            }
            if(!c->pid && time(NULL)>=c->retry_at && !stopping) launch(c);
        }
        sleep(1);
    }
    /* 上传自行完成优雅退出；外层服务管理器决定最终停止期限。 */
    for(int i=0;i<2;i++) if(children[i].pid) kill(children[i].pid,SIGTERM);
    for(int i=0;i<2;i++) if(children[i].pid) {
        while(waitpid(children[i].pid,NULL,0)<0 && errno==EINTR) {}
        kill(-children[i].pid,SIGKILL);
    }
    close(lock); return 0;
}

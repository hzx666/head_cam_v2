#include "service.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#ifdef SERVICE_EMBEDDED
#include "credentials.h"
extern const unsigned char bundled_ossutil_start[], bundled_ossutil_end[];
extern const unsigned char bundled_ca_start[], bundled_ca_end[];
#endif
extern char **environ;
static int put_private(const char *path,const void *data,size_t size,mode_t mode) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,mode);
    if(fd<0) return -1;
    int rc=write_all(fd,data,size) || fsync(fd);
    if(close(fd)) rc=-1;
    return rc ? -1 : 0;
}
static cJSON *embedded_credentials(void) {
#ifdef SERVICE_EMBEDDED
    unsigned char plaintext[sizeof(credential_cipher)+32]; int n=0,last=0;
    EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();
    int ok=ctx && EVP_DecryptInit_ex(ctx,EVP_aes_256_ctr(),NULL,credential_key,credential_iv)==1 &&
        EVP_DecryptUpdate(ctx,plaintext,&n,credential_cipher,sizeof(credential_cipher))==1 &&
        EVP_DecryptFinal_ex(ctx,plaintext+n,&last)==1;
    EVP_CIPHER_CTX_free(ctx);
    cJSON *j=NULL;
    if(ok) { plaintext[n+last]=0; j=json_parse((char*)plaintext,(size_t)(n+last+1)); }
    OPENSSL_cleanse(plaintext,sizeof(plaintext));
    return j;
#else
    return NULL;
#endif
}
int bundle_open(const char *config_override,const char *ossutil_override,char *error) {
    cJSON *creds=NULL;
#ifdef SERVICE_TESTING
    if(config_override) creds=json_load(config_override);
#else
    (void)config_override; (void)ossutil_override;
#endif
    if(!creds) creds=embedded_credentials();
    if(!creds) { snprintf(error,ERROR_SIZE,"Embedded credentials unavailable; rebuild with build credentials"); return -1; }
    const char *required[]={"endpoint","bucket","access_key_id","access_key_secret"};
    for(size_t i=0;i<4;i++) {
        const char *v=str(creds,required[i]);
        if(!*v || strlen(v)>2048 || strpbrk(v,"\r\n")) goto invalid;
    }
    const char *bucket=str(creds,"bucket"), *endpoint=str(creds,"endpoint");
    if(strlen(bucket)>=sizeof(settings.bucket) || strspn(bucket,"abcdefghijklmnopqrstuvwxyz0123456789-")!=strlen(bucket)) goto invalid;
    if(strspn(endpoint,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:")!=strlen(endpoint)) goto invalid;
    if(strpbrk(str(creds,"security_token"),"\r\n")) goto invalid;
    snprintf(settings.bucket,sizeof(settings.bucket),"%s",bucket);
    snprintf(settings.bundle_dir,sizeof(settings.bundle_dir),"/tmp/head-cam-session-XXXXXX");
    if(!mkdtemp(settings.bundle_dir)) { settings.bundle_dir[0]=0; goto io_error; }
    if(path_join(settings.ossutil,sizeof(settings.ossutil),settings.bundle_dir,"ossutil") ||
       path_join(settings.config,sizeof(settings.config),settings.bundle_dir,"credentials") ||
       path_join(settings.ca,sizeof(settings.ca),settings.bundle_dir,"ca.crt")) goto io_error;
#ifdef SERVICE_TESTING
    if(ossutil_override) {
        if(!realpath(ossutil_override,settings.ossutil) || access(settings.ossutil,X_OK)) goto io_error;
    } else
#endif
    {
#ifdef SERVICE_EMBEDDED
        if(put_private(settings.ossutil,bundled_ossutil_start,(size_t)(bundled_ossutil_end-bundled_ossutil_start),0700)) goto io_error;
#else
        goto io_error;
#endif
    }
#ifdef SERVICE_EMBEDDED
    if(put_private(settings.ca,bundled_ca_start,(size_t)(bundled_ca_end-bundled_ca_start),0600)) goto io_error;
    if(setenv("SSL_CERT_FILE",settings.ca,1)) goto io_error;
#endif
    char text[8192];
    int n=snprintf(text,sizeof(text),"[Credentials]\nlanguage=EN\nendpoint=https://%s\naccessKeyID=%s\naccessKeySecret=%s\nstsToken=%s\n",
        endpoint,str(creds,"access_key_id"),str(creds,"access_key_secret"),str(creds,"security_token"));
    int rc=n<0 || (size_t)n>=sizeof(text) || put_private(settings.config,text,(size_t)n,0600);
    OPENSSL_cleanse(text,sizeof(text));
    /* cJSON owns plaintext copies; wipe credential string values before freeing. */
    for(cJSON *v=creds->child;v;v=v->next) if(cJSON_IsString(v)) OPENSSL_cleanse(v->valuestring,strlen(v->valuestring));
    cJSON_Delete(creds);
    if(rc) { snprintf(error,ERROR_SIZE,"Cannot create private ossutil configuration"); return -1; }
    return 0;
invalid:
    snprintf(error,ERROR_SIZE,"Invalid build credentials"); cJSON_Delete(creds); return -1;
io_error:
    snprintf(error,ERROR_SIZE,"Cannot extract private runtime dependencies"); cJSON_Delete(creds); return -1;
}
void bundle_close(void) {
    if(settings.bundle_dir[0]) {
        char path[PATH_MAX];
        const char *names[]={"ossutil","credentials","ca.crt"};
        for(size_t i=0;i<3;i++) if(!path_join(path,sizeof(path),settings.bundle_dir,names[i])) unlink(path);
        rmdir(settings.bundle_dir);
    }
}
static double monotonic(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (double)ts.tv_sec+(double)ts.tv_nsec/1e9; }
int oss_run(const char *const *args,char **output,char *error) {
    return oss_run_progress(args,output,error,NULL,NULL,0);
}
/* ossutil 1.7.19 emits exact, comma-grouped byte counts as "OK size: N".
   Keep a separate bounded frame buffer: output may exceed the diagnostic cap,
   split anywhere across reads, or refresh with CR rather than newline. */
static void progress_frame(const char *line,upload_progress_fn cb,void *context,uint64_t total) {
    if(!cb || (strncmp(line,"Total num:",10) && strncmp(line,"Scanned num:",12))) return;
    const char *p=strstr(line,", OK size: "); if(!p) return; p+=11;
    uint64_t value=0; int digits=0;
    while((*p>='0' && *p<='9') || *p==',') {
        if(*p!=',') { unsigned digit=(unsigned)(*p-'0'); if(value>(UINT64_MAX-digit)/10) return; value=value*10+digit; digits++; }
        p++;
    }
    if(digits && value<=total && (*p==',' || *p==' ' || !*p)) cb(context,"transferring",value,total);
}
int oss_run_progress(const char *const *args,char **output,char *error,
                     upload_progress_fn progress,void *context,uint64_t total) {
    char *argv[80]; int argc=0;
    argv[argc++]=settings.ossutil; argv[argc++]="-c"; argv[argc++]=settings.config;
    for(size_t i=0;args[i];i++) { if(argc>=70) return -1; argv[argc++]=(char*)args[i]; }
    argv[argc++]="--connect-timeout=10"; argv[argc++]="--read-timeout=30"; argv[argc++]="--retry-times=2"; argv[argc]=NULL;
    int pipefd[2]; if(pipe2(pipefd,O_CLOEXEC)) goto failed;
    pid_t parent=getpid(),pid=fork();
    if(pid==0) {
        /* Only syscall/async-signal-safe operations after a multithreaded fork.
           Killing the service must not leave an orphan upload racing a retry. */
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent || setpgid(0,0)) _exit(126);
        int nullfd=open("/dev/null",O_RDONLY);
        if(nullfd<0 || dup2(nullfd,STDIN_FILENO)<0 || dup2(pipefd[1],STDOUT_FILENO)<0 || dup2(pipefd[1],STDERR_FILENO)<0) _exit(126);
        if(nullfd>2) close(nullfd);
        close(pipefd[0]); close(pipefd[1]);
        execve(settings.ossutil,argv,environ); _exit(127);
    }
    close(pipefd[1]);
    if(pid<0) { close(pipefd[0]); goto failed; }
    /* Close the small parent-side process-group race before timeout handling. */
    (void)setpgid(pid,pid);
    fcntl(pipefd[0],F_SETFL,O_NONBLOCK);
    const size_t cap=128*1024; char *buf=malloc(cap+1); size_t used=0;
    if(!buf) { kill(-pid,SIGKILL); waitpid(pid,NULL,0); close(pipefd[0]); goto failed; }
    double deadline=monotonic()+(double)settings.process_timeout; int status=0,finished=0,eof=0,timed_out=0;
    char frame[2048]; size_t frame_used=0; int overflow=0;
    while(!finished || !eof) {
        struct pollfd pollfd={.fd=pipefd[0],.events=POLLIN}; poll(&pollfd,1,100);
        char temp[8192]; ssize_t n;
        while((n=read(pipefd[0],temp,sizeof(temp)))>0) {
            if(progress) for(ssize_t i=0;i<n;i++) {
                if(temp[i]=='\r' || temp[i]=='\n') {
                    frame[frame_used]=0; if(!overflow) progress_frame(frame,progress,context,total);
                    frame_used=0; overflow=0;
                } else if(frame_used+1<sizeof(frame)) frame[frame_used++]=temp[i];
                else overflow=1;
            }
            size_t take=(size_t)n; if(take>cap-used) take=cap-used;
            memcpy(buf+used,temp,take); used+=take;
            /* The last refresh has no trailing CR until the next refresh.
               Parse once its following field is present instead of lagging
               one complete ossutil reporting interval behind. */
            frame[frame_used]=0;
            if(progress && !overflow && (strstr(frame,", Progress:") || strstr(frame,", Speed:")))
                progress_frame(frame,progress,context,total);
        }
        if(!n || (n<0 && errno!=EAGAIN && errno!=EINTR)) eof=1;
        if(!finished) { pid_t w=waitpid(pid,&status,WNOHANG); if(w==pid) finished=1; }
        if(monotonic()>deadline) { timed_out=1; kill(-pid,SIGKILL); if(!finished) while(waitpid(pid,&status,0)<0 && errno==EINTR) {} break; }
    }
    frame[frame_used]=0; if(progress && !overflow) progress_frame(frame,progress,context,total);
    close(pipefd[0]); buf[used]=0;
    if(output) *output=buf; else free(buf);
    if(timed_out) { snprintf(error,ERROR_SIZE,"OSS operation timed out"); return -1; }
    if(!WIFEXITED(status) || WEXITSTATUS(status)) { snprintf(error,ERROR_SIZE,"OSS operation failed (exit %d)",WIFEXITED(status)?WEXITSTATUS(status):-1); return -1; }
    return 0;
failed:
    snprintf(error,ERROR_SIZE,"Cannot start embedded ossutil"); return -1;
}
int oss_self_check(char *error) {
    const char *args[]={"--version",NULL}; char *out=NULL;
    int rc=oss_run(args,&out,error); free(out); return rc;
}

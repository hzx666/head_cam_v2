/* MQTT 3.1.1 QoS0 传输；所有 socket 操作有超时并检查服务端响应。 */
#include "monitor.h"
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

static int send_all(int fd,const void *data,size_t n) {
    const char *p=data;
    while(n) { ssize_t r=send(fd,p,n,MSG_NOSIGNAL); if(r<=0) return -1; p+=r;n-=(size_t)r; }
    return 0;
}
static int packet(int fd,unsigned char type,const void *body,size_t n) {
    unsigned char h[5]; size_t k=1,remaining=n; h[0]=type;
    if(n>1024*1024) return -1;
    do { unsigned char d=remaining%128;remaining/=128;h[k++]=d|(remaining?128:0); } while(remaining);
    return send_all(fd,h,k) || send_all(fd,body,n) ? -1:0;
}
static int response(int fd,unsigned char kind) {
    unsigned char h[2];
    if(recv(fd,h,2,MSG_WAITALL)!=2 || h[0]!=kind) return -1;
    if(kind==0xd0) return h[1]? -1:0;
    unsigned char result[2];
    return h[1]!=2 || recv(fd,result,2,MSG_WAITALL)!=2 || result[1]!=0 ? -1:0;
}
static void field(unsigned char *b,size_t *n,const char *s) {
    size_t len=strlen(s);b[(*n)++]=(unsigned char)(len>>8);b[(*n)++]=(unsigned char)len;
    memcpy(b+*n,s,len);*n+=len;
}
int monitor_connect(const cJSON *c,const char *topic,const char *will) {
    char port[16];snprintf(port,sizeof(port),"%d",(int)num(c,"port"));
    struct addrinfo hints={.ai_socktype=SOCK_STREAM},*addresses=NULL;
    if(getaddrinfo(str(c,"host"),port,&hints,&addresses)) return -1;
    int fd=-1;
    for(struct addrinfo *a=addresses;a;a=a->ai_next) {
        fd=socket(a->ai_family,SOCK_STREAM|SOCK_CLOEXEC,a->ai_protocol);if(fd<0)continue;
        fcntl(fd,F_SETFL,O_NONBLOCK);
        int rc=connect(fd,a->ai_addr,a->ai_addrlen);
        struct pollfd p={fd,POLLOUT,0};int error=0;socklen_t len=sizeof(error);
        if((rc==0 || (errno==EINPROGRESS && poll(&p,1,5000)>0)) && !getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len) && !error) break;
        close(fd);fd=-1;
    }
    freeaddrinfo(addresses);if(fd<0)return -1;
    fcntl(fd,F_SETFL,0);struct timeval timeout={5,0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    unsigned char body[16384];size_t n=0;
    const char *id=str(c,"device_id"),*user=str(c,"username"),*pass=str(c,"password");
    if(strlen(id)+strlen(user)+strlen(pass)+strlen(topic)+strlen(will)>15000) {close(fd);return -1;}
    field(body,&n,"MQTT");body[n++]=4;
    body[n++]=0x26|(*user?0x80:0)|(*pass?0x40:0); /* clean session + retained QoS0 will */
    body[n++]=0;body[n++]=60;
    char client[256];snprintf(client,sizeof(client),"head-cam-%s",id);
    field(body,&n,client);field(body,&n,topic);field(body,&n,will);
    if(*user)field(body,&n,user);
    if(*pass)field(body,&n,pass);
    if(packet(fd,0x10,body,n) || response(fd,0x20)) {close(fd);return -1;}
    return fd;
}
int monitor_publish(int fd,const char *topic,const char *payload,int retain) {
    size_t n=0,size=strlen(topic)+strlen(payload)+2;
    if(strlen(topic)>65535 || size>1024*1024)return -1;
    unsigned char *body=malloc(size);if(!body)return -1;
    field(body,&n,topic);memcpy(body+n,payload,strlen(payload));
    int rc=packet(fd,retain?0x31:0x30,body,size);free(body);return rc;
}
int monitor_ping(int fd) {return packet(fd,0xc0,"",0) || response(fd,0xd0)?-1:0;}
void monitor_disconnect(int fd) {(void)packet(fd,0xe0,"",0);}

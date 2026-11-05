/* MQTT 3.1.1。presence 仍是 QoS 0；OTA 的五条主题用 QoS 1，订阅时要求 Broker 授予 QoS 1。 */
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
static int recv_full(int fd,void *data,size_t n) {
    unsigned char *p=data;
    while(n) {
        ssize_t r=recv(fd,p,n,0);
        if(r==0) return -1;
        if(r<0) { if(errno==EINTR) continue; return -1; }
        p+=r;n-=(size_t)r;
    }
    return 0;
}
static int read_remaining(int fd,size_t *out) {
    size_t value=0;int shift=0;
    for(int i=0;i<4;i++) {
        unsigned char digit;
        if(recv_full(fd,&digit,1)) return -1;
        value+=(size_t)(digit&127)<<shift;
        if(!(digit&128)) { *out=value; return value>1024*1024?-1:0; }
        shift+=7;
    }
    return -1;
}
int monitor_read(int fd,int timeout_ms,unsigned char *type,unsigned char **body,size_t *len) {
    *body=NULL;*len=0;
    struct pollfd p={fd,POLLIN,0};
    int rc=poll(&p,1,timeout_ms);
    if(rc==0 || (rc<0 && errno==EINTR)) return 1;
    if(rc<0 || (p.revents&(POLLERR|POLLNVAL|POLLHUP) && !(p.revents&POLLIN))) return -1;
    unsigned char kind;
    if(recv_full(fd,&kind,1)) return -1;
    size_t n=0;
    if(read_remaining(fd,&n)) return -1;
    unsigned char *buf=NULL;
    if(n) {
        buf=malloc(n);
        if(!buf || recv_full(fd,buf,n)) { free(buf); return -1; }
    }
    *type=kind;*body=buf;*len=n;return 0;
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
int monitor_subscribe(int fd,const char *downlink_req,const char *downlink_resp) {
    const char *topics[2]={downlink_req,downlink_resp};
    size_t n=2;
    for(int i=0;i<2;i++) {
        size_t len=strlen(topics[i]);
        if(!len || len>65535) return -1;
        n+=2+len+1;
    }
    unsigned char *body=malloc(n);if(!body)return -1;
    body[0]=0;body[1]=1;size_t at=2;
    for(int i=0;i<2;i++) {
        size_t len=strlen(topics[i]);
        body[at++]=(unsigned char)(len>>8);body[at++]=(unsigned char)len;
        memcpy(body+at,topics[i],len);at+=len;body[at++]=1;
    }
    int failed=packet(fd,0x82,body,n);free(body);
    if(failed)return -1;
    unsigned char type=0,*resp=NULL;size_t rn=0;
    int rd=monitor_read(fd,5000,&type,&resp,&rn);
    int bad=rd || type!=0x90 || rn!=4 || resp[0] || resp[1]!=1 || resp[2]!=1 || resp[3]!=1;
    free(resp);return bad?-1:0;
}
int monitor_publish_qos1(int fd,const char *topic,const char *payload,unsigned packet_id,int dup) {
    size_t tlen=strlen(topic),plen=strlen(payload),n=0,size=tlen+plen+4;
    if(!packet_id || packet_id>65535 || tlen>65535 || size>1024*1024) return -1;
    unsigned char *body=malloc(size);if(!body)return -1;
    field(body,&n,topic);body[n++]=(unsigned char)(packet_id>>8);body[n++]=(unsigned char)packet_id;
    memcpy(body+n,payload,plen);
    int rc=packet(fd,dup?0x3a:0x32,body,size);free(body);return rc;
}
int monitor_publish(int fd,const char *topic,const char *payload,int retain) {
    size_t n=0,size=strlen(topic)+strlen(payload)+2;
    if(strlen(topic)>65535 || size>1024*1024)return -1;
    unsigned char *body=malloc(size);if(!body)return -1;
    field(body,&n,topic);memcpy(body+n,payload,strlen(payload));
    int rc=packet(fd,retain?0x31:0x30,body,size);free(body);return rc;
}
int monitor_accept_publish(int fd,unsigned char header,unsigned char *body,size_t len,
                           char *topic,size_t topic_cap,const unsigned char **payload,size_t *payload_len) {
    unsigned qos=(header>>1)&3;
    if(len<2)return -1;
    size_t tlen=((size_t)body[0]<<8)|body[1],off=2+tlen;
    if(off>len)return -1;
    if(qos==1) {
        if(off+2>len)return -1;
        unsigned id=((unsigned)body[off]<<8)|body[off+1];off+=2;
        unsigned char ack[2]={(unsigned char)(id>>8),(unsigned char)id};
        if(packet(fd,0x40,ack,2))return -1;
    } else if(qos) return -1;
    if(tlen>=topic_cap) { topic[0]=0;*payload=body+off;*payload_len=0;return 0; }
    memcpy(topic,body+2,tlen);topic[tlen]=0;
    *payload=body+off;*payload_len=len-off;return 0;
}
int monitor_ping(int fd) {return packet(fd,0xc0,"",0);}
void monitor_disconnect(int fd) {(void)packet(fd,0xe0,"",0);}

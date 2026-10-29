#include "service.h"
#include <ctype.h>
#include <errno.h>
#include <lz4frame.h>
#include <zstd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int head_parse(const char *name, head_identity *id, char *error) {
    const char *p = NULL;
    size_t n = strlen(name);
    if (n < 6 || strcasecmp(name+n-5, ".mcap")) goto invalid;
    /* ossutil's metadata syntax uses # as a separator. Reject ambiguous headers. */
    for (size_t i=0;i<n;i++) if ((unsigned char)name[i] < 32 || (unsigned char)name[i] > 126 || name[i]=='#' || name[i]=='/' || name[i]=='\\') goto invalid;
    if (!strncmp(name, "SSIEGO_Cam_", 11)) p = name+11;
    else if (!strncmp(name, "SSI_IRIS2_EGO_", 14)) {
        p = strchr(name+14, '_');
        if (!p || p == name+14) goto invalid;
        p++;
    } else goto invalid;
    const char *end = strchr(p, '_');
    if (!end || end==p || (size_t)(end-p)>=sizeof(id->device)) goto invalid;
    for (const char *c=p;c<end;c++) if (!isalnum((unsigned char)*c)) goto invalid;
    memcpy(id->device,p,(size_t)(end-p)); id->device[end-p]=0;
    p = end+1;
    if (strlen(p)<10 || p[8]!='_') goto invalid;
    for (int i=0;i<8;i++) if (!isdigit((unsigned char)p[i])) goto invalid;
    int y=(p[0]-'0')*1000+(p[1]-'0')*100+(p[2]-'0')*10+p[3]-'0';
    int m=(p[4]-'0')*10+p[5]-'0', d=(p[6]-'0')*10+p[7]-'0';
    static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if (y<1 || m<1 || m>12 || d<1 || d>days[m-1]+(m==2 && y%4==0 && (y%100!=0 || y%400==0))) goto invalid;
    snprintf(id->date,sizeof(id->date),"%.4s-%.2s-%.2s",p,p+4,p+6);
    memcpy(id->session,p+2,6); id->session[6]=0;
    return 0;
invalid:
    snprintf(error,ERROR_SIZE,"Unsupported head_cam filename or invalid capture date (expected SSIEGO_Cam_<device>_<YYYYMMDD>_* or SSI_IRIS2_EGO_<variant>_<device>_<YYYYMMDD>_*)");
    return -1;
}

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint64_t u64(const unsigned char *p) { return (uint64_t)u32(p) | ((uint64_t)u32(p+4)<<32); }
static uint32_t crc_table[256];
static pthread_once_t crc_once=PTHREAD_ONCE_INIT;
static void init_crc(void) {
    for(unsigned i=0;i<256;i++) {
        uint32_t crc=i;
        for(int j=0;j<8;j++) crc=(crc>>1)^((0U-(crc&1U))&0xedb88320U);
        crc_table[i]=crc;
    }
}
static uint32_t crc32_bytes(const unsigned char *p, size_t n) {
    pthread_once(&crc_once,init_crc);
    uint32_t crc=~0U;
    while(n--) crc=crc_table[(crc^*p++)&255U]^(crc>>8);
    return ~crc;
}
typedef struct { uint64_t first,last,count; } times;
static int string_field(const unsigned char *p,size_t n,size_t *offset) {
    if (*offset>n || n-*offset<4) return -1;
    uint32_t len=u32(p+*offset); *offset+=4;
    if (len>n-*offset) return -1;
    *offset+=len; return 0;
}
static int records(const unsigned char *, size_t, times *, int, char *);
static int chunk(const unsigned char *p,size_t n,times *t,char *error) {
    if(n<40) return -1;
    uint64_t raw_size=u64(p+16); uint32_t crc=u32(p+24), clen=u32(p+28);
    if(clen>n-40 || raw_size>64U*1024U*1024U) {
        snprintf(error,ERROR_SIZE,"MCAP chunk exceeds 64 MiB decompression limit or has invalid length"); return -1;
    }
    size_t at=32+(size_t)clen;
    uint64_t packed=u64(p+at); at+=8;
    if(packed>n-at) return -1;
    const unsigned char *data=p+at; unsigned char *raw=NULL;
    int rc=-1;
    if(clen==0) { if(packed!=raw_size) return -1; }
    else {
        raw=malloc((size_t)raw_size+1); if(!raw) return -1;
        if(clen==4 && !memcmp(p+32,"zstd",4)) {
            size_t z=ZSTD_decompress(raw,(size_t)raw_size,data,(size_t)packed);
            if(ZSTD_isError(z) || z!=raw_size) goto done;
        } else if(clen==3 && !memcmp(p+32,"lz4",3)) {
            LZ4F_dctx *ctx=NULL;
            if(LZ4F_isError(LZ4F_createDecompressionContext(&ctx,LZ4F_VERSION))) goto done;
            size_t src=0,dst=0,hint=1;
            while(hint && src<packed) {
                size_t in=(size_t)packed-src,out=(size_t)raw_size-dst;
                hint=LZ4F_decompress(ctx,raw+dst,&out,data+src,&in,NULL);
                if(LZ4F_isError(hint) || (!in && !out)) break;
                src+=in; dst+=out;
            }
            LZ4F_freeDecompressionContext(ctx);
            if(hint || src!=packed || dst!=raw_size) goto done;
        } else { snprintf(error,ERROR_SIZE,"Unsupported MCAP compression"); goto done; }
        data=raw;
    }
    if(crc && crc32_bytes(data,(size_t)raw_size)!=crc) { snprintf(error,ERROR_SIZE,"MCAP chunk CRC mismatch"); goto done; }
    rc=records(data,(size_t)raw_size,t,1,error);
done:
    free(raw); return rc;
}
static int records(const unsigned char *data,size_t size,times *t,int nested,char *error) {
    size_t pos=0; int header=0,ended=0,footer=0;
    while(pos<size) {
        if(size-pos<9) return -1;
        size_t record_start=pos;
        unsigned op=data[pos]; uint64_t len=u64(data+pos+1); pos+=9;
        if(!op || len>size-pos) return -1;
        size_t n=(size_t)len; const unsigned char *p=data+pos;
        if(!nested && !header && op!=1) return -1;
        if(nested && op!=3 && op!=4 && op!=5) return -1;
        if(ended && (op==5 || op==6 || op==15 || op==1)) return -1;
        if(op==1) {
            size_t off=0;
            if(header || string_field(p,n,&off) || string_field(p,n,&off)) return -1;
            header=1;
        } else if(op==2) {
            if(!ended || n!=20 || pos+n!=size) return -1;
            /* Offsets are absolute, including the initial eight magic bytes. */
            uint64_t summary=u64(p), offsets=u64(p+8);
            if(summary && (summary<8 || summary>record_start+8)) return -1;
            if(offsets && (!summary || offsets<summary || offsets>record_start+8)) return -1;
            uint32_t crc=u32(p+16);
            size_t start=summary ? (size_t)summary-8 : record_start;
            if(crc && crc32_bytes(data+start,pos+16-start)!=crc) return -1;
            footer=1;
        } else if(op==3) {
            size_t off=2;
            if(n<2 || string_field(p,n,&off) || string_field(p,n,&off) || string_field(p,n,&off)) return -1;
        } else if(op==4) {
            size_t off=4;
            if(n<4 || string_field(p,n,&off) || string_field(p,n,&off) || string_field(p,n,&off)) return -1;
        } else if(op==5) {
            if(n<22) return -1;
            uint64_t time=u64(p+6);
            if(!t->count || time<t->first) t->first=time;
            if(!t->count || time>t->last) t->last=time;
            t->count++;
        } else if(op==6) {
            if(chunk(p,n,t,error)) return -1;
        } else if(op==15) {
            if(n!=4) return -1;
            uint32_t crc=u32(p);
            /* Data CRC includes leading magic; validated by the caller. */
            (void)crc; ended=1;
        }
        pos+=n;
    }
    return nested || (header && ended && footer) ? 0 : -1;
}
static int read_at(int fd,void *data,size_t n,uint64_t offset) {
    unsigned char *p=data;
    while(n) {
        ssize_t got=pread(fd,p,n,(off_t)offset);
        if(got<0 && errno==EINTR) continue;
        if(got<=0) return -1;
        p+=got; n-=(size_t)got; offset+=(uint64_t)got;
    }
    return 0;
}
static int file_crc(int fd,uint64_t start,uint64_t end,uint32_t expected) {
    unsigned char data[65536]; uint32_t crc=~0U;
    pthread_once(&crc_once,init_crc);
    while(start<end) {
        size_t n=end-start>sizeof(data)?sizeof(data):(size_t)(end-start);
        if(read_at(fd,data,n,start)) return -1;
        for(size_t i=0;i<n;i++) crc=crc_table[(crc^data[i])&255U]^(crc>>8);
        start+=n;
    }
    return ~crc==expected?0:-1;
}
int mcap_duration(int fd,double *duration,char *error) {
    static const unsigned char magic[]={0x89,'M','C','A','P','0','\r','\n'};
    struct stat st; unsigned char small[32]; times t={0};
    if(fstat(fd,&st) || st.st_size<54) goto bad;
    uint64_t size=(uint64_t)st.st_size,pos=8;
    if(read_at(fd,small,8,0) || memcmp(small,magic,8) || read_at(fd,small,8,size-8) || memcmp(small,magic,8)) goto bad;
    int header=0,ended=0,footer=0;
    while(pos<size-8) {
        uint64_t record_start=pos;
        if(size-8-pos<9 || read_at(fd,small,9,pos)) goto bad;
        unsigned op=small[0]; uint64_t length=u64(small+1); pos+=9;
        if(!op || length>size-8-pos || (!header && op!=1) || (ended && (op==1 || op==5 || op==6 || op==15))) goto bad;
        if(op==5) {
            if(length<22 || read_at(fd,small,22,pos)) goto bad;
            uint64_t time=u64(small+6);
            if(!t.count || time<t.first) t.first=time;
            if(!t.count || time>t.last) t.last=time;
            t.count++;
        } else if(op==1 || op==3 || op==4 || op==6) {
            if(length>64U*1024U*1024U) { snprintf(error,ERROR_SIZE,"MCAP record exceeds 64 MiB parser memory limit"); goto bad; }
            unsigned char *record=malloc((size_t)length+9); if(!record) goto bad;
            record[0]=(unsigned char)op;
            for(int i=0;i<8;i++) record[1+i]=(unsigned char)(length>>(8*i));
            int rc=read_at(fd,record+9,(size_t)length,pos);
            if(!rc && op==1) {
                size_t at=0;
                rc=header || string_field(record+9,(size_t)length,&at) || string_field(record+9,(size_t)length,&at); header=1;
            } else if(!rc && op==6) rc=chunk(record+9,(size_t)length,&t,error);
            else if(!rc) rc=records(record,(size_t)length+9,&t,1,error);
            free(record); if(rc) goto bad;
        } else if(op==15) {
            if(length!=4 || read_at(fd,small,4,pos)) goto bad;
            uint32_t crc=u32(small);
            if(crc && file_crc(fd,0,record_start,crc)) goto bad;
            ended=1;
        } else if(op==2) {
            if(!ended || length!=20 || pos+length!=size-8 || read_at(fd,small,20,pos)) goto bad;
            uint64_t summary=u64(small),offsets=u64(small+8); uint32_t crc=u32(small+16);
            if(summary && (summary<8 || summary>record_start)) goto bad;
            if(offsets && (!summary || offsets<summary || offsets>record_start)) goto bad;
            if(crc && file_crc(fd,summary?summary:record_start,pos+16,crc)) goto bad;
            footer=1;
        }
        pos+=length;
    }
    if(!header || !ended || !footer || !t.count) goto bad;
    *duration=round(((double)(t.last-t.first)/1e9)*1000.0)/1000.0;
    return 0;
bad:
    if(!error[0]) snprintf(error,ERROR_SIZE,"Invalid/truncated MCAP or no message timestamps; automatic repair is disabled");
    return -1;
}

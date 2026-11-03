/* 轻量只读扫描：有界遍历，不解析 MCAP 内容，不与上传共享状态。 */
#include "monitor.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

typedef struct { unsigned count,today_count,visited; uint64_t bytes,hbytes[24];unsigned hours[24];char today[9];time_t deadline; } scan;
static int walk(const char *root,unsigned depth,scan *s) {
    if(depth>32 || time(NULL)>s->deadline || monitor_stop)return -1;
    DIR *d=opendir(root);if(!d)return -1;
    struct dirent *e;int rc=0;
    while((e=readdir(d))) {
        if(e->d_name[0]=='.')continue;
        if(++s->visited>100000 || time(NULL)>s->deadline || monitor_stop) {rc=-1;break;}
        char path[PATH_MAX];struct stat st;
        if(path_join(path,sizeof(path),root,e->d_name) || lstat(path,&st)) {rc=-1;break;}
        if(S_ISDIR(st.st_mode)) {if(walk(path,depth+1,s)){rc=-1;break;}continue;}
        size_t len=strlen(e->d_name);
        if(!S_ISREG(st.st_mode) || len<5 || strcasecmp(e->d_name+len-5,".mcap"))continue;
        s->count++;s->bytes+=(uint64_t)st.st_size;
        /* 与 X5 一致从文件名的 YYYYMMDD_HHMMSS 段聚合，未知命名只计总数。 */
        const char *p=e->d_name;
        while((p=strchr(p,'_'))) {
            p++;
            if(strlen(p)>=15 && !strncmp(p,s->today,8) && p[8]=='_' && strspn(p+9,"0123456789")>=6) {
                int h=(p[9]-'0')*10+p[10]-'0';
                if(h<24) {s->today_count++;s->hours[h]++;s->hbytes[h]+=(uint64_t)st.st_size;}break;
            }
        }
    }
    closedir(d);return rc;
}
cJSON *monitor_scan(const cJSON *config) {
    scan s={0};time_t now=time(NULL);struct tm tm;localtime_r(&now,&tm);
    strftime(s.today,sizeof(s.today),"%Y%m%d",&tm);s.deadline=now+30;
    cJSON *out=cJSON_CreateObject();char ts[32];timestamp(ts);js(out,"scan_ts",ts);
    const char *root=str(config,"recording_root"),*storage_path=str(config,"storage_path");
    if(!*storage_path)storage_path=root;
    if(!*root || walk(root,0,&s)) {
        js(out,"error","Recording directory unavailable or scan budget exceeded");return out;
    }
    js(out,"error","");cJSON *r=cJSON_AddObjectToObject(out,"recordings");
    json_number(r,"file_count",s.count);json_number(r,"today_file_count",s.today_count);
    json_number(r,"total_size_bytes",(double)s.bytes);
    char display[64];snprintf(display,sizeof(display),"%.2f GB",(double)s.bytes/1073741824.0);js(r,"total_size_display",display);
    cJSON *hourly=cJSON_AddObjectToObject(r,"hourly");
    const char *columns[]={"hour","count","bytes"};cJSON_AddItemToObject(hourly,"columns",cJSON_CreateStringArray(columns,3));
    cJSON *rows=cJSON_AddArrayToObject(hourly,"rows");
    for(int h=0;h<24;h++) if(s.hours[h]) {
        cJSON *row=cJSON_CreateArray();char hour[16];snprintf(hour,sizeof(hour),"%s%02d",s.today,h);
        cJSON_AddItemToArray(row,cJSON_CreateString(hour));cJSON_AddItemToArray(row,cJSON_CreateNumber(s.hours[h]));
        cJSON_AddItemToArray(row,cJSON_CreateNumber((double)s.hbytes[h]));cJSON_AddItemToArray(rows,row);
    }
    cJSON *storage=cJSON_AddObjectToObject(out,"storage");js(storage,"path",storage_path);
    struct statvfs fs;
    if(!statvfs(storage_path,&fs)) {
        double free_gb=(double)fs.f_bavail*fs.f_frsize/1073741824.0;
        jb(storage,"valid",1);json_number(storage,"free_gb",free_gb);
        json_number(storage,"used_percent",fs.f_blocks?100.0*(fs.f_blocks-fs.f_bfree)/fs.f_blocks:0);
        js(out,"alert",free_gb<num(config,"critical_free_gb")?"critical":free_gb<num(config,"warning_free_gb")?"warning":"none");
    } else {jb(storage,"valid",0);js(out,"alert","unknown");}
    cJSON *battery=cJSON_AddObjectToObject(out,"battery");
    FILE *f=*str(config,"battery_capacity_path")?fopen(str(config,"battery_capacity_path"),"r"):NULL;int soc=-1;
    if(f){if(fscanf(f,"%d",&soc)!=1)soc=-1;fclose(f);}
    int valid=soc>=0 && soc<=100;jb(battery,"valid",valid);
    cJSON_AddItemToObject(battery,"soc",valid?cJSON_CreateNumber(soc):cJSON_CreateNull());return out;
}

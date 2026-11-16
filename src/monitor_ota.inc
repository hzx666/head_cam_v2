/* Included in monitor_main.c: session JSON is owned exclusively by its MQTT loop. */
static pthread_mutex_t ota_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ota_changed = PTHREAD_COND_INITIALIZER;
static cJSON *ota_event;
static cJSON *ota_new_package;
static pthread_t ota_thread;
static _Atomic int ota_running;
static _Atomic int ota_verified;
static int ota_joinable, ota_cancelled, ota_event_saved;
static cJSON *device_cache;
static time_t verification_deadline;
static char boot_identifier[80];
static char download_request[40];
static cJSON *download_envelope;
static time_t download_sent;

static void update_device_cache(const cJSON *value)
{
    pthread_mutex_lock(&ota_mutex);
    cJSON_Delete(device_cache);
    device_cache = value ? cJSON_Duplicate(value, 1) : NULL;
    pthread_mutex_unlock(&ota_mutex);
}
static cJSON *cached_device(void)
{
    pthread_mutex_lock(&ota_mutex);
    cJSON *value = device_cache ? cJSON_Duplicate(device_cache, 1) : NULL;
    pthread_mutex_unlock(&ota_mutex);
    return value;
}
static int worker_cancelled(void)
{
    pthread_mutex_lock(&ota_mutex);
    int result = ota_cancelled || monitor_stop;
    pthread_mutex_unlock(&ota_mutex);
    return result;
}
static int post_ota(const char *state, const char *stage, int progress,
                    const char *error_code, const char *message)
{
    cJSON *event = cJSON_CreateObject();
    js(event, "upgradeStatus", state); js(event, "stage", stage);
    json_number(event, "progress", progress);
    if (error_code) {
        cJSON *error = cJSON_AddObjectToObject(event, "error");
        js(error, "code", error_code); js(error, "message", message);
    }
    pthread_mutex_lock(&ota_mutex);
    while (ota_event && !monitor_stop) pthread_cond_wait(&ota_changed, &ota_mutex);
    if (monitor_stop) { pthread_mutex_unlock(&ota_mutex); cJSON_Delete(event); return -1; }
    ota_event = event; ota_event_saved = 0;
    while (ota_event && !monitor_stop) pthread_cond_wait(&ota_changed, &ota_mutex);
    int result = ota_event_saved ? 0 : -1;
    pthread_mutex_unlock(&ota_mutex);
    return result;
}
static int check_device_ready(void)
{
    cJSON *value = ego_query();
    update_device_cache(value);
    const char *error_code = NULL, *message = NULL;
    if (!value || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value, "stateValid"))) {
        error_code = "SDK_UNAVAILABLE"; message = "无法查询设备状态";
    } else if (num(value, "state") != 0) {
        error_code = "DEVICE_BUSY";
        message = num(value, "state") == 1 ? "设备正在录制" : "设备正在使用";
    } else if (num(value, "charge") != 1) {
        double battery = num(value, "battery");
        /* This bench unit runs on an external supply without a battery. */
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value, "deviceValid")) &&
            !strcmp(str(value, "deviceCode"), "8L6AR120005")) {
            fprintf(stderr, "ota test device treated as charging\n");
        } else if (battery < 0 || battery > 100) {
            error_code = "BATTERY_UNKNOWN"; message = "无法获取有效电量";
        } else if (battery < 20) {
            error_code = "LOW_BATTERY"; message = "未充电时升级要求电量至少20%";
        }
    }
    cJSON_Delete(value);
    if (error_code) { post_ota("failed", "completed", -1, error_code, message); return -1; }
    return 0;
}
typedef struct {
    uint64_t size;
    time_t last_progress;
    int progress;
    int sdk_status;
    int sdk_result;
} ota_progress;
static int download_progress(uint64_t bytes, void *context)
{
    ota_progress *progress = context;
    if (worker_cancelled()) return -1;
    int percent = (int)(bytes * 50 / progress->size);
    time_t now = time(NULL);
    if (percent != progress->progress && now - progress->last_progress >= 5) {
        progress->progress = percent; progress->last_progress = now;
        if (post_ota("upgrading", "downloading", percent, NULL, NULL)) return -1;
    }
    return 0;
}
static void sdk_progress(const cJSON *event, void *context)
{
    ota_progress *progress = context;
    int status = (int)num(event, "status");
    progress->sdk_status = status;
    progress->sdk_result = (int)num(event, "result");
    if(status==4) {post_ota("upgrading","rebooting",95,NULL,NULL);return;}
    int raw = (int)num(event, "progress");
    if (raw < 0 || raw > 100) return;
    int percent = 55 + raw * 40 / 100;
    time_t now = time(NULL);
    if (percent > progress->progress && now - progress->last_progress >= 5) {
        progress->progress = percent; progress->last_progress = now;
        post_ota("upgrading", "installing", percent, NULL, NULL);
    }
}
static cJSON *request_new_package(void)
{
    if (post_ota("upgrading", "downloading", -1, NULL, NULL)) return NULL;
    pthread_mutex_lock(&ota_mutex);
    cJSON_Delete(ota_new_package);
    ota_new_package = cJSON_CreateObject(); /* empty object means renewal requested */
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 120;
    while (ota_new_package && !*str(ota_new_package, "downloadUrl") && !ota_cancelled && !monitor_stop) {
        /* Cancellation is inspected under this mutex, without recursive locking. */
        if (pthread_cond_timedwait(&ota_changed, &ota_mutex, &deadline)) break;
    }
    cJSON *value = ota_new_package; ota_new_package = NULL;
    pthread_mutex_unlock(&ota_mutex);
    if (!value || !*str(value, "downloadUrl")) { cJSON_Delete(value); return NULL; }
    return value;
}
static void *upgrade_worker(void *context)
{
    cJSON *job = context;
    cJSON *package = cJSON_GetObjectItemCaseSensitive(job, "package");
    int operation = -1, result = -1;
    char directory[] = "/tmp/head-cam-ota-XXXXXX", path[PATH_MAX] = "";
    ota_progress progress = {.size=(uint64_t)num(package,"fileSize")};
    if (check_device_ready()) goto done;
    operation = operation_lock(1);
    if (operation < 0) {
        post_ota("failed", "completed", 0, "DEVICE_BUSY", "设备正在上传或更新服务"); goto done;
    }
    struct statvfs space;
    if (!progress.size || progress.size > UINT32_MAX || statvfs("/tmp", &space) ||
        (uint64_t)space.f_bavail * space.f_frsize < progress.size + 32ULL*1024*1024) {
        post_ota("failed", "completed", 0, "INSUFFICIENT_SPACE", "固件下载空间不足"); goto done;
    }
    struct sysinfo memory;
    if (sysinfo(&memory) || (uint64_t)memory.freeram * memory.mem_unit < progress.size + 32ULL*1024*1024) {
        post_ota("failed", "completed", 0, "INSUFFICIENT_MEMORY", "固件下载可用内存不足"); goto done;
    }
    if (!mkdtemp(directory) || path_join(path, sizeof(path), directory, "firmware.bin")) {
        post_ota("failed", "completed", 0, "STORAGE_ERROR", "无法创建固件下载目录"); goto done;
    }
    if (post_ota("upgrading", "downloading", 0, NULL, NULL)) goto done;
    result = ota_download(str(package,"downloadUrl"), path, progress.size, download_progress, &progress);
    if (result == 403 && !worker_cancelled()) {
        cJSON *renewal = request_new_package();
        if (renewal) {
            result = ota_download(str(renewal,"downloadUrl"), path, progress.size, download_progress, &progress);
            cJSON_Delete(renewal);
        }
    }
    if (worker_cancelled()) { post_ota("cancelled", "completed", -1, NULL, NULL); goto done; }
    if (result) { post_ota("failed", "completed", -1, "DOWNLOAD_FAILED", "固件下载失败或下载地址续签失败"); goto done; }
    if (post_ota("upgrading", "verifying", 50, NULL, NULL)) goto done;
    int file = open(path, O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    char digest[65]; struct stat information;
    result = file < 0 || fstat(file, &information) || (uint64_t)information.st_size != progress.size ||
             sha256_fd(file, digest) || strcmp(digest, str(package,"sha256"));
    if (file >= 0) close(file);
    if (result) { post_ota("failed", "completed", -1, "CHECKSUM_MISMATCH", "固件大小或SHA-256校验失败"); goto done; }
    if (check_device_ready()) goto done;
    if (worker_cancelled()) { post_ota("cancelled", "completed", -1, NULL, NULL); goto done; }
    if (post_ota("upgrading", "installing", 55, NULL, NULL)) goto done;
    result = ego_upgrade(path, sdk_progress, &progress);
    if (result || progress.sdk_status != 4) {
        char message[100]; snprintf(message,sizeof(message),"SDK升级失败，状态=%d，错误码=%d",progress.sdk_status,progress.sdk_result);
        post_ota("failed", "completed", -1, "SDK_UPGRADE_FAILED", message); goto done;
    }
done:
    if (*path) unlink(path);
    if (*path) rmdir(directory);
    if(!result && progress.sdk_status==4) while(!monitor_stop && !ota_verified)sleep(1);
    if (operation >= 0) close(operation);
    cJSON_Delete(job);
    pthread_mutex_lock(&ota_mutex); ota_running = 0; pthread_cond_broadcast(&ota_changed); pthread_mutex_unlock(&ota_mutex);
    return NULL;
}
static void ota_after_reply(const char *payload)
{
    cJSON *reply = cJSON_Parse(payload), *job = task_object();
    if (reply && job && !strcmp(str(reply,"method"),"ota.start") &&
        num(reply,"code") == 0 && !strcmp(str(reply,"requestId"),str(job,"acceptedRequestId")) &&
        !strcmp(str(job,"upgradeStatus"),"pending") && !ota_running) {
        if (ota_joinable) { pthread_join(ota_thread,NULL); ota_joinable=0; }
        cJSON *copy = cJSON_Duplicate(job,1);
        ota_cancelled=0; ota_verified=0; ota_running=1;
        if (!copy || pthread_create(&ota_thread,NULL,upgrade_worker,copy)) {
            ota_running=0; cJSON_Delete(copy);
            cJSON_DeleteItemFromObject(job,"error");
            cJSON *error=cJSON_AddObjectToObject(job,"error");
            js(error,"code","TEMPORARY_UNAVAILABLE");js(error,"message","无法启动升级任务");
            char now[40];cst_timestamp(now);bump_status(job,"failed","completed",now);
        } else ota_joinable=1;
    }
    cJSON_Delete(reply);
}
static int ota_tick(const char *topic,const cJSON *config)
{
    pthread_mutex_lock(&ota_mutex);
    if (ota_event) {
        cJSON *job=task_object();
        const char *state=str(ota_event,"upgradeStatus"), *stage=str(ota_event,"stage");
        int cancelled_before_install = ota_cancelled && !strcmp(stage,"installing") &&
                                       strcmp(str(job,"stage"),"installing");
        if (cancelled_before_install) {state="cancelled";stage="completed";}
        char now[40];cst_timestamp(now);
        if (!strcmp(state,"upgrading") && !*str(job,"startedAt")) js(job,"startedAt",now);
        if (num(ota_event,"progress")>=0 && num(ota_event,"progress")>=num(job,"progress")) json_number(job,"progress",num(ota_event,"progress"));
        cJSON_DeleteItemFromObject(job,"error");
        cJSON *error=cJSON_GetObjectItemCaseSensitive(ota_event,"error");
        cJSON_AddItemToObject(job,"error",error?cJSON_Duplicate(error,1):cJSON_CreateNull());
        if (!strcmp(stage,"installing")) js(job,"installBootId",boot_identifier);
        if (!strcmp(stage,"rebooting")) {
            jb(job,"sdkSucceeded",1);
            if (!verification_deadline) verification_deadline=time(NULL)+180;
            ota_fast_query=1;
        }
        if (!strcmp(stage,"completed")) {verification_deadline=0;ota_fast_query=0;}
        bump_status(job,state,stage,!strcmp(stage,"completed")?now:NULL);
        ota_event_saved = !json_save(session_path,session) && !cancelled_before_install;
        fprintf(stderr,"ota stage=%s status=%s progress=%.0f\n",stage,state,num(job,"progress"));
        cJSON_Delete(ota_event);ota_event=NULL;pthread_cond_broadcast(&ota_changed);
    }
    if (ota_new_package && !*str(ota_new_package,"downloadUrl") && !download_envelope) {
        if (!uuid_v4(download_request)) {
            download_envelope=rpc_envelope(config,"rpc_request",download_request,"ota.getDownloadUrl");
            json_number(download_envelope,"expiresAt",(double)(unix_ms()+120000));
            cJSON *data=cJSON_AddObjectToObject(download_envelope,"data"),*job=task_object();
            js(data,"taskId",str(job,"taskId"));js(data,"upgradeId",str(job,"upgradeId"));
            js(data,"firmwareId",str(cJSON_GetObjectItemCaseSensitive(job,"package"),"firmwareId"));
            download_sent=0;
        }
    }
    int renewal_waiting=ota_new_package!=NULL;
    pthread_mutex_unlock(&ota_mutex);
    if (!renewal_waiting) {cJSON_Delete(download_envelope);download_envelope=NULL;download_request[0]=0;}
    if (topic && download_envelope && time(NULL)-download_sent>=10) {
        if (!enqueue_json(topic,cJSON_Duplicate(download_envelope,1))) download_sent=time(NULL);
    }
    if (verification_deadline) {
        cJSON *job=task_object(),*value=cached_device();
        int matched=value && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value,"deviceValid")) &&
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value,"stateValid")) && num(value,"state")==0 &&
            *str(job,"toVersion") && !strcmp(str(value,"version"),str(job,"toVersion"));
        if (matched || time(NULL)>=verification_deadline) {
            char now[40];cst_timestamp(now);
            if (matched) {
                json_number(job,"progress",100);
                cJSON_DeleteItemFromObject(session,"firmware");
                cJSON *firmware=cJSON_AddObjectToObject(session,"firmware");
                js(firmware,"version",str(value,"version"));js(firmware,"upgradeSource","platform");js(firmware,"upgradeId",str(job,"upgradeId"));
                char version[32];snprintf(version,sizeof(version),"%llu",strtoull(str(session,"propertyVersion"),NULL,10)+1);
                js(session,"propertyVersion",version);
                js(job,"propertyVersion",version);cJSON_AddItemToObject(job,"firmware",cJSON_Duplicate(firmware,1));
                bump_status(job,"success","completed",now);
            }
            else {
                cJSON_DeleteItemFromObject(job,"error");cJSON *error=cJSON_AddObjectToObject(job,"error");
                js(error,"code","VERSION_CHECK_TIMEOUT");js(error,"message","重启后3分钟内未确认目标版本");
                bump_status(job,"failed","completed",now);
            }
            verification_deadline=0;
            ota_fast_query=0;
            ota_verified=1;
        }
        cJSON_Delete(value);
    }
    return 0;
}

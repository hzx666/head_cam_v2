#include "ego_sdk_api.h"
#include <cjson/cJSON.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int terminal;

static void emit(cJSON *value)
{
    char *text = cJSON_PrintUnformatted(value);
    if (text) { puts(text); fflush(stdout); free(text); }
    cJSON_Delete(value);
}

static void upgrade_event(int status, int progress, int result, const char *message, void *context)
{
    (void)context;
    pthread_mutex_lock(&output_mutex);
    cJSON *event = cJSON_CreateObject();
    cJSON_AddNumberToObject(event, "status", status);
    cJSON_AddNumberToObject(event, "progress", progress);
    cJSON_AddNumberToObject(event, "result", result);
    cJSON_AddStringToObject(event, "message", message ? message : "");
    emit(event);
    if (status == EGO_UPG_STATUS_SUCCEED || status == EGO_UPG_STATUS_FAILED || status == EGO_UPG_STATUS_CANCELLED) {
        terminal = status;
        pthread_cond_signal(&changed);
    }
    pthread_mutex_unlock(&output_mutex);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 64;
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "SDK load failed: %s\n", dlerror()); return 1; }
    int (*initialize)(uint32_t);
    void (*deinitialize)(void);
    int (*query_device)(EgoDeviceInfo *);
    int (*query_state)(EgoStateInfo *);
    int (*register_callback)(EgoUpgradeCallback, void *);
    int (*upgrade_file)(const char *);
#define LOAD(function, name) do { \
    void *symbol = dlsym(library, name); \
    if (!symbol) { dlclose(library); return 1; } \
    memcpy(&function, &symbol, sizeof(function)); \
} while (0)
    LOAD(initialize, "EgoUpgradeInit");
    LOAD(deinitialize, "EgoUpgradeDeinit");
    LOAD(query_device, "EgoQueryDeviceInfo");
    LOAD(query_state, "EgoQueryStateInfo");
    LOAD(register_callback, "EgoRegisterUpgradeCallback");
    LOAD(upgrade_file, "EgoExecuteUpgradeFile");
#undef LOAD
    /* Match the vendor example for synchronous firmware-transfer requests. */
    int upgrading = argc == 4 && !strcmp(argv[2], "upgrade");
    if (initialize(upgrading ? 15000 : 3000)) { dlclose(library); return 1; }
    int result = 1;
    if (!strcmp(argv[2], "query")) {
        EgoDeviceInfo device = {0};
        EgoStateInfo state = {0};
        int device_result = -1, state_result = -1;
        for (int attempt = 0; attempt < 3; attempt++) {
            device_result = query_device(&device);
            state_result = query_state(&state);
            if (!device_result && !state_result) break;
            sleep(1);
        }
        device.deviceCode[sizeof(device.deviceCode)-1] = 0;
        device.version[sizeof(device.version)-1] = 0;
        device.hardware[sizeof(device.hardware)-1] = 0;
        cJSON *value = cJSON_CreateObject();
        cJSON_AddBoolToObject(value, "deviceValid", !device_result);
        cJSON_AddBoolToObject(value, "stateValid", !state_result);
        cJSON_AddStringToObject(value, "deviceCode", device.deviceCode);
        cJSON_AddStringToObject(value, "version", device.version);
        cJSON_AddStringToObject(value, "hardware", device.hardware);
        cJSON_AddNumberToObject(value, "state", state.state.state);
        cJSON_AddNumberToObject(value, "battery", (signed char)state.battery.batteryLevel);
        cJSON_AddNumberToObject(value, "charge", (unsigned char)state.battery.charge);
        emit(value);
        result = device_result || state_result;
    } else if (upgrading) {
        if (!register_callback(upgrade_event, NULL) && !upgrade_file(argv[3])) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 900;
            pthread_mutex_lock(&output_mutex);
            while (!terminal) {
                if (pthread_cond_timedwait(&changed, &output_mutex, &deadline)) break;
            }
            result = terminal == EGO_UPG_STATUS_SUCCEED ? 0 : 1;
            pthread_mutex_unlock(&output_mutex);
        }
    }
    deinitialize();
    dlclose(library);
    return result;
}

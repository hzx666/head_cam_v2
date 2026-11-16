#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "ego_sdk_api.h"

/* Read-only device compatibility check. No upgrade entry points are loaded. */
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s /absolute/path/libegosdk.so\n", argv[0]);
        return 2;
    }
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "SDK load failed: %s\n", dlerror());
        return 1;
    }
    int (*initialize)(uint32_t);
    void (*deinitialize)(void);
    int (*query_device)(EgoDeviceInfo *);
    int (*query_state)(EgoStateInfo *);
#define LOAD(function, name) do { \
    void *symbol = dlsym(library, name); \
    if (!symbol) { fprintf(stderr, "Missing SDK symbol: %s\n", name); dlclose(library); return 1; } \
    memcpy(&function, &symbol, sizeof(function)); \
} while (0)
    LOAD(initialize, "EgoUpgradeInit");
    LOAD(deinitialize, "EgoUpgradeDeinit");
    LOAD(query_device, "EgoQueryDeviceInfo");
    LOAD(query_state, "EgoQueryStateInfo");
#undef LOAD
    int result = initialize(15000);
    if (result != 0) {
        fprintf(stderr, "SDK initialization failed: %d\n", result);
        dlclose(library);
        return 1;
    }
    EgoDeviceInfo device = {0};
    EgoStateInfo state = {0};
    int device_result = query_device(&device);
    int state_result = query_state(&state);
    printf("device_result=%d code=%.*s version=%.*s hardware=%.*s\n",
           device_result, (int)sizeof(device.deviceCode), device.deviceCode,
           (int)sizeof(device.version), device.version,
           (int)sizeof(device.hardware), device.hardware);
    printf("state_result=%d state=%d battery=%d charge=%d detail=%.*s\n",
           state_result, state.state.state,
           (int)(signed char)state.battery.batteryLevel,
           (int)(unsigned char)state.battery.charge,
           (int)sizeof(state.state.detail), state.state.detail);
    deinitialize();
    dlclose(library);
    return device_result != 0 || state_result != 0;
}

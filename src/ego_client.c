#include "ota.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef EGO_EMBEDDED
extern const unsigned char ego_helper_start[], ego_helper_end[];
extern const unsigned char ego_library_start[], ego_library_end[];
#endif
#ifdef EGO_EMBEDDED
static pthread_mutex_t extraction_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static int resources(char helper[PATH_MAX], char library[PATH_MAX])
{
    const char *override = getenv("HEAD_CAM_EGO_HELPER");
    const char *sdk = getenv("HEAD_CAM_EGO_LIBRARY");
    if (override && sdk) {
        if (strlen(override) >= PATH_MAX || strlen(sdk) >= PATH_MAX) return -1;
        strcpy(helper, override); strcpy(library, sdk); return 0;
    }
#ifdef EGO_EMBEDDED
    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "/tmp/head-cam-sdk-%s", MONITOR_RELEASE_VERSION);
    pthread_mutex_lock(&extraction_mutex);
    int result = private_directory(directory);
    const unsigned char *starts[] = {ego_helper_start, ego_library_start};
    const unsigned char *ends[] = {ego_helper_end, ego_library_end};
    const char *names[] = {"ego-bridge", "libegosdk.so"};
    char *paths[] = {helper, library};
    for (int i = 0; i < 2 && !result; i++) {
        result = path_join(paths[i], PATH_MAX, directory, names[i]);
        if (result) break;
        struct stat st;
        size_t size = (size_t)(ends[i] - starts[i]);
        if (!lstat(paths[i], &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() && (size_t)st.st_size == size) continue;
        int fd = open(paths[i], O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0700);
        if (fd < 0) { result = -1; break; }
        result = write_all(fd, starts[i], size) || fsync(fd);
        close(fd);
        if (result) unlink(paths[i]);
    }
    pthread_mutex_unlock(&extraction_mutex);
    return result;
#else
    (void)helper; (void)library; return -1;
#endif
}

static int run_bridge(const char *mode, const char *path,
                      void (*event)(const cJSON *, void *), void *context)
{
    char helper[PATH_MAX], library[PATH_MAX];
    if (resources(helper, library)) return -1;
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC)) return -1;
    pid_t parent = getpid(), child = fork();
    if (!child) {
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent) _exit(125);
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(125);
        close(descriptors[1]);
        execl(helper, helper, library, mode, path, (char *)NULL);
        _exit(127);
    }
    close(descriptors[1]);
    if (child < 0) { close(descriptors[0]); return -1; }
    char line[8192]; size_t used = 0;
    int failed = 0;
    time_t deadline = time(NULL) + (!strcmp(mode, "query") ? 30 : 1200);
    for (;;) {
        if (time(NULL) >= deadline) { failed = 1; break; }
        struct pollfd descriptor = {.fd=descriptors[0], .events=POLLIN};
        int ready = poll(&descriptor, 1, 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { failed = 1; break; }
        if (!ready) continue;
        char character;
        ssize_t count = read(descriptors[0], &character, 1);
        if (!count) break;
        if (count < 0) { if (errno == EINTR) continue; failed = 1; break; }
        if (character == '\n') {
            line[used] = 0;
            cJSON *value = cJSON_Parse(line);
            if (value) { event(value, context); cJSON_Delete(value); }
            used = 0;
        } else if (used + 1 < sizeof(line)) line[used++] = character;
        else { failed = 1; break; }
    }
    close(descriptors[0]);
    if (failed) kill(child, SIGKILL);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) { if (errno != EINTR) return -1; }
    return failed || !WIFEXITED(status) || WEXITSTATUS(status) ? -1 : 0;
}
static void query_result(const cJSON *value, void *context)
{
    cJSON **output = context;
    cJSON_Delete(*output); *output = cJSON_Duplicate(value, 1);
}
cJSON *ego_query(void)
{
    cJSON *output = NULL;
    run_bridge("query", NULL, query_result, &output);
    return output;
}
int ego_upgrade(const char *path, void (*event)(const cJSON *, void *), void *context)
{
    return run_bridge("upgrade", path, event, context);
}
int operation_lock(int exclusive)
{
    const char *home = getenv("HEAD_CAM_HOME");
    if (!home) return -1;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/runtime/operation.lock", home) >= (int)sizeof(path)) return -1;
    int fd = open(path, O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, (exclusive ? LOCK_EX : LOCK_SH)|LOCK_NB)) { close(fd); return -1; }
    return fd;
}
static int parse_version(const char *text, unsigned long parts[4], char *suffix)
{
    for (int i = 0; i < 4; i++) {
        if (*text < '0' || *text > '9') return -1;
        errno = 0;
        char *end;
        parts[i] = strtoul(text, &end, 10);
        if (errno || *end != '.') return -1;
        if (i >= 2 && end - text != 2) return -1;
        text = end + 1;
    }
    static const unsigned char days_in_month[] = {31,29,31,30,31,30,31,31,30,31,30,31};
    unsigned long month = parts[2], day = parts[3];
    if (month < 1 || month > 12 || day < 1 || day > days_in_month[month - 1]) return -1;
    if (!((*text >= 'A' && *text <= 'Z') || (*text >= 'a' && *text <= 'z')) || text[1]) return -1;
    *suffix = *text;
    return 0;
}
int ota_compare_versions(const char *current, const char *target, int *comparison)
{
    unsigned long left[4], right[4];
    char left_suffix, right_suffix;
    if (parse_version(current, left, &left_suffix) || parse_version(target, right, &right_suffix) ||
        left_suffix != right_suffix) return -1;
    *comparison = 0;
    for (int i = 0; i < 4; i++) {
        if (left[i] != right[i]) { *comparison = left[i] < right[i] ? -1 : 1; break; }
    }
    return 0;
}

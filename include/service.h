#ifndef HEAD_CAM_SERVICE_H
#define HEAD_CAM_SERVICE_H
#include <cjson/cJSON.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>

#define ERROR_SIZE 512
#define SERVICE_VERSION "1.0.0"
#define MAX_FILES 10000

typedef struct {
    char device[128], date[11], session[7];
} head_identity;

typedef struct {
    char runtime[PATH_MAX], allowed_root[PATH_MAX], bundle_dir[PATH_MAX];
    char ossutil[PATH_MAX], config[PATH_MAX], bucket[128], ca[PATH_MAX];
    unsigned process_timeout;
} service_config;

extern service_config settings;
int path_join(char *out, size_t cap, const char *a, const char *b);
int path_inside(const char *path, const char *root);
int write_all(int fd, const void *data, size_t size);
int private_directory(const char *path);
int json_save(const char *path, const cJSON *value);
cJSON *json_load(const char *path);
cJSON *json_parse(const char *text, size_t size);
void js(cJSON *obj, const char *key, const char *value);
void json_number(cJSON *obj, const char *key, double value);
void jb(cJSON *obj, const char *key, int value);
const char *str(const cJSON *obj, const char *key);
double num(const cJSON *obj, const char *key);
void timestamp(char out[32]);
int sha256_fd(int fd, char out[65]);
int same_file(const struct stat *a, const struct stat *b);
int head_parse(const char *name, head_identity *identity, char *error);
int mcap_duration(int fd, double *duration, char *error);

int bundle_open(const char *config_override, const char *ossutil_override, char *error);
void bundle_close(void);
int oss_self_check(char *error);
/* Never exposes raw ossutil output (which may contain request credentials). */
int oss_run(const char *const *args, char **output, char *error);
typedef void (*upload_progress_fn)(void *context, const char *phase, uint64_t bytes, uint64_t total);
int oss_run_progress(const char *const *args, char **output, char *error,
                     upload_progress_fn progress, void *context, uint64_t total);
cJSON *process_file_progress(const char *path, const char *destination, char *error,
                            upload_progress_fn progress, void *context);
int destination_for(const char *name, char *out, size_t cap, char *error);

typedef struct task {
    cJSON *json;
    struct task *next;
} task;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t worker;
    task *first, *last;
    int closing, storage_failed;
} task_manager;
int tasks_start(task_manager *manager, char *error);
void tasks_close(task_manager *manager);
cJSON *task_submit_files(task_manager *manager, const char *path, const cJSON *files, int *http_status);
cJSON *task_get(task_manager *manager, const char *id, int *http_status);
int http_serve(task_manager *manager, const char *host, unsigned port, const char *token);
#endif

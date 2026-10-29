#include "service.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

service_config settings;
static pthread_mutex_t parse_mutex=PTHREAD_MUTEX_INITIALIZER;
cJSON *json_parse(const char *text,size_t size) {
    /* cJSON's parse error pointer is global, even when callers do not read it. */
    pthread_mutex_lock(&parse_mutex);
    cJSON *j=cJSON_ParseWithLengthOpts(text,size,NULL,1);
    pthread_mutex_unlock(&parse_mutex);
    return j;
}
int path_join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s%s%s", a, a[0] && a[strlen(a)-1] == '/' ? "" : "/", b);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
int path_inside(const char *path, const char *root) {
    size_t n = strlen(root);
    return !strcmp(root, "/") || (!strncmp(path, root, n) && (!path[n] || path[n] == '/'));
}
int write_all(int fd, const void *data, size_t size) {
    const unsigned char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}
int private_directory(const char *path) {
    struct stat st;
    if (mkdir(path, 0700) && errno != EEXIST) return -1;
    if (lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) return -1;
    return chmod(path, 0700);
}
int json_save(const char *path, const cJSON *value) {
    char tmp[PATH_MAX], parent[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    char *text = cJSON_PrintUnformatted(value);
    if (!text) return -1;
    int fd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC, 0600);
    int ok = fd >= 0 && !write_all(fd, text, strlen(text)) && !fsync(fd);
    free(text);
    if (fd >= 0 && close(fd)) ok = 0;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash) return -1;
    *slash = 0;
    fd = open(parent, O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    ok = fd >= 0 && !fsync(fd);
    if (fd >= 0) close(fd);
    return ok ? 0 : -1;
}
cJSON *json_load(const char *path) {
    int fd = open(path, O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 128*1024*1024) { close(fd); return NULL; }
    size_t size = (size_t)st.st_size, done = 0;
    char *buf = malloc(size + 1);
    if (!buf) { close(fd); return NULL; }
    while (done < size) {
        ssize_t n = read(fd, buf+done, size-done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    close(fd); buf[done] = 0;
    cJSON *j = done == size ? json_parse(buf, size+1) : NULL;
    free(buf); return j;
}
static void replace(cJSON *obj, const char *key, cJSON *value) {
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddItemToObject(obj, key, value);
}
void js(cJSON *obj, const char *key, const char *value) { replace(obj, key, cJSON_CreateString(value ? value : "")); }
void json_number(cJSON *obj, const char *key, double value) { replace(obj, key, cJSON_CreateNumber(value)); }
void jb(cJSON *obj, const char *key, int value) { replace(obj, key, cJSON_CreateBool(value)); }
const char *str(const cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
double num(const cJSON *obj, const char *key) { return cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(obj, key)); }
void timestamp(char out[32]) {
    time_t now = time(NULL); struct tm tm;
    gmtime_r(&now, &tm); strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}
int sha256_fd(int fd, char out[65]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char data[65536], digest[32]; unsigned int len = 0; off_t offset = 0;
    int ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    while (ok) {
        ssize_t n = pread(fd, data, sizeof(data), offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = 0; break; }
        if (!n) break;
        ok = EVP_DigestUpdate(ctx, data, (size_t)n) == 1; offset += n;
    }
    if (ok) ok = EVP_DigestFinal_ex(ctx, digest, &len) == 1 && len == 32;
    EVP_MD_CTX_free(ctx);
    if (!ok) return -1;
    for (unsigned i=0;i<32;i++) snprintf(out+2*i, 3, "%02x", digest[i]);
    return 0;
}
int same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

#include "oss_upload.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int fd, const char *data, size_t length)
{
    while (length > 0) {
        ssize_t written = write(fd, data, length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        data += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int valid_file_name(const char *file_name)
{
    return file_name != NULL && file_name[0] != '\0' &&
           strcmp(file_name, ".") != 0 && strcmp(file_name, "..") != 0 &&
           strchr(file_name, '/') == NULL && strchr(file_name, '\\') == NULL;
}

int oss_build_destination(const oss_config_t *config,
                          const char *file_name,
                          char *destination, size_t destination_size,
                          char *error, size_t error_size)
{
    const char *prefix;
    size_t prefix_len;
    int written;

    if (config == NULL || !valid_file_name(file_name)) {
        snprintf(error, error_size, "Invalid upload file name");
        return -1;
    }
    prefix = config->object_prefix;
    while (*prefix == '/') {
        ++prefix;
    }
    prefix_len = strlen(prefix);
    while (prefix_len > 0 && prefix[prefix_len - 1U] == '/') {
        --prefix_len;
    }
    if (prefix_len == 0) {
        written = snprintf(destination, destination_size, "oss://%s/%s",
                           config->bucket, file_name);
    } else {
        written = snprintf(destination, destination_size, "oss://%s/%.*s/%s",
                           config->bucket, (int)prefix_len, prefix, file_name);
    }
    if (written < 0 || (size_t)written >= destination_size) {
        snprintf(error, error_size, "OSS destination is too long");
        return -1;
    }
    return 0;
}

static int create_ossutil_config(const oss_config_t *config,
                                 char *path, size_t path_size,
                                 char *error, size_t error_size)
{
    char content[4096];
    int fd;
    int length;

    if (snprintf(path, path_size, "/tmp/.ossutil_demo_%ld_XXXXXX",
                 (long)getpid()) >= (int)path_size) {
        snprintf(error, error_size, "Temporary path is too long");
        return -1;
    }
    fd = mkstemp(path);
    if (fd < 0) {
        snprintf(error, error_size, "Cannot create temporary OSS config: %s",
                 strerror(errno));
        return -1;
    }
    if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        snprintf(error, error_size, "Cannot protect temporary OSS config: %s",
                 strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    length = snprintf(content, sizeof(content),
                      "[Credentials]\n"
                      "language=EN\n"
                      "endpoint=%s\n"
                      "accessKeyID=%s\n"
                      "accessKeySecret=%s\n"
                      "stsToken=%s\n",
                      config->endpoint,
                      config->access_key_id,
                      config->access_key_secret,
                      config->security_token);
    if (length < 0 || (size_t)length >= sizeof(content) ||
        write_all(fd, content, (size_t)length) != 0 || fsync(fd) != 0) {
        snprintf(error, error_size, "Cannot write temporary OSS config: %s",
                 strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    if (close(fd) != 0) {
        snprintf(error, error_size, "Cannot close temporary OSS config: %s",
                 strerror(errno));
        unlink(path);
        return -1;
    }
    return 0;
}

int oss_upload_one(const oss_config_t *config,
                   const char *local_path,
                   const char *destination,
                   char *error, size_t error_size)
{
    char temporary_config[PATH_MAX];
    pid_t child;
    pid_t waited;
    int status = 0;

    if (access(config->ossutil_path, X_OK) != 0) {
        snprintf(error, error_size, "ossutil is not executable: %s",
                 config->ossutil_path);
        return -1;
    }
    if (create_ossutil_config(config, temporary_config, sizeof(temporary_config),
                              error, error_size) != 0) {
        return -1;
    }
    child = fork();
    if (child < 0) {
        snprintf(error, error_size, "Cannot start ossutil: %s", strerror(errno));
        unlink(temporary_config);
        return -1;
    }
    if (child == 0) {
        char *const arguments[] = {
            (char *)config->ossutil_path,
            "-c",
            temporary_config,
            "cp",
            (char *)local_path,
            (char *)destination,
            "--force",
            NULL,
        };
        if (config->ca_cert_path[0] != '\0') {
            (void)setenv("SSL_CERT_FILE", config->ca_cert_path, 1);
        }
        execv(config->ossutil_path, arguments);
        fprintf(stderr, "[ERROR] execv(%s): %s\n",
                config->ossutil_path, strerror(errno));
        _exit(127);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    (void)unlink(temporary_config);
    if (waited < 0) {
        snprintf(error, error_size, "Cannot wait for ossutil: %s", strerror(errno));
        return -1;
    }
    if (!WIFEXITED(status)) {
        snprintf(error, error_size, "ossutil terminated abnormally");
        return -1;
    }
    if (WEXITSTATUS(status) != 0) {
        snprintf(error, error_size, "ossutil failed with exit code %d",
                 WEXITSTATUS(status));
        return -1;
    }
    return 0;
}

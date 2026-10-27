#include "oss_config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static char *trim(char *text)
{
    char *end;
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    if (*text == '\0') {
        return text;
    }
    end = text + strlen(text) - 1U;
    while (end > text && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }
    return text;
}

static int copy_value(char *destination, size_t destination_size,
                      const char *value, const char *key,
                      char *error, size_t error_size)
{
    size_t length = strlen(value);
    if (length >= destination_size) {
        snprintf(error, error_size, "Config value is too long: %s", key);
        return -1;
    }
    memcpy(destination, value, length + 1U);
    return 0;
}

static int set_value(oss_config_t *config, const char *key, const char *value,
                     char *error, size_t error_size)
{
    if (strcmp(key, "endpoint") == 0) {
        return copy_value(config->endpoint, sizeof(config->endpoint), value,
                          key, error, error_size);
    }
    if (strcmp(key, "bucket") == 0) {
        return copy_value(config->bucket, sizeof(config->bucket), value,
                          key, error, error_size);
    }
    if (strcmp(key, "access_key_id") == 0) {
        return copy_value(config->access_key_id, sizeof(config->access_key_id),
                          value, key, error, error_size);
    }
    if (strcmp(key, "access_key_secret") == 0) {
        return copy_value(config->access_key_secret,
                          sizeof(config->access_key_secret), value,
                          key, error, error_size);
    }
    if (strcmp(key, "security_token") == 0) {
        return copy_value(config->security_token, sizeof(config->security_token),
                          value, key, error, error_size);
    }
    if (strcmp(key, "object_prefix") == 0) {
        return copy_value(config->object_prefix, sizeof(config->object_prefix),
                          value, key, error, error_size);
    }
    if (strcmp(key, "ossutil_path") == 0) {
        return copy_value(config->ossutil_path, sizeof(config->ossutil_path),
                          value, key, error, error_size);
    }
    if (strcmp(key, "ca_cert_path") == 0) {
        return copy_value(config->ca_cert_path, sizeof(config->ca_cert_path),
                          value, key, error, error_size);
    }
    snprintf(error, error_size, "Unknown config key: %s", key);
    return -1;
}

int oss_config_load(const char *path, oss_config_t *config,
                    char *error, size_t error_size)
{
    FILE *file;
    char line[4096];
    unsigned int line_number = 0;
    struct stat config_stat;

    if (path == NULL || config == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(config, 0, sizeof(*config));
    if (stat(path, &config_stat) != 0) {
        snprintf(error, error_size, "Cannot stat %s: %s", path, strerror(errno));
        return -1;
    }
    if ((config_stat.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        snprintf(error, error_size,
                 "Unsafe config permissions on %s; run chmod 600 %s", path, path);
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        snprintf(error, error_size, "Cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *content;
        char *separator;
        char *key;
        char *value;

        ++line_number;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            snprintf(error, error_size, "Config line %u is too long", line_number);
            fclose(file);
            return -1;
        }
        content = trim(line);
        if (*content == '\0' || *content == '#') {
            continue;
        }
        separator = strchr(content, '=');
        if (separator == NULL) {
            snprintf(error, error_size, "Config line %u has no '='", line_number);
            fclose(file);
            return -1;
        }
        *separator = '\0';
        key = trim(content);
        value = trim(separator + 1);
        if (set_value(config, key, value, error, error_size) != 0) {
            fclose(file);
            return -1;
        }
    }
    if (ferror(file)) {
        snprintf(error, error_size, "Cannot read %s: %s", path, strerror(errno));
        fclose(file);
        return -1;
    }
    fclose(file);
    if (config->endpoint[0] == '\0' || config->bucket[0] == '\0' ||
        config->access_key_id[0] == '\0' ||
        config->access_key_secret[0] == '\0' ||
        config->ossutil_path[0] == '\0') {
        snprintf(error, error_size,
                 "endpoint, bucket, access_key_id, access_key_secret and "
                 "ossutil_path are required");
        return -1;
    }
    return 0;
}

static int read_line(const char *prompt, char *buffer,
                     size_t buffer_size, int hidden)
{
    struct termios old_settings;
    struct termios new_settings;
    int restore_terminal = 0;

    printf("%s", prompt);
    fflush(stdout);
    if (hidden && isatty(STDIN_FILENO) &&
        tcgetattr(STDIN_FILENO, &old_settings) == 0) {
        new_settings = old_settings;
        new_settings.c_lflag &= (tcflag_t)~ECHO;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_settings) == 0) {
            restore_terminal = 1;
        }
    }
    if (fgets(buffer, (int)buffer_size, stdin) == NULL) {
        if (restore_terminal) {
            (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_settings);
        }
        return -1;
    }
    if (restore_terminal) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_settings);
        printf("\n");
    }
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 0;
}

static void write_value(FILE *file, const char *key, const char *value)
{
    fprintf(file, "%s=%s\n", key, value);
}

int oss_config_write_interactive(const char *path,
                                 const char *default_ossutil_path,
                                 char *error, size_t error_size)
{
    char endpoint[256];
    char bucket[128];
    char access_key_id[256];
    char access_key_secret[256];
    char security_token[2048];
    char object_prefix[512];
    char answer[16];
    int fd;
    FILE *file;

    if (access(path, F_OK) == 0) {
        if (read_line("Config exists. Overwrite it? [y/N]: ", answer,
                      sizeof(answer), 0) != 0 ||
            !(answer[0] == 'y' || answer[0] == 'Y')) {
            snprintf(error, error_size, "Cancelled; existing config was preserved");
            return -1;
        }
    }
    if (read_line("OSS endpoint (example: oss-cn-shanghai.aliyuncs.com): ",
                  endpoint, sizeof(endpoint), 0) != 0 ||
        read_line("Bucket: ", bucket, sizeof(bucket), 0) != 0 ||
        read_line("AccessKey ID: ", access_key_id, sizeof(access_key_id), 0) != 0 ||
        read_line("AccessKey Secret (hidden): ", access_key_secret,
                  sizeof(access_key_secret), 1) != 0 ||
        read_line("STS security token (optional): ", security_token,
                  sizeof(security_token), 1) != 0 ||
        read_line("Object prefix (example: demo): ", object_prefix,
                  sizeof(object_prefix), 0) != 0) {
        snprintf(error, error_size, "Input ended before config was complete");
        return -1;
    }
    if (endpoint[0] == '\0' || bucket[0] == '\0' ||
        access_key_id[0] == '\0' || access_key_secret[0] == '\0') {
        snprintf(error, error_size, "Required config value is empty");
        return -1;
    }

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        snprintf(error, error_size, "Cannot create %s: %s", path, strerror(errno));
        return -1;
    }
    if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        snprintf(error, error_size, "Cannot protect %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    file = fdopen(fd, "w");
    if (file == NULL) {
        snprintf(error, error_size, "Cannot write %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    fprintf(file, "# Alibaba Cloud OSS upload demo configuration\n");
    fprintf(file, "# Keep this file at mode 0600 because it contains a secret.\n");
    write_value(file, "endpoint", endpoint);
    write_value(file, "bucket", bucket);
    write_value(file, "access_key_id", access_key_id);
    write_value(file, "access_key_secret", access_key_secret);
    write_value(file, "security_token", security_token);
    write_value(file, "object_prefix", object_prefix);
    write_value(file, "ossutil_path", default_ossutil_path);
    write_value(file, "ca_cert_path", "/usr/bin/ssl/cacert.crt");
    if (fclose(file) != 0) {
        snprintf(error, error_size, "Cannot finish %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

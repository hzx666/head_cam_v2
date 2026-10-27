#include "oss_config.h"
#include "oss_upload.h"
#include "tf_scan.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PROGRAM_VERSION
#define PROGRAM_VERSION "dev"
#endif

static int executable_directory(const char *argv0, char *output, size_t output_size)
{
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1U);
    char *slash;

    if (length >= 0) {
        executable[length] = '\0';
    } else if (realpath(argv0, executable) == NULL) {
        return -1;
    }
    slash = strrchr(executable, '/');
    if (slash == NULL) {
        return -1;
    }
    *slash = '\0';
    if (strlen(executable) >= output_size) {
        return -1;
    }
    strcpy(output, executable);
    return 0;
}

static int join_path(const char *left, const char *right,
                     char *output, size_t output_size)
{
    size_t left_length = strlen(left);
    int written;
    if (left_length == 0) {
        return -1;
    }
    written = snprintf(output, output_size, "%s%s%s", left,
                       left[left_length - 1U] == '/' ? "" : "/", right);
    return written < 0 || (size_t)written >= output_size ? -1 : 0;
}

static int read_input(const char *prompt, char *buffer, size_t buffer_size)
{
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buffer, (int)buffer_size, stdin) == NULL) {
        return -1;
    }
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 0;
}

static int has_mcap_extension(const char *file_name)
{
    size_t length = strlen(file_name);
    return length >= 5U && strcasecmp(file_name + length - 5U, ".mcap") == 0;
}

static int path_is_inside_tf(const char *file_path,
                             const tf_mount_t *mounts,
                             size_t mount_count)
{
    char resolved_file[PATH_MAX];
    size_t i;
    if (realpath(file_path, resolved_file) == NULL) {
        return 0;
    }
    for (i = 0; i < mount_count; ++i) {
        char resolved_mount[PATH_MAX];
        size_t mount_length;
        if (realpath(mounts[i].mount_point, resolved_mount) == NULL) {
            continue;
        }
        mount_length = strlen(resolved_mount);
        if (strncmp(resolved_file, resolved_mount, mount_length) == 0 &&
            resolved_file[mount_length] == '/') {
            return 1;
        }
    }
    return 0;
}

static void print_usage(const char *program)
{
    printf("Usage:\n");
    printf("  %s                 Scan TF and interactively upload one MCAP\n", program);
    printf("  %s --init-config   Write OSS config beside the executable\n", program);
    printf("  %s --scan-only     Scan TF without reading config or uploading\n", program);
    printf("  %s --help          Show this help\n", program);
}

int main(int argc, char **argv)
{
    char program_dir[PATH_MAX];
    char config_path[PATH_MAX];
    char default_ossutil_path[PATH_MAX];
    char directory[PATH_MAX];
    char file_name[PATH_MAX];
    char local_path[PATH_MAX];
    char destination[PATH_MAX + 1024];
    char error[1024];
    char confirmation[16];
    tf_mount_t mounts[TF_SCAN_MAX_MOUNTS];
    scan_options_t scan_options = {1, 0, 20};
    scan_result_t scan_result;
    oss_config_t config;
    struct stat file_stat;
    size_t mount_count;
    size_t i;
    int scan_failed = 0;

    if (executable_directory(argv[0], program_dir, sizeof(program_dir)) != 0 ||
        join_path(program_dir, "oss_upload.conf", config_path,
                  sizeof(config_path)) != 0 ||
        join_path(program_dir, "ossutilarm64", default_ossutil_path,
                  sizeof(default_ossutil_path)) != 0) {
        fprintf(stderr, "[ERROR] Cannot resolve executable directory\n");
        return 1;
    }
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 ||
                      strcmp(argv[1], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--init-config") == 0) {
        if (oss_config_write_interactive(config_path, default_ossutil_path,
                                         error, sizeof(error)) != 0) {
            fprintf(stderr, "[ERROR] %s\n", error);
            return 1;
        }
        printf("[OK] Config written: %s (mode 0600)\n", config_path);
        return 0;
    }
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--scan-only") != 0)) {
        print_usage(argv[0]);
        return 64;
    }

    printf("data_upload_demo version=%s\n", PROGRAM_VERSION);
    mount_count = tf_find_mounts(mounts, TF_SCAN_MAX_MOUNTS);
    if (mount_count == 0) {
        fprintf(stderr, "[ERROR] No TF card mounted below /mnt/sd/\n");
        return 2;
    }
    printf("[SCAN] detected_tf_mounts=%llu\n", (unsigned long long)mount_count);
    for (i = 0; i < mount_count; ++i) {
        memset(&scan_result, 0, sizeof(scan_result));
        printf("[TF] device=%s mount=%s fs=%s\n",
               mounts[i].source, mounts[i].mount_point, mounts[i].fs_type);
        if (tf_scan_mcap(mounts[i].mount_point, &scan_options, &scan_result) != 0) {
            fprintf(stderr, "[ERROR] Scan failed: %s\n", strerror(errno));
            scan_failed = 1;
        }
        printf("[SCAN] complete=%llu incomplete=%llu errors=%llu\n",
               (unsigned long long)scan_result.complete_count,
               (unsigned long long)scan_result.incomplete_count,
               (unsigned long long)scan_result.error_count);
    }
    if (scan_failed) {
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "--scan-only") == 0) {
        return 0;
    }

    if (oss_config_load(config_path, &config, error, sizeof(error)) != 0) {
        fprintf(stderr, "[ERROR] OSS config: %s\n", error);
        fprintf(stderr, "[HINT] Run: %s --init-config\n", argv[0]);
        return 3;
    }
    if (config.ca_cert_path[0] != '\0' && access(config.ca_cert_path, R_OK) != 0) {
        fprintf(stderr, "[ERROR] CA certificate is not readable: %s\n",
                config.ca_cert_path);
        return 3;
    }
    if (read_input("MCAP directory: ", directory, sizeof(directory)) != 0 ||
        read_input("MCAP file name: ", file_name, sizeof(file_name)) != 0) {
        fprintf(stderr, "[ERROR] Input ended\n");
        return 1;
    }
    if (file_name[0] == '\0' || strchr(file_name, '/') != NULL ||
        strchr(file_name, '\\') != NULL || !has_mcap_extension(file_name)) {
        fprintf(stderr, "[ERROR] File name must be a base name ending in .mcap\n");
        return 4;
    }
    if (join_path(directory, file_name, local_path, sizeof(local_path)) != 0 ||
        stat(local_path, &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
        fprintf(stderr, "[ERROR] MCAP file does not exist or is not regular: %s\n",
                local_path);
        return 4;
    }
    if (file_stat.st_size <= 0) {
        fprintf(stderr, "[ERROR] Refusing to upload an empty file: %s\n", local_path);
        return 4;
    }
    if (!path_is_inside_tf(local_path, mounts, mount_count)) {
        fprintf(stderr, "[ERROR] File is outside the detected TF card: %s\n",
                local_path);
        return 4;
    }
    if (oss_build_destination(&config, file_name, destination, sizeof(destination),
                              error, sizeof(error)) != 0) {
        fprintf(stderr, "[ERROR] %s\n", error);
        return 4;
    }

    printf("\n[UPLOAD PLAN]\n");
    printf("  Local    : %s\n", local_path);
    printf("  Size     : %lld bytes\n", (long long)file_stat.st_size);
    printf("  OSS      : %s\n", destination);
    printf("  Endpoint : %s\n", config.endpoint);
    printf("  Local file will be preserved; an existing OSS object may be overwritten.\n");
    if (read_input("Confirm upload this one file? Type YES: ",
                   confirmation, sizeof(confirmation)) != 0 ||
        strcmp(confirmation, "YES") != 0) {
        printf("[CANCELLED] Nothing was uploaded.\n");
        return 0;
    }
    printf("[UPLOAD] Starting...\n");
    if (oss_upload_one(&config, local_path, destination, error, sizeof(error)) != 0) {
        fprintf(stderr, "[FAILED] %s\n", error);
        return 5;
    }
    printf("[OK] Upload completed: %s\n", destination);
    printf("[OK] Local file preserved: %s\n", local_path);
    return 0;
}

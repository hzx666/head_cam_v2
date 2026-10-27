#include "tf_scan.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_SCAN_DEPTH 128U

static int starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static int ends_with_casefold(const char *text, const char *suffix)
{
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);

    if (text_len < suffix_len) {
        return 0;
    }
    return strcasecmp(text + text_len - suffix_len, suffix) == 0;
}

static int is_mmc_partition(const char *source)
{
    const char *name;
    const char *cursor;

    if (source == NULL || !starts_with(source, "/dev/mmcblk")) {
        return 0;
    }
    name = source + strlen("/dev/mmcblk");
    cursor = name;
    if (!isdigit((unsigned char)*cursor)) {
        return 0;
    }
    while (isdigit((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor++ != 'p' || !isdigit((unsigned char)*cursor)) {
        return 0;
    }
    while (isdigit((unsigned char)*cursor)) {
        ++cursor;
    }
    return *cursor == '\0';
}

static int is_tf_mount(const struct mntent *entry)
{
    if (entry == NULL || entry->mnt_fsname == NULL || entry->mnt_dir == NULL) {
        return 0;
    }

    /*
     * The target reports /sys/class/block/mmcblk0/removable as 0 even though
     * /dev/mmcblk0p1 is its TF card.  The board-specific mount contract is
     * therefore a safer signal than the removable bit on this device.
     */
    return is_mmc_partition(entry->mnt_fsname) &&
           (strcmp(entry->mnt_dir, "/mnt/sd") == 0 ||
            starts_with(entry->mnt_dir, "/mnt/sd/"));
}

size_t tf_find_mounts(tf_mount_t *mounts, size_t capacity)
{
    FILE *mount_file;
    struct mntent *entry;
    size_t count = 0;

    if (mounts == NULL || capacity == 0) {
        return 0;
    }

    mount_file = setmntent("/proc/self/mounts", "r");
    if (mount_file == NULL) {
        mount_file = setmntent("/proc/mounts", "r");
    }
    if (mount_file == NULL) {
        return 0;
    }

    while ((entry = getmntent(mount_file)) != NULL) {
        if (!is_tf_mount(entry)) {
            continue;
        }
        if (count >= capacity) {
            break;
        }
        snprintf(mounts[count].source, sizeof(mounts[count].source), "%s",
                 entry->mnt_fsname);
        snprintf(mounts[count].mount_point, sizeof(mounts[count].mount_point), "%s",
                 entry->mnt_dir);
        snprintf(mounts[count].fs_type, sizeof(mounts[count].fs_type), "%s",
                 entry->mnt_type != NULL ? entry->mnt_type : "unknown");
        ++count;
    }
    endmntent(mount_file);
    return count;
}

static int is_ignored_system_directory(const char *name)
{
    static const char *const ignored[] = {
        ".Spotlight-V100",
        ".Trashes",
        ".fseventsd",
        "System Volume Information",
        "$RECYCLE.BIN",
    };
    size_t i;

    for (i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
        if (strcasecmp(name, ignored[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static char *join_path(const char *parent, const char *name)
{
    size_t parent_len = strlen(parent);
    size_t name_len = strlen(name);
    int needs_separator = parent_len > 0 && parent[parent_len - 1] != '/';
    size_t size = parent_len + (size_t)needs_separator + name_len + 1U;
    char *path = malloc(size);

    if (path == NULL) {
        return NULL;
    }
    snprintf(path, size, "%s%s%s", parent, needs_separator ? "/" : "", name);
    return path;
}

static void print_file(const char *path, const struct stat *st, int incomplete)
{
    char size_text[64];
    char time_text[32] = "unknown";
    struct tm local_time;

    tf_format_bytes((uint64_t)st->st_size, size_text, sizeof(size_text));
    if (localtime_r(&st->st_mtime, &local_time) != NULL) {
        (void)strftime(time_text, sizeof(time_text), "%Y-%m-%dT%H:%M:%S%z",
                       &local_time);
    }
    printf("[MCAP] status=%s size=%s mtime=%s path=%s\n",
           incomplete ? "incomplete" : "complete",
           size_text,
           time_text,
           path);
}

static int scan_dir(const char *directory,
                    unsigned int depth,
                    const scan_options_t *options,
                    scan_result_t *result)
{
    DIR *dir;
    struct dirent *entry;
    int saved_errno = 0;

    if (depth > MAX_SCAN_DEPTH) {
        result->error_count++;
        fprintf(stderr, "[WARN] Maximum directory depth reached: %s\n", directory);
        return 0;
    }

    dir = opendir(directory);
    if (dir == NULL) {
        result->error_count++;
        fprintf(stderr, "[WARN] Cannot open directory: %s: %s\n",
                directory, strerror(errno));
        return 0;
    }

    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        char *path;
        struct stat st;
        int incomplete;
        int complete;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (is_ignored_system_directory(entry->d_name)) {
            result->skipped_system_dirs++;
            continue;
        }

        path = join_path(directory, entry->d_name);
        if (path == NULL) {
            result->error_count++;
            saved_errno = ENOMEM;
            break;
        }
        if (lstat(path, &st) != 0) {
            result->error_count++;
            fprintf(stderr, "[WARN] Cannot stat: %s: %s\n", path, strerror(errno));
            free(path);
            errno = 0;
            continue;
        }

        if (S_ISLNK(st.st_mode)) {
            free(path);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            (void)scan_dir(path, depth + 1U, options, result);
            free(path);
            errno = 0;
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            free(path);
            continue;
        }

        incomplete = ends_with_casefold(entry->d_name, ".mcap.incomplete");
        complete = !incomplete && ends_with_casefold(entry->d_name, ".mcap");
        if (!complete && !(incomplete && options->include_incomplete)) {
            free(path);
            continue;
        }

        if (incomplete) {
            result->incomplete_count++;
        } else {
            result->complete_count++;
        }
        result->total_bytes += (uint64_t)st.st_size;
        if (!options->count_only &&
            (options->print_limit == 0 || result->printed_count < options->print_limit)) {
            print_file(path, &st, incomplete);
            result->printed_count++;
        }
        free(path);
        errno = 0;
    }

    if (entry == NULL && errno != 0 && saved_errno == 0) {
        result->error_count++;
        saved_errno = errno;
        fprintf(stderr, "[WARN] Directory read failed: %s: %s\n",
                directory, strerror(errno));
    }
    (void)closedir(dir);
    if (saved_errno != 0) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int tf_scan_mcap(const char *root,
                 const scan_options_t *options,
                 scan_result_t *result)
{
    struct stat st;

    if (root == NULL || options == NULL || result == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (lstat(root, &st) != 0) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return scan_dir(root, 0, options, result);
}

void tf_format_bytes(uint64_t bytes, char *buffer, size_t buffer_size)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    size_t unit = 0;

    while (value >= 1024.0 && unit + 1U < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        (void)snprintf(buffer, buffer_size, "%llu %s",
                       (unsigned long long)bytes, units[unit]);
    } else {
        (void)snprintf(buffer, buffer_size, "%.2f %s", value, units[unit]);
    }
}

#include "tf_scan.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef PROGRAM_VERSION
#define PROGRAM_VERSION "dev"
#endif

static void print_usage(const char *program)
{
    printf("TF card MCAP scanner %s\n", PROGRAM_VERSION);
    printf("Usage:\n");
    printf("  %s [--count-only] [--complete-only] [--limit N]\n", program);
    printf("  %s --root PATH [--count-only] [--complete-only] [--limit N]\n", program);
    printf("\nOptions:\n");
    printf("  --root PATH       Scan a specified directory instead of auto-detecting TF.\n");
    printf("  --count-only      Do not print each MCAP path.\n");
    printf("  --complete-only   Ignore files ending in .mcap.incomplete.\n");
    printf("  --limit N         Print at most N paths (default 200, 0 means unlimited).\n");
    printf("  --help            Show this help.\n");
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static int scan_one_root(const char *source,
                         const char *root,
                         const char *fs_type,
                         const scan_options_t *options,
                         scan_result_t *grand_total)
{
    scan_result_t result;
    char size_text[64];
    uint64_t found;

    memset(&result, 0, sizeof(result));
    printf("\n[TF] device=%s mount=%s fs=%s\n",
           source != NULL ? source : "manual",
           root,
           fs_type != NULL ? fs_type : "unknown");

    if (tf_scan_mcap(root, options, &result) != 0) {
        fprintf(stderr, "[ERROR] Cannot scan %s: %s\n", root, strerror(errno));
        grand_total->error_count++;
        return -1;
    }

    found = result.complete_count + result.incomplete_count;
    tf_format_bytes(result.total_bytes, size_text, sizeof(size_text));
    printf("[SUMMARY] complete=%llu incomplete=%llu total=%llu size=%s errors=%llu\n",
           (unsigned long long)result.complete_count,
           (unsigned long long)result.incomplete_count,
           (unsigned long long)found,
           size_text,
           (unsigned long long)result.error_count);
    if (!options->count_only && options->print_limit != 0 &&
        found > result.printed_count) {
        printf("[SUMMARY] %llu path(s) omitted by --limit.\n",
               (unsigned long long)(found - result.printed_count));
    }

    grand_total->complete_count += result.complete_count;
    grand_total->incomplete_count += result.incomplete_count;
    grand_total->total_bytes += result.total_bytes;
    grand_total->printed_count += result.printed_count;
    grand_total->skipped_system_dirs += result.skipped_system_dirs;
    grand_total->error_count += result.error_count;
    return 0;
}

int main(int argc, char **argv)
{
    const char *manual_root = NULL;
    scan_options_t options = {
        .include_incomplete = 1,
        .count_only = 0,
        .print_limit = 200,
    };
    scan_result_t total;
    tf_mount_t mounts[TF_SCAN_MAX_MOUNTS];
    struct stat st;
    size_t mount_count;
    size_t i;
    int scan_failures = 0;

    memset(&total, 0, sizeof(total));
    for (i = 1; i < (size_t)argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--count-only") == 0) {
            options.count_only = 1;
            continue;
        }
        if (strcmp(argv[i], "--complete-only") == 0) {
            options.include_incomplete = 0;
            continue;
        }
        if (strcmp(argv[i], "--root") == 0) {
            if (++i >= (size_t)argc) {
                fprintf(stderr, "[ERROR] --root requires a path.\n");
                return 64;
            }
            manual_root = argv[i];
            continue;
        }
        if (strcmp(argv[i], "--limit") == 0) {
            if (++i >= (size_t)argc || parse_u64(argv[i], &options.print_limit) != 0) {
                fprintf(stderr, "[ERROR] --limit requires a non-negative integer.\n");
                return 64;
            }
            continue;
        }
        fprintf(stderr, "[ERROR] Unknown option: %s\n", argv[i]);
        print_usage(argv[0]);
        return 64;
    }

    printf("data_upload_scan version=%s mode=read-only\n", PROGRAM_VERSION);

    if (manual_root != NULL) {
        if (stat(manual_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "[ERROR] --root is not an accessible directory: %s\n",
                    manual_root);
            return 66;
        }
        scan_failures += scan_one_root("manual", manual_root, "manual",
                                       &options, &total) != 0;
    } else {
        mount_count = tf_find_mounts(mounts, TF_SCAN_MAX_MOUNTS);
        if (mount_count == 0) {
            fprintf(stderr,
                    "[ERROR] No mounted TF card found. Expected /dev/mmcblk*p* "
                    "mounted below /mnt/sd/.\n");
            fprintf(stderr,
                    "[HINT] Use --root PATH to test a known directory safely.\n");
            return 2;
        }
        printf("[INFO] detected_tf_mounts=%llu\n", (unsigned long long)mount_count);
        for (i = 0; i < mount_count; ++i) {
            scan_failures += scan_one_root(mounts[i].source,
                                           mounts[i].mount_point,
                                           mounts[i].fs_type,
                                           &options,
                                           &total) != 0;
        }
    }

    printf("\n[TOTAL] complete=%llu incomplete=%llu files=%llu scan_errors=%llu\n",
           (unsigned long long)total.complete_count,
           (unsigned long long)total.incomplete_count,
           (unsigned long long)(total.complete_count + total.incomplete_count),
           (unsigned long long)total.error_count);
    return scan_failures == 0 ? 0 : 1;
}

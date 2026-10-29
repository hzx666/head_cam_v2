#include "tf_scan.h"

#include <ctype.h>
#include <mntent.h>
#include <stdio.h>
#include <string.h>


static int starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
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

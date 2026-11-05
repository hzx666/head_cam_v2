#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <lzma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "packed_payload.h"

extern const unsigned char payload_start[], payload_end[];

static int unpack(int fd) {
    lzma_stream decoder = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&decoder, 16U << 20, 0) != LZMA_OK)
        return -1;
    decoder.next_in = payload_start;
    decoder.avail_in = (size_t)(payload_end - payload_start);
    unsigned char buffer[65536];
    lzma_ret result;
    do {
        decoder.next_out = buffer;
        decoder.avail_out = sizeof(buffer);
        result = lzma_code(&decoder, LZMA_FINISH);
        size_t count = sizeof(buffer) - decoder.avail_out;
        if (decoder.total_out > PAYLOAD_SIZE) {
            result = LZMA_DATA_ERROR;
            break;
        }
        for (size_t offset = 0; offset < count;) {
            ssize_t written = write(fd, buffer + offset, count - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { result = LZMA_PROG_ERROR; break; }
            offset += (size_t)written;
        }
    } while (result == LZMA_OK);
    int valid = result == LZMA_STREAM_END && !decoder.avail_in && decoder.total_out == PAYLOAD_SIZE;
    lzma_end(&decoder);
    return valid ? 0 : -1;
}

int main(int argc, char **argv) {
    (void)argc;
    char origin[PATH_MAX], directory[160], core[200], temporary[208], lockpath[200];
    ssize_t length = readlink("/proc/self/exe", origin, sizeof(origin)-1);
    if (length <= 0 || length >= (ssize_t)sizeof(origin)-1) return 1;
    origin[length] = '\0';
    if (setenv("HEAD_CAM_PACKED_EXE", origin, 1)) return 1;
    snprintf(directory, sizeof(directory), "/tmp/head-cam-core-%lu-%s",
             (unsigned long)geteuid(), PAYLOAD_TAG);
    struct stat st;
    if ((mkdir(directory, 0700) && errno != EEXIST) || lstat(directory, &st) ||
        !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) {
        fputs("Cannot use private program cache in /tmp\n", stderr);
        return 1;
    }
    snprintf(core, sizeof(core), "%s/head-cam-service", directory);
    snprintf(temporary, sizeof(temporary), "%s/core.new", directory);
    snprintf(lockpath, sizeof(lockpath), "%s/lock", directory);
    int lock = open(lockpath, O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX)) return 1;
    if (lstat(core, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0777) != 0700 || st.st_size != PAYLOAD_SIZE) {
        int fd = open(temporary, O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC|O_NOFOLLOW, 0700);
        int failed = fd < 0 || unpack(fd);
        if (fd >= 0 && close(fd)) failed = 1;
        if (failed || rename(temporary, core)) {
            unlink(temporary);
            fputs("Cannot unpack head-cam-service into /tmp\n", stderr);
            return 1;
        }
    }
    close(lock);
    execv(core, argv);
    perror("Cannot execute unpacked head-cam-service");
    return 1;
}

#include "ota.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <errno.h>

/* Signed URLs are never written to logs. Only HTTPS with verified peers is accepted. */
static int connect_host(const char *host, const char *port)
{
    struct addrinfo hints = {.ai_socktype=SOCK_STREAM}, *addresses = NULL;
    if (getaddrinfo(host, port, &hints, &addresses)) return -1;
    int fd = -1;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (fd < 0) continue;
        int result = connect(fd, address->ai_addr, address->ai_addrlen);
        if (result && errno == EINPROGRESS) {
            struct pollfd descriptor = {.fd=fd, .events=POLLOUT};
            int error = 0; socklen_t size = sizeof(error);
            result = poll(&descriptor, 1, 10000) <= 0 ||
                     getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error;
        }
        if (!result) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        struct timeval timeout = {.tv_sec=15};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }
    return fd;
}
static int ssl_line(SSL *ssl, char *line, size_t capacity)
{
    size_t used = 0;
    while (used + 1 < capacity) {
        char byte;
        if (SSL_read(ssl, &byte, 1) != 1) return -1;
        if (byte == '\n') {
            if (used && line[used-1] == '\r') used--;
            line[used] = 0; return 0;
        }
        line[used++] = byte;
    }
    return -1;
}
int ota_download(const char *url, const char *path, uint64_t expected,
                 ota_download_progress progress, void *context)
{
    if (strncmp(url, "https://", 8) || strpbrk(url, "\r\n\t ") || strlen(url) > 8192) return -1;
    const char *start = url + 8, *slash = strchr(start, '/');
    if (!slash) return -1;
    size_t host_length = (size_t)(slash - start);
    if (!host_length || host_length >= 256) return -1;
    char authority[256], host[256], port[8] = "443";
    memcpy(authority, start, host_length); authority[host_length] = 0;
    if (strchr(authority, '@') || strchr(authority, '#')) return -1;
    strcpy(host, authority);
    char *colon = strchr(host, ':');
    if (colon) {
        if (strlen(colon+1) >= sizeof(port) || !*(colon+1) || strspn(colon+1, "0123456789") != strlen(colon+1)) return -1;
        strcpy(port, colon+1); *colon = 0;
    }
    int result = -1, socket_fd = -1, file = -1;
    SSL_CTX *ssl_context = SSL_CTX_new(TLS_client_method());
    SSL *ssl = NULL;
    if (!ssl_context) return -1;
    SSL_CTX_set_verify(ssl_context, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_min_proto_version(ssl_context, TLS1_2_VERSION);
    int ca_loaded = 0;
#ifdef SERVICE_EMBEDDED
    extern const unsigned char bundled_ca_start[], bundled_ca_end[];
    BIO *certificates = BIO_new_mem_buf(bundled_ca_start, (int)(bundled_ca_end-bundled_ca_start));
    X509 *certificate;
    while (certificates && (certificate = PEM_read_bio_X509(certificates, NULL, NULL, NULL))) {
        if (X509_STORE_add_cert(SSL_CTX_get_cert_store(ssl_context), certificate)) ca_loaded = 1;
        X509_free(certificate);
    }
    BIO_free(certificates); ERR_clear_error();
#else
    ca_loaded = SSL_CTX_set_default_verify_paths(ssl_context);
#endif
    if (!ca_loaded) goto cleanup;
    socket_fd = connect_host(host, port);
    if (socket_fd < 0) goto cleanup;
    ssl = SSL_new(ssl_context);
    if (!ssl || !SSL_set_fd(ssl, socket_fd) || !SSL_set_tlsext_host_name(ssl, host) ||
        !SSL_set1_host(ssl, host) || SSL_connect(ssl) != 1) goto cleanup;
    char request[8704];
    int length = snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n", slash, authority);
    if (length < 0 || (size_t)length >= sizeof(request)) goto cleanup;
    for (int sent = 0; sent < length;) {
        int count = SSL_write(ssl, request + sent, length - sent);
        if (count <= 0) goto cleanup;
        sent += count;
    }
    char line[8192]; int status = 0;
    if (ssl_line(ssl, line, sizeof(line)) || sscanf(line, "HTTP/%*s %d", &status) != 1) goto cleanup;
    if (status == 401 || status == 403) { result = 403; goto cleanup; }
    if (status != 200) goto cleanup;
    int chunked = 0, length_seen = 0;
    unsigned headers = 0;
    for (;;) {
        if (++headers > 100 || ssl_line(ssl, line, sizeof(line))) goto cleanup;
        if (!*line) break;
        if (!strncasecmp(line, "Content-Length:", 15)) {
            char *end; errno = 0;
            unsigned long long value = strtoull(line + 15, &end, 10);
            if (errno || *end || value != expected) goto cleanup;
            length_seen = 1;
        }
        if (!strncasecmp(line, "Transfer-Encoding:", 18)) {
            const char *value = line + 18;
            while (*value == ' ') value++;
            if (strcasecmp(value, "chunked")) goto cleanup;
            chunked = 1;
        }
    }
    if (!length_seen && !chunked) goto cleanup;
    file = open(path, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0600);
    if (file < 0) goto cleanup;
    uint64_t total = 0;
    for (;;) {
        uint64_t remaining = expected - total;
        if (chunked) {
            if (ssl_line(ssl, line, sizeof(line))) goto cleanup;
            char *end; errno = 0;
            remaining = strtoull(line, &end, 16);
            if (errno || end == line || (*end && *end != ';') || remaining > expected-total) goto cleanup;
            if (!remaining) { if (total != expected) goto cleanup; break; }
        } else if (!remaining) break;
        while (remaining) {
            unsigned char buffer[32768];
            int size = remaining > sizeof(buffer) ? (int)sizeof(buffer) : (int)remaining;
            int count = SSL_read(ssl, buffer, size);
            if (count <= 0 || write_all(file, buffer, (size_t)count)) goto cleanup;
            total += (uint64_t)count; remaining -= (uint64_t)count;
            if (progress(total, context)) goto cleanup;
        }
        if (chunked && (ssl_line(ssl, line, sizeof(line)) || *line)) goto cleanup;
    }
    result = fsync(file) ? -1 : 0;
cleanup:
    if (file >= 0) { close(file); if (result) unlink(path); }
    SSL_free(ssl); SSL_CTX_free(ssl_context);
    if (socket_fd >= 0) close(socket_fd);
    return result;
}

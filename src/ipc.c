#include "ipc.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

int bg_ipc_socket_path(char *out, size_t out_len) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    int n;
    if (xdg && *xdg) {
        n = snprintf(out, out_len, "%s/background.sock", xdg);
    } else {
        n = snprintf(out, out_len, "/tmp/background-%u.sock", (unsigned)getuid());
    }
    if (n < 0 || (size_t)n >= out_len) return -1;
    return 0;
}

int bg_ipc_server_listen(void) {
    char path[PATH_MAX];
    if (bg_ipc_socket_path(path, sizeof(path)) < 0) {
        LOG_ERR("ipc: socket path too long");
        return -1;
    }

    // If a stale socket file exists and nobody is listening, remove it.
    // We try connecting first; if that succeeds, another daemon is alive.
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0) {
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        size_t plen = strlen(path);
        if (plen >= sizeof(addr.sun_path)) {
            LOG_ERR("ipc: socket path too long for AF_UNIX (%zu bytes)", plen);
            close(probe);
            return -1;
        }
        memcpy(addr.sun_path, path, plen + 1);
        if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(probe);
            LOG_ERR("ipc: another daemon is already running at %s", path);
            return -1;
        }
        close(probe);
        // ECONNREFUSED / ENOENT → safe to (re)create.
        unlink(path);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOG_ERR("socket: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    {
        size_t plen = strlen(path);
        if (plen >= sizeof(addr.sun_path)) {
            LOG_ERR("ipc: socket path too long for AF_UNIX (%zu bytes)", plen);
            close(fd);
            return -1;
        }
        memcpy(addr.sun_path, path, plen + 1);
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("bind(%s): %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        LOG_ERR("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path, 0600);
    LOG_INFO("ipc: listening at %s", path);
    return fd;
}

int bg_ipc_client_connect(void) {
    char path[PATH_MAX];
    if (bg_ipc_socket_path(path, sizeof(path)) < 0) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    {
        size_t plen = strlen(path);
        if (plen >= sizeof(addr.sun_path)) {
            LOG_ERR("ipc: socket path too long for AF_UNIX (%zu bytes)", plen);
            close(fd);
            return -1;
        }
        memcpy(addr.sun_path, path, plen + 1);
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int bg_ipc_read_line(int fd, char **out) {
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) return -1;
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r == 0) {
            if (len == 0) { free(buf); return -1; }
            break;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return -1;
        }
        if (c == '\n') break;
        buf[len++] = c;
    }
    buf[len] = '\0';
    *out = buf;
    return 0;
}

int bg_ipc_write_line(int fd, const char *s) {
    size_t len = strlen(s);
    size_t total_len = len + 1;
    char *buf = malloc(total_len);
    if (!buf) return -1;
    memcpy(buf, s, len);
    buf[len] = '\n';
    size_t off = 0;
    while (off < total_len) {
        ssize_t w = write(fd, buf + off, total_len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return -1;
        }
        off += (size_t)w;
    }
    free(buf);
    return 0;
}

int bg_ipc_client_request(int fd, const char *json_request, char **response) {
    if (bg_ipc_write_line(fd, json_request) < 0) return -1;
    char *line = NULL;
    if (bg_ipc_read_line(fd, &line) < 0) return -1;
    if (response) *response = line; else free(line);
    return 0;
}

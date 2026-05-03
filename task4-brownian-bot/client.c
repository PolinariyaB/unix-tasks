#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CONFIG_FILE "config"
#define RESPONSE_BUF_CAP 4096

static int read_socket_name(char *name, size_t cap) {
    FILE *f = fopen(CONFIG_FILE, "r");
    size_t len;
    if (f == NULL) {
        return -1;
    }
    if (fgets(name, (int)cap, f) == NULL) {
        fclose(f);
        errno = EINVAL;
        return -1;
    }
    fclose(f);
    len = strcspn(name, "\r\n");
    name[len] = '\0';
    if (len == 0 || strchr(name, '/') != NULL) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int connect_socket(void) {
    char sock_name[108];
    char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct sockaddr_un addr;
    int fd = -1;

    if (read_socket_name(sock_name, sizeof(sock_name)) < 0) {
        return -1;
    }
    if (snprintf(sock_path, sizeof(sock_path), "/tmp/%s", sock_name) >= (int)sizeof(sock_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int read_line_fd(int fd, char *buf, size_t cap) {
    static char inbuf[RESPONSE_BUF_CAP];
    static size_t in_len = 0;
    static size_t in_off = 0;
    size_t len = 0;

    while (len + 1 < cap) {
        if (in_off == in_len) {
            ssize_t r;

            do {
                r = read(fd, inbuf, sizeof(inbuf));
            } while (r < 0 && errno == EINTR);

            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                return -1;
            }
            if (r == 0) {
                errno = ECONNRESET;
                return -1;
            }
            in_off = 0;
            in_len = (size_t)r;
        }

        buf[len] = inbuf[in_off++];
        if (buf[len] == '\n') {
            len++;
            buf[len] = '\0';
            return 0;
        }
        len++;
    }
    errno = EMSGSIZE;
    return -1;
}

int main(void) {
    struct sigaction sa_ignore;
    memset(&sa_ignore, 0, sizeof(sa_ignore));
    sa_ignore.sa_handler = SIG_IGN;
    sigemptyset(&sa_ignore.sa_mask);
    sigaction(SIGPIPE, &sa_ignore, NULL);

    int fd = connect_socket();
    char in_line[128];
    char out_line[128];

    if (fd < 0) {
        perror("connect");
        return 1;
    }

    while (fgets(in_line, sizeof(in_line), stdin) != NULL) {
        size_t len = strlen(in_line);
        if (len == 0) {
            continue;
        }
        if (write_all(fd, in_line, len) < 0) {
            perror("write");
            close(fd);
            return 1;
        }
        if (read_line_fd(fd, out_line, sizeof(out_line)) < 0) {
            perror("read");
            close(fd);
            return 1;
        }
        if (fputs(out_line, stdout) == EOF) {
            close(fd);
            return 1;
        }
    }

    close(fd);
    return 0;
}

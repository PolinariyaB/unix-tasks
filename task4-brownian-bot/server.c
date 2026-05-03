#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define CONFIG_FILE "config"
#define LOG_PATH "/tmp/brownian-bot.log"
#define MAX_CLIENTS 1024
#define CLIENT_IN_CAP 8192
#define READ_CHUNK 2048
#define MAX_LINE_LEN 10


struct client {
    int fd;
    char inbuf[CLIENT_IN_CAP];
    size_t in_len;
    char *outbuf;
    size_t out_len;
    size_t out_off;
};

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo) {
    (void)signo;
    g_stop = 1;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

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
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }
    if (strchr(name, '/') != NULL) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static void close_client(struct client *c, FILE *logf) {
    if (c->fd >= 0) {
        if (logf != NULL) {
            fprintf(logf, "CLOSE fd=%d\n", c->fd);
            fflush(logf);
        }
        close(c->fd);
        c->fd = -1;
    }
    free(c->outbuf);
    c->outbuf = NULL;
    c->out_len = 0;
    c->out_off = 0;
    c->in_len = 0;
}

static int queue_response(struct client *c, const char *line) {
    size_t need = strlen(line);
    char *new_buf = realloc(c->outbuf, c->out_len + need);
    if (new_buf == NULL) {
        return -1;
    }
    c->outbuf = new_buf;
    memcpy(c->outbuf + c->out_len, line, need);
    c->out_len += need;
    return 0;
}

static int parse_and_queue_lines(struct client *c, long *state, FILE *logf) {
    size_t start = 0;

    while (start < c->in_len) {
        char *nl = memchr(c->inbuf + start, '\n', c->in_len - start);
        long delta;
        char tmp[MAX_LINE_LEN + 1];
        char response[64];
        char *end = NULL;
        size_t line_len;

        if (nl == NULL) {
            if (c->in_len - start > MAX_LINE_LEN) {
                return -1;
            }
            break;
        }

        line_len = (size_t)(nl - (c->inbuf + start));
        if (line_len > MAX_LINE_LEN) {
            return -1;
        }

        memcpy(tmp, c->inbuf + start, line_len);
        tmp[line_len] = '\0';

        if (line_len > 0 && tmp[line_len - 1] == '\r') {
            tmp[line_len - 1] = '\0';
        }

        errno = 0;
        delta = strtol(tmp, &end, 10);
        if (errno != 0 || end == tmp || *end != '\0') {
            return -1;
        }

        *state += delta;

        if (snprintf(response, sizeof(response), "%ld\n", *state) < 0) {
            return -1;
        }

        fprintf(logf, "RECV fd=%d line=%s\n", c->fd, tmp);
        fprintf(logf, "SEND fd=%d line=%s", c->fd, response);
        fflush(logf);

        if (queue_response(c, response) < 0) {
            return -1;
        }

        start += line_len + 1;
    }

    if (start > 0) {
        memmove(c->inbuf, c->inbuf + start, c->in_len - start);
        c->in_len -= start;
    }

    return 0;
}

static int flush_client_output(struct client *c) {
    while (c->out_off < c->out_len) {
        ssize_t sent = send(c->fd, c->outbuf + c->out_off, c->out_len - c->out_off, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            return -1;
        }
        if (sent == 0) {
            return -1;
        }
        c->out_off += (size_t)sent;
    }

    if (c->out_off == c->out_len) {
        free(c->outbuf);
        c->outbuf = NULL;
        c->out_len = 0;
        c->out_off = 0;
    }
    return 0;
}

int main(void) {
    char sock_name[108];
    char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct sockaddr_un addr;
    struct client *clients = malloc(sizeof(struct client) * MAX_CLIENTS);
    if (!clients) {
        fprintf(stderr, "Failed to allocate memory\n");
        return 1;
    }
    struct pollfd pfds[MAX_CLIENTS + 1];
    int listen_fd = -1;
    int i;
    int reuse = 1;
    long state = 0;
    FILE *logf = NULL;

    struct sigaction sa;
    struct sigaction sa_ignore;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    memset(&sa_ignore, 0, sizeof(sa_ignore));
    sa_ignore.sa_handler = SIG_IGN;
    sigemptyset(&sa_ignore.sa_mask);
    sigaction(SIGPIPE, &sa_ignore, NULL);

    if (read_socket_name(sock_name, sizeof(sock_name)) < 0) {
        perror("read config");
        free(clients);
        return 1;
    }
    if (snprintf(sock_path, sizeof(sock_path), "/tmp/%s", sock_name) >= (int)sizeof(sock_path)) {
        fprintf(stderr, "socket path too long\n");
        return 1;
    }

    logf = fopen(LOG_PATH, "a");
    if (logf == NULL) {
        perror("open log");
        return 1;
    }
    fprintf(logf, "START state=0\n");
    fflush(logf);

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        fclose(logf);
        return 1;
    }
    if (set_nonblocking(listen_fd) < 0) {
        perror("fcntl");
        close(listen_fd);
        fclose(logf);
        return 1;
    }
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
    }

    unlink(sock_path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        fclose(logf);
        return 1;
    }
    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(listen_fd);
        unlink(sock_path);
        fclose(logf);
        return 1;
    }

    for (i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].in_len = 0;
        clients[i].outbuf = NULL;
        clients[i].out_len = 0;
        clients[i].out_off = 0;
    }

    while (!g_stop) {
        int nfds = 1;
        pfds[0].fd = listen_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) {
                continue;
            }
            pfds[nfds].fd = clients[i].fd;
            pfds[nfds].events = POLLIN;
            if (clients[i].out_len > clients[i].out_off) {
                pfds[nfds].events |= POLLOUT;
            }
            pfds[nfds].revents = 0;
            nfds++;
        }

        if (poll(pfds, (nfds_t)nfds, 1000) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            if (logf != NULL) {
                fprintf(logf, "ERROR where=poll errno=%d\n", errno);
                fflush(logf);
            }
            break;
        }

        if (pfds[0].revents & POLLIN) {
            while (1) {
                int cfd = accept(listen_fd, NULL, NULL);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    perror("accept");
                    if (logf != NULL) {
                        fprintf(logf, "ERROR where=accept errno=%d\n", errno);
                        fflush(logf);
                    }
                    g_stop = 1;
                    break;
                }
                if (set_nonblocking(cfd) < 0) {
                    close(cfd);
                    continue;
                }
                for (i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0) {
                        clients[i].fd = cfd;
                        clients[i].in_len = 0;
                        clients[i].outbuf = NULL;
                        clients[i].out_len = 0;
                        clients[i].out_off = 0;
                        #ifdef __linux__
                        void *heap = sbrk(0);
                        if (heap != (void *)-1) {
                            fprintf(logf, "CONNECT fd=%d heap=%p\n", cfd, heap);
                        } else {
                            fprintf(logf, "CONNECT fd=%d heap=unknown\n", cfd);
                        }
                        #else
                        fprintf(logf, "CONNECT fd=%d heap=unsupported\n", cfd);
                        #endif
                        fflush(logf);
                        break;
                    }
                }
                if (i == MAX_CLIENTS) {
                    close(cfd);
                }
            }
        }

        nfds = 1;
        for (i = 0; i < MAX_CLIENTS; i++) {
            struct client *c;
            bool dead = false;
            bool peer_closed = false;
            char tmp[READ_CHUNK];

            if (clients[i].fd < 0) {
                continue;
            }
            c = &clients[i];

            if (pfds[nfds].revents & (POLLERR | POLLNVAL)) {
                close_client(c, logf);
                nfds++;
                continue;
            }
            if ((pfds[nfds].revents & POLLHUP) && !(pfds[nfds].revents & POLLIN)) {
                peer_closed = true;
            }

            if (pfds[nfds].revents & POLLIN) {
                while (1) {
                    ssize_t r = recv(c->fd, tmp, sizeof(tmp), 0);
                    if (r < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        dead = true;
                        break;
                    }
                    if (r == 0) {
                        peer_closed = true;
                        break;
                    }
                    if (c->in_len + (size_t)r > sizeof(c->inbuf)) {
                        dead = true;
                        break;
                    }
                    memcpy(c->inbuf + c->in_len, tmp, (size_t)r);
                    c->in_len += (size_t)r;
                    if (parse_and_queue_lines(c, &state, logf) < 0) {
                        dead = true;
                        break;
                    }
                }
            }

            if (!dead && (pfds[nfds].revents & POLLOUT)) {
                if (flush_client_output(c) < 0) {
                    dead = true;
                }
            }
            if (!dead && peer_closed && c->out_off == c->out_len) {
                dead = true;
            }

            if (dead) {
                close_client(c, logf);
            }
            nfds++;
        }
    }

    for (i = 0; i < MAX_CLIENTS; i++) {
        close_client(&clients[i], logf);
    }
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    unlink(sock_path);
    if (logf != NULL) {
        fprintf(logf, "STOP\n");
        fclose(logf);
    }
    free(clients);
    return 0;
}

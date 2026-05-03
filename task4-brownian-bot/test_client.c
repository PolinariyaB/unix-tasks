#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
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

static int connect_with_retry(void) {
    int delay_ms = 100;

    for (int i = 0; i < 10; i++) {
        int fd = connect_socket();
        if (fd >= 0) {
            return fd;
        }
        if (errno == ECONNREFUSED || errno == ENOENT) {
            struct timespec req;
            req.tv_sec = delay_ms / 1000;
            req.tv_nsec = (delay_ms % 1000) * 1000000L;
            while (nanosleep(&req, &req) < 0 && errno == EINTR) {
            }
            delay_ms *= 2;
            continue;
        }
        break;
    }

    return -1;
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

static int nanosleep_seconds(double sec) {
    struct timespec req;
    struct timespec rem;

    if (sec <= 0.0) {
        return 0;
    }

    req.tv_sec = (time_t)sec;
    req.tv_nsec = (long)((sec - (double)req.tv_sec) * 1e9);

    while (nanosleep(&req, &rem) < 0) {
        if (errno != EINTR) {
            return -1;
        }
        req = rem;
    }
    return 0;
}

static double now_seconds(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int read_line_fd(int fd, char *buf, size_t cap) {
    static char inbuf[RESPONSE_BUF_CAP];
    static size_t in_len = 0;
    static size_t in_off = 0;
    size_t len = 0;
    int retries = 0;

    while (len + 1 < cap) {
        if (in_off == in_len) {
            ssize_t r;

            do {
                r = read(fd, inbuf, sizeof(inbuf));
            } while (r < 0 && errno == EINTR);

            if (r < 0) {
                if ((errno == EAGAIN || errno == EWOULDBLOCK) && retries < 100) {
                    retries++;
                    struct timespec ts = {0, 1000000}; // 1ms
                    nanosleep(&ts, NULL);
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
            retries = 0;
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

int main(int argc, char **argv) {
    FILE *in = NULL;
    FILE *logf = NULL;
    int fd = -1;
    double delay_sec;
    double total_delay = 0.0;
    double started_at = 0.0;
    double finished_at = 0.0;
    int chunk_left = 1;
    int ch;
    char resp[256];
    unsigned int seed;
    int exit_code = 1;
    const char *error_msg = "";
    int saved_errno = 0;
    struct sigaction sa_ignore;

    memset(&sa_ignore, 0, sizeof(sa_ignore));
    sa_ignore.sa_handler = SIG_IGN;
    sigemptyset(&sa_ignore.sa_mask);
    sigaction(SIGPIPE, &sa_ignore, NULL);

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <numbers_file> <delay_seconds> <client_log>\n", argv[0]);
        return 1;
    }

    logf = fopen(argv[3], "w");
    if (logf == NULL) {
        perror("open client_log");
        return 1;
    }

    errno = 0;
    delay_sec = strtod(argv[2], NULL);
    if (delay_sec < 0.0) {
        fprintf(stderr, "delay must be >= 0\n");
        error_msg = "invalid_delay";
        saved_errno = errno;
        goto done;
    }

    in = fopen(argv[1], "rb");
    if (in == NULL) {
        perror("open numbers_file");
        error_msg = "open_numbers_file";
        saved_errno = errno;
        goto done;
    }

    fd = connect_with_retry();
    if (fd < 0) {
        perror("connect");
        error_msg = "connect_with_retry";
        saved_errno = errno;
        goto done;
    }

    seed = (unsigned int)(time(NULL) ^ getpid());
    started_at = now_seconds();
    chunk_left = (int)((rand_r(&seed) % 255U) + 1U);

    while ((ch = fgetc(in)) != EOF) {
        char c = (char)ch;

        if (write_all(fd, &c, 1) < 0) {
            perror("write");
            error_msg = "write_stream";
            saved_errno = errno;
            goto done;
        }

        if (c == '\n') {
            if (read_line_fd(fd, resp, sizeof(resp)) < 0) {
                perror("read response");
                error_msg = "read_response";
                saved_errno = errno;
                goto done;
            }
        }

        chunk_left--;
        if (chunk_left == 0) {
            if (nanosleep_seconds(delay_sec) < 0) {
                perror("nanosleep");
                error_msg = "nanosleep";
                saved_errno = errno;
                goto done;
            }
            total_delay += delay_sec;
            chunk_left = (int)((rand_r(&seed) % 255U) + 1U);
        }
    }

    if (ferror(in)) {
        perror("read numbers_file");
        error_msg = "read_numbers_file";
        saved_errno = errno;
        goto done;
    }

    finished_at = now_seconds();
    exit_code = 0;

done:
    if (finished_at == 0.0) {
        finished_at = now_seconds();
    }

    if (logf != NULL) {
        if (started_at == 0.0) {
            started_at = finished_at;
        }

        fprintf(logf, "total_delay=%.6f\n", total_delay);
        fprintf(logf, "started_at=%.6f\n", started_at);
        fprintf(logf, "finished_at=%.6f\n", finished_at);

        if (exit_code == 0) {
            fprintf(logf, "status=ok\n");
        } else {
            fprintf(logf, "status=error\n");
            fprintf(logf, "error=%s errno=%d\n", error_msg, saved_errno);
        }

        fclose(logf);
    }

    if (fd >= 0) {
        close(fd);
    }
    if (in != NULL) {
        fclose(in);
    }

    return exit_code;
}

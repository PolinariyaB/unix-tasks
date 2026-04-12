#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static volatile sig_atomic_t stop_flag;

static void handle_sigint(int signo)
{
    (void)signo;
    stop_flag = 1;
}

static int install_sigint_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        return -1;
    }
    return 0;
}

static ssize_t write_full(int fd, const void *buf, size_t count)
{
    const unsigned char *p = buf;
    size_t left = count;

    while (left > 0) {
        ssize_t n = write(fd, p, left);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return (ssize_t)count;
}

static ssize_t read_full(int fd, void *buf, size_t count)
{
    unsigned char *p = buf;
    size_t left = count;

    while (left > 0) {
        ssize_t n = read(fd, p, left);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return (ssize_t)(p - (unsigned char *)buf);
}

static int open_retry(const char *path, int flags, mode_t mode)
{
    for (;;) {
        int fd;

        if ((flags & O_CREAT) != 0) {
            fd = open(path, flags, mode);
        } else {
            fd = open(path, flags);
        }
        if (fd >= 0) {
            return fd;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

static void usleep_retry(useconds_t usec)
{
    if (usec == 0U) {
        return;
    }

    for (;;) {
        if (usleep(usec) == 0) {
            return;
        }
        if (errno == EINTR) {
            if (stop_flag) {
                return;
            }
            continue;
        }
        perror("usleep");
        exit(EXIT_FAILURE);
    }
}

static void sleep_interruptible(unsigned int seconds)
{
    unsigned int left = seconds;

    while (left > 0U && !stop_flag) {
        unsigned int r = sleep(left);

        left = r;
    }
}

static int try_create_lock_exclusive(const char *lckpath, int *fd_out)
{
    int fd = open_retry(lckpath, O_WRONLY | O_CREAT | O_EXCL, 0600);

    if (fd >= 0) {
        *fd_out = fd;
        return 0;
    }
    if (errno == EEXIST) {
        return 1;
    }
    perror("open");
    exit(EXIT_FAILURE);
}

static void write_pid_line(int fd, pid_t pid)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%jd\n", (intmax_t)pid);

    if (n < 0 || (size_t)n >= sizeof(buf)) {
        fprintf(stderr, "snprintf pid failed\n");
        exit(EXIT_FAILURE);
    }
    if (write_full(fd, buf, (size_t)n) < 0) {
        perror("write");
        exit(EXIT_FAILURE);
    }
}

static int close_retry(int fd)
{
    for (;;) {
        if (close(fd) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

static int unlink_retry(const char *path)
{
    for (;;) {
        if (unlink(path) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

/*
 * Expected on-disk format: ASCII decimal PID followed by a single '\n',
 * with no leading/trailing garbage.
 */
static int parse_pid_file_content(const char *data, size_t len, pid_t *out_pid)
{
    size_t i = 0;

    if (len == 0) {
        return -1;
    }

    while (i < len && data[i] >= '0' && data[i] <= '9') {
        i++;
    }
    if (i == 0) {
        return -1;
    }
    if (i >= len || data[i] != '\n') {
        return -1;
    }
    if (i + 1U != len) {
        return -1;
    }

    unsigned long long acc = 0ULL;

    for (size_t j = 0; j < i; j++) {
        acc = acc * 10ULL + (unsigned long long)(data[j] - '0');
        if (acc > (unsigned long long)INT_MAX) {
            return -1;
        }
    }
    if (acc == 0ULL) {
        return -1;
    }
    *out_pid = (pid_t)acc;
    return 0;
}

enum verify_result {
    VERIFY_OK = 0,
    VERIFY_VANISHED = 1,
    VERIFY_BAD_CONTENT = 2,
    VERIFY_PID_MISMATCH = 3,
};

static enum verify_result verify_lock_file_owner(const char *lckpath, pid_t self)
{
    unsigned char buf[128];
    int fd = open_retry(lckpath, O_RDONLY, 0);

    if (fd < 0) {
        if (errno == ENOENT) {
            return VERIFY_VANISHED;
        }
        perror("open");
        exit(EXIT_FAILURE);
    }

    ssize_t n = read_full(fd, buf, sizeof(buf));

    if (n < 0) {
        (void)close_retry(fd);
        perror("read");
        exit(EXIT_FAILURE);
    }

    for (;;) {
        unsigned char scratch[64];
        ssize_t m = read(fd, scratch, sizeof(scratch));

        if (m < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close_retry(fd);
            perror("read");
            exit(EXIT_FAILURE);
        }
        if (m == 0) {
            break;
        }
        (void)close_retry(fd);
        return VERIFY_BAD_CONTENT;
    }

    if (close_retry(fd) != 0) {
        perror("close");
        exit(EXIT_FAILURE);
    }

    pid_t file_pid;
    int prc = parse_pid_file_content((const char *)buf, (size_t)n, &file_pid);

    if (prc != 0) {
        return VERIFY_BAD_CONTENT;
    }
    if (file_pid != self) {
        return VERIFY_PID_MISMATCH;
    }
    return VERIFY_OK;
}

static void write_stats_line_atomic_append(const char *stats_path, pid_t pid, uint64_t locks)
{
    char line[128];
    int n = snprintf(line, sizeof(line), "PID=%jd LOCKS=%" PRIu64 "\n", (intmax_t)pid, locks);

    if (n < 0 || (size_t)n >= sizeof(line)) {
        fprintf(stderr, "snprintf stats failed\n");
        exit(EXIT_FAILURE);
    }

    int fd = open_retry(stats_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (fd < 0) {
        perror("open");
        exit(EXIT_FAILURE);
    }
    if (write_full(fd, line, (size_t)n) < 0) {
        perror("write");
        (void)close_retry(fd);
        exit(EXIT_FAILURE);
    }
    if (close_retry(fd) != 0) {
        perror("close");
        exit(EXIT_FAILURE);
    }
}

static void build_lock_path(char *out, size_t out_sz, const char *myfile)
{
    int n = snprintf(out, out_sz, "%s.lck", myfile);

    if (n < 0 || (size_t)n >= out_sz) {
        fprintf(stderr, "lock path too long\n");
        exit(EXIT_FAILURE);
    }
}

static void usage(FILE *fp, const char *argv0)
{
    fprintf(fp, "usage: %s [-s stats.txt] [-d delay_ms] myfile\n", argv0);
}

int main(int argc, char **argv)
{
    const char *stats_path = "stats.txt";
    unsigned int delay_ms = 100U;
    int opt;

    while ((opt = getopt(argc, argv, "s:d:")) != -1) {
        switch (opt) {
        case 's':
            stats_path = optarg;
            break;
        case 'd': {
            char *end = NULL;
            unsigned long v;

            errno = 0;
            v = strtoul(optarg, &end, 10);
            if (errno != 0 || end == optarg || *end != '\0' || v > 1000000UL) {
                fprintf(stderr, "invalid -d value\n");
                usage(stderr, argv[0]);
                return EXIT_FAILURE;
            }
            delay_ms = (unsigned int)v;
            break;
        }
        default:
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind + 1 != argc) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    const char *myfile = argv[optind];
    char lckpath[PATH_MAX];

    build_lock_path(lckpath, sizeof(lckpath), myfile);

    stop_flag = 0;
    if (install_sigint_handler() != 0) {
        return EXIT_FAILURE;
    }

    const pid_t self = getpid();
    uint64_t locks = 0;

    for (;;) {
        if (stop_flag) {
            break;
        }

        int lock_fd;
        int trc = try_create_lock_exclusive(lckpath, &lock_fd);

        if (trc != 0) {
            unsigned int delay_with_jitter = delay_ms + (unsigned int)(self % 17U);
            useconds_t usec = (useconds_t)delay_with_jitter * 1000U;

            usleep_retry(usec);
            continue;
        }

        write_pid_line(lock_fd, self);
        if (close_retry(lock_fd) != 0) {
            perror("close");
            exit(EXIT_FAILURE);
        }

        sleep_interruptible(1U);

        enum verify_result vr = verify_lock_file_owner(lckpath, self);

        if (vr == VERIFY_VANISHED) {
            if (stop_flag) {
                break;
            }
            continue;
        }
        if (vr == VERIFY_BAD_CONTENT) {
            fprintf(stderr, "lock file has unexpected contents\n");
            exit(EXIT_FAILURE);
        }
        if (vr == VERIFY_PID_MISMATCH) {
            fprintf(stderr, "lock file owned by a different pid; refusing to unlink\n");
            exit(EXIT_FAILURE);
        }

        if (unlink_retry(lckpath) != 0) {
            if (errno == ENOENT) {
                if (stop_flag) {
                    break;
                }
                continue;
            }
            perror("unlink");
            exit(EXIT_FAILURE);
        }

        locks++;

        if (!stop_flag) {
            unsigned int delay_with_jitter = delay_ms + (unsigned int)(self % 17U);

            usleep_retry((useconds_t)delay_with_jitter * 1000U);
        }

        if (stop_flag) {
            break;
        }
    }

    write_stats_line_atomic_append(stats_path, self, locks);
    return EXIT_SUCCESS;
}

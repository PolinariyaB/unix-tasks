#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOG_PATH "/tmp/myinit.log"
#define POLL_NS 100000000L

static int log_fd = -1;
static char *g_config_path = NULL;

static volatile sig_atomic_t reload_requested;
static volatile sig_atomic_t shutdown_requested;

static int reloading;
static int shutting_down;

struct config_entry {
    char **argv;
    int argc;
    char *stdin_path;
    char *stdout_path;
    char *cmd_for_log;
};

struct config {
    struct config_entry *entries;
    size_t count;
};

static struct config g_config;
static pid_t *g_children;

static void handle_sighup(int signo)
{
    (void)signo;
    reload_requested = 1;
}

static void handle_shutdown(int signo)
{
    (void)signo;
    shutdown_requested = 1;
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

static void log_fmt(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    int n;

    if (log_fd < 0) {
        return;
    }

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(buf)) {
        (void)write_full(log_fd, "log_fmt: message too long\n", 26);
        return;
    }
    if (write_full(log_fd, buf, (size_t)n) < 0) {
        /* best effort */
    }
}

static int is_absolute_path(const char *path)
{
    return path != NULL && path[0] == '/';
}

static int ignore_job_control_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    if (getppid() != (pid_t)1) {
        if (sigaction(SIGTTOU, &sa, NULL) == -1) {
            return -1;
        }
        if (sigaction(SIGTTIN, &sa, NULL) == -1) {
            return -1;
        }
        if (sigaction(SIGTSTP, &sa, NULL) == -1) {
            return -1;
        }
    }
    return 0;
}

static int close_all_fds(void)
{
    struct rlimit rl;
    rlim_t fd;
    rlim_t maxfd;

    if (getrlimit(RLIMIT_NOFILE, &rl) == -1) {
        return -1;
    }
    maxfd = rl.rlim_max;
    if (maxfd == RLIM_INFINITY) {
        maxfd = 4096;
    }
    if (maxfd > 65536) {
        maxfd = 65536;
    }
    for (fd = 0; fd < maxfd; fd++) {
        (void)close((int)fd);
    }
    return 0;
}

static int attach_dev_null_stdio(void)
{
    int d = open_retry("/dev/null", O_RDWR, 0);

    if (d < 0) {
        return -1;
    }
    if (dup2(d, STDIN_FILENO) == -1) {
        return -1;
    }
    if (dup2(d, STDOUT_FILENO) == -1) {
        return -1;
    }
    if (dup2(d, STDERR_FILENO) == -1) {
        return -1;
    }
    if (d > STDERR_FILENO) {
        (void)close(d);
    }
    return 0;
}

static int open_log_file(void)
{
    log_fd = open_retry(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    return log_fd >= 0 ? 0 : -1;
}

static void write_pidfile_if_requested(void)
{
    const char *path = getenv("MYINIT_PIDFILE");
    char line[64];
    int fd;
    int n;

    if (path == NULL || path[0] == '\0') {
        return;
    }
    if (!is_absolute_path(path)) {
        return;
    }
    fd = open_retry(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_fmt("ERROR pidfile open %s errno=%d\n", path, errno);
        return;
    }
    n = snprintf(line, sizeof(line), "%d\n", (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(line)) {
        (void)close(fd);
        return;
    }
    if (write_full(fd, line, (size_t)n) < 0) {
        /* ignore */
    }
    (void)close(fd);
}

static int daemonize(void)
{
    pid_t pid;

    if (ignore_job_control_signals() != 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        _exit(EXIT_SUCCESS);
    }

    if (setsid() == -1) {
        return -1;
    }

    if (close_all_fds() != 0) {
        return -1;
    }

    if (chdir("/") == -1) {
        return -1;
    }

    if (attach_dev_null_stdio() != 0) {
        return -1;
    }

    if (open_log_file() != 0) {
        return -1;
    }

    write_pidfile_if_requested();
    return 0;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sighup;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGHUP, &sa, NULL) == -1) {
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_shutdown;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        return -1;
    }
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        return -1;
    }

    return 0;
}

static void free_config(struct config *cfg)
{
    size_t i;

    if (cfg->entries == NULL) {
        return;
    }
    for (i = 0; i < cfg->count; i++) {
        int j;

        for (j = 0; j < cfg->entries[i].argc; j++) {
            free(cfg->entries[i].argv[j]);
        }
        free(cfg->entries[i].argv);
        free(cfg->entries[i].stdin_path);
        free(cfg->entries[i].stdout_path);
        free(cfg->entries[i].cmd_for_log);
    }
    free(cfg->entries);
    cfg->entries = NULL;
    cfg->count = 0;
}

static int validate_entry_paths(const struct config_entry *e)
{
    if (!is_absolute_path(e->stdin_path) || !is_absolute_path(e->stdout_path)) {
        return -1;
    }
    if (e->argc < 1 || !is_absolute_path(e->argv[0])) {
        return -1;
    }
    return 0;
}

static char *build_cmd_for_log(const struct config_entry *e)
{
    size_t need = 1;
    char *out;
    size_t pos;
    int i;

    for (i = 0; i < e->argc; i++) {
        need += strlen(e->argv[i]) + 1U;
    }
    out = malloc(need);
    if (out == NULL) {
        return NULL;
    }
    pos = 0;
    for (i = 0; i < e->argc; i++) {
        size_t len = strlen(e->argv[i]);

        if (pos > 0) {
            out[pos++] = ' ';
        }
        memcpy(out + pos, e->argv[i], len);
        pos += len;
    }
    out[pos] = '\0';
    return out;
}

static int push_config_line(struct config *cfg, char *line)
{
    char *saveptr = NULL;
    char *tok;
    char *tokens[256];
    int nt = 0;
    struct config_entry ent;
    size_t newcap;
    struct config_entry *newentries;
    int j;

    memset(&ent, 0, sizeof(ent));

    for (tok = strtok_r(line, " \t", &saveptr); tok != NULL;
         tok = strtok_r(NULL, " \t", &saveptr)) {
        if (nt >= 256) {
            return -1;
        }
        tokens[nt++] = tok;
    }
    if (nt < 3) {
        return 0;
    }

    ent.stdin_path = strdup(tokens[nt - 2]);
    ent.stdout_path = strdup(tokens[nt - 1]);
    if (ent.stdin_path == NULL || ent.stdout_path == NULL) {
        free(ent.stdin_path);
        free(ent.stdout_path);
        return -1;
    }

    ent.argc = nt - 2;
    ent.argv = calloc((size_t)ent.argc + 1U, sizeof(char *));
    if (ent.argv == NULL) {
        free(ent.stdin_path);
        free(ent.stdout_path);
        return -1;
    }
    for (j = 0; j < ent.argc; j++) {
        ent.argv[j] = strdup(tokens[j]);
        if (ent.argv[j] == NULL) {
            int k;

            for (k = 0; k < j; k++) {
                free(ent.argv[k]);
            }
            free(ent.argv);
            free(ent.stdin_path);
            free(ent.stdout_path);
            return -1;
        }
    }
    ent.argv[ent.argc] = NULL;

    ent.cmd_for_log = build_cmd_for_log(&ent);
    if (ent.cmd_for_log == NULL) {
        for (j = 0; j < ent.argc; j++) {
            free(ent.argv[j]);
        }
        free(ent.argv);
        free(ent.stdin_path);
        free(ent.stdout_path);
        return -1;
    }

    if (validate_entry_paths(&ent) != 0) {
        free(ent.cmd_for_log);
        for (j = 0; j < ent.argc; j++) {
            free(ent.argv[j]);
        }
        free(ent.argv);
        free(ent.stdin_path);
        free(ent.stdout_path);
        return -1;
    }

    newcap = (cfg->count + 1U) * sizeof(struct config_entry);
    newentries = realloc(cfg->entries, newcap);
    if (newentries == NULL) {
        free(ent.cmd_for_log);
        for (j = 0; j < ent.argc; j++) {
            free(ent.argv[j]);
        }
        free(ent.argv);
        free(ent.stdin_path);
        free(ent.stdout_path);
        return -1;
    }
    cfg->entries = newentries;
    cfg->entries[cfg->count] = ent;
    cfg->count++;
    return 0;
}

static int parse_config_file(const char *path, struct config *cfg)
{
    int fd;
    struct stat st;
    char *buf;
    size_t pos;
    size_t line_start;

    fd = open_retry(path, O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) == -1) {
        (void)close(fd);
        return -1;
    }
    if (st.st_size < 0 || st.st_size > (off_t)(16 * 1024 * 1024)) {
        (void)close(fd);
        errno = EFBIG;
        return -1;
    }
    buf = malloc((size_t)st.st_size + 1U);
    if (buf == NULL) {
        free_config(cfg);
        (void)close(fd);
        return -1;
    }
    pos = 0;
    while (pos < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + pos, (size_t)st.st_size - pos);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free_config(cfg);
            free(buf);
            (void)close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        pos += (size_t)n;
    }
    (void)close(fd);
    buf[pos] = '\0';

    line_start = 0;
    for (pos = 0; pos <= (size_t)st.st_size; pos++) {
        if (buf[pos] == '\n' || buf[pos] == '\0') {
            buf[pos] = '\0';
            while (line_start < pos && (buf[line_start] == ' ' || buf[line_start] == '\t')) {
                line_start++;
            }
            if (line_start < pos && buf[line_start] != '#') {
                if (push_config_line(cfg, buf + line_start) != 0) {
                    free(buf);
                    free_config(cfg);
                    return -1;
                }
            }
            line_start = pos + 1U;
        }
    }
    free(buf);
    return 0;
}

static size_t find_child_index(pid_t pid)
{
    size_t i;

    for (i = 0; i < g_config.count; i++) {
        if (g_children[i] == pid) {
            return i;
        }
    }
    return (size_t)-1;
}

static size_t count_running_children(void)
{
    size_t i;
    size_t c = 0;

    for (i = 0; i < g_config.count; i++) {
        if (g_children[i] != 0) {
            c++;
        }
    }
    return c;
}

static void log_exit_status(size_t idx, pid_t pid, int status)
{
    if (WIFEXITED(status)) {
        log_fmt("EXIT idx=%zu pid=%d code=%d\n", idx, (int)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        log_fmt("EXIT idx=%zu pid=%d signal=%d\n", idx, (int)pid, WTERMSIG(status));
    } else {
        log_fmt("EXIT idx=%zu pid=%d unknown\n", idx, (int)pid);
    }
}

static void log_stop_status(size_t idx, pid_t pid, int status)
{
    if (WIFEXITED(status)) {
        log_fmt("STOP idx=%zu pid=%d code=%d\n", idx, (int)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        log_fmt("STOP idx=%zu pid=%d signal=%d\n", idx, (int)pid, WTERMSIG(status));
    } else {
        log_fmt("STOP idx=%zu pid=%d unknown\n", idx, (int)pid);
    }
}

static int spawn_child_at(size_t idx, pid_t oldpid_for_restart)
{
    const struct config_entry *e = &g_config.entries[idx];
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        log_fmt("ERROR fork errno=%d\n", errno);
        return -1;
    }
    if (pid == 0) {
        int infd;
        int outfd;

        infd = open_retry(e->stdin_path, O_RDONLY, 0);
        if (infd < 0) {
            _exit(126);
        }
        outfd = open_retry(e->stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outfd < 0) {
            _exit(126);
        }
        if (dup2(infd, STDIN_FILENO) == -1) {
            _exit(126);
        }
        if (dup2(outfd, STDOUT_FILENO) == -1) {
            _exit(126);
        }
        if (dup2(outfd, STDERR_FILENO) == -1) {
            _exit(126);
        }
        if (infd > STDERR_FILENO) {
            (void)close(infd);
        }
        if (outfd > STDERR_FILENO && outfd != infd) {
            (void)close(outfd);
        }
        (void)execv(e->argv[0], e->argv);
        _exit(127);
    }

    if (oldpid_for_restart != 0) {
        log_fmt("RESTART idx=%zu oldpid=%d newpid=%d\n", idx, (int)oldpid_for_restart, (int)pid);
    }
    log_fmt("START idx=%zu pid=%d cmd=%s\n", idx, (int)pid, e->cmd_for_log);
    g_children[idx] = pid;
    return 0;
}

static int spawn_all_children(void)
{
    size_t i;

    for (i = 0; i < g_config.count; i++) {
        if (g_children[i] != 0) {
            continue;
        }
        if (spawn_child_at(i, 0) != 0) {
            return -1;
        }
    }
    return 0;
}

static void send_sigterm_to_all_children(void)
{
    size_t i;

    for (i = 0; i < g_config.count; i++) {
        if (g_children[i] != 0) {
            (void)kill(g_children[i], SIGTERM);
        }
    }
}

static void reap_children_until_none_blocking(void)
{
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        size_t idx;

        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                break;
            }
            log_fmt("ERROR waitpid errno=%d\n", errno);
            break;
        }
        idx = find_child_index(pid);
        if (idx == (size_t)-1) {
            log_fmt("WARN unknown child pid=%d\n", (int)pid);
            continue;
        }
        g_children[idx] = 0;
        if (reloading || shutting_down) {
            log_stop_status(idx, pid, status);
        } else {
            log_exit_status(idx, pid, status);
        }
    }
}

static int reload_config(void)
{
    sigset_t block, old;

    sigemptyset(&block);
    sigaddset(&block, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &block, &old) == -1) {
        log_fmt("ERROR sigprocmask errno=%d\n", errno);
        return -1;
    }

    reloading = 1;
    log_fmt("RELOAD requested\n");

    send_sigterm_to_all_children();
    reap_children_until_none_blocking();

    free_config(&g_config);
    memset(&g_config, 0, sizeof(g_config));

    if (parse_config_file(g_config_path, &g_config) != 0) {
        log_fmt("ERROR parse_config errno=%d\n", errno);
        reloading = 0;
        (void)sigprocmask(SIG_SETMASK, &old, NULL);
        return -1;
    }
    if (g_config.count == 0U) {
        log_fmt("ERROR empty config after reload\n");
        reloading = 0;
        (void)sigprocmask(SIG_SETMASK, &old, NULL);
        return -1;
    }

    free(g_children);
    g_children = calloc(g_config.count, sizeof(pid_t));
    if (g_children == NULL) {
        log_fmt("ERROR calloc children\n");
        reloading = 0;
        (void)sigprocmask(SIG_SETMASK, &old, NULL);
        return -1;
    }

    if (spawn_all_children() != 0) {
        reloading = 0;
        (void)sigprocmask(SIG_SETMASK, &old, NULL);
        return -1;
    }

    reloading = 0;
    if (sigprocmask(SIG_SETMASK, &old, NULL) == -1) {
        log_fmt("ERROR sigprocmask restore errno=%d\n", errno);
        return -1;
    }

    return 0;
}

static void shutdown_myinit(void)
{
    shutting_down = 1;
    log_fmt("SHUTDOWN requested\n");
    send_sigterm_to_all_children();
    reap_children_until_none_blocking();
}

static void short_sleep_poll(void)
{
    struct timespec req;
    struct timespec rem;

    req.tv_sec = 0;
    req.tv_nsec = POLL_NS;
    rem.tv_sec = 0;
    rem.tv_nsec = 0;

    for (;;) {
        if (reload_requested || shutdown_requested) {
            return;
        }
        if (nanosleep(&req, &rem) == 0) {
            return;
        }
        if (errno != EINTR) {
            return;
        }
        req = rem;
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s /absolute/path/to/config.txt\n", argv0);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (!is_absolute_path(argv[1])) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    g_config_path = strdup(argv[1]);
    if (g_config_path == NULL) {
        perror("strdup");
        return EXIT_FAILURE;
    }

    if (daemonize() != 0) {
        /* cannot log yet */
        return EXIT_FAILURE;
    }

    log_fmt("myinit started config=%s\n", g_config_path);

    if (install_signal_handlers() != 0) {
        log_fmt("ERROR sigaction errno=%d\n", errno);
        return EXIT_FAILURE;
    }

    memset(&g_config, 0, sizeof(g_config));
    if (parse_config_file(g_config_path, &g_config) != 0) {
        log_fmt("ERROR initial parse_config errno=%d\n", errno);
        return EXIT_FAILURE;
    }
    if (g_config.count == 0) {
        log_fmt("ERROR empty config\n");
        return EXIT_FAILURE;
    }

    g_children = calloc(g_config.count, sizeof(pid_t));
    if (g_children == NULL) {
        log_fmt("ERROR calloc children\n");
        return EXIT_FAILURE;
    }

    if (spawn_all_children() != 0) {
        return EXIT_FAILURE;
    }

    for (;;) {
        int status;
        pid_t pid;
        size_t idx;

        if (shutdown_requested) {
            shutdown_myinit();
            break;
        }
        if (reload_requested) {
            reload_requested = 0;
            if (reload_config() != 0) {
                shutdown_myinit();
                break;
            }
            continue;
        }

        pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            idx = find_child_index(pid);
            if (idx == (size_t)-1) {
                log_fmt("WARN unknown child pid=%d\n", (int)pid);
                continue;
            }
            g_children[idx] = 0;
            if (reloading || shutting_down) {
                log_stop_status(idx, pid, status);
            } else {
                pid_t oldp = pid;

                log_exit_status(idx, pid, status);
                if (spawn_child_at(idx, oldp) != 0) {
                    log_fmt("ERROR restart failed idx=%zu\n", idx);
                    shutdown_myinit();
                    break;
                }
            }
            continue;
        }
        if (pid < 0) {
            if (errno == ECHILD) {
                if (g_config.count > 0 && count_running_children() == 0U) {
                    log_fmt("WARN no children running; respawning all\n");
                    if (spawn_all_children() != 0) {
                        shutdown_myinit();
                        break;
                    }
                }
            } else if (errno != EINTR) {
                log_fmt("ERROR waitpid errno=%d\n", errno);
                break;
            }
        }

        short_sleep_poll();
    }

    free_config(&g_config);
    free(g_children);
    free(g_config_path);
    if (log_fd >= 0) {
        (void)close(log_fd);
    }
    return EXIT_SUCCESS;
}

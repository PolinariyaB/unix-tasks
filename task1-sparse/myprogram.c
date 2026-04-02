#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>


#define DEFAULT_BLOCK_SIZE 4096

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static ssize_t full_write(int fd, const void *buf, size_t count) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t written = 0;

    while (written < count) {
        ssize_t rc = write(fd, p + written, count - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            errno = EIO;
            return -1;
        }
        written += (size_t)rc;
    }

    return (ssize_t)written;
}

static int block_is_all_zeroes(const unsigned char *buf, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i) {
        if (buf[i] != 0) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char *argv[]) {
    int in_fd = STDIN_FILENO;
    int out_fd;
    const char *in_path = NULL;
    const char *out_path;
    size_t block_size = DEFAULT_BLOCK_SIZE;
    unsigned char *buf;
    off_t logical_size = 0;

    {
        int opt;
        while ((opt = getopt(argc, argv, "b:")) != -1) {
            switch (opt) {
                case 'b': {
                    char *endptr = NULL;
                    unsigned long parsed;

                    errno = 0;
                    parsed = strtoul(optarg, &endptr, 10);
                    if (errno != 0 || endptr == optarg || *endptr != '\0' || parsed == 0) {
                        fprintf(stderr, "Invalid block size: %s\n", optarg);
                        return EXIT_FAILURE;
                    }
                    block_size = (size_t)parsed;
                    break;
                }
                default:
                    fprintf(stderr, "Usage: %s [-b block_size] <out_file>\n", argv[0]);
                    fprintf(stderr, "   or: %s [-b block_size] <in_file> <out_file>\n", argv[0]);
                    return EXIT_FAILURE;
            }
        }

        if (argc - optind == 1) {
            out_path = argv[optind];
        } else if (argc - optind == 2) {
            in_path = argv[optind];
            out_path = argv[optind + 1];
        } else {
            fprintf(stderr, "Usage: %s [-b block_size] <out_file>\n", argv[0]);
            fprintf(stderr, "   or: %s [-b block_size] <in_file> <out_file>\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (in_path != NULL) {
        in_fd = open(in_path, O_RDONLY);
        if (in_fd < 0) {
            die("open input");
        }
    }

    out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        if (in_path != NULL) {
            close(in_fd);
        }
        die("open output");
    }

    buf = (unsigned char *)malloc(block_size);
    if (buf == NULL) {
        if (in_path != NULL) {
            close(in_fd);
        }
        close(out_fd);
        die("malloc");
    }

    for (;;) {
        ssize_t rc = read(in_fd, buf, block_size);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            if (in_path != NULL) {
                close(in_fd);
            }
            close(out_fd);
            die("read");
        }
        if (rc == 0) {
            break;
        }

        if (block_is_all_zeroes(buf, (size_t)rc)) {
            if (lseek(out_fd, rc, SEEK_CUR) == (off_t)-1) {
                free(buf);
                if (in_path != NULL) {
                    close(in_fd);
                }
                close(out_fd);
                die("lseek");
            }
        } else {
            if (full_write(out_fd, buf, (size_t)rc) < 0) {
                free(buf);
                if (in_path != NULL) {
                    close(in_fd);
                }
                close(out_fd);
                die("write");
            }
        }
        logical_size += (off_t)rc;
    }

    if (ftruncate(out_fd, logical_size) < 0) {
        free(buf);
        if (in_path != NULL) {
            close(in_fd);
        }
        close(out_fd);
        die("ftruncate");
    }

    free(buf);
    if (in_path != NULL) {
        if (close(in_fd) < 0) {
            close(out_fd);
            die("close input");
        }
    }
    if (close(out_fd) < 0) {
        die("close output");
    }

    return EXIT_SUCCESS;
}

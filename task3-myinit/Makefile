CC=gcc
CFLAGS=-Wall -Wextra -Werror -std=c11 -D_DEFAULT_SOURCE

TARGETS=myinit test_worker

.PHONY: all clean

all: $(TARGETS)

myinit: myinit.c
	$(CC) $(CFLAGS) -o $@ myinit.c

test_worker: test_worker.c
	$(CC) $(CFLAGS) -o $@ test_worker.c

clean:
	rm -f $(TARGETS)

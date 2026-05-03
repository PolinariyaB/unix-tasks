CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11

TARGETS = server client test_client

all: $(TARGETS)

server: server.c
	$(CC) $(CFLAGS) -o $@ $<

client: client.c
	$(CC) $(CFLAGS) -o $@ $<

test_client: test_client.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS) *.o result.txt

.PHONY: all clean

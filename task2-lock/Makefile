CC=gcc
CFLAGS=-Wall -Wextra -Werror -std=c11 -D_DEFAULT_SOURCE

TARGET=lockprog

.PHONY: all clean binary

all: $(TARGET)

binary: $(TARGET)

$(TARGET): lockprog.c
	$(CC) $(CFLAGS) -o $@ lockprog.c

clean:
	rm -f $(TARGET)

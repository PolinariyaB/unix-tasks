#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        return EXIT_FAILURE;
    }

    (void)printf("%s pid=%d\n", argv[1], (int)getpid());
    (void)fflush(stdout);

    for (;;) {
        (void)pause();
    }
}

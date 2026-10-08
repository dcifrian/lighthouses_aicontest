/* Minimal bot for engine benchmarks: answers every turn with a random
   one-cell move, using almost no CPU or memory itself.
   Build: cc -O2 -o fastbot tools/bots/fastbot.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *line;
static size_t cap;

static int readline_(void) { return getline(&line, &cap, stdin) > 0; }

int main(int argc, char **argv) {
    srand(argc > 1 ? atoi(argv[1]) : 1);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!readline_()) return 0;
    printf("{\"name\": \"fastbot%s\"}\n", argc > 1 ? argv[1] : "");
    while (readline_()) {
        printf("{\"command\": \"move\", \"x\": %d, \"y\": %d}\n", rand() % 3 - 1, rand() % 3 - 1);
        if (!readline_()) break;
    }
    return 0;
}

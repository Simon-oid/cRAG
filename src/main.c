#include <stdio.h>
#include <string.h>
#include "crag.h"

int cmd_index(int argc, char **argv);
int cmd_query(int argc, char **argv);

static void usage(const char *prog) {
    fprintf(stderr, "cRAG %s\n", CRAG_VERSION);
    fprintf(stderr, "usage: %s <command> [args]\n", prog);
    fprintf(stderr, "commands:\n");
    fprintf(stderr, "  index <dir>   [--backend openai|local] [--db path] [--url url]\n");
    fprintf(stderr, "  query <text>  [--backend openai|local] [--db path] [--top-k N] [--generate]\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "index") == 0) return cmd_index(argc - 2, argv + 2);
    if (strcmp(argv[1], "query") == 0) return cmd_query(argc - 2, argv + 2);
    usage(argv[0]);
    return 1;
}

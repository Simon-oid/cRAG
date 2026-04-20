#include <stdio.h>
#include <string.h>

#include "crag.h"

static void usage(const char *prog) {
    fprintf(stderr, "cRAG %s\n", CRAG_VERSION);
    fprintf(stderr, "usage: %s <command> [args]\n", prog);
    fprintf(stderr, "commands:\n");
    fprintf(stderr, "  index <dir>     build index from documents in <dir>\n");
    fprintf(stderr, "  query <text>    retrieve top-k chunks for <text>\n");
}

static int cmd_index(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "index: not implemented\n");
    return 1;
}

static int cmd_query(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "query: not implemented\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "index") == 0) return cmd_index(argc - 2, argv + 2);
    if (strcmp(argv[1], "query") == 0) return cmd_query(argc - 2, argv + 2);
    usage(argv[0]);
    return 1;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "crag.h"
#include "store.h"

int cmd_index(int argc, char **argv);
int cmd_query(int argc, char **argv);

const char *crag_errstr(int err) {
    switch (err) {
        case CRAG_OK:            return "ok";
        case CRAG_EINVAL:        return "invalid argument";
        case CRAG_ENOMEM:        return "out of memory";
        case CRAG_EIO:           return "I/O error";
        case CRAG_EPARSE:        return "parse error";
        case CRAG_EHTTP:         return "HTTP error";
        case CRAG_EDIM_MISMATCH: return "embedding dim mismatch";
        case CRAG_EDB:           return "database error";
        case CRAG_ECONTEXT:      return "input exceeds embedder context";
        default:                 return "unknown error";
    }
}

static int cmd_stats(int argc, char **argv) {
    const char *db_path = NULL;
    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "--db") == 0 || strcmp(argv[i], "-i") == 0) && i+1 < argc)
            db_path = argv[++i];
    }
    if (!db_path) db_path = getenv("CRAG_DB");
    if (!db_path) db_path = "./crag.db";

    crag_store_t store;
    if (store_open(db_path, &store) != 0) return 3;

    crag_store_stats_t stats;
    if (store_stats(&store, &stats) != 0) {
        fprintf(stderr, "stats: query failed\n");
        store_close(&store);
        return 1;
    }

    printf("db:     %s\n", db_path);
    printf("chunks: %d\n", stats.chunks);
    printf("files:  %d\n", stats.files);
    printf("dim:    %d\n", stats.dim);

    store_close(&store);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "cRAG %s\n", CRAG_VERSION);
    fprintf(stderr, "usage: %s <command> [args]\n\n", prog);
    fprintf(stderr, "commands:\n");
    fprintf(stderr, "  index <dir>  [--db path] [--ext txt,md] [--backend local|openai]\n");
    fprintf(stderr, "               [--max-bytes N] [--url url] [--force]\n");
    fprintf(stderr, "  query <text> [--db path] [-k N] [--format text|json] [--generate]\n");
    fprintf(stderr, "               [--search cosine|bm25|hybrid] [--backend local|openai] [--url url]\n");
    fprintf(stderr, "  stats        [--db path]\n");
    fprintf(stderr, "\nenv vars:\n");
    fprintf(stderr, "  CRAG_DB              default db path\n");
    fprintf(stderr, "  CRAG_EMBED_BACKEND   local|openai (default: local)\n");
    fprintf(stderr, "  CRAG_EMBED_URL       embedding endpoint\n");
    fprintf(stderr, "  OPENAI_API_KEY       for openai backend\n");
    fprintf(stderr, "  CRAG_GEN_BACKEND     anthropic|openai|ollama|local (default: local)\n");
    fprintf(stderr, "  CRAG_GEN_URL         generation endpoint\n");
    fprintf(stderr, "  CRAG_GEN_MODEL       model name\n");
    fprintf(stderr, "  CRAG_GEN_MIN_SCORE   min top-1 cosine to call model (default 0.60)\n");
    fprintf(stderr, "  ANTHROPIC_API_KEY    for anthropic backend\n");
}

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int rc = 0;
    if (argc < 2) { usage(argv[0]); rc = 1; goto done; }
    if (strcmp(argv[1], "index")     == 0) { rc = cmd_index(argc-2, argv+2); goto done; }
    if (strcmp(argv[1], "query")     == 0) { rc = cmd_query(argc-2, argv+2); goto done; }
    if (strcmp(argv[1], "stats")     == 0) { rc = cmd_stats(argc-2, argv+2); goto done; }
    if (strcmp(argv[1], "--version") == 0 ||
        strcmp(argv[1], "-v")        == 0) { printf("cRAG %s\n", CRAG_VERSION); goto done; }
    usage(argv[0]); rc = 2;
done:
    curl_global_cleanup();
    return rc;
}

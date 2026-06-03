#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rerank.h"

/* forward declaration from rerank_llamacpp.c */
crag_rerank_backend_t *rerank_llamacpp_create(const char *url);

crag_rerank_backend_t *rerank_backend_create(const char *name, const char *url) {
    if (!name || strcmp(name, "llamacpp") == 0 || strcmp(name, "local") == 0)
        return rerank_llamacpp_create(url);
    fprintf(stderr, "rerank: unknown backend '%s'\n", name);
    return NULL;
}

void rerank_backend_free(crag_rerank_backend_t *b) {
    if (!b) return;
    free(b->ctx);
    free(b);
}

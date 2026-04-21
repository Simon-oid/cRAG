#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embed.h"
#include "http.h"
#include "util.h"

#define DEFAULT_URL "http://localhost:8080/embedding"

typedef struct {
    char url[512];
} local_ctx_t;

static int embed_one_local(const char *text, float *vec, int *dim, void *ctx_) {
    local_ctx_t *ctx = ctx_;

    char *escaped = json_escape_dup(text);
    if (!escaped) return -1;

    size_t body_sz = strlen(escaped) + 32;
    char *body = malloc(body_sz);
    if (!body) { free(escaped); return -1; }
    snprintf(body, body_sz, "{\"content\":\"%s\"}", escaped);
    free(escaped);

    http_buf_t buf = {0};
    int status = http_post_json(ctx->url, NULL, body, &buf);
    free(body);

    if (status != 200) {
        fprintf(stderr, "embed_local: HTTP %d: %s\n", status, buf.data ? buf.data : "");
        free(buf.data);
        return -1;
    }

    size_t n = 0;
    int rc = json_extract_floats(buf.data, "\"embedding\":", vec, CRAG_EMBED_DIM_MAX, &n);
    free(buf.data);
    if (rc != 0) return -1;
    *dim = (int)n;
    return 0;
}

crag_embed_backend_t *embed_local_create(const char *url) {
    crag_embed_backend_t *b = malloc(sizeof *b);
    local_ctx_t *ctx = malloc(sizeof *ctx);
    if (!b || !ctx) { free(b); free(ctx); return NULL; }

    const char *u = url ? url : getenv("CRAG_EMBED_URL");
    strncpy(ctx->url, u ? u : DEFAULT_URL, sizeof ctx->url - 1);
    ctx->url[sizeof ctx->url - 1] = '\0';

    b->name       = "local";
    b->embed_one  = embed_one_local;
    b->embed_many = NULL; /* local server has no batch endpoint */
    b->ctx        = ctx;
    return b;
}

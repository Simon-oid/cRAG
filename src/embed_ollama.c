#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embed.h"
#include "http.h"
#include "util.h"

#define DEFAULT_URL   "http://localhost:11434/api/embeddings"
#define DEFAULT_MODEL "nomic-embed-text"

typedef struct {
    char url[512];
    char model[128];
} ollama_ctx_t;

static int embed_one_ollama(const char *text, float *vec, int *dim, void *ctx_) {
    ollama_ctx_t *ctx = ctx_;

    char *escaped = json_escape_dup(text);
    if (!escaped) return -1;

    size_t body_sz = strlen(escaped) + strlen(ctx->model) + 64;
    char *body = malloc(body_sz);
    if (!body) { free(escaped); return -1; }
    snprintf(body, body_sz,
             "{\"model\":\"%s\",\"prompt\":\"%s\"}", ctx->model, escaped);
    free(escaped);

    http_buf_t buf = {0};
    int status = http_post_json(ctx->url, NULL, body, &buf);
    free(body);

    if (status != 200) {
        fprintf(stderr, "embed_ollama: HTTP %d: %s\n", status, buf.data ? buf.data : "");
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

crag_embed_backend_t *embed_ollama_create(const char *url, const char *model) {
    crag_embed_backend_t *b = malloc(sizeof *b);
    ollama_ctx_t *ctx = malloc(sizeof *ctx);
    if (!b || !ctx) { free(b); free(ctx); return NULL; }

    const char *u = url   ? url   : getenv("CRAG_EMBED_URL");
    const char *m = model ? model : getenv("CRAG_EMBED_MODEL");

    strncpy(ctx->url,   u ? u : DEFAULT_URL,   sizeof ctx->url   - 1);
    strncpy(ctx->model, m ? m : DEFAULT_MODEL, sizeof ctx->model - 1);
    ctx->url[sizeof ctx->url - 1]     = '\0';
    ctx->model[sizeof ctx->model - 1] = '\0';

    b->name       = "ollama";
    b->embed_one  = embed_one_ollama;
    b->embed_many = NULL;
    b->ctx        = ctx;
    return b;
}

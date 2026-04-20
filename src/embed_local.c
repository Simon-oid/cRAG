#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embed.h"
#include "http.h"

#define DEFAULT_URL "http://localhost:8080/embedding"

typedef struct {
    char url[512];
} local_ctx_t;

static int embed_one_local(const char *text, float *vec, int *dim, void *ctx_) {
    local_ctx_t *ctx = ctx_;

    size_t tlen = strlen(text);
    char *escaped = malloc(tlen * 2 + 1);
    if (!escaped) return -1;
    size_t j = 0;
    for (size_t i = 0; i < tlen; i++) {
        if (text[i] == '"' || text[i] == '\\') escaped[j++] = '\\';
        else if (text[i] == '\n') { escaped[j++] = '\\'; escaped[j++] = 'n'; continue; }
        else if (text[i] == '\r') { escaped[j++] = '\\'; escaped[j++] = 'r'; continue; }
        escaped[j++] = text[i];
    }
    escaped[j] = '\0';

    size_t body_sz = j + 32;
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

    char *arr = strstr(buf.data, "\"embedding\":");
    if (!arr) { free(buf.data); return -1; }
    arr = strchr(arr, '[');
    if (!arr) { free(buf.data); return -1; }
    arr++;

    int d = 0;
    while (*arr && *arr != ']' && d < CRAG_EMBED_DIM_MAX) {
        while (*arr == ' ' || *arr == ',') arr++;
        if (*arr == ']' || *arr == '\0') break;
        vec[d++] = strtof(arr, &arr);
    }
    *dim = d;
    free(buf.data);
    return d > 0 ? 0 : -1;
}

crag_embed_backend_t *embed_local_create(const char *url) {
    crag_embed_backend_t *b = malloc(sizeof *b);
    local_ctx_t *ctx = malloc(sizeof *ctx);
    if (!b || !ctx) { free(b); free(ctx); return NULL; }

    const char *u = url ? url : getenv("CRAG_EMBED_URL");
    strncpy(ctx->url, u ? u : DEFAULT_URL, sizeof ctx->url - 1);

    b->name = "local";
    b->embed_one = embed_one_local;
    b->ctx = ctx;
    return b;
}

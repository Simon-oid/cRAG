#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embed.h"
#include "http.h"

#define DEFAULT_URL "https://api.openai.com/v1/embeddings"
#define DEFAULT_MODEL "text-embedding-3-small"

typedef struct {
    char url[512];
    char auth_header[256];
} oai_ctx_t;

static int embed_one_oai(const char *text, float *vec, int *dim, void *ctx_) {
    oai_ctx_t *ctx = ctx_;

    /* escape text: replace " and \ */
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

    size_t body_sz = j + 128;
    char *body = malloc(body_sz);
    if (!body) { free(escaped); return -1; }
    snprintf(body, body_sz,
             "{\"model\":\"%s\",\"input\":\"%s\"}", DEFAULT_MODEL, escaped);
    free(escaped);

    const char *headers[] = { ctx->auth_header, NULL };
    http_buf_t buf = {0};
    int status = http_post_json(ctx->url, headers, body, &buf);
    free(body);

    if (status != 200) {
        fprintf(stderr, "embed_api: HTTP %d: %s\n", status, buf.data ? buf.data : "");
        free(buf.data);
        return -1;
    }

    /* parse "embedding":[...] from data[0] */
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

crag_embed_backend_t *embed_oai_create(const char *url, const char *api_key) {
    if (!api_key) api_key = getenv("OPENAI_API_KEY");
    if (!api_key) { fprintf(stderr, "embed_api: OPENAI_API_KEY not set\n"); return NULL; }

    crag_embed_backend_t *b = malloc(sizeof *b);
    oai_ctx_t *ctx = malloc(sizeof *ctx);
    if (!b || !ctx) { free(b); free(ctx); return NULL; }

    strncpy(ctx->url, url ? url : DEFAULT_URL, sizeof ctx->url - 1);
    snprintf(ctx->auth_header, sizeof ctx->auth_header, "Authorization: Bearer %s", api_key);

    b->name = "openai";
    b->embed_one = embed_one_oai;
    b->ctx = ctx;
    return b;
}

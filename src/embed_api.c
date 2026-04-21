#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embed.h"
#include "http.h"
#include "util.h"

#define DEFAULT_URL   "https://api.openai.com/v1/embeddings"
#define DEFAULT_MODEL "text-embedding-3-small"
#define BATCH_SIZE    256

typedef struct {
    char url[512];
    char auth_header[256];
} oai_ctx_t;

static int embed_one_oai(const char *text, float *vec, int *dim, void *ctx_) {
    oai_ctx_t *ctx = ctx_;

    char *escaped = json_escape_dup(text);
    if (!escaped) return -1;

    size_t body_sz = strlen(escaped) + 128;
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

    size_t n = 0;
    int rc = json_extract_floats(buf.data, "\"embedding\":", vec, CRAG_EMBED_DIM_MAX, &n);
    free(buf.data);
    if (rc != 0) return -1;
    *dim = (int)n;
    return 0;
}

static int embed_many_oai(const char **texts, int n,
                          float *vecs, int *dims, void *ctx_) {
    oai_ctx_t *ctx = ctx_;

    /* build JSON array: ["t0","t1",...] */
    size_t arr_sz = 3; /* [ + ] + NUL */
    for (int i = 0; i < n; i++) arr_sz += strlen(texts[i]) * 6 + 4;
    char *arr = malloc(arr_sz);
    if (!arr) return -1;
    char *wp = arr;
    *wp++ = '[';
    for (int i = 0; i < n; i++) {
        char *esc = json_escape_dup(texts[i]);
        if (!esc) { free(arr); return -1; }
        size_t el = strlen(esc);
        *wp++ = '"';
        memcpy(wp, esc, el); wp += el;
        *wp++ = '"';
        if (i < n - 1) *wp++ = ',';
        free(esc);
    }
    *wp++ = ']';
    *wp   = '\0';

    size_t body_sz = strlen(arr) + 64;
    char *body = malloc(body_sz);
    if (!body) { free(arr); return -1; }
    snprintf(body, body_sz, "{\"model\":\"%s\",\"input\":%s}", DEFAULT_MODEL, arr);
    free(arr);

    const char *headers[] = { ctx->auth_header, NULL };
    http_buf_t buf = {0};
    int status = http_post_json(ctx->url, headers, body, &buf);
    free(body);

    if (status != 200) {
        fprintf(stderr, "embed_api: batch HTTP %d: %s\n", status, buf.data ? buf.data : "");
        free(buf.data);
        return -1;
    }

    /* parse data[*].embedding arrays in order */
    const char *p = buf.data;
    for (int i = 0; i < n; i++) {
        const char *emb = strstr(p, "\"embedding\":");
        if (!emb) { free(buf.data); return -1; }
        size_t cnt = 0;
        float *row = vecs + (size_t)i * CRAG_EMBED_DIM_MAX;
        if (json_extract_floats(emb, "\"embedding\":", row, CRAG_EMBED_DIM_MAX, &cnt) != 0) {
            free(buf.data);
            return -1;
        }
        dims[i] = (int)cnt;
        p = emb + 12; /* advance past this key */
    }

    free(buf.data);
    return 0;
}

crag_embed_backend_t *embed_oai_create(const char *url, const char *api_key) {
    if (!api_key) api_key = getenv("OPENAI_API_KEY");
    if (!api_key) { fprintf(stderr, "embed_api: OPENAI_API_KEY not set\n"); return NULL; }

    crag_embed_backend_t *b = malloc(sizeof *b);
    oai_ctx_t *ctx = malloc(sizeof *ctx);
    if (!b || !ctx) { free(b); free(ctx); return NULL; }

    strncpy(ctx->url, url ? url : DEFAULT_URL, sizeof ctx->url - 1);
    ctx->url[sizeof ctx->url - 1] = '\0';
    snprintf(ctx->auth_header, sizeof ctx->auth_header,
             "Authorization: Bearer %s", api_key);

    b->name       = "openai";
    b->embed_one  = embed_one_oai;
    b->embed_many = embed_many_oai;
    b->ctx        = ctx;
    return b;
}

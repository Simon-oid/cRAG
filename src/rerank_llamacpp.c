#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crag.h"
#include "rerank.h"
#include "http.h"
#include "util.h"

#define DEFAULT_URL "http://localhost:8090/rerank"

typedef struct {
    char url[512];
    char model[128];
} llamacpp_ctx_t;

/* Parse a llama.cpp / Cohere-style rerank response into scores[0..n-1].
   The response shape is:
     {"results":[{"index":I,"relevance_score":S}, ...], ...}
   (older builds use "score" instead of "relevance_score"). Results may be
   in any order, so we key off the per-object "index". scores[] is filled
   only for indices that appear; the caller pre-seeds it with a floor so
   any doc the server omits sorts last. Returns #pairs parsed, or -1 if the
   response carried no usable score at all.

   Non-static so the offline unit test (tests/test_rerank.c) can exercise it
   without a live server. */
int rerank_parse_scores(const char *json, int n, float *scores) {
    if (!json) return -1;
    int parsed = 0;
    const char *p = json;
    while ((p = strstr(p, "\"index\"")) != NULL) {
        p += 7;                                  /* past "index" */
        while (*p == ' ' || *p == ':' || *p == '\t') p++;
        char *endp = NULL;
        long idx = strtol(p, &endp, 10);
        if (endp == p) {                         /* not a number; keep scanning */
            if (*p == '\0') break;               /* never step past the terminator */
            p++; continue;
        }
        p = endp;

        const char *sk = strstr(p, "\"relevance_score\"");
        size_t klen = 17;
        const char *sk2 = strstr(p, "\"score\"");
        if (!sk || (sk2 && sk2 < sk)) { sk = sk2; klen = 7; }
        if (!sk) break;                          /* no score follows: malformed */
        const char *q = sk + klen;
        while (*q == ' ' || *q == ':' || *q == '\t') q++;
        char *ends = NULL;
        double sc = strtod(q, &ends);
        if (ends == q) { p = sk + klen; continue; }
        p = ends;

        if (idx >= 0 && idx < n) { scores[idx] = (float)sc; parsed++; }
    }
    return parsed > 0 ? parsed : -1;
}

static int rerank_llamacpp(const char *query, const char **docs, int n,
                           float *scores, void *ctx_) {
    llamacpp_ctx_t *ctx = ctx_;
    if (n <= 0) return 0;

    /* Seed with a floor so any doc the server fails to score sorts last. */
    for (int i = 0; i < n; i++) scores[i] = -1e30f;

    char *eq = json_escape_dup(query);
    if (!eq) return CRAG_ENOMEM;

    /* size the body: per-doc escaped length + quotes/comma, plus header. */
    size_t body_sz = strlen(eq) + strlen(ctx->model) + 64;
    char **edocs = calloc((size_t)n, sizeof *edocs);
    if (!edocs) { free(eq); return CRAG_ENOMEM; }
    for (int i = 0; i < n; i++) {
        edocs[i] = json_escape_dup(docs[i] ? docs[i] : "");
        if (!edocs[i]) {
            for (int j = 0; j < i; j++) free(edocs[j]);
            free(edocs); free(eq);
            return CRAG_ENOMEM;
        }
        body_sz += strlen(edocs[i]) + 4;
    }

    char *body = malloc(body_sz);
    if (!body) {
        for (int i = 0; i < n; i++) free(edocs[i]);
        free(edocs); free(eq);
        return CRAG_ENOMEM;
    }

    char *w = body;
    w += sprintf(w, "{\"model\":\"%s\",\"query\":\"%s\",\"documents\":[",
                 ctx->model, eq);
    for (int i = 0; i < n; i++)
        w += sprintf(w, "%s\"%s\"", i ? "," : "", edocs[i]);
    w += sprintf(w, "]}");

    free(eq);
    for (int i = 0; i < n; i++) free(edocs[i]);
    free(edocs);

    http_buf_t buf = {0};
    int status = http_post_json(ctx->url, NULL, body, &buf);
    free(body);

    if (status != 200) {
        fprintf(stderr, "rerank_llamacpp: HTTP %d: %s\n",
                status, buf.data ? buf.data : "(no body)");
        free(buf.data);
        return CRAG_EHTTP;
    }

    int got = rerank_parse_scores(buf.data, n, scores);
    free(buf.data);
    if (got < 0) {
        fprintf(stderr, "rerank_llamacpp: no scores in response\n");
        return CRAG_EPARSE;
    }
    return 0;
}

crag_rerank_backend_t *rerank_llamacpp_create(const char *url) {
    crag_rerank_backend_t *b = malloc(sizeof *b);
    llamacpp_ctx_t *ctx = malloc(sizeof *ctx);
    if (!b || !ctx) { free(b); free(ctx); return NULL; }

    const char *u = url ? url : getenv("CRAG_RERANK_URL");
    const char *m = getenv("CRAG_RERANK_MODEL");

    strncpy(ctx->url, u ? u : DEFAULT_URL, sizeof ctx->url - 1);
    strncpy(ctx->model, m ? m : "reranker", sizeof ctx->model - 1);
    ctx->url[sizeof ctx->url - 1]     = '\0';
    ctx->model[sizeof ctx->model - 1] = '\0';

    b->name   = "llamacpp";
    b->rerank = rerank_llamacpp;
    b->ctx    = ctx;
    return b;
}

#ifndef RERANK_H
#define RERANK_H

/* Cross-encoder reranking backend. Where embedding scores a query and a
   document independently (bi-encoder, fast, approximate), a reranker scores
   the (query, document) pair jointly (cross-encoder, slower, more accurate).
   It is the second stage of a retrieve-then-rerank pipeline: a recall-heavy
   first stage (hybrid) pulls a deep candidate pool, the reranker reorders it,
   and the top-k is returned.

   Same pluggable-vtable shape as embed_backend_t / generator_t: a backend
   POSTs to an HTTP endpoint. Default backend "llamacpp" talks to
   llama.cpp's /rerank endpoint (run llama-server with --reranking). */

typedef struct crag_rerank_backend crag_rerank_backend_t;

struct crag_rerank_backend {
    const char *name;
    /* Score the relevance of each doc to `query`. Writes scores[i] for
       docs[i] (higher = more relevant; scale is backend-defined, used only
       for ordering). Returns 0 on success, <0 (crag_err_t) on error. */
    int (*rerank)(const char *query, const char **docs, int n,
                  float *scores, void *ctx);
    void *ctx;
};

/* name: "llamacpp" (default if NULL). url: override endpoint (NULL = default
   or CRAG_RERANK_URL). Returns NULL on unknown backend / OOM. */
crag_rerank_backend_t *rerank_backend_create(const char *name, const char *url);
void rerank_backend_free(crag_rerank_backend_t *b);

#endif

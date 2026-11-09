/* lib.c — the public library API (include/crag_lib.h) over store.c.
 *
 * A thin layer: argument checks, one error vocabulary (crag_err_t), the
 * index-wide vector dim enforced on every add and query, and results
 * handed out as crag_hit_t with their strings moved, not copied. */
#include "crag_lib.h"
#include "store.h"

#include <stdlib.h>
#include <string.h>

#define CRAG_LIB_K_MAX 1000   /* one search returns at most this many hits */

struct crag_db {
    crag_store_t store;
    int          dim;         /* 0 until the first chunk fixes it */
};

int crag_db_open(const char *path, crag_db_t **out) {
    if (!path || !out) return CRAG_EINVAL;
    crag_db_t *db = calloc(1, sizeof *db);
    if (!db) return CRAG_ENOMEM;
    if (store_open(path, &db->store) != 0) {
        free(db);
        return CRAG_EDB;
    }
    crag_store_stats_t st;
    if (store_stats(&db->store, &st) == 0) db->dim = st.dim;
    *out = db;
    return CRAG_OK;
}

void crag_db_close(crag_db_t *db) {
    if (!db) return;
    store_close(&db->store);
    free(db);
}

/* The index holds one dim; the first chunk sets it. */
static int check_dim(const crag_db_t *db, int dim) {
    if (dim <= 0 || dim > CRAG_EMBED_DIM_MAX) return CRAG_EINVAL;
    if (db->dim && dim != db->dim) return CRAG_EDIM_MISMATCH;
    return CRAG_OK;
}

int crag_db_add(crag_db_t *db, const char *source, const char *text,
                const float *vec, int dim) {
    if (!db || !source || !text || !vec) return CRAG_EINVAL;
    int rc = check_dim(db, dim);
    if (rc != CRAG_OK) return rc;

    /* crag_chunk_t carries a fixed CRAG_EMBED_DIM_MAX array: heap, not stack. */
    crag_chunk_t *c = calloc(1, sizeof *c);
    if (!c) return CRAG_ENOMEM;
    c->source = (char *)source;      /* read only by store_insert */
    c->text   = (char *)text;
    c->dim    = dim;
    memcpy(c->vec, vec, (size_t)dim * sizeof(float));
    rc = store_insert(&db->store, c) == 0 ? CRAG_OK : CRAG_EDB;
    free(c);
    if (rc == CRAG_OK && !db->dim) db->dim = dim;
    return rc;
}

int crag_db_begin(crag_db_t *db) {
    if (!db) return CRAG_EINVAL;
    return store_begin(&db->store) == 0 ? CRAG_OK : CRAG_EDB;
}

int crag_db_commit(crag_db_t *db) {
    if (!db) return CRAG_EINVAL;
    return store_commit(&db->store) == 0 ? CRAG_OK : CRAG_EDB;
}

/* Which inputs each mode needs. */
static int check_query(const crag_db_t *db, crag_mode_t mode,
                       const float *qvec, int qdim, const char *qtext, int k) {
    if (k <= 0 || k > CRAG_LIB_K_MAX) return CRAG_EINVAL;
    if (mode != CRAG_MODE_COSINE && mode != CRAG_MODE_BM25 &&
        mode != CRAG_MODE_HYBRID)
        return CRAG_EINVAL;
    if (mode != CRAG_MODE_BM25) {
        if (!qvec) return CRAG_EINVAL;
        int rc = check_dim(db, qdim);
        if (rc != CRAG_OK) return rc;
    }
    if (mode != CRAG_MODE_COSINE && !qtext) return CRAG_EINVAL;
    return CRAG_OK;
}

/* Run the store search for `mode`; fills res[] and the per-mode scores. */
static int run_search(crag_db_t *db, crag_mode_t mode, const float *qvec,
                      int qdim, const char *qtext, int k, crag_chunk_t *res,
                      float *flat, crag_score_t *fused) {
    if (mode == CRAG_MODE_COSINE)
        return store_search(&db->store, qvec, qdim, k, res, flat);
    if (mode == CRAG_MODE_BM25)
        return store_search_bm25(&db->store, qtext, k, res, flat);
    return store_search_hybrid(&db->store, qvec, qdim, qtext, k, res, fused);
}

static void fill_hit(crag_hit_t *h, crag_chunk_t *c, crag_mode_t mode,
                     float flat, const crag_score_t *fused) {
    h->id     = c->id;
    h->source = c->source;           /* ownership moves to the hit */
    h->text   = c->text;
    c->source = c->text = NULL;
    if (mode == CRAG_MODE_COSINE) {
        h->score = h->cosine = flat;
        h->bm25 = 0.0f;
    } else if (mode == CRAG_MODE_BM25) {
        h->score = h->bm25 = flat;
        h->cosine = CRAG_SCORE_NA;
    } else {
        h->score  = fused->fusion;
        h->cosine = fused->cosine;
        h->bm25   = fused->bm25;
    }
}

int crag_db_search(crag_db_t *db, crag_mode_t mode,
                   const float *qvec, int qdim, const char *qtext, int k,
                   crag_hit_t **out_hits, int *out_n) {
    if (!db || !out_hits || !out_n) return CRAG_EINVAL;
    int rc = check_query(db, mode, qvec, qdim, qtext, k);
    if (rc != CRAG_OK) return rc;

    crag_chunk_t *res   = calloc((size_t)k, sizeof *res);
    float        *flat  = calloc((size_t)k, sizeof *flat);
    crag_score_t *fused = calloc((size_t)k, sizeof *fused);
    if (!res || !flat || !fused) {
        free(res); free(flat); free(fused);
        return CRAG_ENOMEM;
    }
    int n = run_search(db, mode, qvec, qdim, qtext, k, res, flat, fused);
    crag_hit_t *hits = n > 0 ? calloc((size_t)n, sizeof *hits) : NULL;
    if (n < 0) {
        rc = CRAG_EDB;
    } else if (n > 0 && !hits) {
        rc = CRAG_ENOMEM;
    } else {
        for (int i = 0; i < n; i++)
            fill_hit(&hits[i], &res[i], mode, flat[i], &fused[i]);
        *out_hits = hits;
        *out_n = n;
    }
    for (int i = 0; i < (n > 0 ? n : 0); i++) {   /* whatever was not moved */
        free(res[i].source);
        free(res[i].text);
    }
    if (rc != CRAG_OK) free(hits);
    free(res); free(flat); free(fused);
    return rc;
}

void crag_hits_free(crag_hit_t *hits, int n) {
    if (!hits) return;
    for (int i = 0; i < n; i++) {
        free(hits[i].source);
        free(hits[i].text);
    }
    free(hits);
}

int crag_db_stats(crag_db_t *db, int *chunks, int *sources, int *dim) {
    if (!db) return CRAG_EINVAL;
    crag_store_stats_t st;
    if (store_stats(&db->store, &st) != 0) return CRAG_EDB;
    if (chunks)  *chunks  = st.chunks;
    if (sources) *sources = st.files;
    if (dim)     *dim     = st.dim;
    return CRAG_OK;
}

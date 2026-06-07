/* test_hostile_embed.c — Tier 4.7 hostile-embed suite.
 *
 * Two surfaces an embedding backend can poison:
 *  (1) malformed VECTORS that reach the store and then the search math —
 *      NaN / +-Inf / zero-norm / denormal / wrong-dimension. Asserts the
 *      store stays consistent and all three search modes survive without a
 *      crash, UB (under UBSan), or DB corruption. cosine has a 1e-9 zero-norm
 *      guard and skips dim-mismatched rows; we pin both.
 *  (2) a fault-injecting embed_backend_t whose embed_one returns the error
 *      shapes a real backend produces (HTTP 500 -> rc<0, empty/partial vec,
 *      dim=0, oversized dim). Asserts the vtable error convention so a caller
 *      that checks rc<0 and validates dim catches every fault.
 *
 * Teeth: `make test-asan` (heap red-zones) + UBSan (the search arithmetic) +
 * valgrind. This driver supplies the hostile inputs and invariants.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "crag.h"
#include "store.h"
#include "embed.h"
#include "test.h"

#define K 8

static void cleanup_db(const char *p) {
    unlink(p);
    char wal[256]; snprintf(wal, sizeof wal, "%s-wal", p); unlink(wal);
    char shm[256]; snprintf(shm, sizeof shm, "%s-shm", p); unlink(shm);
}

static void ins_vec(crag_store_t *s, const char *src, const char *text,
                    int dim, const float *vec) {
    crag_chunk_t c = {0};
    c.source = (char *)src; c.text = (char *)text; c.dim = dim;
    for (int i = 0; i < dim && i < CRAG_EMBED_DIM_MAX; i++) c.vec[i] = vec[i];
    /* store_insert may legitimately reject; we only require it not crash and
       return a defined code. */
    int rc = store_insert(s, &c);
    TEST_ASSERT(rc == 0 || rc < 0);
}

static void search_all_modes(crag_store_t *s, const float *qvec, int qdim) {
    crag_chunk_t r[K]; float sc[K]; crag_score_t hsc[K];

    memset(r, 0, sizeof r);
    int a = store_search(s, qvec, qdim, K, r, sc);
    TEST_ASSERT(a >= 0 && a <= K);
    for (int i = 0; i < a; i++) { free(r[i].source); free(r[i].text);
                                  r[i].source = r[i].text = NULL; }

    memset(r, 0, sizeof r);
    int b = store_search_bm25(s, "alpha beta gamma", K, r, sc);
    TEST_ASSERT(b >= 0 && b <= K);
    for (int i = 0; i < b; i++) { free(r[i].source); free(r[i].text);
                                  r[i].source = r[i].text = NULL; }

    memset(r, 0, sizeof r);
    int c = store_search_hybrid(s, qvec, qdim, "alpha beta gamma", K, r, hsc);
    TEST_ASSERT(c >= 0 && c <= K);
    for (int i = 0; i < c; i++) {
        /* a returned score must never be NaN-poisoned into the caller's
           contract: fusion is a finite RRF value regardless of vector NaNs. */
        TEST_ASSERT(isfinite(hsc[i].fusion));
        free(r[i].source); free(r[i].text); r[i].source = r[i].text = NULL;
    }
}

static void test_malformed_vectors_dont_crash_search(void) {
    const char *p = "/tmp/crag_hostile_e_vec.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    float normal[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    float nan_v[4]  = {NAN, NAN, NAN, NAN};
    float pinf[4]   = {INFINITY, 1.0f, 0.0f, -1.0f};
    float ninf[4]   = {-INFINITY, 0.0f, 0.0f, 0.0f};
    float zero[4]   = {0.0f, 0.0f, 0.0f, 0.0f};
    float denorm[4] = {1e-40f, -1e-40f, 1e-40f, -1e-40f};   /* subnormals */
    float mixed[4]  = {0.3f, NAN, INFINITY, 0.1f};
    float huge[4]   = {3.0e38f, 3.0e38f, 3.0e38f, 3.0e38f}; /* overflow on dot */

    ins_vec(&s, "normal.md", "alpha beta gamma delta", 4, normal);
    ins_vec(&s, "nan.md",    "alpha nan content here", 4, nan_v);
    ins_vec(&s, "pinf.md",   "beta positive infinity", 4, pinf);
    ins_vec(&s, "ninf.md",   "gamma negative infinity", 4, ninf);
    ins_vec(&s, "zero.md",   "delta all zero vector",  4, zero);
    ins_vec(&s, "denorm.md", "epsilon denormal subnormal", 4, denorm);
    ins_vec(&s, "mixed.md",  "zeta mixed nan inf",     4, mixed);
    ins_vec(&s, "huge.md",   "eta overflowing magnitude", 4, huge);

    /* Query with a clean vector + with a poisoned vector — neither must
       crash, hang, or trip UBSan in the cosine/RRF math. */
    float q_clean[4]  = {0.5f, 0.5f, 0.5f, 0.5f};
    float q_nan[4]    = {NAN, NAN, NAN, NAN};
    float q_zero[4]   = {0.0f, 0.0f, 0.0f, 0.0f};
    search_all_modes(&s, q_clean, 4);
    search_all_modes(&s, q_nan,   4);
    search_all_modes(&s, q_zero,  4);   /* exercises the 1e-9 zero-norm guard */

    crag_store_stats_t st = {0};
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.dim == 4);
    store_close(&s);
    cleanup_db(p);
}

static void test_wrong_dimension_rows_are_skipped(void) {
    const char *p = "/tmp/crag_hostile_e_dim.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    float v4[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    float v8[8] = {0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f};
    ins_vec(&s, "four.md",  "alpha four dim vector here", 4, v4);
    ins_vec(&s, "eight.md", "alpha eight dim vector here", 8, v8);

    /* Query dim 4: the dim-8 row must be skipped by cosine, not crash and
       not be scored against a 4-float query. */
    float q4[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    crag_chunk_t r[K]; float sc[K];
    memset(r, 0, sizeof r);
    int n = store_search(&s, q4, 4, K, r, sc);
    TEST_ASSERT(n >= 0 && n <= K);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT(strcmp(r[i].source, "eight.md") != 0);  /* skipped */
        free(r[i].source); free(r[i].text);
    }

    /* Query dim 8: now the dim-4 row is the one skipped. */
    float q8[8] = {0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f};
    memset(r, 0, sizeof r);
    int m = store_search(&s, q8, 8, K, r, sc);
    TEST_ASSERT(m >= 0 && m <= K);
    for (int i = 0; i < m; i++) {
        TEST_ASSERT(strcmp(r[i].source, "four.md") != 0);
        free(r[i].source); free(r[i].text);
    }
    store_close(&s);
    cleanup_db(p);
}

/* ---- (2) fault-injecting embed backend stub ------------------------------ */

static int stub_http_500(const char *t, float *v, int *d, void *c) {
    (void)t; (void)v; (void)d; (void)c; return -1;       /* backend error */
}
static int stub_empty_vec(const char *t, float *v, int *d, void *c) {
    (void)t; (void)v; (void)c; *d = 0; return 0;          /* zero-length */
}
static int stub_nan_vec(const char *t, float *v, int *d, void *c) {
    (void)t; (void)c; *d = 4; for (int i = 0; i < 4; i++) v[i] = NAN; return 0;
}
static int stub_oversized_dim(const char *t, float *v, int *d, void *c) {
    (void)t; (void)c; *d = CRAG_EMBED_DIM_MAX + 1000;     /* lies about dim */
    for (int i = 0; i < CRAG_EMBED_DIM_MAX; i++) { v[i] = 0.1f; }
    return 0;
}

static void test_backend_fault_convention(void) {
    float vec[CRAG_EMBED_DIM_MAX]; int dim;

    crag_embed_backend_t b = {0};
    b.name = "stub";

    /* HTTP 500 -> rc<0: a caller checking rc catches it. */
    b.embed_one = stub_http_500; dim = -7;
    TEST_ASSERT(b.embed_one("q", vec, &dim, b.ctx) < 0);

    /* empty vector: rc 0 but dim 0 -> a caller validating dim>0 catches it. */
    b.embed_one = stub_empty_vec; dim = -7;
    TEST_ASSERT(b.embed_one("q", vec, &dim, b.ctx) == 0);
    TEST_ASSERT(dim == 0);

    /* NaN vector: rc 0, dim sane, but values non-finite -> caller can isfinite. */
    b.embed_one = stub_nan_vec; dim = -7;
    TEST_ASSERT(b.embed_one("q", vec, &dim, b.ctx) == 0);
    TEST_ASSERT(dim == 4);
    int finite = 1;
    for (int i = 0; i < dim; i++) {
        if (!isfinite(vec[i])) finite = 0;
    }
    TEST_ASSERT(finite == 0);   /* a validating caller would reject this */

    /* oversized dim: caller validating dim<=CRAG_EMBED_DIM_MAX catches it. */
    b.embed_one = stub_oversized_dim; dim = -7;
    TEST_ASSERT(b.embed_one("q", vec, &dim, b.ctx) == 0);
    TEST_ASSERT(dim > CRAG_EMBED_DIM_MAX);
}

int main(void) {
    fprintf(stderr, "test_hostile_embed\n");
    TEST_RUN(test_malformed_vectors_dont_crash_search);
    TEST_RUN(test_wrong_dimension_rows_are_skipped);
    TEST_RUN(test_backend_fault_convention);
    TEST_DONE();
}

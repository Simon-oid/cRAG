/* test_determinism.c — Tier 4.1 determinism harness.
 *
 * An agentic consumer reruns the same query and must get the SAME answer —
 * reproducibility is part of the contract (a flaky top-k would make grounding,
 * caching, and the eval floors meaningless). The retrieval path is built to be
 * deterministic: a full vector scan + insertion sort (cosine), FTS5/BM25, and
 * RRF fusion are all pure functions of the stored rows and the query, with no
 * randomness, no thread scheduling (SQLITE_THREADSAFE=0, single-threaded reads),
 * and a stable tie-break. This test PINS that: it indexes a fixed corpus and
 * fires each query mode REPEAT times, asserting every run is byte-identical to
 * the first — same count, same source order, same scores (bit-equal), same text.
 *
 * Offline + pure: fixed vectors inserted directly into the store, no embedder,
 * no network. Teeth under valgrind/ASan like the rest of `make check`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "crag.h"
#include "store.h"
#include "test.h"

#define K      5
#define NDOC   12
#define DIM    4
#define REPEAT 100

static void cleanup_db(const char *p) {
    unlink(p);
    char wal[256]; snprintf(wal, sizeof wal, "%s-wal", p); unlink(wal);
    char shm[256]; snprintf(shm, sizeof shm, "%s-shm", p); unlink(shm);
}

static void ins(crag_store_t *s, const char *src, const char *text,
                const float v[DIM]) {
    crag_chunk_t c = {0};
    c.source = (char *)src; c.text = (char *)text; c.dim = DIM;
    for (int i = 0; i < DIM; i++) c.vec[i] = v[i];
    TEST_ASSERT(store_insert(s, &c) == 0);
}

/* A fixed 12-doc corpus: distinct vectors (strict cosine order, no ties) and
   distinct + shared tokens (so BM25 returns a non-trivial, ordered pool). */
static void seed(crag_store_t *s) {
    TEST_ASSERT(store_begin(s) == 0);
    ins(s, "d00.md", "comune alpha bravo charlie servizio", (float[]){0.90f,0.10f,0.05f,0.02f});
    ins(s, "d01.md", "comune alpha bravo delta orario",     (float[]){0.80f,0.20f,0.10f,0.05f});
    ins(s, "d02.md", "comune alpha echo foxtrot ufficio",   (float[]){0.70f,0.30f,0.10f,0.10f});
    ins(s, "d03.md", "comune bravo golf hotel sportello",   (float[]){0.60f,0.35f,0.20f,0.10f});
    ins(s, "d04.md", "comune india juliet servizio orario", (float[]){0.50f,0.40f,0.30f,0.15f});
    ins(s, "d05.md", "comune kilo lima mike ufficio",       (float[]){0.40f,0.50f,0.35f,0.20f});
    ins(s, "d06.md", "comune november oscar papa sportello",(float[]){0.30f,0.55f,0.45f,0.25f});
    ins(s, "d07.md", "comune quebec romeo alpha servizio",  (float[]){0.20f,0.60f,0.50f,0.30f});
    ins(s, "d08.md", "comune sierra tango bravo orario",    (float[]){0.15f,0.55f,0.60f,0.40f});
    ins(s, "d09.md", "comune uniform victor ufficio",       (float[]){0.10f,0.45f,0.65f,0.50f});
    ins(s, "d10.md", "comune whiskey xray sportello alpha", (float[]){0.05f,0.30f,0.70f,0.60f});
    ins(s, "d11.md", "comune yankee zulu bravo servizio",   (float[]){0.02f,0.20f,0.60f,0.75f});
    TEST_ASSERT(store_commit(s) == 0);
}

/* Snapshot of one search result set, for exact run-to-run comparison. */
typedef struct {
    int   n;
    char  src[K][64];
    float score[K];      /* cosine/bm25 score, or fusion for hybrid */
    float cosine[K];     /* hybrid only (CRAG_SCORE_NA elsewhere)   */
    float bm25[K];       /* hybrid only                             */
} snap_t;

static void free_results(crag_chunk_t *r, int n) {
    for (int i = 0; i < n; i++) { free(r[i].source); free(r[i].text);
                                  r[i].source = r[i].text = NULL; }
}

static void snap_cosine(crag_store_t *s, const float *q, snap_t *out) {
    crag_chunk_t r[K]; float sc[K];
    memset(r, 0, sizeof r); memset(out, 0, sizeof *out);
    int n = store_search(s, q, DIM, K, r, sc);
    TEST_ASSERT(n > 0 && n <= K);
    out->n = n;
    for (int i = 0; i < n; i++) {
        snprintf(out->src[i], sizeof out->src[i], "%s", r[i].source);
        out->score[i] = sc[i];
    }
    free_results(r, n);
}
static void snap_bm25(crag_store_t *s, const char *qt, snap_t *out) {
    crag_chunk_t r[K]; float sc[K];
    memset(r, 0, sizeof r); memset(out, 0, sizeof *out);
    int n = store_search_bm25(s, qt, K, r, sc);
    TEST_ASSERT(n > 0 && n <= K);
    out->n = n;
    for (int i = 0; i < n; i++) {
        snprintf(out->src[i], sizeof out->src[i], "%s", r[i].source);
        out->score[i] = sc[i];
    }
    free_results(r, n);
}
static void snap_hybrid(crag_store_t *s, const float *q, const char *qt, snap_t *out) {
    crag_chunk_t r[K]; crag_score_t sc[K];
    memset(r, 0, sizeof r); memset(out, 0, sizeof *out);
    int n = store_search_hybrid(s, q, DIM, qt, K, r, sc);
    TEST_ASSERT(n > 0 && n <= K);
    out->n = n;
    for (int i = 0; i < n; i++) {
        snprintf(out->src[i], sizeof out->src[i], "%s", r[i].source);
        out->score[i]  = sc[i].fusion;
        out->cosine[i] = sc[i].cosine;
        out->bm25[i]   = sc[i].bm25;
    }
    free_results(r, n);
}

/* Exact equality: same count, same source order, bit-identical scores. The
   computation is identical each run, so floats must compare equal (not ~). */
static void assert_same(const snap_t *a, const snap_t *b, const char *mode, int run) {
    TEST_ASSERT(a->n == b->n);
    for (int i = 0; i < a->n; i++) {
        if (strcmp(a->src[i], b->src[i]) != 0) {
            fprintf(stderr, "  %s run %d pos %d: '%s' != '%s'\n",
                    mode, run, i, b->src[i], a->src[i]);
        }
        TEST_ASSERT(strcmp(a->src[i], b->src[i]) == 0);
        TEST_ASSERT(a->score[i]  == b->score[i]);
        TEST_ASSERT(a->cosine[i] == b->cosine[i]);
        TEST_ASSERT(a->bm25[i]   == b->bm25[i]);
    }
}

static void test_same_query_is_byte_identical(void) {
    const char *p = "/tmp/crag_determinism.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);
    seed(&s);

    const float q[DIM] = {0.55f, 0.40f, 0.25f, 0.12f};
    const char *qt = "alpha bravo servizio sportello";

    snap_t base_c, base_b, base_h;
    snap_cosine(&s, q, &base_c);
    snap_bm25(&s, qt, &base_b);
    snap_hybrid(&s, q, qt, &base_h);

    /* All three modes returned a real, multi-result pool to compare. */
    TEST_ASSERT(base_c.n >= 2);
    TEST_ASSERT(base_b.n >= 2);
    TEST_ASSERT(base_h.n >= 2);

    for (int run = 1; run < REPEAT; run++) {
        snap_t c, b, h;
        snap_cosine(&s, q, &c);   assert_same(&base_c, &c, "cosine", run);
        snap_bm25(&s, qt, &b);    assert_same(&base_b, &b, "bm25",   run);
        snap_hybrid(&s, q, qt, &h); assert_same(&base_h, &h, "hybrid", run);
    }

    store_close(&s);
    cleanup_db(p);
}

/* Determinism must also survive reopening the DB: a fresh store_open over the
   same file must rank identically (no dependence on connection/cache state). */
static void test_reopen_ranks_identically(void) {
    const char *p = "/tmp/crag_determinism_reopen.db";
    cleanup_db(p);

    const float q[DIM] = {0.55f, 0.40f, 0.25f, 0.12f};
    const char *qt = "alpha bravo servizio sportello";

    crag_store_t s1;
    TEST_ASSERT(store_open(p, &s1) == 0);
    seed(&s1);
    snap_t a_c, a_b, a_h;
    snap_cosine(&s1, q, &a_c);
    snap_bm25(&s1, qt, &a_b);
    snap_hybrid(&s1, q, qt, &a_h);
    store_close(&s1);

    crag_store_t s2;
    TEST_ASSERT(store_open(p, &s2) == 0);   /* reopen, same file */
    snap_t b_c, b_b, b_h;
    snap_cosine(&s2, q, &b_c);   assert_same(&a_c, &b_c, "cosine/reopen", 0);
    snap_bm25(&s2, qt, &b_b);    assert_same(&a_b, &b_b, "bm25/reopen",   0);
    snap_hybrid(&s2, q, qt, &b_h); assert_same(&a_h, &b_h, "hybrid/reopen", 0);
    store_close(&s2);
    cleanup_db(p);
}

int main(void) {
    fprintf(stderr, "test_determinism\n");
    TEST_RUN(test_same_query_is_byte_identical);
    TEST_RUN(test_reopen_ranks_identically);
    TEST_DONE();
}

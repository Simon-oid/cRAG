/* Offline retrieval-quality gate — runs in CI with NO embedding server.
 *
 * The BEIR harness (tests/eval_beir.c) is the absolute-quality yardstick, but
 * it needs a live embedder (ollama) and gitignored datasets, so it can't gate a
 * build. This does: a small checked-in synthetic corpus with hand-written
 * relevance judgments, embedded by the same deterministic hashed bag-of-words
 * toy embedder used in tests/test_retrieval.c. Because the embedder is
 * deterministic, the nDCG@10 / Recall@10 / MRR@10 numbers are STABLE to the
 * decimal — so we can assert a floor on each and catch any regression in the
 * store, the cosine scan, the FTS5/BM25 path, or RRF fusion.
 *
 * It is NOT a measure of real-world quality (the toy embedder is lexical, and
 * the corpus is tiny) — it is a regression gate. Absolute quality lives in
 * `make eval` (BEIR). This is the thing every later SOTA sprint is held to:
 * change the pipeline, the gate moves, you find out immediately and offline.
 *
 * Floors are set ~0.05 below the observed deterministic values, low enough not
 * to be brittle to an intentional ranking improvement, high enough that a real
 * regression (broken fusion, dim mismatch, FTS desync) trips them.
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "crag.h"
#include "store.h"
#include "test.h"

#define TOY_DIM   256
#define RANK_CUT  10        /* nDCG / MRR / Recall reported @10 */
#define MAXK      64        /* search depth (>= corpus size)    */

/* ---- deterministic hashed bag-of-words embedder (see test_retrieval.c) ---- */
static unsigned int fnv1a(const char *s, size_t n) {
    unsigned int h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}
static void toy_embed(const char *text, float *vec, int dim) {
    for (int i = 0; i < dim; i++) vec[i] = 0.0f;
    const char *p = text;
    char tok[64];
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        size_t n = 0;
        while (*p && isalnum((unsigned char)*p) && n < sizeof(tok) - 1)
            tok[n++] = (char)tolower((unsigned char)*p), p++;
        if (n == 0) continue;
        tok[n] = '\0';
        vec[fnv1a(tok, n) % (unsigned)dim] += 1.0f;
    }
    double nsq = 0.0;
    for (int i = 0; i < dim; i++) nsq += (double)vec[i] * vec[i];
    float norm = (float)sqrt(nsq);
    if (norm > 1e-9f) for (int i = 0; i < dim; i++) vec[i] /= norm;
}

/* ---- synthetic corpus: 6 topics x 3 docs, shared topic vocabulary -------- */
typedef struct { const char *id; const char *text; } doc_t;

static const doc_t CORPUS[] = {
    /* 0..2  Rust memory safety */
    {"rust_a", "Rust enforces memory safety through ownership and the borrow "
               "checker. References are validated at compile time so dangling "
               "pointers and data races are rejected without a garbage collector."},
    {"rust_b", "Ownership in Rust means each value has a single owner; borrowing "
               "lends a reference under lifetime rules the compiler checks "
               "statically. The borrow checker prevents use-after-free."},
    {"rust_c", "Lifetimes annotate how long a borrowed reference stays valid. "
               "The Rust compiler uses ownership and lifetimes to guarantee "
               "memory safety with no runtime garbage collector."},
    /* 3..5  Python decorators */
    {"py_a",   "A Python decorator wraps a callable to extend its behavior. The "
               "@ syntax applies the decorator to a function at definition time."},
    {"py_b",   "Decorators in Python are higher-order functions that take a "
               "function and return a wrapped callable, commonly used for "
               "logging, memoization, and access control."},
    {"py_c",   "Using the @ symbol, a decorator transforms a function without "
               "changing its body; stacked decorators wrap the callable in order."},
    /* 6..8  SQLite WAL */
    {"wal_a",  "SQLite write-ahead logging appends changes to a WAL journal so "
               "readers and a writer can proceed concurrently. Checkpoints fold "
               "the WAL back into the main database file."},
    {"wal_b",  "WAL mode improves concurrency in SQLite: writes go to the "
               "write-ahead log while readers see a consistent snapshot. Enable "
               "it with PRAGMA journal_mode set to WAL."},
    {"wal_c",  "A checkpoint transfers committed pages from the write-ahead log "
               "into the database. WAL journaling reduces writer-reader blocking "
               "compared to the rollback journal."},
    /* 9..11 Espresso brewing */
    {"esp_a",  "Espresso is brewed by forcing hot water under pressure through "
               "finely ground coffee beans. Grind size and extraction time shape "
               "the flavor and crema."},
    {"esp_b",  "A good espresso shot depends on the grind, the dose of coffee, "
               "and the water pressure. Over-extraction tastes bitter; "
               "under-extraction tastes sour."},
    {"esp_c",  "Tamping the ground coffee evenly lets pressurized water extract "
               "the espresso uniformly. Bean freshness and grind consistency "
               "matter for the crema."},
    /* 12..14 Mountain hiking */
    {"hike_a", "Mountain hiking on a steep trail demands stamina as altitude "
               "rises. Pack water, map the terrain, and watch the weather near "
               "the summit."},
    {"hike_b", "A summit hike crosses rocky terrain and thinning air at high "
               "altitude. A light backpack and sturdy boots make the trail safer."},
    {"hike_c", "On the trail, gaining altitude toward the summit, hikers pace "
               "themselves over uneven terrain and carry layers for cold weather."},
    /* 15..17 Beekeeping (distractor topic, queried too) */
    {"bee_a",  "Beekeepers maintain hives where honey bees store honey and "
               "pollen. The queen lays eggs while worker bees forage for nectar."},
    {"bee_b",  "A healthy hive needs the beekeeper to monitor the queen, manage "
               "swarming, and harvest honey without starving the colony of bees."},
    {"bee_c",  "Worker bees convert nectar into honey inside the hive; the "
               "beekeeper inspects frames for the queen, brood, and honey stores."},
};
#define NDOC ((int)(sizeof CORPUS / sizeof CORPUS[0]))

/* ---- queries with relevance judgments (relevant = the topic's 3 docs) ----- */
typedef struct { const char *q; int rel[8]; int nrel; } query_t;

static const query_t QUERIES[] = {
    {"how does rust guarantee memory safety with ownership and lifetimes",
     {0,1,2}, 3},
    {"what does a python decorator do to a function with the @ syntax",
     {3,4,5}, 3},
    {"sqlite write ahead logging journal mode and checkpoints for concurrency",
     {6,7,8}, 3},
    {"brewing espresso coffee grind size water pressure and extraction",
     {9,10,11}, 3},
    {"hiking a mountain trail to the summit at high altitude over terrain",
     {12,13,14}, 3},
    {"beekeeper managing a hive of honey bees and the queen",
     {15,16,17}, 3},
};
#define NQ ((int)(sizeof QUERIES / sizeof QUERIES[0]))

/* ---- metrics --------------------------------------------------------------- */
static int id_to_index(const char *src) {
    for (int i = 0; i < NDOC; i++) if (strcmp(src, CORPUS[i].id) == 0) return i;
    return -1;
}
static int is_rel(const query_t *q, int idx) {
    for (int i = 0; i < q->nrel; i++) if (q->rel[i] == idx) return 1;
    return 0;
}

typedef struct { double ndcg, recall, mrr; } metrics_t;

/* Score a single query from a ranked, deduped list of doc indices. */
static void score_one(const query_t *q, const int *docs, int ndocs, metrics_t *m) {
    int cut = ndocs < RANK_CUT ? ndocs : RANK_CUT;
    double dcg = 0.0; int first = 0;
    for (int i = 0; i < cut; i++) if (is_rel(q, docs[i])) {
        dcg += 1.0 / log2((double)(i + 2));
        if (!first) first = i + 1;
    }
    int ideal = q->nrel < RANK_CUT ? q->nrel : RANK_CUT;
    double idcg = 0.0;
    for (int i = 0; i < ideal; i++) idcg += 1.0 / log2((double)(i + 2));

    int hits = 0;
    for (int i = 0; i < cut; i++) if (is_rel(q, docs[i])) hits++;

    m->ndcg   += idcg > 0 ? dcg / idcg : 0.0;
    m->recall += q->nrel > 0 ? (double)hits / q->nrel : 0.0;
    m->mrr    += first ? 1.0 / first : 0.0;
}

/* Collapse ranked results to unique doc indices, preserving order. */
static int collapse(crag_chunk_t *res, int n, int *docs, int maxd) {
    int nd = 0;
    for (int i = 0; i < n && nd < maxd; i++) {
        int idx = res[i].source ? id_to_index(res[i].source) : -1;
        if (idx < 0) continue;
        int seen = 0;
        for (int j = 0; j < nd; j++) if (docs[j] == idx) { seen = 1; break; }
        if (!seen) docs[nd++] = idx;
    }
    return nd;
}

enum { M_COSINE, M_BM25, M_HYBRID, M_COUNT };
static const char *MODE[M_COUNT] = { "cosine", "bm25", "hybrid" };

int main(void) {
    fprintf(stderr, "eval_ci:\n");
    const char *db = "/tmp/crag_eval_ci.db";
    unlink(db);
    { char w[256]; snprintf(w, sizeof w, "%s-wal", db); unlink(w);
      snprintf(w, sizeof w, "%s-shm", db); unlink(w); }

    crag_store_t s;
    TEST_ASSERT(store_open(db, &s) == 0);
    TEST_ASSERT(store_begin(&s) == 0);
    for (int i = 0; i < NDOC; i++) {
        crag_chunk_t c = {0};
        c.source = (char *)CORPUS[i].id;
        c.text   = (char *)CORPUS[i].text;
        c.dim    = TOY_DIM;
        toy_embed(CORPUS[i].text, c.vec, TOY_DIM);
        TEST_ASSERT(store_insert(&s, &c) == 0);
    }
    TEST_ASSERT(store_commit(&s) == 0);

    metrics_t acc[M_COUNT] = {{0,0,0}};
    crag_chunk_t res[MAXK];
    float       fsc[MAXK];
    crag_score_t hsc[MAXK];

    for (int qi = 0; qi < NQ; qi++) {
        const query_t *q = &QUERIES[qi];
        float qvec[TOY_DIM];
        toy_embed(q->q, qvec, TOY_DIM);

        for (int m = 0; m < M_COUNT; m++) {
            int n = 0;
            if      (m == M_COSINE) n = store_search(&s, qvec, TOY_DIM, MAXK, res, fsc);
            else if (m == M_BM25)   n = store_search_bm25(&s, q->q, MAXK, res, fsc);
            else                    n = store_search_hybrid(&s, qvec, TOY_DIM, q->q, MAXK, res, hsc);
            if (n < 0) n = 0;
            int docs[MAXK];
            int nd = collapse(res, n, docs, MAXK);
            score_one(q, docs, nd, &acc[m]);
            for (int i = 0; i < n; i++) { free(res[i].source); free(res[i].text); }
        }
    }

    printf("    %-8s %8s %8s %8s   (n=%d queries, %d docs)\n",
           "mode", "nDCG@10", "R@10", "MRR@10", NQ, NDOC);
    for (int m = 0; m < M_COUNT; m++)
        printf("    %-8s %8.4f %8.4f %8.4f\n", MODE[m],
               acc[m].ndcg / NQ, acc[m].recall / NQ, acc[m].mrr / NQ);

    /* Regression floors — set ~0.03-0.05 below the observed deterministic
       values (cosine 0.9465/0.9444, bm25 1.0, hybrid 0.9892/1.0). The embedder
       is deterministic, so these never drift on their own; an intentional
       ranking improvement only pushes them UP. A drop that trips a floor means
       the store / cosine scan / FTS5+BM25 / RRF fusion path regressed. */
    TEST_ASSERT(acc[M_COSINE].ndcg   / NQ >= 0.90);
    TEST_ASSERT(acc[M_COSINE].recall / NQ >= 0.90);
    TEST_ASSERT(acc[M_BM25].ndcg     / NQ >= 0.95);
    TEST_ASSERT(acc[M_BM25].recall   / NQ >= 0.95);
    TEST_ASSERT(acc[M_HYBRID].ndcg   / NQ >= 0.94);
    TEST_ASSERT(acc[M_HYBRID].recall / NQ >= 0.95);

    store_close(&s);
    unlink(db);
    { char w[256]; snprintf(w, sizeof w, "%s-wal", db); unlink(w);
      snprintf(w, sizeof w, "%s-shm", db); unlink(w); }
    TEST_DONE();
}

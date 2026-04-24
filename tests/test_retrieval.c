/* End-to-end retrieval test.
 *
 * Builds a small "golden dataset" of topic-distinct markdown docs, runs them
 * through the real chunker and store, and verifies that a query on each
 * topic retrieves the matching document at the top of the ranking.
 *
 * A deterministic hashed bag-of-words embedder is used in place of any real
 * embedding backend so retrieval quality is measured without network I/O. */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "crag.h"
#include "chunk.h"
#include "store.h"
#include "test.h"

#define TOY_DIM 256

static int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

/* FNV-1a 32-bit hash — deterministic, no deps. */
static unsigned int fnv1a(const char *s, size_t n) {
    unsigned int h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

/* Toy embedder: lowercase, split on non-alnum, accumulate 1.0 per token at
 * hash(token) % dim, then L2 normalize. Dim large enough to keep collisions
 * between our distinct topic vocabularies rare. */
static void toy_embed(const char *text, float *vec, int dim) {
    for (int i = 0; i < dim; i++) vec[i] = 0.0f;

    const char *p = text;
    char tok[64];
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        size_t n = 0;
        while (*p && isalnum((unsigned char)*p) && n < sizeof(tok) - 1) {
            tok[n++] = (char)tolower((unsigned char)*p);
            p++;
        }
        if (n == 0) continue;
        tok[n] = '\0';
        unsigned int h = fnv1a(tok, n);
        vec[h % (unsigned)dim] += 1.0f;
    }

    double norm_sq = 0.0;
    for (int i = 0; i < dim; i++) norm_sq += (double)vec[i] * vec[i];
    float norm = (float)sqrt(norm_sq);
    if (norm > 1e-9f) {
        for (int i = 0; i < dim; i++) vec[i] /= norm;
    }
}

static void cleanup_db(const char *p) {
    unlink(p);
    char wal[256]; snprintf(wal, sizeof wal, "%s-wal", p); unlink(wal);
    char shm[256]; snprintf(shm, sizeof shm, "%s-shm", p); unlink(shm);
}

static void free_result_row(crag_chunk_t *r) {
    free(r->source); r->source = NULL;
    free(r->text);   r->text   = NULL;
}

/* Chunk a file, toy-embed each chunk, insert into the store. */
static int ingest_file(crag_store_t *s, const char *path) {
    char *texts[16] = {0};
    int n = chunk_file(path, 2048, texts, 16);
    if (n <= 0) return n;

    for (int i = 0; i < n; i++) {
        crag_chunk_t c = {0};
        c.source = (char *)path;
        c.text   = texts[i];
        c.dim    = TOY_DIM;
        toy_embed(texts[i], c.vec, TOY_DIM);
        if (store_insert(s, &c) != 0) {
            for (int j = i; j < n; j++) free(texts[j]);
            return -1;
        }
        free(texts[i]);
    }
    return n;
}

/* Returns 0 if the top-1 hit comes from `expected_source`, else -1.
 * Always frees any returned rows. */
static int query_topic(crag_store_t *s, const char *query_text,
                       const char *expected_source, int k,
                       float *top1_score_out) {
    float qvec[TOY_DIM];
    toy_embed(query_text, qvec, TOY_DIM);

    crag_chunk_t results[8] = {0};
    float scores[8] = {0};
    int n = store_search(s, qvec, TOY_DIM, k, results, scores);

    int ok = -1;
    if (n > 0 && results[0].source &&
        strcmp(results[0].source, expected_source) == 0) {
        ok = 0;
    }
    if (top1_score_out) *top1_score_out = n > 0 ? scores[0] : 0.0f;

    for (int i = 0; i < n; i++) free_result_row(&results[i]);
    return ok;
}

/* --- docs --- */

static const char *DOC_RUST =
    "# Rust Memory Model\n\n"
    "Rust enforces ownership and borrowing at compile time. The borrow checker "
    "statically rejects dangling references and data races without a garbage collector. "
    "Lifetimes annotate how long references remain valid.\n";

static const char *DOC_PYTHON =
    "# Python Decorators\n\n"
    "A decorator in Python wraps a callable to modify its behavior. The @ syntax "
    "applies a decorator at definition time. Decorators are commonly used for "
    "memoization, logging, and access control around functions.\n";

static const char *DOC_SQL =
    "# SQLite WAL Mode\n\n"
    "Write-Ahead Logging in SQLite improves concurrency for readers while writes "
    "append to a journal file. Checkpoints periodically fold the WAL back into "
    "the main database. WAL mode is enabled with PRAGMA journal_mode.\n";

/* --- tests --- */

static void test_topic_queries_retrieve_matching_doc(void) {
    const char *db   = "/tmp/crag_test_retr.db";
    const char *rust = "/tmp/crag_test_retr_rust.md";
    const char *py   = "/tmp/crag_test_retr_py.md";
    const char *sql  = "/tmp/crag_test_retr_sql.md";
    cleanup_db(db);

    write_file(rust, DOC_RUST);
    write_file(py,   DOC_PYTHON);
    write_file(sql,  DOC_SQL);

    crag_store_t s;
    TEST_ASSERT(store_open(db, &s) == 0);
    TEST_ASSERT(ingest_file(&s, rust) > 0);
    TEST_ASSERT(ingest_file(&s, py)   > 0);
    TEST_ASSERT(ingest_file(&s, sql)  > 0);

    float sc = 0.0f;
    TEST_ASSERT(query_topic(&s,
        "how does rust prevent dangling references using lifetimes",
        rust, 3, &sc) == 0);
    TEST_ASSERT(sc > 0.0f);

    TEST_ASSERT(query_topic(&s,
        "what does a python decorator do with the @ symbol",
        py, 3, &sc) == 0);
    TEST_ASSERT(sc > 0.0f);

    TEST_ASSERT(query_topic(&s,
        "enabling write ahead logging in sqlite with pragma journal_mode",
        sql, 3, &sc) == 0);
    TEST_ASSERT(sc > 0.0f);

    store_close(&s);
    cleanup_db(db);
    unlink(rust); unlink(py); unlink(sql);
}

static void test_topk_ranking_is_monotonic(void) {
    const char *db   = "/tmp/crag_test_retr_rank.db";
    const char *rust = "/tmp/crag_test_retr_rank_rust.md";
    const char *py   = "/tmp/crag_test_retr_rank_py.md";
    const char *sql  = "/tmp/crag_test_retr_rank_sql.md";
    cleanup_db(db);

    write_file(rust, DOC_RUST);
    write_file(py,   DOC_PYTHON);
    write_file(sql,  DOC_SQL);

    crag_store_t s;
    TEST_ASSERT(store_open(db, &s) == 0);
    ingest_file(&s, rust);
    ingest_file(&s, py);
    ingest_file(&s, sql);

    float qvec[TOY_DIM];
    toy_embed("rust ownership borrow checker lifetimes", qvec, TOY_DIM);

    crag_chunk_t results[8] = {0};
    float scores[8] = {0};
    int n = store_search(&s, qvec, TOY_DIM, 3, results, scores);
    TEST_ASSERT(n == 3);
    TEST_ASSERT(scores[0] >= scores[1]);
    TEST_ASSERT(scores[1] >= scores[2]);
    /* Topic match should dominate unrelated docs. */
    TEST_ASSERT(scores[0] > scores[2]);

    for (int i = 0; i < n; i++) free_result_row(&results[i]);
    store_close(&s);
    cleanup_db(db);
    unlink(rust); unlink(py); unlink(sql);
}

static void test_off_topic_query_scores_lower_than_on_topic(void) {
    const char *db   = "/tmp/crag_test_retr_offtopic.db";
    const char *rust = "/tmp/crag_test_retr_offtopic_rust.md";
    cleanup_db(db);
    write_file(rust, DOC_RUST);

    crag_store_t s;
    TEST_ASSERT(store_open(db, &s) == 0);
    ingest_file(&s, rust);

    float qvec_on[TOY_DIM], qvec_off[TOY_DIM];
    toy_embed("rust borrow checker lifetimes", qvec_on, TOY_DIM);
    toy_embed("banana pineapple tropical fruit smoothie",  qvec_off, TOY_DIM);

    crag_chunk_t r1[1] = {0}, r2[1] = {0};
    float s1[1] = {0}, s2[1] = {0};
    int n1 = store_search(&s, qvec_on,  TOY_DIM, 1, r1, s1);
    int n2 = store_search(&s, qvec_off, TOY_DIM, 1, r2, s2);
    TEST_ASSERT(n1 == 1);
    TEST_ASSERT(n2 == 1);
    TEST_ASSERT(s1[0] > s2[0]);

    free_result_row(&r1[0]);
    free_result_row(&r2[0]);
    store_close(&s);
    cleanup_db(db);
    unlink(rust);
}

static void test_topk_bounded_by_corpus_size(void) {
    /* Requesting k larger than the corpus must return exactly N hits. */
    const char *db  = "/tmp/crag_test_retr_small.db";
    const char *one = "/tmp/crag_test_retr_small_one.md";
    cleanup_db(db);
    write_file(one, "# Only\n\nJust one short paragraph here.\n");

    crag_store_t s;
    TEST_ASSERT(store_open(db, &s) == 0);
    ingest_file(&s, one);

    float qvec[TOY_DIM];
    toy_embed("paragraph", qvec, TOY_DIM);

    crag_chunk_t results[5] = {0};
    float scores[5] = {0};
    int n = store_search(&s, qvec, TOY_DIM, 5, results, scores);
    TEST_ASSERT(n == 1);

    for (int i = 0; i < n; i++) free_result_row(&results[i]);
    store_close(&s);
    cleanup_db(db);
    unlink(one);
}

int main(void) {
    fprintf(stderr, "test_retrieval:\n");
    TEST_RUN(test_topic_queries_retrieve_matching_doc);
    TEST_RUN(test_topk_ranking_is_monotonic);
    TEST_RUN(test_off_topic_query_scores_lower_than_on_topic);
    TEST_RUN(test_topk_bounded_by_corpus_size);
    TEST_DONE();
}

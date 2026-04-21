#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "crag.h"
#include "store.h"
#include "test.h"

static void cleanup_db(const char *p) {
    unlink(p);
    char wal[256]; snprintf(wal, sizeof wal, "%s-wal", p); unlink(wal);
    char shm[256]; snprintf(shm, sizeof shm, "%s-shm", p); unlink(shm);
}

static void free_result_row(crag_chunk_t *r) {
    free(r->source); r->source = NULL;
    free(r->text);   r->text   = NULL;
}

static void test_open_and_close(void) {
    const char *p = "/tmp/crag_test_store_oc.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);
    store_close(&s);
    cleanup_db(p);
}

static void test_insert_then_stats(void) {
    const char *p = "/tmp/crag_test_store_ins.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    crag_chunk_t c = {0};
    c.source = "doc1.md"; c.text = "hello"; c.dim = 4;
    c.vec[0] = 1.0f;
    TEST_ASSERT(store_insert(&s, &c) == 0);

    c.source = "doc1.md"; c.text = "world";
    TEST_ASSERT(store_insert(&s, &c) == 0);

    c.source = "doc2.md"; c.text = "foo";
    TEST_ASSERT(store_insert(&s, &c) == 0);

    crag_store_stats_t st = {0};
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.chunks == 3);
    TEST_ASSERT(st.files  == 2);
    TEST_ASSERT(st.dim    == 4);

    store_close(&s);
    cleanup_db(p);
}

static void test_search_returns_topk_in_order(void) {
    const char *p = "/tmp/crag_test_store_search.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    crag_chunk_t c = {0};
    c.source = "a.md"; c.dim = 4;

    memset(c.vec, 0, sizeof c.vec);
    c.text = "exact";      c.vec[0] = 1.0f;
    store_insert(&s, &c);

    memset(c.vec, 0, sizeof c.vec);
    c.text = "close";      c.vec[0] = 1.0f; c.vec[1] = 0.1f;
    store_insert(&s, &c);

    memset(c.vec, 0, sizeof c.vec);
    c.text = "orthogonal"; c.vec[1] = 1.0f;
    store_insert(&s, &c);

    float qvec[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    crag_chunk_t results[3] = {0};
    float scores[3] = {0};
    int n = store_search(&s, qvec, 4, 3, results, scores);

    TEST_ASSERT(n == 3);
    TEST_ASSERT(scores[0] >= scores[1]);
    TEST_ASSERT(scores[1] >= scores[2]);
    TEST_ASSERT(strcmp(results[0].text, "exact")      == 0);
    TEST_ASSERT(strcmp(results[2].text, "orthogonal") == 0);

    for (int i = 0; i < n; i++) free_result_row(&results[i]);
    store_close(&s);
    cleanup_db(p);
}

static void test_search_skips_dim_mismatches(void) {
    const char *p = "/tmp/crag_test_store_dim.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    crag_chunk_t c = {0};
    c.source = "x.md"; c.text = "t"; c.dim = 4; c.vec[0] = 1.0f;
    store_insert(&s, &c);

    float qvec[8] = {1.0f, 0, 0, 0, 0, 0, 0, 0};
    crag_chunk_t results[2] = {0};
    float scores[2] = {0};
    int n = store_search(&s, qvec, 8, 2, results, scores);
    TEST_ASSERT(n == 0);

    store_close(&s);
    cleanup_db(p);
}

static void test_search_rejects_invalid_dim(void) {
    const char *p = "/tmp/crag_test_store_invdim.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    float qvec[1] = {1.0f};
    crag_chunk_t results[1] = {0};
    float scores[1] = {0};

    TEST_ASSERT(store_search(&s, qvec, 0, 1, results, scores)                   == -1);
    TEST_ASSERT(store_search(&s, qvec, -1, 1, results, scores)                  == -1);
    TEST_ASSERT(store_search(&s, qvec, CRAG_EMBED_DIM_MAX + 1, 1, results, scores) == -1);

    store_close(&s);
    cleanup_db(p);
}

static void test_transaction_commit_persists(void) {
    const char *p = "/tmp/crag_test_store_tx.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    TEST_ASSERT(store_begin(&s) == 0);
    crag_chunk_t c = {0};
    c.source = "t.md"; c.text = "in-tx"; c.dim = 2; c.vec[0] = 1.0f;
    for (int i = 0; i < 10; i++) TEST_ASSERT(store_insert(&s, &c) == 0);
    TEST_ASSERT(store_commit(&s) == 0);

    crag_store_stats_t st = {0};
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.chunks == 10);

    store_close(&s);
    cleanup_db(p);
}

static void test_persistence_across_reopen(void) {
    const char *p = "/tmp/crag_test_store_reopen.db";
    cleanup_db(p);

    {
        crag_store_t s;
        TEST_ASSERT(store_open(p, &s) == 0);
        crag_chunk_t c = {0};
        c.source = "persist.md"; c.text = "durable"; c.dim = 3;
        c.vec[0] = 0.5f; c.vec[1] = 0.5f; c.vec[2] = 0.5f;
        store_insert(&s, &c);
        store_close(&s);
    }
    {
        crag_store_t s;
        TEST_ASSERT(store_open(p, &s) == 0);
        crag_store_stats_t st = {0};
        TEST_ASSERT(store_stats(&s, &st) == 0);
        TEST_ASSERT(st.chunks == 1);
        TEST_ASSERT(st.dim    == 3);
        store_close(&s);
    }
    cleanup_db(p);
}

int main(void) {
    fprintf(stderr, "test_store:\n");
    TEST_RUN(test_open_and_close);
    TEST_RUN(test_insert_then_stats);
    TEST_RUN(test_search_returns_topk_in_order);
    TEST_RUN(test_search_skips_dim_mismatches);
    TEST_RUN(test_search_rejects_invalid_dim);
    TEST_RUN(test_transaction_commit_persists);
    TEST_RUN(test_persistence_across_reopen);
    TEST_DONE();
}

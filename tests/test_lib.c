/* test_lib — the public library API (include/crag_lib.h).
 *
 * Exercises what a binding relies on: open / add / search in all three
 * modes / stats / close, the ownership of returned hits, the dim
 * contract, and argument checks. In-memory index, tiny hand-made vectors,
 * so the expected ranking is known exactly. */
#include "test.h"
#include "crag_lib.h"

#include <string.h>

#define DIM 3

static crag_db_t *fixture(void) {
    crag_db_t *db = NULL;
    if (crag_db_open(":memory:", &db) != CRAG_OK) return NULL;
    const float a[DIM] = {1, 0, 0}, b[DIM] = {0, 1, 0}, c[DIM] = {0.9f, 0.1f, 0};
    crag_db_begin(db);
    crag_db_add(db, "alpha.md", "the quick brown fox", a, DIM);
    crag_db_add(db, "beta.md", "lazy dogs sleep all day", b, DIM);
    crag_db_add(db, "gamma.md", "a quick red fox jumps", c, DIM);
    crag_db_commit(db);
    return db;
}

static void test_open_add_stats(void) {
    crag_db_t *db = fixture();
    TEST_ASSERT(db != NULL);
    int chunks = 0, sources = 0, dim = 0;
    TEST_ASSERT(crag_db_stats(db, &chunks, &sources, &dim) == CRAG_OK);
    TEST_ASSERT(chunks == 3 && sources == 3 && dim == DIM);
    crag_db_close(db);
}

static void test_cosine_ranks_by_vector(void) {
    crag_db_t *db = fixture();
    const float q[DIM] = {1, 0, 0};
    crag_hit_t *hits = NULL; int n = 0;
    TEST_ASSERT(crag_db_search(db, CRAG_MODE_COSINE, q, DIM, NULL, 2, &hits, &n) == CRAG_OK);
    TEST_ASSERT(n == 2);
    TEST_ASSERT(n == 2 && strcmp(hits[0].source, "alpha.md") == 0);
    TEST_ASSERT(n == 2 && strcmp(hits[1].source, "gamma.md") == 0);
    TEST_ASSERT(n == 2 && hits[0].score > 0.99f && hits[0].cosine == hits[0].score);
    crag_hits_free(hits, n);
    crag_db_close(db);
}

static void test_bm25_needs_no_vector(void) {
    crag_db_t *db = fixture();
    crag_hit_t *hits = NULL; int n = 0;
    TEST_ASSERT(crag_db_search(db, CRAG_MODE_BM25, NULL, 0, "dogs", 5, &hits, &n) == CRAG_OK);
    TEST_ASSERT(n == 1 && strcmp(hits[0].source, "beta.md") == 0);
    TEST_ASSERT(n == 1 && hits[0].bm25 > 0.0f && hits[0].cosine == -2.0f);
    crag_hits_free(hits, n);
    /* No lexical match at all: zero hits, NULL array, still OK. */
    hits = (crag_hit_t *)1; n = -1;
    TEST_ASSERT(crag_db_search(db, CRAG_MODE_BM25, NULL, 0, "zebra", 5, &hits, &n) == CRAG_OK);
    TEST_ASSERT(n == 0 && hits == NULL);
    crag_db_close(db);
}

static void test_hybrid_fuses_both(void) {
    crag_db_t *db = fixture();
    /* gamma.md is first for cosine (the query is its own vector) AND for
     * BM25 (it alone has both words), so RRF puts it first. With a query
     * vector nearer alpha, alpha would be 1st/2nd and gamma 2nd/1st: an
     * exact RRF tie (1/61 + 1/62 each), so no order could be asserted. */
    const float q[DIM] = {0.9f, 0.1f, 0};
    crag_hit_t *hits = NULL; int n = 0;
    TEST_ASSERT(crag_db_search(db, CRAG_MODE_HYBRID, q, DIM, "red fox", 3, &hits, &n) == CRAG_OK);
    TEST_ASSERT(n >= 2 && strcmp(hits[0].source, "gamma.md") == 0);
    TEST_ASSERT(n >= 2 && hits[0].cosine > 0.9f && hits[0].bm25 > 0.0f);
    crag_hits_free(hits, n);
    crag_db_close(db);
}

static void test_dim_contract(void) {
    crag_db_t *db = fixture();
    const float v4[4] = {1, 0, 0, 0};
    TEST_ASSERT(crag_db_add(db, "x", "y", v4, 4) == CRAG_EDIM_MISMATCH);
    crag_hit_t *hits = NULL; int n = 0;
    TEST_ASSERT(crag_db_search(db, CRAG_MODE_COSINE, v4, 4, NULL, 1, &hits, &n) == CRAG_EDIM_MISMATCH);
    TEST_ASSERT(crag_db_add(db, "x", "y", v4, 0) == CRAG_EINVAL);
    TEST_ASSERT(crag_db_add(db, "x", "y", v4, CRAG_EMBED_DIM_MAX + 1) == CRAG_EINVAL);
    crag_db_close(db);
}

static void test_argument_checks(void) {
    crag_db_t *db = fixture();
    const float q[DIM] = {1, 0, 0};
    crag_hit_t *hits = NULL; int n = 0;
    TEST_ASSERT(crag_db_search(db, CRAG_MODE_COSINE, NULL, DIM, NULL, 1, &hits, &n) == CRAG_EINVAL);
    TEST_ASSERT(crag_db_search(db, CRAG_MODE_BM25, NULL, 0, NULL, 1, &hits, &n) == CRAG_EINVAL);
    TEST_ASSERT(crag_db_search(db, CRAG_MODE_HYBRID, q, DIM, NULL, 1, &hits, &n) == CRAG_EINVAL);
    TEST_ASSERT(crag_db_search(db, CRAG_MODE_COSINE, q, DIM, NULL, 0, &hits, &n) == CRAG_EINVAL);
    TEST_ASSERT(crag_db_search(db, (crag_mode_t)7, q, DIM, "x", 1, &hits, &n) == CRAG_EINVAL);
    TEST_ASSERT(crag_db_add(db, NULL, "t", q, DIM) == CRAG_EINVAL);
    TEST_ASSERT(crag_db_open(NULL, &db) == CRAG_EINVAL);
    crag_db_close(db);
    crag_db_close(NULL);                 /* NULL-safe */
    crag_hits_free(NULL, 3);
}

static void test_persists_on_disk(void) {
    const char *path = "/tmp/crag_test_lib.db";
    remove(path);
    crag_db_t *db = NULL;
    const float v[DIM] = {0, 0, 1};
    TEST_ASSERT(crag_db_open(path, &db) == CRAG_OK);
    TEST_ASSERT(crag_db_add(db, "disk.md", "kept on disk", v, DIM) == CRAG_OK);
    crag_db_close(db);
    TEST_ASSERT(crag_db_open(path, &db) == CRAG_OK);
    int chunks = 0, dim = 0;
    TEST_ASSERT(crag_db_stats(db, &chunks, NULL, &dim) == CRAG_OK && chunks == 1 && dim == DIM);
    /* The reopened index remembers its dim. */
    const float v2[2] = {1, 0};
    TEST_ASSERT(crag_db_add(db, "x", "y", v2, 2) == CRAG_EDIM_MISMATCH);
    crag_db_close(db);
    remove(path);
}

int main(void) {
    TEST_RUN(test_open_add_stats);
    TEST_RUN(test_cosine_ranks_by_vector);
    TEST_RUN(test_bm25_needs_no_vector);
    TEST_RUN(test_hybrid_fuses_both);
    TEST_RUN(test_dim_contract);
    TEST_RUN(test_argument_checks);
    TEST_RUN(test_persists_on_disk);
    TEST_DONE();
}

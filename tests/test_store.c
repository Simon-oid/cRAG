#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "crag.h"
#include "store.h"
#include "test.h"

static int approx(float a, float b) { return fabsf(a - b) < 1e-3f; }

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

static void test_file_is_current_roundtrip(void) {
    const char *p = "/tmp/crag_test_store_iscur.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    /* No row yet -> not current */
    TEST_ASSERT(store_file_is_current(&s, "a.md", 100, 200) == 0);

    /* Upsert and confirm exact match is current */
    TEST_ASSERT(store_file_upsert(&s, "a.md", 100, 200, 1000) == 0);
    TEST_ASSERT(store_file_is_current(&s, "a.md", 100, 200) == 1);

    /* mtime mismatch -> stale */
    TEST_ASSERT(store_file_is_current(&s, "a.md", 101, 200) == 0);
    /* size mismatch -> stale */
    TEST_ASSERT(store_file_is_current(&s, "a.md", 100, 201) == 0);

    /* Upsert again with new (mtime,size) -> now that pair is current */
    TEST_ASSERT(store_file_upsert(&s, "a.md", 101, 201, 1001) == 0);
    TEST_ASSERT(store_file_is_current(&s, "a.md", 101, 201) == 1);
    TEST_ASSERT(store_file_is_current(&s, "a.md", 100, 200) == 0);

    store_close(&s);
    cleanup_db(p);
}

static void test_delete_chunks_for(void) {
    const char *p = "/tmp/crag_test_store_delsrc.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    crag_chunk_t c = {0};
    c.dim = 2; c.vec[0] = 1.0f;
    c.source = "a.md"; c.text = "t1"; TEST_ASSERT(store_insert(&s, &c) == 0);
    c.source = "a.md"; c.text = "t2"; TEST_ASSERT(store_insert(&s, &c) == 0);
    c.source = "b.md"; c.text = "t3"; TEST_ASSERT(store_insert(&s, &c) == 0);

    TEST_ASSERT(store_delete_chunks_for(&s, "a.md") == 0);

    crag_store_stats_t st = {0};
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.chunks == 1);
    TEST_ASSERT(st.files  == 1);

    store_close(&s);
    cleanup_db(p);
}

static void test_sweep_missing_clears_stale(void) {
    const char *p = "/tmp/crag_test_store_sweep.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    /* Two files: "kept.md" (indexed_at=2000) and "gone.md" (indexed_at=1000) */
    crag_chunk_t c = {0};
    c.dim = 2; c.vec[0] = 1.0f;
    c.source = "kept.md"; c.text = "k"; TEST_ASSERT(store_insert(&s, &c) == 0);
    c.source = "gone.md"; c.text = "g1"; TEST_ASSERT(store_insert(&s, &c) == 0);
    c.source = "gone.md"; c.text = "g2"; TEST_ASSERT(store_insert(&s, &c) == 0);

    TEST_ASSERT(store_file_upsert(&s, "kept.md", 10, 20, 2000) == 0);
    TEST_ASSERT(store_file_upsert(&s, "gone.md", 11, 21, 1000) == 0);

    /* Run timestamp = 1500; only "gone.md" is stale. */
    int files_removed = -1, chunks_removed = -1;
    TEST_ASSERT(store_sweep_missing(&s, 1500, &files_removed, &chunks_removed) == 0);
    TEST_ASSERT(files_removed  == 1);
    TEST_ASSERT(chunks_removed == 2);

    crag_store_stats_t st = {0};
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.chunks == 1);  /* only kept.md's chunk left */
    TEST_ASSERT(st.files  == 1);

    /* Row for gone.md must be gone; kept.md must still match. */
    TEST_ASSERT(store_file_is_current(&s, "gone.md", 11, 21) == 0);
    TEST_ASSERT(store_file_is_current(&s, "kept.md", 10, 20) == 1);

    store_close(&s);
    cleanup_db(p);
}

static void test_file_touch_updates_indexed_at(void) {
    const char *p = "/tmp/crag_test_store_touch.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    /* Seed a chunk + files row with an old indexed_at. */
    crag_chunk_t c = {0};
    c.dim = 2; c.vec[0] = 1.0f;
    c.source = "x.md"; c.text = "t"; TEST_ASSERT(store_insert(&s, &c) == 0);
    TEST_ASSERT(store_file_upsert(&s, "x.md", 5, 6, 1000) == 0);

    /* Touch brings it up to "seen" at run_ts=2000, so a 1500-cutoff sweep
       must NOT remove it. */
    TEST_ASSERT(store_file_touch(&s, "x.md", 2000) == 0);
    int files_removed = -1, chunks_removed = -1;
    TEST_ASSERT(store_sweep_missing(&s, 1500, &files_removed, &chunks_removed) == 0);
    TEST_ASSERT(files_removed  == 0);
    TEST_ASSERT(chunks_removed == 0);

    store_close(&s);
    cleanup_db(p);
}

static void test_bm25_retrieves_lexical_match(void) {
    const char *p = "/tmp/crag_test_store_bm25.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    /* Seed three docs. Only the first contains the exact word "gnupg".
       The embedding vectors are all zero — cosine alone can't tell them
       apart, so this test isolates the BM25 path. */
    crag_chunk_t c = {0};
    c.dim = 4;
    c.source = "gpg.md";  c.text = "GnuPG signs and verifies messages";
    TEST_ASSERT(store_insert(&s, &c) == 0);
    c.source = "sq.md";   c.text = "Sequoia is a modern OpenPGP library";
    TEST_ASSERT(store_insert(&s, &c) == 0);
    c.source = "img.md";  c.text = "Cats napping in the sunshine";
    TEST_ASSERT(store_insert(&s, &c) == 0);

    crag_chunk_t results[3] = {0};
    float scores[3] = {0};
    int n = store_search_bm25(&s, "gnupg signing", 3, results, scores);
    TEST_ASSERT(n >= 1);
    TEST_ASSERT(strcmp(results[0].source, "gpg.md") == 0);
    TEST_ASSERT(scores[0] > 0.0f);  /* negated bm25: higher = better */

    for (int i = 0; i < n; i++) free_result_row(&results[i]);
    store_close(&s);
    cleanup_db(p);
}

static void test_highlight_wraps_matched_terms(void) {
    const char *p = "/tmp/crag_test_store_highlight.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    crag_chunk_t c = {0};
    c.dim = 4;
    c.source = "gpg.md";
    c.text   = "GnuPG signs and verifies messages";
    TEST_ASSERT(store_insert(&s, &c) == 0);
    int gpg_id = sqlite3_last_insert_rowid(s.db);

    char *hl = store_highlight_for_id(&s, "gnupg verifies", gpg_id, "<<", ">>");
    TEST_ASSERT(hl != NULL);
    if (hl) {
        TEST_ASSERT(strstr(hl, "<<GnuPG>>")    != NULL);
        TEST_ASSERT(strstr(hl, "<<verifies>>") != NULL);
        TEST_ASSERT(strstr(hl, "signs")        != NULL);   /* unmatched: untouched */
        free(hl);
    }

    /* No usable tokens → NULL (caller falls back to raw). */
    char *hl_empty = store_highlight_for_id(&s, "?? !!", gpg_id, "<<", ">>");
    TEST_ASSERT(hl_empty == NULL);

    /* Chunk that doesn't match the query → NULL. */
    c.source = "cat.md"; c.text = "Cats napping in the sunshine";
    TEST_ASSERT(store_insert(&s, &c) == 0);
    int cat_id = sqlite3_last_insert_rowid(s.db);
    char *hl_miss = store_highlight_for_id(&s, "gnupg", cat_id, "<<", ">>");
    TEST_ASSERT(hl_miss == NULL);

    store_close(&s);
    cleanup_db(p);
}

static void test_bm25_empty_query_returns_zero(void) {
    const char *p = "/tmp/crag_test_store_bm25_empty.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    crag_chunk_t c = {0};
    c.dim = 2; c.source = "a.md"; c.text = "hello"; c.vec[0] = 1.0f;
    TEST_ASSERT(store_insert(&s, &c) == 0);

    crag_chunk_t results[3] = {0};
    float scores[3] = {0};
    /* All-punctuation → no usable tokens → 0 hits (not an error). */
    TEST_ASSERT(store_search_bm25(&s, "??? !!! ***", 3, results, scores) == 0);
    /* NULL is safe too. */
    TEST_ASSERT(store_search_bm25(&s, NULL, 3, results, scores) == 0);

    store_close(&s);
    cleanup_db(p);
}

static void test_hybrid_fuses_both_retrievers(void) {
    const char *p = "/tmp/crag_test_store_hybrid.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    /* Three docs with distinct vectors:
         vecA = (1,0,0,0), text: "alpha bravo"  -> cosine wins on vecA
         vecB = (0,1,0,0), text: "charlie delta" -> cosine near-miss
         vecC = (0,0,1,0), text: "alpha echo"   -> BM25 also hits "alpha" */
    crag_chunk_t c = {0};
    c.dim = 4;
    c.source = "A.md"; c.text = "alpha bravo";
    memset(c.vec, 0, sizeof c.vec); c.vec[0] = 1.0f;
    TEST_ASSERT(store_insert(&s, &c) == 0);
    c.source = "B.md"; c.text = "charlie delta";
    memset(c.vec, 0, sizeof c.vec); c.vec[1] = 1.0f;
    TEST_ASSERT(store_insert(&s, &c) == 0);
    c.source = "C.md"; c.text = "alpha echo";
    memset(c.vec, 0, sizeof c.vec); c.vec[2] = 1.0f;
    TEST_ASSERT(store_insert(&s, &c) == 0);

    /* Query vector aligns with A exactly. Query text contains "alpha",
       which matches both A and C via BM25. Hybrid top-1 should still be
       A (high cosine + BM25 hit), but C should beat B overall because
       BM25 lifts C while B has zero lexical signal. */
    float qvec[4] = {1.0f, 0, 0, 0};
    crag_chunk_t results[3] = {0};
    crag_score_t scores[3] = {0};
    int n = store_search_hybrid(&s, qvec, 4, "alpha", 3, results, scores);
    TEST_ASSERT(n == 3);
    TEST_ASSERT(strcmp(results[0].source, "A.md") == 0);
    /* fusion scores come from RRF → positive, and monotonically decreasing */
    TEST_ASSERT(scores[0].fusion > 0.0f);
    TEST_ASSERT(scores[0].fusion >= scores[1].fusion);
    TEST_ASSERT(scores[1].fusion >= scores[2].fusion);
    /* A's vector equals the query → cosine 1.0; A also matches BM25 "alpha". */
    TEST_ASSERT(approx(scores[0].cosine, 1.0f));
    TEST_ASSERT(scores[0].bm25 > 0.0f);
    /* C must rank higher than B (it got a BM25 boost that B didn't). The raw
       scores must distinguish them even though both have cosine ~0: C has a
       lexical hit ("alpha"), B has none. */
    int rank_b = -1, rank_c = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(results[i].source, "B.md") == 0) rank_b = i;
        if (strcmp(results[i].source, "C.md") == 0) rank_c = i;
    }
    TEST_ASSERT(rank_c >= 0);
    TEST_ASSERT(rank_b >= 0);
    TEST_ASSERT(rank_c < rank_b);
    TEST_ASSERT(approx(scores[rank_c].cosine, 0.0f));
    TEST_ASSERT(scores[rank_c].bm25 > 0.0f);   /* C: lexical hit  */
    TEST_ASSERT(approx(scores[rank_b].cosine, 0.0f));
    TEST_ASSERT(scores[rank_b].bm25 == 0.0f);  /* B: no lexical hit */

    for (int i = 0; i < n; i++) free_result_row(&results[i]);
    store_close(&s);
    cleanup_db(p);
}

static void test_hybrid_dedupes_shared_results(void) {
    /* If cosine and BM25 both return the same chunk at rank 1, hybrid must
       not emit it twice. Also verifies RRF boost (sum of contributions). */
    const char *p = "/tmp/crag_test_store_hybrid_dedupe.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    crag_chunk_t c = {0};
    c.dim = 2; c.source = "only.md"; c.text = "unique token"; c.vec[0] = 1.0f;
    TEST_ASSERT(store_insert(&s, &c) == 0);

    float qvec[2] = {1.0f, 0};
    crag_chunk_t results[3] = {0};
    crag_score_t scores[3] = {0};
    int n = store_search_hybrid(&s, qvec, 2, "unique", 3, results, scores);
    TEST_ASSERT(n == 1);  /* dedup to a single unique doc */
    TEST_ASSERT(strcmp(results[0].source, "only.md") == 0);
    /* Present in both retrievers at rank 1 → RRF = 2 * 1/61 ≈ 0.0328 */
    TEST_ASSERT(scores[0].fusion > 0.03f && scores[0].fusion < 0.04f);
    /* Raw scores survive the fusion: exact vector match → cosine 1.0,
       and the "unique" token gives a positive BM25 hit. */
    TEST_ASSERT(approx(scores[0].cosine, 1.0f));
    TEST_ASSERT(scores[0].bm25 > 0.0f);

    for (int i = 0; i < n; i++) free_result_row(&results[i]);
    store_close(&s);
    cleanup_db(p);
}

static void test_hybrid_scores_bm25_only_hit(void) {
    /* A doc that ranks outside the cosine pool but matches BM25 must still
       come back with a real cosine score, computed from its stored vector
       (the !have_cos recompute path), not left as CRAG_SCORE_NA. */
    const char *p = "/tmp/crag_test_store_hybrid_bm25only.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    /* 20 filler docs with vec=(1,0): identical high cosine to the query,
       enough to fill the cosine pool (max(k*4,20)=20 for k=5). */
    crag_chunk_t c = {0};
    c.dim = 2;
    for (int i = 0; i < 20; i++) {
        char name[32]; snprintf(name, sizeof name, "fill%d.md", i);
        c.source = name; c.text = "filler text";
        memset(c.vec, 0, sizeof c.vec); c.vec[0] = 1.0f;
        TEST_ASSERT(store_insert(&s, &c) == 0);
    }
    /* Target doc: orthogonal vector (cosine 0 → outside the pool of 20),
       but its text carries the query token, so only BM25 surfaces it. */
    c.source = "target.md"; c.text = "zebra crossing";
    memset(c.vec, 0, sizeof c.vec); c.vec[1] = 1.0f;
    TEST_ASSERT(store_insert(&s, &c) == 0);

    float qvec[2] = {1.0f, 0};
    crag_chunk_t results[5] = {0};
    crag_score_t scores[5] = {0};
    int n = store_search_hybrid(&s, qvec, 2, "zebra", 5, results, scores);
    TEST_ASSERT(n >= 1);
    int rank = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(results[i].source, "target.md") == 0) rank = i;
    TEST_ASSERT(rank >= 0);                          /* BM25 surfaced it     */
    TEST_ASSERT(scores[rank].bm25 > 0.0f);           /* lexical hit recorded */
    TEST_ASSERT(scores[rank].cosine != CRAG_SCORE_NA); /* cosine recomputed  */
    TEST_ASSERT(approx(scores[rank].cosine, 0.0f));  /* orthogonal → ~0      */

    for (int i = 0; i < n; i++) free_result_row(&results[i]);
    store_close(&s);
    cleanup_db(p);
}

static void test_delete_chunks_for_updates_fts(void) {
    /* The chunks_ad trigger must keep FTS in sync so a deleted chunk is
       not returned by BM25. Without the trigger this would silently leak. */
    const char *p = "/tmp/crag_test_store_fts_sync.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);

    crag_chunk_t c = {0};
    c.dim = 2; c.vec[0] = 1.0f;
    c.source = "del.md"; c.text = "quokka sunrise";
    TEST_ASSERT(store_insert(&s, &c) == 0);
    c.source = "keep.md"; c.text = "koala dinner";
    TEST_ASSERT(store_insert(&s, &c) == 0);

    /* Sanity: BM25 finds "quokka" initially. */
    crag_chunk_t r[2] = {0}; float sc[2] = {0};
    TEST_ASSERT(store_search_bm25(&s, "quokka", 2, r, sc) == 1);
    for (int i = 0; i < 1; i++) free_result_row(&r[i]);

    /* Delete "del.md"; BM25 should now return 0 hits for "quokka". */
    TEST_ASSERT(store_delete_chunks_for(&s, "del.md") == 0);
    memset(r, 0, sizeof r); memset(sc, 0, sizeof sc);
    TEST_ASSERT(store_search_bm25(&s, "quokka", 2, r, sc) == 0);

    store_close(&s);
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
    TEST_RUN(test_file_is_current_roundtrip);
    TEST_RUN(test_delete_chunks_for);
    TEST_RUN(test_sweep_missing_clears_stale);
    TEST_RUN(test_file_touch_updates_indexed_at);
    TEST_RUN(test_bm25_retrieves_lexical_match);
    TEST_RUN(test_bm25_empty_query_returns_zero);
    TEST_RUN(test_highlight_wraps_matched_terms);
    TEST_RUN(test_hybrid_fuses_both_retrievers);
    TEST_RUN(test_hybrid_dedupes_shared_results);
    TEST_RUN(test_hybrid_scores_bm25_only_hit);
    TEST_RUN(test_delete_chunks_for_updates_fts);
    TEST_DONE();
}

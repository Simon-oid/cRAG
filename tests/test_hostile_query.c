/* test_hostile_query.c — Tier 4.7 hostile-query suite.
 *
 * Feeds adversarial query strings to the BM25 and hybrid search paths (the
 * FTS5 MATCH surface an agent/attacker reaches through crag_search). Asserts
 * the store never crashes, never returns a negative/garbage count, never
 * corrupts the DB, and that any rows it does return are well-formed. The real
 * teeth are `make test-asan` (heap red-zones on the internal FTS5 sanitize
 * buffers) and `make valgrind`; this driver just supplies the hostile inputs
 * and the invariants. SQLite uses a bound parameter for MATCH, so SQL-injection
 * strings are inert by construction — we assert that too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "crag.h"
#include "store.h"
#include "test.h"

#define K 5

static void cleanup_db(const char *p) {
    unlink(p);
    char wal[256]; snprintf(wal, sizeof wal, "%s-wal", p); unlink(wal);
    char shm[256]; snprintf(shm, sizeof shm, "%s-shm", p); unlink(shm);
}

static void ins(crag_store_t *s, const char *src, const char *text) {
    crag_chunk_t c = {0};
    c.source = (char *)src; c.text = (char *)text; c.dim = 4;
    c.vec[0] = 0.5f; c.vec[1] = 0.5f; c.vec[2] = 0.5f; c.vec[3] = 0.5f;
    TEST_ASSERT(store_insert(s, &c) == 0);
}

static void seed(crag_store_t *s) {
    ins(s, "a.md", "the quick brown fox jumps over the lazy dog");
    ins(s, "b.md", "gnupg signing key management and revocation");
    ins(s, "c.md", "caffè società città però — accented italian tokens");
    ins(s, "d.md", "NEAR AND OR NOT column matchinfo offsets");
    ins(s, "e.md", "plain ordinary words for baseline matching here");
}

/* Drive both search modes with one query string; assert sane returns and
   free any rows. Returns nothing — failures land via TEST_ASSERT. */
static void probe(crag_store_t *s, const char *q) {
    crag_chunk_t r[K];
    float        sc[K];
    crag_score_t hsc[K];
    float qvec[4] = {0.5f, 0.5f, 0.5f, 0.5f};

    memset(r, 0, sizeof r);
    int n = store_search_bm25(s, q, K, r, sc);
    TEST_ASSERT(n >= 0 && n <= K);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT(r[i].source != NULL);   /* well-formed row */
        TEST_ASSERT(r[i].text   != NULL);
        free(r[i].source); free(r[i].text);
        r[i].source = r[i].text = NULL;
    }

    memset(r, 0, sizeof r);
    int m = store_search_hybrid(s, qvec, 4, q, K, r, hsc);
    TEST_ASSERT(m >= 0 && m <= K);
    for (int i = 0; i < m; i++) {
        TEST_ASSERT(r[i].source != NULL);
        TEST_ASSERT(r[i].text   != NULL);
        free(r[i].source); free(r[i].text);
        r[i].source = r[i].text = NULL;
    }
}

static void test_fts5_syntax(void) {
    const char *p = "/tmp/crag_hostile_q_fts.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);
    seed(&s);

    const char *qs[] = {
        "\"unterminated quote",
        "\"\"\"\"\"",
        "fox AND",
        "AND OR NOT",
        "OR OR OR OR",
        "NEAR(fox dog, 3)",
        "column : value",
        "title:fox body:dog",
        "fox*",
        "*fox",
        "*",
        "^fox$",
        "(((fox)))",
        "fox -dog",
        "fox NOT dog",
        "{nested} [brackets] <angle>",
        "fox\"dog\"cat",
        "a/b\\c|d&e",
        "'; DROP TABLE chunks; --",
        "1) OR 1=1; --",
        "%%%___%%%",
        "\\\\\\\\",
        "....",
        "??? !!! ***",
        "+-+-+-",
    };
    for (size_t i = 0; i < sizeof qs / sizeof *qs; i++) probe(&s, qs[i]);

    /* DB still intact after the barrage. */
    crag_store_stats_t st = {0};
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.chunks == 5);
    store_close(&s);
    cleanup_db(p);
}

static void test_unicode_and_bytes(void) {
    const char *p = "/tmp/crag_hostile_q_uni.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);
    seed(&s);

    const char *qs[] = {
        "caffè società",                 /* accented, should match c.md */
        "\xC3\xA8\xC3\xA9\xC3\xA0",       /* è é à bytes */
        "\xE2\x80\x8B\xE2\x80\x8C",       /* zero-width space / non-joiner */
        "\xE2\x80\x8F fox",              /* RTL mark then a token */
        "\xF0\x9F\x98\x80\xF0\x9F\x94\x91", /* emoji 😀🔑 */
        "\xEF\xBB\xBF fox",              /* UTF-8 BOM then token */
        "fox\xC3",                       /* truncated 2-byte sequence */
        "fox\xE2\x80",                   /* truncated 3-byte sequence */
        "fox\xF0\x9F\x98",               /* truncated 4-byte sequence */
        "\xFF\xFE\xFD\xFC",              /* invalid UTF-8 lead bytes */
        "\x01\x02\x03 fox \x7F",         /* control chars around a token */
        "ＦＯＸ",                          /* fullwidth latin */
    };
    for (size_t i = 0; i < sizeof qs / sizeof *qs; i++) probe(&s, qs[i]);

    crag_store_stats_t st = {0};
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.chunks == 5);
    store_close(&s);
    cleanup_db(p);
}

static void test_empty_blank_and_null(void) {
    const char *p = "/tmp/crag_hostile_q_empty.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);
    seed(&s);

    probe(&s, "");
    probe(&s, "   ");
    probe(&s, "\t\n\r\f\v");
    probe(&s, "        \t        ");

    /* NULL qtext: bm25 must tolerate it (documented to sanitize internally). */
    crag_chunk_t r[K]; float sc[K];
    memset(r, 0, sizeof r);
    TEST_ASSERT(store_search_bm25(&s, NULL, K, r, sc) >= 0);

    store_close(&s);
    cleanup_db(p);
}

static void test_huge_query(void) {
    const char *p = "/tmp/crag_hostile_q_huge.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);
    seed(&s);

    /* 100 KB query: alternating real token + punctuation so the sanitizer
       has heavy work to do (strip, quote, OR). */
    size_t big = 100 * 1024;
    char *q = (char *)malloc(big + 1);
    TEST_ASSERT(q != NULL);
    for (size_t i = 0; i < big; i++)
        q[i] = (i % 6 == 5) ? ' ' : (i % 6 < 3 ? "fox"[i % 3] : "!@#"[i % 3]);
    q[big] = '\0';
    probe(&s, q);

    /* Same length but one giant unbroken token (no whitespace to split on). */
    memset(q, 'z', big); q[big] = '\0';
    probe(&s, q);
    free(q);

    crag_store_stats_t st = {0};
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.chunks == 5);
    store_close(&s);
    cleanup_db(p);
}

int main(void) {
    fprintf(stderr, "test_hostile_query\n");
    TEST_RUN(test_fts5_syntax);
    TEST_RUN(test_unicode_and_bytes);
    TEST_RUN(test_empty_blank_and_null);
    TEST_RUN(test_huge_query);
    TEST_DONE();
}

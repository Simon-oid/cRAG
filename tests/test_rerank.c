/* Offline tests for the cross-encoder rerank response parser. No live
   server: we feed llama.cpp / Cohere-style JSON straight into the parser. */
#include <math.h>
#include "test.h"

/* non-static in src/rerank_llamacpp.c */
int rerank_parse_scores(const char *json, int n, float *scores);

static void seed(float *s, int n) { for (int i = 0; i < n; i++) s[i] = -1e30f; }

static void test_basic_in_order(void) {
    const char *json =
        "{\"model\":\"r\",\"results\":["
        "{\"index\":0,\"relevance_score\":0.10},"
        "{\"index\":1,\"relevance_score\":0.90},"
        "{\"index\":2,\"relevance_score\":0.50}]}";
    float s[3]; seed(s, 3);
    int got = rerank_parse_scores(json, 3, s);
    TEST_ASSERT(got == 3);
    TEST_ASSERT(fabsf(s[0] - 0.10f) < 1e-6);
    TEST_ASSERT(fabsf(s[1] - 0.90f) < 1e-6);
    TEST_ASSERT(fabsf(s[2] - 0.50f) < 1e-6);
}

static void test_out_of_order(void) {
    /* Cohere/Jina return results sorted by score desc; index points back. */
    const char *json =
        "{\"results\":["
        "{\"index\":1,\"relevance_score\":8.0},"
        "{\"index\":2,\"relevance_score\":3.0},"
        "{\"index\":0,\"relevance_score\":-2.5}]}";
    float s[3]; seed(s, 3);
    int got = rerank_parse_scores(json, 3, s);
    TEST_ASSERT(got == 3);
    TEST_ASSERT(fabsf(s[0] - (-2.5f)) < 1e-6);
    TEST_ASSERT(fabsf(s[1] - 8.0f) < 1e-6);
    TEST_ASSERT(fabsf(s[2] - 3.0f) < 1e-6);
}

static void test_score_key_fallback(void) {
    const char *json =
        "{\"results\":[{\"index\":0,\"score\":1.25},{\"index\":1,\"score\":0.0}]}";
    float s[2]; seed(s, 2);
    int got = rerank_parse_scores(json, 2, s);
    TEST_ASSERT(got == 2);
    TEST_ASSERT(fabsf(s[0] - 1.25f) < 1e-6);
    TEST_ASSERT(fabsf(s[1] - 0.0f) < 1e-6);
}

static void test_negative_and_exponent(void) {
    const char *json =
        "{\"results\":[{\"index\":0,\"relevance_score\":-1.5e-2},"
        "{\"index\":1,\"relevance_score\":2.0e1}]}";
    float s[2]; seed(s, 2);
    int got = rerank_parse_scores(json, 2, s);
    TEST_ASSERT(got == 2);
    TEST_ASSERT(fabsf(s[0] - (-0.015f)) < 1e-6);
    TEST_ASSERT(fabsf(s[1] - 20.0f) < 1e-6);
}

static void test_index_out_of_range_ignored(void) {
    /* A bogus index must not corrupt memory or count as a hit. */
    const char *json =
        "{\"results\":[{\"index\":7,\"relevance_score\":9.9},"
        "{\"index\":0,\"relevance_score\":0.4}]}";
    float s[2]; seed(s, 2);
    int got = rerank_parse_scores(json, 2, s);
    TEST_ASSERT(got == 1);                 /* only index 0 is in range */
    TEST_ASSERT(fabsf(s[0] - 0.4f) < 1e-6);
    TEST_ASSERT(s[1] < -1e29f);            /* untouched floor */
}

static void test_empty_and_malformed(void) {
    float s[2]; seed(s, 2);
    TEST_ASSERT(rerank_parse_scores("{\"results\":[]}", 2, s) == -1);
    TEST_ASSERT(rerank_parse_scores("garbage", 2, s) == -1);
    TEST_ASSERT(rerank_parse_scores(NULL, 2, s) == -1);
}

/* Regression (found by tests/test_fuzz.c under ASan): an "index" key with no
   number after it sitting at the very end of the buffer made the no-number
   branch step p past the NUL, so the next strstr() read out of bounds. The
   parser must stop at the terminator, not walk off it. */
static void test_trailing_index_no_number(void) {
    float s[2]; seed(s, 2);
    TEST_ASSERT(rerank_parse_scores("{\"index\"", 2, s) == -1);
    TEST_ASSERT(rerank_parse_scores("\"index\":", 2, s) == -1);
    TEST_ASSERT(rerank_parse_scores("\"index\"   ", 2, s) == -1);
    /* A valid pair followed by a dangling trailing index must still parse. */
    TEST_ASSERT(rerank_parse_scores(
        "{\"results\":[{\"index\":0,\"score\":1.0}],\"index\"", 2, s) == 1);
}

int main(void) {
    fprintf(stderr, "test_rerank\n");
    TEST_RUN(test_basic_in_order);
    TEST_RUN(test_out_of_order);
    TEST_RUN(test_score_key_fallback);
    TEST_RUN(test_negative_and_exponent);
    TEST_RUN(test_index_out_of_range_ignored);
    TEST_RUN(test_empty_and_malformed);
    TEST_RUN(test_trailing_index_no_number);
    TEST_DONE();
}

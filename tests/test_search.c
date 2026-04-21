#include <math.h>
#include "search.h"
#include "test.h"

static int approx(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void test_identical_vectors_score_one(void) {
    float a[3] = {1.0f, 2.0f, 3.0f};
    float b[3] = {1.0f, 2.0f, 3.0f};
    TEST_ASSERT(approx(search_cosine(a, b, 3), 1.0f));
}

static void test_orthogonal_vectors_score_zero(void) {
    float a[3] = {1.0f, 0.0f, 0.0f};
    float b[3] = {0.0f, 1.0f, 0.0f};
    TEST_ASSERT(approx(search_cosine(a, b, 3), 0.0f));
}

static void test_opposite_vectors_score_minus_one(void) {
    float a[2] = { 1.0f, 0.0f };
    float b[2] = {-1.0f, 0.0f };
    TEST_ASSERT(approx(search_cosine(a, b, 2), -1.0f));
}

static void test_cosine_is_scale_invariant(void) {
    float a[3] = {1.0f, 2.0f, 3.0f};
    float b[3] = {2.0f, 4.0f, 6.0f};
    TEST_ASSERT(approx(search_cosine(a, b, 3), 1.0f));
}

static void test_zero_vectors_do_not_crash(void) {
    float z[3] = {0.0f, 0.0f, 0.0f};
    float a[3] = {1.0f, 2.0f, 3.0f};
    /* divisor has +1e-9 guard — must return finite */
    float s = search_cosine(z, a, 3);
    TEST_ASSERT(isfinite(s));
}

int main(void) {
    fprintf(stderr, "test_search:\n");
    TEST_RUN(test_identical_vectors_score_one);
    TEST_RUN(test_orthogonal_vectors_score_zero);
    TEST_RUN(test_opposite_vectors_score_minus_one);
    TEST_RUN(test_cosine_is_scale_invariant);
    TEST_RUN(test_zero_vectors_do_not_crash);
    TEST_DONE();
}

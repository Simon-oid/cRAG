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

/* Every length, so the 8-lane body and the scalar tail are both covered. */
static void test_dot_norm_matches_double_precision_at_every_length(void) {
    float a[37], b[37];
    for (int i = 0; i < 37; i++) { a[i] = (float)((i * 7) % 11) - 5.0f; b[i] = (float)((i * 3) % 13) - 6.0f; }
    for (int dim = 1; dim <= 37; dim++) {
        double dd = 0, nn = 0;
        for (int i = 0; i < dim; i++) { dd += (double)a[i] * b[i]; nn += (double)b[i] * b[i]; }
        float dot, norm2;
        search_dot_norm(a, b, dim, &dot, &norm2);
        TEST_ASSERT(fabs(dot - dd) < 1e-3 && fabs(norm2 - nn) < 1e-3);
    }
}

/* The scan scores with search_dot_norm and a precomputed query norm; the
   hybrid rescoring calls search_cosine. They must agree to the bit, or a
   chunk could rank differently depending on the path that scored it. */
static void test_cosine_equals_the_scan_formula_bit_for_bit(void) {
    float a[1024], b[1024];
    for (int i = 0; i < 1024; i++) { a[i] = sinf((float)i); b[i] = cosf((float)i * 0.7f); }
    float dot, nb, qa, na;
    search_dot_norm(a, b, 1024, &dot, &nb);
    search_dot_norm(a, a, 1024, &qa, &na);
    float scan = dot / (sqrtf(na) * sqrtf(nb) + 1e-9f);
    TEST_ASSERT(scan == search_cosine(a, b, 1024));
}

int main(void) {
    fprintf(stderr, "test_search:\n");
    TEST_RUN(test_identical_vectors_score_one);
    TEST_RUN(test_orthogonal_vectors_score_zero);
    TEST_RUN(test_opposite_vectors_score_minus_one);
    TEST_RUN(test_cosine_is_scale_invariant);
    TEST_RUN(test_zero_vectors_do_not_crash);
    TEST_RUN(test_dot_norm_matches_double_precision_at_every_length);
    TEST_RUN(test_cosine_equals_the_scan_formula_bit_for_bit);
    TEST_DONE();
}

#include <math.h>
#include "search.h"

/* Eight independent accumulators, summed in a fixed order at the end. The
 * order of every addition is set here, in C, not by the instruction set:
 * the compiler vectorises the lanes (SSE2 by default, AVX2 in the clone
 * below) without reassociating them, so each build returns the same bits on
 * every x86-64 CPU and the result never depends on which clone ran. One
 * accumulator, as before, could not be vectorised without -ffast-math and
 * ran one multiply-add at a time. */
#define LANES 8

/* Not under TSan: the clone's ifunc resolver runs before the TSan runtime
   is up, and the binary crashes at startup. The result is the same either
   way, only slower. */
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && defined(__linux__) \
    && !defined(__SANITIZE_THREAD__)
#define CRAG_CLONES __attribute__((target_clones("avx2", "default")))
#else
#define CRAG_CLONES
#endif

static inline float lane_sum(const float v[LANES]) {
    return ((v[0] + v[1]) + (v[2] + v[3])) + ((v[4] + v[5]) + (v[6] + v[7]));
}

CRAG_CLONES
void search_dot_norm(const float *q, const float *v, int dim, float *dot, float *norm2) {
    float d[LANES] = {0}, n[LANES] = {0};
    int i = 0;
    for (; i + LANES <= dim; i += LANES) {
        for (int j = 0; j < LANES; j++) {
            d[j] += q[i + j] * v[i + j];
            n[j] += v[i + j] * v[i + j];
        }
    }
    float ds = lane_sum(d), ns = lane_sum(n);
    for (; i < dim; i++) {
        ds += q[i] * v[i];
        ns += v[i] * v[i];
    }
    *dot = ds;
    *norm2 = ns;
}

CRAG_CLONES
float search_dot(const float *q, const float *v, int dim) {
    float d[LANES] = {0};
    int i = 0;
    for (; i + LANES <= dim; i += LANES) {
        for (int j = 0; j < LANES; j++) d[j] += q[i + j] * v[i + j];
    }
    float ds = lane_sum(d);
    for (; i < dim; i++) ds += q[i] * v[i];
    return ds;
}

float search_cosine(const float *a, const float *b, int dim) {
    float dot, nb, aa, na;
    search_dot_norm(a, b, dim, &dot, &nb);
    search_dot_norm(a, a, dim, &aa, &na);
    (void)aa;
    return dot / (sqrtf(na) * sqrtf(nb) + 1e-9f);
}

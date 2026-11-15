/* Exact cosine scan: latency of store_search, checked against a double-
 * precision brute force.
 *
 *   make bench-scan                       # N=10000 and N=100000, dim 1024
 *   ./bench/bench_scan N [dim] [queries] [db]
 *
 * Vectors are pseudo-random (fixed seed) and each query is a stored vector
 * plus noise, so every query has a clear nearest neighbour and a ranked tail,
 * as real embeddings do. Each chunk carries 1000 characters of text, about
 * a real chunk's size, so the scan reads rows as large as the index's.
 * "same top-10" compares the ids store_search returns with the reference's,
 * in order; a tie within float rounding could differ, so it is reported,
 * not asserted.
 */
#define _POSIX_C_SOURCE 200809L  /* clock_gettime */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "crag.h"
#include "store.h"

#define K 10
#define TEXT_CHARS 1000

static unsigned long long rng_state = 0x9e3779b97f4a7c15ULL;

static float rnd(void) {           /* uniform in [-1, 1), xorshift64* */
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    unsigned long long r = rng_state * 2685821657736338717ULL;
    return (float)((double)(r >> 11) / (double)(1ULL << 53)) * 2.0f - 1.0f;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* ids (1-based insertion order) of the top K by double-precision cosine */
static void reference_topk(const float *base, int n, int dim, const float *q, int *ids) {
    double best[K];
    for (int j = 0; j < K; j++) { best[j] = -2.0; ids[j] = -1; }
    double qn = 0;
    for (int d = 0; d < dim; d++) qn += (double)q[d] * q[d];
    for (int i = 0; i < n; i++) {
        const float *v = base + (size_t)i * dim;
        double dot = 0, vn = 0;
        for (int d = 0; d < dim; d++) { dot += (double)q[d] * v[d]; vn += (double)v[d] * v[d]; }
        double s = dot / (sqrt(qn) * sqrt(vn));
        if (s <= best[K - 1]) continue;
        int j = K - 1;
        while (j > 0 && best[j - 1] < s) { best[j] = best[j - 1]; ids[j] = ids[j - 1]; j--; }
        best[j] = s;
        ids[j] = i + 1;
    }
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 10000;
    int dim = argc > 2 ? atoi(argv[2]) : 1024;
    int nq = argc > 3 ? atoi(argv[3]) : 100;
    const char *db = argc > 4 ? argv[4] : ":memory:";
    if (n < K || dim < 1 || dim > CRAG_EMBED_DIM_MAX || nq < 1) {
        fprintf(stderr, "usage: %s N [dim] [queries] [db]\n", argv[0]);
        return 2;
    }

    float *base = malloc((size_t)n * dim * sizeof *base);
    crag_chunk_t *chunk = calloc(1, sizeof *chunk);
    crag_chunk_t *res = calloc(K, sizeof *res);
    double *lat = malloc((size_t)nq * sizeof *lat);
    char *text = malloc(TEXT_CHARS + 1);
    if (!base || !chunk || !res || !lat || !text) { fprintf(stderr, "out of memory\n"); return 1; }
    memset(text, 'x', TEXT_CHARS);
    text[TEXT_CHARS] = '\0';

    crag_store_t store;
    if (store_open(db, &store) != 0) { fprintf(stderr, "cannot open %s\n", db); return 1; }
    store_begin(&store);
    char src[32];
    for (int i = 0; i < n; i++) {
        float *v = base + (size_t)i * dim;
        for (int d = 0; d < dim; d++) v[d] = rnd();
        snprintf(src, sizeof src, "d%d", i + 1);
        chunk->source = src;
        chunk->text = text;
        chunk->dim = dim;
        memcpy(chunk->vec, v, (size_t)dim * sizeof *v);
        if (store_insert(&store, chunk) != 0) { fprintf(stderr, "insert failed\n"); return 1; }
    }
    store_commit(&store);

    float *q = malloc((size_t)dim * sizeof *q);
    float scores[K];
    int same = 0;
    for (int t = 0; t < nq; t++) {
        const float *target = base + (size_t)((t * 7919) % n) * dim;
        for (int d = 0; d < dim; d++) q[d] = target[d] + 0.5f * rnd();
        double t0 = now_ms();
        int got = store_search(&store, q, dim, K, res, scores);
        lat[t] = now_ms() - t0;
        int ref[K];
        reference_topk(base, n, dim, q, ref);
        int match = got == K;
        for (int j = 0; j < got; j++) {
            if (match && res[j].id != ref[j]) match = 0;
            free(res[j].source);
            free(res[j].text);
        }
        same += match;
    }
    qsort(lat, (size_t)nq, sizeof *lat, cmp_double);
    printf("N=%d dim=%d queries=%d  P50 %.2f ms  P95 %.2f ms  same top-%d as reference: %d/%d\n",
           n, dim, nq, lat[nq / 2], lat[(nq * 95) / 100 < nq ? (nq * 95) / 100 : nq - 1],
           K, same, nq);

    store_close(&store);
    free(q); free(base); free(chunk); free(res); free(lat); free(text);
    return same == nq ? 0 : 1;
}

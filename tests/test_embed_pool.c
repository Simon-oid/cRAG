/* TSan stress test for the embed worker pool.
 *
 * Drives embed_pool_create / _run / _destroy in isolation with an
 * in-process toy backend. Goals:
 *   - many workers contend over the same jobs[] slots and shared
 *     bookkeeping (next / done) under the pool's lock + cvs;
 *   - many sequential batches reuse the pool, exercising the wait /
 *     signal cycle on has_work + all_done;
 *   - shutdown unblocks all idle workers cleanly.
 *
 * The previous test suite never spun up the pool — test_retrieval used
 * a synchronous toy embedder and bypassed crag_index entirely — so TSan
 * had nothing to chew on. This binary fills that gap. */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "crag.h"
#include "embed.h"
#include "embed_pool.h"
#include "test.h"

#define TOY_DIM 64

/* Deterministic hashed bag-of-words: mirrors test_retrieval's toy_embed
 * but conforms to the crag_embed_backend_t function pointer shape so it
 * plugs into the real pool. */
static unsigned int fnv1a(const char *s, size_t n) {
    unsigned int h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static int toy_embed_one(const char *text, float *vec, int *dim, void *ctx) {
    (void)ctx;
    for (int i = 0; i < TOY_DIM; i++) vec[i] = 0.0f;
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
        vec[fnv1a(tok, n) % (unsigned)TOY_DIM] += 1.0f;
    }
    *dim = TOY_DIM;
    return 0;
}

/* Slow variant — sleeps briefly inside the handler to widen the race
 * window. With 8 workers and 64 jobs/batch, a 100µs sleep guarantees
 * multiple workers are in-flight simultaneously when TSan inspects
 * accesses to next / done / jobs. */
static int toy_embed_one_slow(const char *text, float *vec, int *dim, void *ctx) {
    struct timespec ts = { 0, 100 * 1000 };   /* 100µs */
    nanosleep(&ts, NULL);
    return toy_embed_one(text, vec, dim, ctx);
}

static crag_embed_backend_t make_backend(int slow) {
    crag_embed_backend_t b = {0};
    b.name       = "test-toy";
    b.embed_one  = slow ? toy_embed_one_slow : toy_embed_one;
    b.embed_many = NULL;
    b.ctx        = NULL;
    return b;
}

/* ---- tests ------------------------------------------------------------ */

static void test_single_worker_runs_jobs(void) {
    crag_embed_backend_t b = make_backend(0);
    embed_pool_t *p = embed_pool_create(&b, 1);
    TEST_ASSERT(p != NULL);

    embed_job_t jobs[16] = {0};
    const char *words[] = {
        "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta",
        "iota", "kappa", "lambda", "mu", "nu", "xi", "omicron", "pi"
    };
    for (int i = 0; i < 16; i++) jobs[i].text = words[i];

    embed_pool_run(p, jobs, 16);

    for (int i = 0; i < 16; i++) {
        TEST_ASSERT(jobs[i].rc  == 0);
        TEST_ASSERT(jobs[i].dim == TOY_DIM);
    }
    embed_pool_destroy(p);
}

static void test_eight_workers_concurrent_batch(void) {
    /* 8 workers, 64-job batch with a slow handler so multiple workers
     * are guaranteed in-flight at any instant. This is the TSan
     * mainline: contention on next++ / done++ / jobs[]. */
    crag_embed_backend_t b = make_backend(1);
    embed_pool_t *p = embed_pool_create(&b, 8);
    TEST_ASSERT(p != NULL);

    enum { N = 64 };
    embed_job_t jobs[N] = {0};
    char *texts[N];
    for (int i = 0; i < N; i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "stress chunk number %d alpha beta gamma", i);
        texts[i]   = strdup(buf);
        jobs[i].text = texts[i];
    }

    embed_pool_run(p, jobs, N);

    int ok = 0;
    for (int i = 0; i < N; i++) {
        if (jobs[i].rc == 0 && jobs[i].dim == TOY_DIM) ok++;
        free(texts[i]);
    }
    TEST_ASSERT(ok == N);
    embed_pool_destroy(p);
}

static void test_many_sequential_batches_reuse_pool(void) {
    /* Exercises the wait/signal cycle: every batch transitions workers
     * idle → busy → idle. Bugs in the all_done signalling or the
     * next/n_jobs reset surface as either a hang (test timeout) or a
     * stale-data race that TSan flags. */
    crag_embed_backend_t b = make_backend(0);
    embed_pool_t *p = embed_pool_create(&b, 4);
    TEST_ASSERT(p != NULL);

    for (int round = 0; round < 50; round++) {
        embed_job_t jobs[8] = {0};
        const char *t = "one two three four five six seven eight";
        for (int i = 0; i < 8; i++) jobs[i].text = t;
        embed_pool_run(p, jobs, 8);
        for (int i = 0; i < 8; i++) {
            TEST_ASSERT(jobs[i].rc  == 0);
            TEST_ASSERT(jobs[i].dim == TOY_DIM);
        }
    }
    embed_pool_destroy(p);
}

static void test_destroy_unblocks_idle_workers(void) {
    /* Pool created, never run — every worker is parked in cond_wait on
     * has_work. destroy() must broadcast shutdown and join cleanly. */
    crag_embed_backend_t b = make_backend(0);
    embed_pool_t *p = embed_pool_create(&b, 4);
    TEST_ASSERT(p != NULL);

    /* Give workers a moment to actually park in cond_wait — without
     * this, some may not yet have reached the wait and the broadcast
     * is a no-op for them (still correct, but doesn't exercise the
     * shutdown-wakes-waiter path we care about). */
    struct timespec ts = { 0, 10 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    embed_pool_destroy(p);
    /* Reaching here without hang = pass. */
    TEST_ASSERT(1);
}

static void test_destroy_after_batch_completion(void) {
    crag_embed_backend_t b = make_backend(0);
    embed_pool_t *p = embed_pool_create(&b, 8);
    TEST_ASSERT(p != NULL);

    embed_job_t jobs[32] = {0};
    for (int i = 0; i < 32; i++) jobs[i].text = "alpha bravo charlie delta";
    embed_pool_run(p, jobs, 32);

    embed_pool_destroy(p);
    TEST_ASSERT(1);
}

int main(void) {
    fprintf(stderr, "test_embed_pool:\n");
    TEST_RUN(test_single_worker_runs_jobs);
    TEST_RUN(test_eight_workers_concurrent_batch);
    TEST_RUN(test_many_sequential_batches_reuse_pool);
    TEST_RUN(test_destroy_unblocks_idle_workers);
    TEST_RUN(test_destroy_after_batch_completion);
    TEST_DONE();
}

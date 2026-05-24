#ifndef CRAG_EMBED_POOL_H
#define CRAG_EMBED_POOL_H

#include "crag.h"
#include "embed.h"

/* Per-chunk embed task. Worker fills vec/dim/rc; caller reads them.
   text is borrowed; neither side frees it. */
typedef struct {
    const char *text;
    float       vec[CRAG_EMBED_DIM_MAX];
    int         dim;
    int         rc;
} embed_job_t;

typedef struct embed_pool embed_pool_t;

/* Create a persistent worker pool of size `n` (clamped to [1, 32]) bound
   to `emb`. Returns NULL on failure. */
embed_pool_t *embed_pool_create(crag_embed_backend_t *emb, int n);

/* Submit jobs[0..n) and block until all are processed. Concurrency-safe
   only against itself: the caller must not call run() from multiple
   threads on the same pool. */
void embed_pool_run(embed_pool_t *p, embed_job_t *jobs, int n);

/* Signal shutdown, join all workers, destroy synchronization primitives,
   free the pool. Safe to call with NULL. */
void embed_pool_destroy(embed_pool_t *p);

#endif

/* Persistent embed worker pool.
 *
 * Extracted from index.c so it can be unit-tested in isolation under
 * TSan with a custom backend — driving cmd_index over a synthetic
 * corpus would also exercise nftw/store/chunk noise, making any race
 * findings harder to localize. The same shape that index.c used: one
 * pthread pool spun up once, drained per file via embed_pool_run(). */

#include <pthread.h>
#include <stdlib.h>
#include "embed_pool.h"

#define EMBED_POOL_WORKERS_MAX 32

struct embed_pool {
    pthread_t            *threads;
    int                   n_threads;
    crag_embed_backend_t *emb;

    embed_job_t          *jobs;     /* current batch, NULL when idle */
    int                   n_jobs;
    int                   next;     /* next job index to claim */
    int                   done;     /* count of completed jobs */
    int                   shutdown;

    pthread_mutex_t       lock;
    pthread_cond_t        has_work;
    pthread_cond_t        all_done;
};

static void *embed_worker(void *arg) {
    embed_pool_t *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        while (!p->shutdown && p->next >= p->n_jobs)
            pthread_cond_wait(&p->has_work, &p->lock);
        if (p->shutdown) { pthread_mutex_unlock(&p->lock); return NULL; }
        int j = p->next++;
        pthread_mutex_unlock(&p->lock);

        embed_job_t *job = &p->jobs[j];
        job->dim = 0;
        job->rc  = p->emb->embed_one(job->text, job->vec, &job->dim,
                                     p->emb->ctx);

        pthread_mutex_lock(&p->lock);
        p->done++;
        if (p->done >= p->n_jobs) pthread_cond_signal(&p->all_done);
        pthread_mutex_unlock(&p->lock);
    }
}

embed_pool_t *embed_pool_create(crag_embed_backend_t *emb, int n) {
    if (n < 1) n = 1;
    if (n > EMBED_POOL_WORKERS_MAX) n = EMBED_POOL_WORKERS_MAX;
    embed_pool_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->emb = emb;
    p->n_threads = n;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->has_work, NULL);
    pthread_cond_init(&p->all_done, NULL);
    p->threads = calloc((size_t)n, sizeof(pthread_t));
    if (!p->threads) { free(p); return NULL; }
    for (int i = 0; i < n; i++) {
        if (pthread_create(&p->threads[i], NULL, embed_worker, p) != 0) {
            /* abort any started threads, return NULL */
            pthread_mutex_lock(&p->lock);
            p->shutdown = 1;
            pthread_cond_broadcast(&p->has_work);
            pthread_mutex_unlock(&p->lock);
            for (int j = 0; j < i; j++) pthread_join(p->threads[j], NULL);
            free(p->threads); free(p);
            return NULL;
        }
    }
    return p;
}

void embed_pool_run(embed_pool_t *p, embed_job_t *jobs, int n) {
    pthread_mutex_lock(&p->lock);
    p->jobs   = jobs;
    p->n_jobs = n;
    p->next   = 0;
    p->done   = 0;
    pthread_cond_broadcast(&p->has_work);
    while (p->done < p->n_jobs)
        pthread_cond_wait(&p->all_done, &p->lock);
    p->jobs   = NULL;
    p->n_jobs = 0;
    pthread_mutex_unlock(&p->lock);
}

void embed_pool_destroy(embed_pool_t *p) {
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    p->shutdown = 1;
    pthread_cond_broadcast(&p->has_work);
    pthread_mutex_unlock(&p->lock);
    for (int i = 0; i < p->n_threads; i++) pthread_join(p->threads[i], NULL);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->has_work);
    pthread_cond_destroy(&p->all_done);
    free(p->threads);
    free(p);
}

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ftw.h>
#include <pthread.h>
#include "crag.h"
#include "chunk.h"
#include "embed.h"
#include "store.h"

#define MAX_CHUNKS_PER_FILE 512
#define BATCH_SZ            256
#define COMMIT_EVERY_FILES  20
#define WORKERS_MAX         32

/* Per-chunk embed task. Worker fills vec/dim/rc; writer reads them.
   text is borrowed from the chunker's output array — neither side frees it. */
typedef struct {
    const char *text;
    float       vec[CRAG_EMBED_DIM_MAX];
    int         dim;
    int         rc;
} embed_job_t;

/* Persistent worker pool: spun up once at index start, drained per file.
   embed_pool_run() is a barrier — main thread blocks until every job in
   the batch has been processed. That keeps per-file ordering trivial:
   workers race over the jobs[] slots, the writer reads them in order. */
typedef struct {
    pthread_t       *threads;
    int              n_threads;
    crag_embed_backend_t *emb;

    embed_job_t     *jobs;     /* current batch, NULL when idle */
    int              n_jobs;
    int              next;     /* next job index to claim */
    int              done;     /* count of completed jobs */
    int              shutdown;

    pthread_mutex_t  lock;
    pthread_cond_t   has_work;
    pthread_cond_t   all_done;
} embed_pool_t;

/* Basenames that are documentation in nearly every project but ship without
   an extension. Opt-in via --include-known-docs so the default behavior of
   --ext stays predictable. Match is case-insensitive (Linux distros are
   inconsistent: README vs Readme; ChangeLog vs CHANGELOG). */
static const char *KNOWN_DOC_BASENAMES[] = {
    "README", "FAQ", "INSTALL", "NEWS", "HACKING", "AUTHORS",
    "COPYING", "LICENSE", "NOTES", "THANKS", "TODO",
    "ChangeLog", "CHANGELOG", "HISTORY",
    NULL
};

/* walk state passed via file-scope struct (nftw provides no user-data pointer) */
static struct {
    char          *exts[32];  /* owned by parse_exts / free_exts */
    int            next;
    int            include_known_docs;  /* --include-known-docs */
    crag_embed_backend_t *emb;
    crag_store_t  *store;
    int            max_chars;
    int            total_chunks;
    int            total_errors;
    int            total_files;
    int            files_since_commit;
    /* drop-reason aggregates for the final summary */
    int            drops_context;
    int            drops_other;
    int            files_fully_dropped;
    int            files_partial;
    /* incremental reindex */
    long long      run_ts;          /* unix seconds captured at walk start */
    int            force;           /* --force: re-embed even if unchanged */
    int            files_skipped;   /* matched (path,mtime,size) in store */
    int            files_indexed;   /* re-embedded (new or stale) */
    /* parallel embed pool — NULL when --workers <= 1 or backend is batched */
    embed_pool_t  *pool;
} g_walk;

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

static embed_pool_t *embed_pool_create(crag_embed_backend_t *emb, int n) {
    if (n < 1) n = 1;
    if (n > WORKERS_MAX) n = WORKERS_MAX;
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

/* Submit jobs[0..n) and block until all are processed. */
static void embed_pool_run(embed_pool_t *p, embed_job_t *jobs, int n) {
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

static void embed_pool_destroy(embed_pool_t *p) {
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

static void free_exts(void) {
    for (int i = 0; i < g_walk.next; i++) { free(g_walk.exts[i]); g_walk.exts[i] = NULL; }
    g_walk.next = 0;
}

static int has_ext(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (dot) {
        for (int i = 0; i < g_walk.next; i++) {
            if (strcasecmp(dot + 1, g_walk.exts[i]) == 0) return 1;
        }
    }
    if (g_walk.include_known_docs && !dot) {
        for (const char **n = KNOWN_DOC_BASENAMES; *n; n++) {
            if (strcasecmp(base, *n) == 0) return 1;
        }
    }
    return 0;
}

/* Embed one chunk's text and insert it into the store. If the embedder
   rejects the input as too long for its context window (CRAG_ECONTEXT),
   split the text roughly in half at a word boundary and recurse on each
   half. This fixes the char-vs-token mismatch: the chunker limits by
   character count, but real embedders limit by token count, and for
   symbol-dense text (URLs, markdown tables, code) 2048 chars can tokenize
   past small windows like mxbai-embed-large's 512. */
#define EMBED_SPLIT_MAX_DEPTH 3
#define EMBED_SPLIT_MIN_CHARS 200

/* Returns 1 if at least one (sub-)chunk landed in the store; 0 if the
   original chunk was fully dropped (all split attempts failed too). */
static int embed_and_insert(const char *source, const char *text, int depth) {
    crag_chunk_t chunk = {0};
    chunk.source = (char *)source;
    chunk.text   = (char *)text;

    int rc = g_walk.emb->embed_one(text, chunk.vec, &chunk.dim,
                                   g_walk.emb->ctx);
    if (rc == 0) {
        if (store_insert(g_walk.store, &chunk) == 0) {
            g_walk.total_chunks++;
            return 1;
        }
        g_walk.total_errors++;
        g_walk.drops_other++;
        return 0;
    }

    if (rc == CRAG_ECONTEXT && depth < EMBED_SPLIT_MAX_DEPTH) {
        size_t len = strlen(text);
        if (len >= EMBED_SPLIT_MIN_CHARS) {
            size_t half = len / 2;
            /* nudge the split to the next newline or space so we don't
               cut mid-word; cap the scan so we don't walk far */
            const char *split = text + half;
            for (const char *p = text + half; *p && p < text + half + 64; p++) {
                if (*p == '\n' || *p == ' ') { split = p + 1; break; }
            }
            size_t left_len = (size_t)(split - text);
            char *left  = strndup(text, left_len);
            char *right = strdup(split);
            if (left && right) {
                int l = embed_and_insert(source, left,  depth + 1);
                int r = embed_and_insert(source, right, depth + 1);
                free(left); free(right);
                return l || r;
            }
            free(left); free(right);
        }
    }

    /* Unrecoverable leaf drop — classify the reason for the summary. */
    if (rc == CRAG_ECONTEXT) {
        g_walk.drops_context++;
        fprintf(stderr, "index: chunk still too long for embedder (depth %d) in '%s' — dropping\n",
                depth, source);
    } else {
        g_walk.drops_other++;
        fprintf(stderr, "index: embed failed (rc=%d) for chunk in '%s'\n", rc, source);
    }
    g_walk.total_errors++;
    return 0;
}

/* Pool-driven embed for a batch, with CRAG_ECONTEXT splits funneled back
   through the pool round-by-round. Each round runs all currently-pending
   tasks in parallel; chunks that succeed get inserted; chunks that hit
   CRAG_ECONTEXT (and still have headroom under EMBED_SPLIT_MAX_DEPTH) get
   split into halves and queued for the next round. Loop terminates when
   the queue drains.

   Trade-off vs the sequential embed_and_insert(): per-chunk insertion
   order changes (split halves land later than untouched chunks). The
   store doesn't depend on insert order for retrieval, and the win is
   that the recursive splits — which were the dominant cost on
   token-dense files — also benefit from worker parallelism. */
typedef struct { char *text; int depth; int needs_free; } chunk_task_t;

static void embed_chunks_via_pool(const char *fpath,
                                   const char **texts, int n,
                                   int *file_dropped) {
    chunk_task_t *cur = calloc((size_t)n, sizeof *cur);
    if (!cur) {
        for (int i = 0; i < n; i++) { (*file_dropped)++; g_walk.total_errors++; g_walk.drops_other++; }
        return;
    }
    for (int i = 0; i < n; i++) cur[i].text = (char *)texts[i];
    int ncur = n;

    while (ncur > 0) {
        embed_job_t *jobs = calloc((size_t)ncur, sizeof *jobs);
        if (!jobs) {
            for (int i = 0; i < ncur; i++) {
                if (cur[i].needs_free) free(cur[i].text);
                (*file_dropped)++; g_walk.total_errors++; g_walk.drops_other++;
            }
            free(cur);
            return;
        }
        for (int i = 0; i < ncur; i++) jobs[i].text = cur[i].text;
        embed_pool_run(g_walk.pool, jobs, ncur);

        /* Pre-count next round so we can size the array exactly. */
        int nnext = 0;
        for (int i = 0; i < ncur; i++) {
            if (jobs[i].rc == CRAG_ECONTEXT &&
                cur[i].depth < EMBED_SPLIT_MAX_DEPTH &&
                strlen(cur[i].text) >= EMBED_SPLIT_MIN_CHARS)
                nnext += 2;
        }
        chunk_task_t *next = nnext ? calloc((size_t)nnext, sizeof *next) : NULL;
        int nidx = 0;

        for (int i = 0; i < ncur; i++) {
            if (jobs[i].rc == 0) {
                crag_chunk_t chunk = {0};
                chunk.source = (char *)fpath;
                chunk.text   = cur[i].text;
                chunk.dim    = jobs[i].dim;
                memcpy(chunk.vec, jobs[i].vec,
                       (size_t)jobs[i].dim * sizeof(float));
                if (store_insert(g_walk.store, &chunk) == 0) {
                    g_walk.total_chunks++;
                } else {
                    g_walk.total_errors++; g_walk.drops_other++; (*file_dropped)++;
                }
            } else if (jobs[i].rc == CRAG_ECONTEXT &&
                       cur[i].depth < EMBED_SPLIT_MAX_DEPTH &&
                       strlen(cur[i].text) >= EMBED_SPLIT_MIN_CHARS) {
                /* split at next word boundary past the midpoint, mirroring
                   the sequential embed_and_insert() splitter */
                size_t len = strlen(cur[i].text);
                size_t half = len / 2;
                const char *split = cur[i].text + half;
                for (const char *p = cur[i].text + half;
                     *p && p < cur[i].text + half + 64; p++) {
                    if (*p == '\n' || *p == ' ') { split = p + 1; break; }
                }
                size_t left_len = (size_t)(split - cur[i].text);
                char *left  = strndup(cur[i].text, left_len);
                char *right = strdup(split);
                if (left && right) {
                    next[nidx].text = left;
                    next[nidx].depth = cur[i].depth + 1;
                    next[nidx].needs_free = 1;
                    nidx++;
                    next[nidx].text = right;
                    next[nidx].depth = cur[i].depth + 1;
                    next[nidx].needs_free = 1;
                    nidx++;
                } else {
                    free(left); free(right);
                    g_walk.drops_other++; g_walk.total_errors++; (*file_dropped)++;
                }
            } else {
                if (jobs[i].rc == CRAG_ECONTEXT) {
                    g_walk.drops_context++;
                    fprintf(stderr,
                            "index: chunk still too long for embedder (depth %d) in '%s' — dropping\n",
                            cur[i].depth, fpath);
                } else {
                    g_walk.drops_other++;
                    fprintf(stderr, "index: embed failed (rc=%d) for chunk in '%s'\n",
                            jobs[i].rc, fpath);
                }
                g_walk.total_errors++;
                (*file_dropped)++;
            }
        }

        free(jobs);
        for (int i = 0; i < ncur; i++) if (cur[i].needs_free) free(cur[i].text);
        free(cur);
        cur = next;
        ncur = nidx;
    }
}

static int walk_cb(const char *fpath, const struct stat *sb,
                   int typeflag, struct FTW *ftwbuf) {
    (void)ftwbuf;
    if (typeflag != FTW_F || !has_ext(fpath)) return 0;

    long long mtime = (long long)sb->st_mtime;
    long long size  = (long long)sb->st_size;
    g_walk.total_files++;

    /* Incremental skip: if (path,mtime,size) matches a prior indexed file,
       just mark it seen-this-run and move on. --force bypasses the check. */
    if (!g_walk.force) {
        int cur = store_file_is_current(g_walk.store, fpath, mtime, size);
        if (cur == 1) {
            store_file_touch(g_walk.store, fpath, g_walk.run_ts);
            g_walk.files_skipped++;
            goto after_file;
        }
    }

    /* New file, changed file, or --force: delete any prior rows for this
       path so we don't accumulate duplicates across runs. */
    store_delete_chunks_for(g_walk.store, fpath);

    char *texts[MAX_CHUNKS_PER_FILE];
    int n = chunk_file(fpath, g_walk.max_chars, texts, MAX_CHUNKS_PER_FILE);
    if (n < 0) {
        fprintf(stderr, "index: failed to chunk '%s'\n", fpath);
        return 0;
    }

    int file_dropped = 0;

    /* embed in batches */
    for (int start = 0; start < n; ) {
        int batch = n - start;
        if (batch > BATCH_SZ) batch = BATCH_SZ;
        const char **batch_texts = (const char **)texts + start;

        if (g_walk.emb->embed_many) {
            float *vecs = calloc((size_t)batch * CRAG_EMBED_DIM_MAX, sizeof(float));
            int   *dims = calloc((size_t)batch, sizeof(int));
            if (!vecs || !dims) {
                free(vecs); free(dims);
                for (int i = 0; i < n; i++) free(texts[i]);
                return 0;
            }
            if (g_walk.emb->embed_many(batch_texts, batch, vecs, dims, g_walk.emb->ctx) == 0) {
                for (int i = 0; i < batch; i++) {
                    if (dims[i] <= 0) {
                        g_walk.total_errors++; g_walk.drops_other++; file_dropped++;
                        continue;
                    }
                    crag_chunk_t chunk = {0};
                    chunk.source = (char *)fpath;
                    chunk.text   = texts[start + i];
                    chunk.dim    = dims[i];
                    memcpy(chunk.vec, vecs + (size_t)i * CRAG_EMBED_DIM_MAX,
                           (size_t)dims[i] * sizeof(float));
                    if (store_insert(g_walk.store, &chunk) == 0) {
                        g_walk.total_chunks++;
                    } else {
                        g_walk.total_errors++; g_walk.drops_other++; file_dropped++;
                    }
                }
            } else {
                fprintf(stderr, "index: batch embed failed for '%s'\n", fpath);
                g_walk.total_errors += batch;
                g_walk.drops_other  += batch;
                file_dropped        += batch;
            }
            free(vecs); free(dims);
        } else if (g_walk.pool) {
            embed_chunks_via_pool(fpath, batch_texts, batch, &file_dropped);
        } else {
            /* fallback: one by one, with recursive split on CRAG_ECONTEXT */
            for (int i = 0; i < batch; i++) {
                if (embed_and_insert(fpath, batch_texts[i], 0) == 0)
                    file_dropped++;
            }
        }
        start += batch;
    }

    for (int i = 0; i < n; i++) free(texts[i]);

    if (n > 0 && file_dropped > 0) {
        if (file_dropped >= n) {
            fprintf(stderr, "partial: ALL %d chunks of '%s' dropped\n", n, fpath);
            g_walk.files_fully_dropped++;
        } else {
            fprintf(stderr, "partial: %d/%d chunks of '%s' dropped\n",
                    file_dropped, n, fpath);
            g_walk.files_partial++;
        }
    }

    /* Only record the file as indexed if at least one chunk landed. Files
       whose every chunk was dropped are intentionally NOT upserted — that
       way the next run (possibly with a fixed embedder) will retry them
       instead of silently skipping. */
    if (n > 0 && file_dropped < n) {
        store_file_upsert(g_walk.store, fpath, mtime, size, g_walk.run_ts);
        g_walk.files_indexed++;
    }

after_file:
    /* Bound the blast radius of an interrupt: commit every N files and open
       a fresh transaction. Without this, a single final COMMIT wrapping the
       whole nftw() walk means a killed/crashed process loses all work. */
    if (++g_walk.files_since_commit >= COMMIT_EVERY_FILES) {
        if (store_commit(g_walk.store) == 0 && store_begin(g_walk.store) == 0)
            g_walk.files_since_commit = 0;
    }
    return 0;
}

static void parse_exts(const char *extstr) {
    char *dup = strdup(extstr);
    if (!dup) return;
    g_walk.next = 0;
    char *tok = strtok(dup, ",");
    while (tok && g_walk.next < 32) {
        char *s = strdup(tok);
        if (!s) break;
        g_walk.exts[g_walk.next++] = s;
        tok = strtok(NULL, ",");
    }
    free(dup);
}

int cmd_index(int argc, char **argv) {
    const char *dir      = NULL;
    const char *backend  = getenv("CRAG_EMBED_BACKEND") ? getenv("CRAG_EMBED_BACKEND") : "local";
    const char *db_path  = NULL;
    const char *url      = NULL;
    const char *extstr   = "txt,md";
    int max_chars = CHUNK_MAX_CHARS_DEFAULT;
    int force = 0;
    int workers = 1;
    int include_known_docs = 0;

    for (int i = 0; i < argc; i++) {
        if      (strcmp(argv[i], "--backend")   == 0 && i+1 < argc) backend   = argv[++i];
        else if (strcmp(argv[i], "--db")        == 0 && i+1 < argc) db_path   = argv[++i];
        else if ((strcmp(argv[i], "-o")         == 0) && i+1 < argc) db_path  = argv[++i];
        else if (strcmp(argv[i], "--url")       == 0 && i+1 < argc) url       = argv[++i];
        else if (strcmp(argv[i], "--ext")       == 0 && i+1 < argc) extstr    = argv[++i];
        else if (strcmp(argv[i], "--max-bytes") == 0 && i+1 < argc) max_chars = atoi(argv[++i]);
        else if (strcmp(argv[i], "--workers")   == 0 && i+1 < argc) workers   = atoi(argv[++i]);
        else if (strcmp(argv[i], "--force")     == 0) force = 1;
        else if (strcmp(argv[i], "--include-known-docs") == 0) include_known_docs = 1;
        else if (argv[i][0] != '-') dir = argv[i];
    }
    if (workers < 1) workers = 1;
    if (workers > WORKERS_MAX) workers = WORKERS_MAX;

    if (!dir) { fprintf(stderr, "index: missing <dir>\n"); return 2; }
    if (!db_path) db_path = getenv("CRAG_DB");
    if (!db_path) db_path = "./crag.db";

    parse_exts(extstr);

    crag_embed_backend_t *emb = embed_backend_create(backend, url, NULL);
    if (!emb) return 1;

    crag_store_t store;
    if (store_open(db_path, &store) != 0) { embed_backend_free(emb); return 3; }

    g_walk.emb                 = emb;
    g_walk.store               = &store;
    g_walk.max_chars           = max_chars;
    g_walk.total_chunks        = 0;
    g_walk.total_errors        = 0;
    g_walk.total_files         = 0;
    g_walk.files_since_commit  = 0;
    g_walk.drops_context       = 0;
    g_walk.drops_other         = 0;
    g_walk.files_fully_dropped = 0;
    g_walk.files_partial       = 0;
    g_walk.run_ts              = (long long)time(NULL);
    g_walk.force               = force;
    g_walk.include_known_docs  = include_known_docs;
    g_walk.files_skipped       = 0;
    g_walk.files_indexed       = 0;
    g_walk.pool                = NULL;

    /* Spin up the pool only when it would actually help: when the backend
       has no native batch path. embed_many already pipelines at the HTTP
       layer (openai), and the local stub returns instantly. Workers > 1
       with a batched backend would just contend on the HTTP server. */
    if (workers > 1 && emb->embed_many == NULL) {
        g_walk.pool = embed_pool_create(emb, workers);
        if (!g_walk.pool)
            fprintf(stderr, "index: failed to start worker pool, falling back to sequential\n");
    }

    store_begin(&store);
    nftw(dir, walk_cb, 64, FTW_PHYS);

    /* Sweep files that were in the store but weren't seen on disk this run.
       Their `indexed_at` is still from a prior run, so it's < g_walk.run_ts. */
    int files_removed = 0, chunks_removed = 0;
    store_sweep_missing(&store, g_walk.run_ts, &files_removed, &chunks_removed);

    store_commit(&store);

    embed_pool_destroy(g_walk.pool);
    g_walk.pool = NULL;

    fprintf(stderr, "\n=== index summary ===\n");
    fprintf(stderr, "  files scanned:       %d\n", g_walk.total_files);
    fprintf(stderr, "  files unchanged:     %d\n", g_walk.files_skipped);
    fprintf(stderr, "  files indexed:       %d%s\n", g_walk.files_indexed,
            force ? " (--force)" : "");
    if (workers > 1)
        fprintf(stderr, "  workers:             %d%s\n", workers,
                emb->embed_many ? " (ignored: backend is batched)" : "");
    fprintf(stderr, "  files removed:       %d\n", files_removed);
    fprintf(stderr, "  chunks stored:       %d\n", g_walk.total_chunks);
    fprintf(stderr, "  chunks removed:      %d\n", chunks_removed);
    fprintf(stderr, "  chunks dropped:      %d  (context=%d, other=%d)\n",
            g_walk.total_errors, g_walk.drops_context, g_walk.drops_other);
    fprintf(stderr, "  files partial:       %d\n", g_walk.files_partial);
    fprintf(stderr, "  files fully dropped: %d\n", g_walk.files_fully_dropped);
    fprintf(stderr, "  db:                  %s\n", db_path);

    store_close(&store);
    embed_backend_free(emb);
    free_exts();

    /* Exit codes: 4 = nothing indexed but errors; 5 = partial corpus hole
       (at least one file lost all its chunks). CI/automation can catch
       silent data loss by checking for non-zero. */
    if (g_walk.total_chunks == 0 && g_walk.total_errors > 0) return 4;
    if (g_walk.files_fully_dropped > 0) return 5;
    return 0;
}

#ifndef STORE_H
#define STORE_H

#include <sqlite3.h>
#include "crag.h"

typedef struct { sqlite3 *db; } crag_store_t;

typedef struct {
    int chunks;
    int files;
    int dim;
} crag_store_stats_t;

int  store_open(const char *path, crag_store_t *s);
int  store_insert(crag_store_t *s, const crag_chunk_t *chunk);
int  store_begin(crag_store_t *s);
int  store_commit(crag_store_t *s);
/* Returns number of results (<=k). results[] and scores[] must be pre-allocated to k. */
int  store_search(crag_store_t *s, const float *qvec, int dim, int k,
                  crag_chunk_t *results, float *scores);
/* BM25 full-text search over chunks.text via FTS5. `qtext` is the raw user
   query; it's sanitized internally (non-token chars stripped, tokens quoted
   and ORed). Scores returned are BM25 values (higher = better match; note
   SQLite reports BM25 as negative internally, we negate for convenience). */
int  store_search_bm25(crag_store_t *s, const char *qtext, int k,
                       crag_chunk_t *results, float *scores);
/* Hybrid search: fuses cosine and BM25 via Reciprocal Rank Fusion. Scores
   are fusion scores (sum of 1/(RRF_K + rank_i) across retrievers). Each
   underlying retriever pulls max(k*4, 20) candidates before fusion. */
int  store_search_hybrid(crag_store_t *s,
                         const float *qvec, int dim,
                         const char *qtext, int k,
                         crag_chunk_t *results, float *scores);
int  store_stats(crag_store_t *s, crag_store_stats_t *out);
void store_close(crag_store_t *s);

/* Returns a malloc'd copy of chunk `id`'s text with each FTS-matched token
   wrapped in `open`/`close`. Returns NULL if `qtext` has no usable tokens,
   the chunk doesn't match, or on error — caller falls back to raw text. */
char *store_highlight_for_id(crag_store_t *s, const char *qtext, int id,
                             const char *open, const char *close);

/* --- incremental reindex helpers ------------------------------------------ */
/* Returns 1 if the `files` row for `path` matches (mtime,size), 0 if it
   differs or no row exists, <0 on error. */
int store_file_is_current(crag_store_t *s, const char *path,
                          long long mtime, long long size);
/* Record that we successfully indexed `path` at (mtime,size) during the run
   identified by `run_ts`. Inserts or replaces the `files` row. */
int store_file_upsert(crag_store_t *s, const char *path,
                      long long mtime, long long size, long long run_ts);
/* Mark a file as "seen this run" without touching its chunks or (mtime,size).
   Used when a file is unchanged on disk and we skip re-embedding. */
int store_file_touch(crag_store_t *s, const char *path, long long run_ts);
/* Remove every chunk row with source = path. Does not touch the `files` row. */
int store_delete_chunks_for(crag_store_t *s, const char *path);
/* Delete every `files` row whose indexed_at < run_ts plus the chunks of the
   files removed. Out params count what was removed (either may be NULL). */
int store_sweep_missing(crag_store_t *s, long long run_ts,
                        int *files_removed, int *chunks_removed);

#endif

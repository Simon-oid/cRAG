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

/* Marks a crag_score_t field that could not be computed (e.g. cosine when no
   query vector was supplied). Sits outside cosine's [-1,1] range so callers
   can detect it. */
#define CRAG_SCORE_NA (-2.0f)

/* Per-result score breakdown for hybrid search. The fusion score ranks
   results but is NOT a relevance magnitude — its range is structural
   (~0.033 = ranked #1 by both retrievers, ~0.016 = #1 by only one), so it
   cannot be thresholded. `cosine` and `bm25` are the raw per-retriever
   scores; callers should threshold on `cosine` (a calibrated 0–1 relevance)
   to tell a real match from the one-retriever floor. */
typedef struct {
    float fusion;   /* RRF score: Σ 1/(RRF_K + rank_i). Used for ranking.   */
    float cosine;   /* cosine similarity in [-1,1], or CRAG_SCORE_NA.        */
    float bm25;     /* negated BM25 (higher=better); 0 if no lexical hit.    */
} crag_score_t;

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
/* Hybrid search: fuses cosine and BM25 via Reciprocal Rank Fusion and ranks
   results by the fusion score. Per result it fills a crag_score_t carrying
   the fusion score plus the raw cosine and BM25 scores — see crag_score_t for
   why the raw scores matter. `cosine` is populated for every returned result,
   including ones found only via BM25 (scored from the stored vector). Each
   underlying retriever pulls max(k*4, 20) candidates before fusion. results[]
   and scores[] must be pre-allocated to k. */
int  store_search_hybrid(crag_store_t *s,
                         const float *qvec, int dim,
                         const char *qtext, int k,
                         crag_chunk_t *results, crag_score_t *scores);
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

/* crag_lib.h — cRAG as a library: a stable C API over the index.
 *
 * The `crag` program embeds, chunks and generates; this API is the part
 * underneath that is worth linking from other code (and from Python, see
 * python/): an on-disk index of text chunks with their vectors, searched
 * by exact cosine, BM25 (SQLite FTS5) or both fused with Reciprocal Rank
 * Fusion. Bring your own vectors: the library never calls an embedder.
 *
 * Ownership rules (the same for every function):
 *   - pointers passed IN are borrowed for the duration of the call only;
 *     the library copies what it keeps (text, source, vector);
 *   - results handed OUT belong to the caller and are released with the
 *     matching free function (crag_hits_free, crag_db_close);
 *   - return values are 0 (CRAG_OK) or a negative crag_err_t; out
 *     parameters are untouched on failure unless stated otherwise.
 *
 * Threading: one crag_db_t must be used by one thread at a time (SQLite is
 * built single-threaded). Separate handles may be used from separate
 * threads.
 */
#ifndef CRAG_LIB_H
#define CRAG_LIB_H

#include "crag.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Version of this API (independent of the crag program's version). */
#define CRAG_LIB_VERSION "1.0.0"

typedef struct crag_db crag_db_t;

typedef enum {
    CRAG_MODE_COSINE = 0,   /* exact scan; needs a query vector            */
    CRAG_MODE_BM25   = 1,   /* FTS5 BM25; needs query text, no vector      */
    CRAG_MODE_HYBRID = 2,   /* RRF over both; needs vector and text        */
} crag_mode_t;

typedef struct {
    long long id;       /* chunk id in the index                            */
    char     *source;   /* owned; the source label given to crag_db_add     */
    char     *text;     /* owned; the chunk text                            */
    float     score;    /* the ranking score of the mode: cosine for COSINE,
                         * BM25 (higher = better) for BM25, the RRF fusion
                         * score for HYBRID. RRF scores rank but are not a
                         * relevance magnitude: threshold on `cosine`.       */
    float     cosine;   /* cosine similarity, or -2 when not computed        */
    float     bm25;     /* BM25 score (higher = better), 0 if no lexical hit */
} crag_hit_t;

/* Open (creating if needed) the index at `path`; ":memory:" gives an
 * in-memory index. On CRAG_OK *out is a handle for crag_db_close. */
int  crag_db_open(const char *path, crag_db_t **out);
void crag_db_close(crag_db_t *db);

/* Add one chunk. `vec` (borrowed) has `dim` floats, 1..CRAG_EMBED_DIM_MAX;
 * every chunk of an index shares one dim (CRAG_EDIM_MISMATCH otherwise).
 * Wrap many adds in crag_db_begin / crag_db_commit for speed. */
int crag_db_add(crag_db_t *db, const char *source, const char *text,
                const float *vec, int dim);
int crag_db_begin(crag_db_t *db);
int crag_db_commit(crag_db_t *db);

/* Top-`k` search. `qvec` / `qdim` are needed for COSINE and HYBRID,
 * `qtext` for BM25 and HYBRID (pass NULL where unused). On CRAG_OK
 * *out_hits holds *out_n (<= k) hits, best first, to be released with
 * crag_hits_free; with no match *out_n is 0 and *out_hits NULL. */
int crag_db_search(crag_db_t *db, crag_mode_t mode,
                   const float *qvec, int qdim, const char *qtext, int k,
                   crag_hit_t **out_hits, int *out_n);
void crag_hits_free(crag_hit_t *hits, int n);

/* Chunk count, distinct sources and vector dim (0 for an empty index). */
int crag_db_stats(crag_db_t *db, int *chunks, int *sources, int *dim);

#ifdef __cplusplus
}
#endif

#endif

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
int  store_stats(crag_store_t *s, crag_store_stats_t *out);
void store_close(crag_store_t *s);

#endif

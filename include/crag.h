#ifndef CRAG_H
#define CRAG_H

#define CRAG_VERSION "0.0.1"

#define CRAG_EMBED_DIM_MAX 4096

typedef struct {
    int    id;
    char  *source;
    char  *text;
    float  vec[CRAG_EMBED_DIM_MAX];
    int    dim;
} crag_chunk_t;

#endif

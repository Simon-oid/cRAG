#ifndef CRAG_H
#define CRAG_H

#define CRAG_VERSION "0.2.0"

#define CRAG_EMBED_DIM_MAX 4096

typedef struct {
    int    id;
    char  *source;
    char  *text;
    float  vec[CRAG_EMBED_DIM_MAX];
    int    dim;
} crag_chunk_t;

typedef enum {
    CRAG_OK            =  0,
    CRAG_EINVAL        = -1,
    CRAG_ENOMEM        = -2,
    CRAG_EIO           = -3,
    CRAG_EPARSE        = -4,
    CRAG_EHTTP         = -5,
    CRAG_EDIM_MISMATCH = -6,
    CRAG_EDB           = -7,
    CRAG_ECONTEXT      = -8,  /* input exceeds embedder's context window */
} crag_err_t;

const char *crag_errstr(int err);

#endif

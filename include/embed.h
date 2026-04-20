#ifndef EMBED_H
#define EMBED_H

#include "crag.h"

typedef struct crag_embed_backend crag_embed_backend_t;

struct crag_embed_backend {
    const char *name;
    int (*embed_one)(const char *text, float *vec, int *dim, void *ctx);
    void       *ctx;
};

/* name: "openai" or "local"
   url: override endpoint (NULL = default)
   api_key: NULL = read from env */
crag_embed_backend_t *embed_backend_create(const char *name, const char *url,
                                           const char *api_key);
void embed_backend_free(crag_embed_backend_t *b);

#endif

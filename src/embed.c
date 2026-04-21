#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embed.h"

/* forward declarations from embed_api.c / embed_local.c / embed_ollama.c */
crag_embed_backend_t *embed_oai_create(const char *url, const char *api_key);
crag_embed_backend_t *embed_local_create(const char *url);
crag_embed_backend_t *embed_ollama_create(const char *url, const char *model);

crag_embed_backend_t *embed_backend_create(const char *name, const char *url,
                                           const char *api_key) {
    if (!name || strcmp(name, "openai") == 0)
        return embed_oai_create(url, api_key);
    if (strcmp(name, "local") == 0)
        return embed_local_create(url);
    if (strcmp(name, "ollama") == 0)
        return embed_ollama_create(url, NULL);
    fprintf(stderr, "embed: unknown backend '%s'\n", name);
    return NULL;
}

void embed_backend_free(crag_embed_backend_t *b) {
    if (!b) return;
    free(b->ctx);
    free(b);
}

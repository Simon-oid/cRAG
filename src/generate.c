#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "generate.h"

/* forward declarations from gen_anthropic.c / gen_local.c */
crag_generator_t *gen_anthropic_create(const crag_gen_cfg_t *cfg);
crag_generator_t *gen_openai_create(const crag_gen_cfg_t *cfg);
crag_generator_t *gen_local_create(const crag_gen_cfg_t *cfg);

crag_generator_t *generator_create(const crag_gen_cfg_t *cfg) {
    if (!cfg || !cfg->backend) return NULL;
    if (strcmp(cfg->backend, "anthropic") == 0) return gen_anthropic_create(cfg);
    if (strcmp(cfg->backend, "openai")    == 0) return gen_openai_create(cfg);
    if (strcmp(cfg->backend, "local")     == 0) return gen_local_create(cfg);
    fprintf(stderr, "generate: unknown backend '%s'\n", cfg->backend);
    return NULL;
}

crag_generator_t *generator_from_env(void) {
    crag_gen_cfg_t cfg = {0};
    cfg.backend    = getenv("CRAG_GEN_BACKEND");
    if (!cfg.backend) cfg.backend = "local";
    cfg.url        = getenv("CRAG_GEN_URL");
    cfg.model      = getenv("CRAG_GEN_MODEL");
    cfg.api_key    = getenv("CRAG_GEN_API_KEY");
    const char *mt = getenv("CRAG_GEN_MAX_TOKENS");
    cfg.max_tokens = mt ? atoi(mt) : 1024;
    return generator_create(&cfg);
}

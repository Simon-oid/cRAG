#ifndef GENERATE_H
#define GENERATE_H

typedef struct crag_generator crag_generator_t;

typedef struct {
    const char *backend;    /* "anthropic" | "openai" | "ollama" | "local" */
    const char *url;
    const char *model;
    const char *api_key;
    int         max_tokens;
} crag_gen_cfg_t;

struct crag_generator {
    char *(*run)(crag_generator_t *g, const char *system_prompt, const char *user_prompt);
    void  (*cleanup)(crag_generator_t *g);
};

/* Run generation. Returns malloc'd answer text (caller frees), or NULL on error. */
static inline char *generator_run(crag_generator_t *g,
                                   const char *sys, const char *user) {
    return g->run(g, sys, user);
}

static inline void generator_free(crag_generator_t *g) {
    if (!g) return;
    g->cleanup(g);
}

/* Factory: create from cfg or env vars (CRAG_GEN_BACKEND, URL, MODEL, API_KEY, MAX_TOKENS). */
crag_generator_t *generator_create(const crag_gen_cfg_t *cfg);
crag_generator_t *generator_from_env(void);

#endif

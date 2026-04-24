#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "generate.h"
#include "http.h"
#include "util.h"

#define DEFAULT_URL   "http://localhost:11434/api/generate"
#define DEFAULT_MODEL "llama3.2:3b"

typedef struct {
    crag_generator_t base;   /* must be first */
    char url[512];
    char model[128];
    int  max_tokens;
} ollama_gen_t;

static char *ollama_run(crag_generator_t *g_,
                         const char *system_prompt,
                         const char *user_prompt) {
    ollama_gen_t *g = (ollama_gen_t *)g_;

    char *esys  = json_escape_dup(system_prompt ? system_prompt : "");
    char *euser = json_escape_dup(user_prompt   ? user_prompt   : "");
    if (!esys || !euser) { free(esys); free(euser); return NULL; }

    size_t body_sz = strlen(esys) + strlen(euser) + strlen(g->model) + 256;
    char *body = malloc(body_sz);
    if (!body) { free(esys); free(euser); return NULL; }

    snprintf(body, body_sz,
        "{\"model\":\"%s\",\"stream\":false,"
        "\"system\":\"%s\",\"prompt\":\"%s\","
        "\"options\":{\"num_predict\":%d}}",
        g->model, esys, euser, g->max_tokens);
    free(esys); free(euser);

    http_buf_t buf = {0};
    int status = http_post_json(g->url, NULL, body, &buf);
    free(body);

    if (status != 200) {
        fprintf(stderr, "gen_ollama: HTTP %d: %s\n", status, buf.data ? buf.data : "");
        free(buf.data);
        return NULL;
    }

    char *tp = strstr(buf.data, "\"response\":");
    if (!tp) { fprintf(stderr, "gen_ollama: no response field in output\n"); free(buf.data); return NULL; }
    tp += 11;
    while (*tp == ' ') tp++;
    if (*tp != '"') { free(buf.data); return NULL; }
    tp++;

    size_t out_sz = strlen(tp) + 1;
    char *out = malloc(out_sz);
    if (!out) { free(buf.data); return NULL; }
    size_t wi = 0;
    while (*tp && *tp != '"') {
        if (*tp == '\\' && *(tp+1)) {
            tp++;
            switch (*tp) {
                case 'n': out[wi++] = '\n'; break;
                case 't': out[wi++] = '\t'; break;
                case 'r': break;
                default:  out[wi++] = *tp;  break;
            }
        } else {
            out[wi++] = *tp;
        }
        tp++;
    }
    out[wi] = '\0';
    free(buf.data);
    return out;
}

static void ollama_cleanup(crag_generator_t *g) { free(g); }

crag_generator_t *gen_ollama_create(const crag_gen_cfg_t *cfg) {
    ollama_gen_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;

    const char *u = cfg->url   ? cfg->url   : getenv("CRAG_GEN_URL");
    const char *m = cfg->model ? cfg->model : getenv("CRAG_GEN_MODEL");

    strncpy(g->url,   u ? u : DEFAULT_URL,   sizeof g->url   - 1);
    strncpy(g->model, m ? m : DEFAULT_MODEL, sizeof g->model - 1);
    g->url[sizeof g->url - 1]     = '\0';
    g->model[sizeof g->model - 1] = '\0';
    g->max_tokens = cfg->max_tokens > 0 ? cfg->max_tokens : 1024;

    g->base.run     = ollama_run;
    g->base.cleanup = ollama_cleanup;
    return (crag_generator_t *)g;
}

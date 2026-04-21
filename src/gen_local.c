#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "generate.h"
#include "http.h"
#include "util.h"

#define DEFAULT_URL "http://localhost:8080/completion"

typedef struct {
    crag_generator_t base;   /* must be first */
    char url[512];
    int  max_tokens;
} local_gen_t;

static char *local_run(crag_generator_t *g_,
                        const char *system_prompt,
                        const char *user_prompt) {
    local_gen_t *g = (local_gen_t *)g_;

    /* combine system + user into a single prompt string */
    const char *sys  = system_prompt ? system_prompt : "";
    const char *user = user_prompt   ? user_prompt   : "";

    char *esys  = json_escape_dup(sys);
    char *euser = json_escape_dup(user);
    if (!esys || !euser) { free(esys); free(euser); return NULL; }

    size_t body_sz = strlen(esys) + strlen(euser) + 64;
    char *body = malloc(body_sz);
    if (!body) { free(esys); free(euser); return NULL; }
    snprintf(body, body_sz,
             "{\"prompt\":\"%s\\n\\n%s\",\"n_predict\":%d}",
             esys, euser, g->max_tokens);
    free(esys); free(euser);

    http_buf_t buf = {0};
    int status = http_post_json(g->url, NULL, body, &buf);
    free(body);

    if (status != 200) {
        fprintf(stderr, "gen_local: HTTP %d: %s\n", status, buf.data ? buf.data : "");
        free(buf.data);
        return NULL;
    }

    char *tp = strstr(buf.data, "\"content\":");
    if (!tp) { fprintf(stderr, "gen_local: no content in response\n"); free(buf.data); return NULL; }
    tp += 10;
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

static void local_cleanup(crag_generator_t *g) { free(g); }

static crag_generator_t *local_gen_new(const crag_gen_cfg_t *cfg) {
    local_gen_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    const char *u = cfg->url ? cfg->url : getenv("CRAG_GEN_URL");
    strncpy(g->url, u ? u : DEFAULT_URL, sizeof g->url - 1);
    g->max_tokens   = cfg->max_tokens > 0 ? cfg->max_tokens : 1024;
    g->base.run     = local_run;
    g->base.cleanup = local_cleanup;
    return (crag_generator_t *)g;
}

crag_generator_t *gen_local_create(const crag_gen_cfg_t *cfg)  { return local_gen_new(cfg); }
crag_generator_t *gen_openai_create(const crag_gen_cfg_t *cfg) { return local_gen_new(cfg); }

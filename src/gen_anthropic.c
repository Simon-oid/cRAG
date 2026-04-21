#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "generate.h"
#include "http.h"
#include "util.h"

#define DEFAULT_URL   "https://api.anthropic.com/v1/messages"
#define DEFAULT_MODEL "claude-sonnet-4-6"

typedef struct {
    crag_generator_t base;   /* must be first */
    char url[512];
    char auth_header[256];
    char model[128];
    int  max_tokens;
} anthropic_gen_t;

static char *anthropic_run(crag_generator_t *g_,
                            const char *system_prompt,
                            const char *user_prompt) {
    anthropic_gen_t *g = (anthropic_gen_t *)g_;

    char *esys  = json_escape_dup(system_prompt ? system_prompt : "");
    char *euser = json_escape_dup(user_prompt   ? user_prompt   : "");
    if (!esys || !euser) { free(esys); free(euser); return NULL; }

    size_t body_sz = strlen(esys) + strlen(euser) + 256;
    char *body = malloc(body_sz);
    if (!body) { free(esys); free(euser); return NULL; }

    snprintf(body, body_sz,
        "{\"model\":\"%s\",\"max_tokens\":%d,"
        "\"system\":\"%s\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
        g->model, g->max_tokens, esys, euser);
    free(esys); free(euser);

    const char *headers[] = { g->auth_header, "anthropic-version: 2023-06-01", NULL };
    http_buf_t buf = {0};
    int status = http_post_json(g->url, headers, body, &buf);
    free(body);

    if (status != 200) {
        fprintf(stderr, "gen_anthropic: HTTP %d: %s\n", status, buf.data ? buf.data : "");
        free(buf.data);
        return NULL;
    }

    char *tp = strstr(buf.data, "\"text\":");
    if (!tp) { fprintf(stderr, "gen_anthropic: no text in response\n"); free(buf.data); return NULL; }
    tp += 7;
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

static void anthropic_cleanup(crag_generator_t *g) { free(g); }

crag_generator_t *gen_anthropic_create(const crag_gen_cfg_t *cfg) {
    const char *api_key = cfg->api_key ? cfg->api_key : getenv("ANTHROPIC_API_KEY");
    if (!api_key) { fprintf(stderr, "gen_anthropic: ANTHROPIC_API_KEY not set\n"); return NULL; }

    anthropic_gen_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;

    strncpy(g->url,   cfg->url   ? cfg->url   : DEFAULT_URL,   sizeof g->url   - 1);
    strncpy(g->model, cfg->model ? cfg->model : DEFAULT_MODEL, sizeof g->model - 1);
    snprintf(g->auth_header, sizeof g->auth_header, "x-api-key: %s", api_key);
    g->max_tokens = cfg->max_tokens > 0 ? cfg->max_tokens : 1024;

    g->base.run     = anthropic_run;
    g->base.cleanup = anthropic_cleanup;
    return (crag_generator_t *)g;
}

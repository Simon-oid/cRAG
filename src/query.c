#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crag.h"
#include "embed.h"
#include "store.h"
#include "generate.h"
#include "util.h"

#define DEFAULT_TOP_K 5
#define RAG_SYSTEM    "You are a helpful assistant. Answer using only the provided context."

int cmd_query(int argc, char **argv) {
    const char *query_text = NULL;
    const char *backend    = getenv("CRAG_EMBED_BACKEND") ? getenv("CRAG_EMBED_BACKEND") : "local";
    const char *db_path    = NULL;
    const char *url        = NULL;
    const char *format     = "text";
    int top_k  = DEFAULT_TOP_K;
    int do_gen = 0;

    for (int i = 0; i < argc; i++) {
        if      (strcmp(argv[i], "--backend")  == 0 && i+1 < argc) backend    = argv[++i];
        else if (strcmp(argv[i], "--db")       == 0 && i+1 < argc) db_path    = argv[++i];
        else if (strcmp(argv[i], "-i")         == 0 && i+1 < argc) db_path    = argv[++i];
        else if (strcmp(argv[i], "--url")      == 0 && i+1 < argc) url        = argv[++i];
        else if (strcmp(argv[i], "--top-k")    == 0 && i+1 < argc) top_k      = atoi(argv[++i]);
        else if (strcmp(argv[i], "-k")         == 0 && i+1 < argc) top_k      = atoi(argv[++i]);
        else if (strcmp(argv[i], "--format")   == 0 && i+1 < argc) format     = argv[++i];
        else if (strcmp(argv[i], "--generate") == 0) do_gen = 1;
        else if (argv[i][0] != '-') query_text = argv[i];
    }

    if (!query_text) { fprintf(stderr, "query: missing <text>\n"); return 2; }
    if (top_k <= 0 || top_k > 1000) {
        fprintf(stderr, "query: invalid --top-k (must be 1..1000)\n");
        return 2;
    }
    if (!db_path) db_path = getenv("CRAG_DB");
    if (!db_path) db_path = "./crag.db";

    crag_embed_backend_t *emb = embed_backend_create(backend, url, NULL);
    if (!emb) return 1;

    crag_chunk_t qchunk = {0};
    if (emb->embed_one(query_text, qchunk.vec, &qchunk.dim, emb->ctx) != 0) {
        fprintf(stderr, "query: embedding failed\n");
        embed_backend_free(emb);
        return 4;
    }

    crag_store_t store;
    if (store_open(db_path, &store) != 0) { embed_backend_free(emb); return 3; }

    crag_chunk_t *results = calloc((size_t)top_k, sizeof(crag_chunk_t));
    float        *scores  = calloc((size_t)top_k, sizeof(float));
    if (!results || !scores) {
        free(results); free(scores);
        store_close(&store); embed_backend_free(emb);
        return 1;
    }

    int n = store_search(&store, qchunk.vec, qchunk.dim, top_k, results, scores);

    int rc = 0;
    if (!do_gen) {
        int json_out = strcmp(format, "json") == 0;
        if (json_out) printf("[\n");
        for (int i = 0; i < n; i++) {
            if (json_out) {
                char *esrc  = json_escape_dup(results[i].source);
                char *etext = json_escape_dup(results[i].text);
                printf("  {\"score\":%.6f,\"source\":\"%s\",\"text\":\"%s\"}%s\n",
                       scores[i],
                       esrc  ? esrc  : "",
                       etext ? etext : "",
                       i < n-1 ? "," : "");
                free(esrc); free(etext);
            } else {
                printf("[%.4f] %s\n%s\n\n", scores[i], results[i].source, results[i].text);
            }
        }
        if (json_out) printf("]\n");
    } else {
        /* build context from results */
        size_t ctx_sz = 1;
        for (int i = 0; i < n; i++) {
            size_t tl = strlen(results[i].text);
            if (tl > (size_t)-1 - ctx_sz - 4) { rc = 1; goto cleanup; }
            ctx_sz += tl + 2;
        }
        char *context = malloc(ctx_sz);
        if (!context) { rc = 1; goto cleanup; }
        char *wp = context;
        for (int i = 0; i < n; i++) {
            size_t tl = strlen(results[i].text);
            memcpy(wp, results[i].text, tl); wp += tl;
            *wp++ = '\n'; *wp++ = '\n';
        }
        *wp = '\0';

        size_t prompt_sz = strlen(context) + strlen(query_text) + 32;
        char *prompt = malloc(prompt_sz);
        if (!prompt) { free(context); rc = 1; goto cleanup; }
        snprintf(prompt, prompt_sz, "Context:\n%s\nQuestion: %s", context, query_text);
        free(context);

        crag_generator_t *gen = generator_from_env();
        if (!gen) {
            fprintf(stderr, "query: failed to create generator\n");
            free(prompt); rc = 1; goto cleanup;
        }

        char *answer = generator_run(gen, RAG_SYSTEM, prompt);
        free(prompt);
        generator_free(gen);

        if (!answer) { rc = 1; goto cleanup; }

        if (strcmp(format, "json") == 0) {
            char *eans = json_escape_dup(answer);
            printf("{\"answer\":\"%s\"}\n", eans ? eans : "");
            free(eans);
        } else {
            printf("%s\n", answer);
        }
        free(answer);
    }

cleanup:
    for (int i = 0; i < n; i++) { free(results[i].source); free(results[i].text); }
    free(results); free(scores);
    store_close(&store);
    embed_backend_free(emb);
    return rc;
}

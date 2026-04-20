#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crag.h"
#include "embed.h"
#include "store.h"
#include "http.h"

#define CLAUDE_URL   "https://api.anthropic.com/v1/messages"
#define CLAUDE_MODEL "claude-sonnet-4-6"
#define DEFAULT_TOP_K 5

static int generate(const char *question, crag_chunk_t *chunks, int n) {
    const char *api_key = getenv("ANTHROPIC_API_KEY");
    if (!api_key) { fprintf(stderr, "query: ANTHROPIC_API_KEY not set\n"); return -1; }

    /* build context string */
    size_t ctx_sz = 0;
    for (int i = 0; i < n; i++) ctx_sz += strlen(chunks[i].text) + 4;
    char *context = malloc(ctx_sz + 1);
    if (!context) return -1;
    context[0] = '\0';
    for (int i = 0; i < n; i++) {
        strcat(context, chunks[i].text);
        strcat(context, "\n\n");
    }

    /* escape context and question for JSON */
    size_t clen = strlen(context), qlen = strlen(question);
    char *ectx = malloc(clen * 2 + 1);
    char *eq   = malloc(qlen * 2 + 1);
    if (!ectx || !eq) { free(ectx); free(eq); free(context); return -1; }

    size_t j = 0;
    for (size_t i = 0; i < clen; i++) {
        char c = context[i];
        if (c == '"' || c == '\\') ectx[j++] = '\\';
        else if (c == '\n') { ectx[j++] = '\\'; ectx[j++] = 'n'; continue; }
        else if (c == '\r') { ectx[j++] = '\\'; ectx[j++] = 'r'; continue; }
        ectx[j++] = c;
    }
    ectx[j] = '\0';

    j = 0;
    for (size_t i = 0; i < qlen; i++) {
        char c = question[i];
        if (c == '"' || c == '\\') eq[j++] = '\\';
        else if (c == '\n') { eq[j++] = '\\'; eq[j++] = 'n'; continue; }
        else if (c == '\r') { eq[j++] = '\\'; eq[j++] = 'r'; continue; }
        eq[j++] = c;
    }
    eq[j] = '\0';

    size_t body_sz = strlen(ectx) + strlen(eq) + 256;
    char *body = malloc(body_sz);
    if (!body) { free(ectx); free(eq); free(context); return -1; }

    snprintf(body, body_sz,
        "{"
        "\"model\":\"%s\","
        "\"max_tokens\":1024,"
        "\"system\":\"You are a helpful assistant. Answer using the provided context.\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"Context:\\n%s\\nQuestion: %s\"}]"
        "}",
        CLAUDE_MODEL, ectx, eq);

    free(ectx); free(eq); free(context);

    char auth_hdr[256];
    snprintf(auth_hdr, sizeof auth_hdr, "x-api-key: %s", api_key);
    const char *headers[] = {
        auth_hdr,
        "anthropic-version: 2023-06-01",
        NULL
    };

    http_buf_t buf = {0};
    int status = http_post_json(CLAUDE_URL, headers, body, &buf);
    free(body);

    if (status != 200) {
        fprintf(stderr, "generate: HTTP %d: %s\n", status, buf.data ? buf.data : "");
        free(buf.data);
        return -1;
    }

    /* parse content[0].text */
    char *text_field = strstr(buf.data, "\"text\":");
    if (!text_field) { fprintf(stderr, "generate: no text in response\n"); free(buf.data); return -1; }
    text_field += 7;
    while (*text_field == ' ') text_field++;
    if (*text_field != '"') { free(buf.data); return -1; }
    text_field++;

    /* unescape and print */
    while (*text_field && *text_field != '"') {
        if (*text_field == '\\' && *(text_field+1)) {
            text_field++;
            switch (*text_field) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'r': break;
                default:  putchar(*text_field); break;
            }
        } else {
            putchar(*text_field);
        }
        text_field++;
    }
    putchar('\n');

    free(buf.data);
    return 0;
}

int cmd_query(int argc, char **argv) {
    const char *query_text = NULL;
    const char *backend    = "openai";
    const char *db_path    = NULL;
    const char *url        = NULL;
    int top_k   = DEFAULT_TOP_K;
    int do_gen  = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i+1 < argc) backend = argv[++i];
        else if (strcmp(argv[i], "--db") == 0 && i+1 < argc) db_path = argv[++i];
        else if (strcmp(argv[i], "--url") == 0 && i+1 < argc) url = argv[++i];
        else if (strcmp(argv[i], "--top-k") == 0 && i+1 < argc) top_k = atoi(argv[++i]);
        else if (strcmp(argv[i], "--generate") == 0) do_gen = 1;
        else if (argv[i][0] != '-') query_text = argv[i];
    }

    if (!query_text) { fprintf(stderr, "query: missing <text>\n"); return 1; }
    if (!db_path) db_path = getenv("CRAG_DB");
    if (!db_path) db_path = "./crag.db";

    crag_embed_backend_t *emb = embed_backend_create(backend, url, NULL);
    if (!emb) return 1;

    crag_chunk_t qchunk = {0};
    if (emb->embed_one(query_text, qchunk.vec, &qchunk.dim, emb->ctx) != 0) {
        fprintf(stderr, "query: embedding failed\n");
        embed_backend_free(emb);
        return 1;
    }

    crag_store_t store;
    if (store_open(db_path, &store) != 0) { embed_backend_free(emb); return 1; }

    crag_chunk_t *results = calloc((size_t)top_k, sizeof(crag_chunk_t));
    float        *scores  = calloc((size_t)top_k, sizeof(float));
    if (!results || !scores) { free(results); free(scores); store_close(&store); embed_backend_free(emb); return 1; }

    int n = store_search(&store, qchunk.vec, qchunk.dim, top_k, results, scores);

    if (!do_gen) {
        for (int i = 0; i < n; i++) {
            printf("[%.4f] %s\n%s\n\n", scores[i], results[i].source, results[i].text);
        }
    } else {
        generate(query_text, results, n);
    }

    for (int i = 0; i < n; i++) { free(results[i].source); free(results[i].text); }
    free(results); free(scores);
    store_close(&store);
    embed_backend_free(emb);
    return 0;
}

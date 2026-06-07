#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "crag.h"
#include "embed.h"
#include "store.h"
#include "generate.h"
#include "util.h"

#define DEFAULT_TOP_K 5
#define DEFAULT_MIN_SCORE 0.60f

#define RAG_SYSTEM \
    "You are a retrieval-augmented assistant. Answer the user's question " \
    "using only the provided context. Follow these rules:\n" \
    "1. Read the whole context carefully. If it contains the answer — even as " \
    "a single sentence, number, or date — answer the question directly, " \
    "quoting or closely paraphrasing the relevant passage.\n" \
    "2. Use only the context: do not add outside knowledge and do not invent " \
    "facts, dates, names, or numbers.\n" \
    "3. A yes/no question is answerable when the context states the fact " \
    "either way; answer 'yes' or 'no' accordingly (for example, if the " \
    "context says something is not used, the answer is no).\n" \
    "4. Do not substitute a related fact for the one asked. If the context " \
    "genuinely lacks the information needed — including when it covers the " \
    "topic but not the specific detail requested (for example a different " \
    "number than the one asked) — reply exactly: Not in context.\n" \
    "5. Be concise."

int cmd_query(int argc, char **argv) {
    const char *query_text = NULL;
    const char *backend    = getenv("CRAG_EMBED_BACKEND") ? getenv("CRAG_EMBED_BACKEND") : "local";
    const char *db_path    = NULL;
    const char *url        = NULL;
    const char *format     = "text";
    const char *search     = "hybrid";  /* hybrid default: uses cosine + BM25 */
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
        else if (strcmp(argv[i], "--search")   == 0 && i+1 < argc) search     = argv[++i];
        else if (strcmp(argv[i], "--generate") == 0) do_gen = 1;
        else if (argv[i][0] != '-') query_text = argv[i];
    }

    if (!query_text) { fprintf(stderr, "query: missing <text>\n"); return 2; }
    if (top_k <= 0 || top_k > 1000) {
        fprintf(stderr, "query: invalid --top-k (must be 1..1000)\n");
        return 2;
    }
    int mode_cosine = strcmp(search, "cosine") == 0;
    int mode_bm25   = strcmp(search, "bm25")   == 0;
    int mode_hybrid = strcmp(search, "hybrid") == 0;
    if (!mode_cosine && !mode_bm25 && !mode_hybrid) {
        fprintf(stderr, "query: invalid --search '%s' (cosine|bm25|hybrid)\n", search);
        return 2;
    }
    if (!db_path) db_path = getenv("CRAG_DB");
    if (!db_path) db_path = "./crag.db";

    /* BM25-only mode never needs to embed the query. */
    crag_embed_backend_t *emb = NULL;
    crag_chunk_t qchunk = {0};
    if (!mode_bm25) {
        emb = embed_backend_create(backend, url, NULL);
        if (!emb) return 1;
        if (emb->embed_one(query_text, qchunk.vec, &qchunk.dim, emb->ctx) != 0) {
            fprintf(stderr, "query: embedding failed\n");
            embed_backend_free(emb);
            return 4;
        }
    }

    crag_store_t store;
    if (store_open(db_path, &store) != 0) {
        if (emb) embed_backend_free(emb);
        return 3;
    }

    crag_chunk_t *results = calloc((size_t)top_k, sizeof(crag_chunk_t));
    /* cosine/bm25 modes return one float per result; hybrid returns a
       crag_score_t breakdown. Only one array is populated per run. */
    float        *scores  = calloc((size_t)top_k, sizeof(float));
    crag_score_t *hscores = calloc((size_t)top_k, sizeof(crag_score_t));
    if (!results || !scores || !hscores) {
        free(results); free(scores); free(hscores);
        store_close(&store);
        if (emb) embed_backend_free(emb);
        return 1;
    }

    int n;
    if (mode_cosine)      n = store_search(&store, qchunk.vec, qchunk.dim, top_k, results, scores);
    else if (mode_bm25)   n = store_search_bm25(&store, query_text, top_k, results, scores);
    else                  n = store_search_hybrid(&store, qchunk.vec, qchunk.dim,
                                                  query_text, top_k, results, hscores);
    if (n < 0) n = 0;

    int rc = 0;
    if (!do_gen) {
        int json_out = strcmp(format, "json") == 0;
        /* Highlight matched terms in text output for bm25/hybrid. ANSI bold
           yellow when stdout is a terminal; plain `<<token>>` when piped or
           redirected so the markers survive in scripts and saved logs. JSON
           always stays unhighlighted to keep the payload machine-clean. */
        int highlight_on = !json_out && !mode_cosine;
        const char *open  = isatty(fileno(stdout)) ? "\033[1;33m" : "<<";
        const char *close = isatty(fileno(stdout)) ? "\033[0m"    : ">>";
        if (json_out) printf("[\n");
        for (int i = 0; i < n; i++) {
            if (json_out) {
                char *esrc  = json_escape_dup(results[i].source);
                char *etext = json_escape_dup(results[i].text);
                /* Hybrid also emits the raw cosine/bm25 breakdown so callers
                   can threshold on calibrated relevance; the fusion `score`
                   alone cannot be thresholded. */
                if (mode_hybrid) {
                    printf("  {\"score\":%.6f,\"cosine\":%.6f,\"bm25\":%.6f,"
                           "\"source\":\"%s\",\"text\":\"%s\"}%s\n",
                           hscores[i].fusion, hscores[i].cosine, hscores[i].bm25,
                           esrc  ? esrc  : "",
                           etext ? etext : "",
                           i < n-1 ? "," : "");
                } else {
                    printf("  {\"score\":%.6f,\"source\":\"%s\",\"text\":\"%s\"}%s\n",
                           scores[i],
                           esrc  ? esrc  : "",
                           etext ? etext : "",
                           i < n-1 ? "," : "");
                }
                free(esrc); free(etext);
            } else {
                char *hl = highlight_on
                    ? store_highlight_for_id(&store, query_text, results[i].id, open, close)
                    : NULL;
                if (mode_hybrid) {
                    printf("[%.4f cos=%.3f bm25=%.2f] %s\n%s\n\n",
                           hscores[i].fusion, hscores[i].cosine, hscores[i].bm25,
                           results[i].source, hl ? hl : results[i].text);
                } else {
                    printf("[%.4f] %s\n%s\n\n",
                           scores[i], results[i].source,
                           hl ? hl : results[i].text);
                }
                free(hl);
            }
        }
        if (json_out) printf("]\n");
    } else {
        /* Confidence gate: only meaningful in cosine mode because BM25 and
         * RRF scores live on different scales. For bm25/hybrid we fall back
         * to "refuse iff zero results" and rely on the RAG_SYSTEM prompt to
         * catch off-topic context. Override threshold via CRAG_GEN_MIN_SCORE.
         */
        /* Gate threshold is embedder-aware: CRAG_GEN_MIN_SCORE overrides, else
         * a measured per-model default (bge-m3 -> 0.38; the 0.60 fallback is
         * mxbai-calibrated and would wrongly refuse genuine bge-m3 answers). */
        const char *thresh_env  = getenv("CRAG_GEN_MIN_SCORE");
        const char *embed_model = getenv("CRAG_EMBED_MODEL");
        float min_score = gen_min_score_resolve(thresh_env, embed_model,
                                                DEFAULT_MIN_SCORE);
        float top_score = (n > 0) ? (mode_hybrid ? hscores[0].fusion : scores[0]) : 0.0f;
        int   gate_active = mode_cosine;

        if (n == 0 || (gate_active && top_score < min_score)) {
            const char *msg = "Not in context.";
            if (strcmp(format, "json") == 0) {
                printf("{\"answer\":\"%s\",\"refused\":true,"
                       "\"top_score\":%.6f,\"min_score\":%.4f}\n",
                       msg, top_score, min_score);
            } else {
                fprintf(stderr,
                    "query: refusing to generate — top score %.4f below "
                    "threshold %.4f\n", top_score, min_score);
                printf("%s\n", msg);
            }
            goto cleanup;
        }

        /* Generation-context selection (TODO item E): CRAG_GEN_K caps the prompt
         * context to the top-N retrieved chunks, decoupled from --top-k which
         * still drives retrieval + display. A larger gen model (e.g. qwen2.5:7b)
         * can be misled into a wrong/over-cautious answer by a low-confidence
         * tail chunk; capping the context removes that distractor. Results are
         * sorted by score descending, so this is a prefix cut. Env-gated, OFF by
         * default (unset = feed all n, the prior behavior).
         *
         * Measured on the widened Italian answer harness (65 answerable Qs,
         * tests/manual/answers.sh): CRAG_GEN_K=2 lifts 7b correct 63->64 with
         * zero regression on the default 3b (correct/abstain/refuse all
         * unchanged). CRAG_GEN_K=1 is too aggressive — it drops a 3b abstention
         * (invents a fact from a single chunk) for no correctness gain, so 2 is
         * the recommended floor. The effect is small and model-dependent, hence
         * opt-in rather than a default change. */
        int gen_n = n;
        const char *gen_k_env = getenv("CRAG_GEN_K");
        if (gen_k_env && *gen_k_env) {
            int gk = atoi(gen_k_env);
            if (gk > 0 && gk < gen_n) gen_n = gk;
        }
        if (gen_n < n)
            fprintf(stderr, "query: generating from top %d of %d retrieved "
                            "chunks\n", gen_n, n);

        /* build context from results */
        size_t ctx_sz = 1;
        for (int i = 0; i < gen_n; i++) {
            size_t tl = strlen(results[i].text);
            if (tl > (size_t)-1 - ctx_sz - 4) { rc = 1; goto cleanup; }
            ctx_sz += tl + 2;
        }
        char *context = malloc(ctx_sz);
        if (!context) { rc = 1; goto cleanup; }
        char *wp = context;
        for (int i = 0; i < gen_n; i++) {
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
    free(results); free(scores); free(hscores);
    store_close(&store);
    embed_backend_free(emb);
    return rc;
}

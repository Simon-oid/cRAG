/* eval_beir — BEIR-style retrieval quality harness for cRAG.
 *
 * Measures cRAG's real retrieval path (cosine / bm25 / hybrid) against a
 * BEIR dataset with human relevance judgments (qrels), computing the
 * standard IR metrics nDCG@10, Recall@10, MRR@10. This is the load-bearing
 * "are we state of the art" measurement: same query, same corpus, real
 * ground truth, compared against published baselines.
 *
 * Two subcommands, sharing one JSON string extractor:
 *
 *   eval_beir materialize <corpus.jsonl> <outdir>
 *       Write one <docid>.txt per corpus doc (title + blank line + text)
 *       so `crag index` can walk them. Idempotent at the file level.
 *
 *   eval_beir eval <db> <queries.jsonl> <qrels.tsv> [-k N] [--csv path]
 *       Embed each judged query, retrieve, map chunk->docid, score.
 *       Prints a per-mode table; optionally appends a CSV row per mode.
 *
 * Pure C11, links cRAG internals. Query embedding uses the ollama backend
 * by default (override via CRAG_EMBED_BACKEND / CRAG_EMBED_MODEL), matching
 * how the corpus was indexed.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>

#include "crag.h"
#include "embed.h"
#include "store.h"
#include "rerank.h"

#define DOCID_MAX     32       /* BEIR corpus ids are short strings        */
#define MAX_REL       1024     /* relevant docs/query (scifact <=~5, nfcorpus <=475) */
#define CHUNK_K       150      /* chunks pulled before collapsing to docs  */
#define RANK_CUT      10       /* nDCG/MRR reported @10                     */
#define MAX_DOCS      100      /* deepest doc cutoff we collapse to        */

/* Recall is reported at several depths. recall@10 is the live quality;
   recall@100 is the candidate-pool ceiling a reranker could reorder into
   the top — if recall@100 ~= recall@10 the pool is saturated and a
   reranker cannot help; if it is much higher there is real headroom. */
#define NCUT 3
static const int CUTS[NCUT] = { 10, 50, 100 };

/* ----------------------------- JSON helper ------------------------------ */

/* Append codepoint cp as UTF-8 to *out (advancing it). */
static void utf8_put(char **out, unsigned cp) {
    char *o = *out;
    if (cp < 0x80) {
        *o++ = (char)cp;
    } else if (cp < 0x800) {
        *o++ = (char)(0xC0 | (cp >> 6));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | (cp >> 12));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | (cp >> 18));
        *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    *out = o;
}

static unsigned hex4(const char *p) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    }
    return v;
}

/* Extract the string value of a top-level key from a JSON line.
   `key` is the bare name, e.g. "text". Returns a malloc'd decoded
   string, or NULL if the key (as "key":") is not found. */
static char *json_get_str(const char *line, const char *key) {
    char pat[64];
    int pn = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (pn <= 0 || pn >= (int)sizeof pat) return NULL;

    const char *p = strstr(line, pat);
    if (!p) return NULL;
    p += pn;
    while (*p && (*p == ' ' || *p == '\t')) p++;
    if (*p != ':') return NULL;
    p++;
    while (*p && (*p == ' ' || *p == '\t')) p++;
    if (*p != '"') return NULL;
    p++;

    /* decoded length <= remaining length */
    char *out = malloc(strlen(p) + 1);
    if (!out) return NULL;
    char *w = out;

    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': *w++ = '\n'; p++; break;
                case 't': *w++ = '\t'; p++; break;
                case 'r': *w++ = '\r'; p++; break;
                case 'b': *w++ = '\b'; p++; break;
                case 'f': *w++ = '\f'; p++; break;
                case '/': *w++ = '/';  p++; break;
                case '\\': *w++ = '\\'; p++; break;
                case '"': *w++ = '"';  p++; break;
                case 'u': {
                    if (!(p[1] && p[2] && p[3] && p[4])) { p++; break; }
                    unsigned cp = hex4(p + 1);
                    p += 5;
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        p[0] == '\\' && p[1] == 'u') {
                        unsigned lo = hex4(p + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    utf8_put(&w, cp);
                    break;
                }
                default: if (*p) *w++ = *p++; break;
            }
        } else {
            *w++ = *p++;
        }
    }
    *w = '\0';
    return out;
}

/* ----------------------------- materialize ------------------------------ */

static int cmd_materialize(const char *corpus_path, const char *outdir) {
    if (mkdir(outdir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "materialize: mkdir %s: %s\n", outdir, strerror(errno));
        return 1;
    }
    FILE *f = fopen(corpus_path, "r");
    if (!f) {
        fprintf(stderr, "materialize: open %s: %s\n", corpus_path, strerror(errno));
        return 1;
    }

    char  *line = NULL;
    size_t cap  = 0;
    ssize_t len;
    long written = 0;
    while ((len = getline(&line, &cap, f)) > 0) {
        char *id    = json_get_str(line, "_id");
        char *title = json_get_str(line, "title");
        char *text  = json_get_str(line, "text");
        if (id && text) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s.txt", outdir, id);
            FILE *o = fopen(path, "w");
            if (o) {
                if (title && *title) fprintf(o, "%s\n\n", title);
                fputs(text, o);
                fputc('\n', o);
                fclose(o);
                written++;
            }
        }
        free(id); free(title); free(text);
    }
    free(line);
    fclose(f);
    printf("materialize: wrote %ld docs to %s\n", written, outdir);
    return written > 0 ? 0 : 1;
}

/* -------------------------------- eval ---------------------------------- */

typedef struct {
    char qid[DOCID_MAX];
    char rel[MAX_REL][DOCID_MAX];
    int  nrel;
    char *text;          /* filled from queries.jsonl; NULL if missing */
} qjudge_t;

static qjudge_t *g_q = NULL;
static int       g_nq = 0, g_qcap = 0;

static qjudge_t *find_or_add(const char *qid) {
    for (int i = 0; i < g_nq; i++)
        if (strcmp(g_q[i].qid, qid) == 0) return &g_q[i];
    if (g_nq == g_qcap) {
        g_qcap = g_qcap ? g_qcap * 2 : 256;
        g_q = realloc(g_q, (size_t)g_qcap * sizeof *g_q);
        if (!g_q) { perror("realloc"); exit(1); }
    }
    qjudge_t *q = &g_q[g_nq++];
    memset(q, 0, sizeof *q);
    snprintf(q->qid, sizeof q->qid, "%s", qid);
    return q;
}

/* Load qrels TSV: "query-id\tcorpus-id\tscore", skipping the header.
   Treat any score >= 1 as relevant (binary). */
static int load_qrels(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "qrels: open %s: %s\n", path, strerror(errno)); return -1; }
    char *line = NULL; size_t cap = 0; ssize_t len;
    int lineno = 0, judged = 0;
    while ((len = getline(&line, &cap, f)) > 0) {
        lineno++;
        char qid[DOCID_MAX], did[DOCID_MAX]; int score = 0;
        if (lineno == 1 && strstr(line, "query-id")) continue;  /* header */
        if (sscanf(line, "%31s %31s %d", qid, did, &score) < 2) continue;
        if (score < 1) continue;
        qjudge_t *q = find_or_add(qid);
        if (q->nrel < MAX_REL) {
            snprintf(q->rel[q->nrel], DOCID_MAX, "%s", did);
            q->nrel++;
            judged++;
        }
    }
    free(line); fclose(f);
    fprintf(stderr, "qrels: %d queries, %d judgments\n", g_nq, judged);
    return 0;
}

/* Attach query text from queries.jsonl to the judged queries we loaded. */
static int load_queries(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "queries: open %s: %s\n", path, strerror(errno)); return -1; }
    char *line = NULL; size_t cap = 0; ssize_t len;
    int matched = 0;
    while ((len = getline(&line, &cap, f)) > 0) {
        char *id = json_get_str(line, "_id");
        if (!id) continue;
        for (int i = 0; i < g_nq; i++) {
            if (strcmp(g_q[i].qid, id) == 0) {
                if (!g_q[i].text) { g_q[i].text = json_get_str(line, "text"); matched++; }
                break;
            }
        }
        free(id);
    }
    free(line); fclose(f);
    fprintf(stderr, "queries: matched text for %d/%d judged queries\n", matched, g_nq);
    return 0;
}

/* "/tmp/x/31715818.txt" -> "31715818" into buf */
static void source_to_docid(const char *src, char *buf, size_t bufsz) {
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    snprintf(buf, bufsz, "%s", base);
    char *dot = strrchr(buf, '.');
    if (dot) *dot = '\0';
}

static int is_relevant(const qjudge_t *q, const char *docid) {
    for (int i = 0; i < q->nrel; i++)
        if (strcmp(q->rel[i], docid) == 0) return 1;
    return 0;
}

typedef struct { double ndcg; double recall[NCUT]; double mrr; int n; } acc_t;

/* Score one query given an already-ranked, deduped list of doc ids.
   nDCG and MRR are computed @RANK_CUT; recall is computed at every depth
   in CUTS[] so we can see the candidate-pool ceiling vs the live top-10. */
static void score_query(const qjudge_t *q, char docs[][DOCID_MAX], int ndocs,
                        acc_t *a) {
    int cut = ndocs < RANK_CUT ? ndocs : RANK_CUT;

    double dcg = 0.0;
    int first_rank = 0;
    for (int i = 0; i < cut; i++) {
        if (is_relevant(q, docs[i])) {
            dcg += 1.0 / log2((double)(i + 2));   /* rel=1, rank i (0-based) */
            if (!first_rank) first_rank = i + 1;
        }
    }
    int ideal = q->nrel < RANK_CUT ? q->nrel : RANK_CUT;
    double idcg = 0.0;
    for (int i = 0; i < ideal; i++) idcg += 1.0 / log2((double)(i + 2));

    a->ndcg += idcg > 0 ? dcg / idcg : 0.0;
    a->mrr  += first_rank ? 1.0 / first_rank : 0.0;

    for (int c = 0; c < NCUT; c++) {
        int depth = CUTS[c] < ndocs ? CUTS[c] : ndocs;
        int hits = 0;
        for (int i = 0; i < depth; i++) if (is_relevant(q, docs[i])) hits++;
        a->recall[c] += q->nrel > 0 ? (double)hits / q->nrel : 0.0;
    }
    a->n++;
}

/* Collapse ranked chunk results to unique doc ids preserving first-seen
   order. Returns number of unique docs written to docs[]. */
static int collapse_docs(crag_chunk_t *res, int nres, char docs[][DOCID_MAX],
                         int maxdocs) {
    int nd = 0;
    for (int i = 0; i < nres && nd < maxdocs; i++) {
        char did[DOCID_MAX];
        source_to_docid(res[i].source, did, sizeof did);
        int seen = 0;
        for (int j = 0; j < nd; j++) if (strcmp(docs[j], did) == 0) { seen = 1; break; }
        if (!seen) snprintf(docs[nd++], DOCID_MAX, "%s", did);
    }
    return nd;
}

/* Like collapse_docs but visits res[] in the order given by order[0..nres-1]
   (a permutation of chunk indices). Used after reranking, which produces a
   reordered view of the same retrieved pool without moving the chunks. */
static int collapse_docs_ordered(crag_chunk_t *res, const int *order, int nres,
                                 char docs[][DOCID_MAX], int maxdocs) {
    int nd = 0;
    for (int oi = 0; oi < nres && nd < maxdocs; oi++) {
        int i = order[oi];
        char did[DOCID_MAX];
        source_to_docid(res[i].source, did, sizeof did);
        int seen = 0;
        for (int j = 0; j < nd; j++) if (strcmp(docs[j], did) == 0) { seen = 1; break; }
        if (!seen) snprintf(docs[nd++], DOCID_MAX, "%s", did);
    }
    return nd;
}

enum { M_COSINE, M_BM25, M_HYBRID, M_RERANK, M_COUNT };
static const char *mode_name[M_COUNT] = { "cosine", "bm25", "hybrid", "rerank" };

static int run_eval(const char *db_path, const char *queries_path,
                    const char *qrels_path, int k_chunks, const char *csv_path,
                    const char *rr_name, const char *rr_url, int rr_pool,
                    const char *dataset) {
    if (load_qrels(qrels_path) < 0) return 1;
    if (load_queries(queries_path) < 0) return 1;

    crag_store_t store;
    if (store_open(db_path, &store) != 0) {
        fprintf(stderr, "eval: cannot open db %s\n", db_path);
        return 1;
    }
    crag_store_stats_t st = {0};
    store_stats(&store, &st);
    fprintf(stderr, "eval: db has %d chunks across %d files (dim %d)\n",
            st.chunks, st.files, st.dim);

    const char *backend = getenv("CRAG_EMBED_BACKEND");
    if (!backend) backend = "ollama";
    crag_embed_backend_t *emb = embed_backend_create(backend, NULL, NULL);
    if (!emb) { store_close(&store); return 1; }

    /* The rerank pass reuses a deep hybrid pool; size all per-query buffers to
       hold the larger of the two retrieval depths. */
    crag_rerank_backend_t *rr = NULL;
    if (rr_name) {
        rr = rerank_backend_create(rr_name, rr_url);
        if (!rr) { embed_backend_free(emb); store_close(&store); return 1; }
        fprintf(stderr, "eval: rerank backend '%s' active, pool=%d\n",
                rr->name, rr_pool);
    }
    int cap = k_chunks;
    if (rr && rr_pool > cap) cap = rr_pool;

    crag_chunk_t *res     = calloc((size_t)cap, sizeof *res);
    float        *scores  = calloc((size_t)cap, sizeof *scores);
    crag_score_t *hscores = calloc((size_t)cap, sizeof *hscores);
    char (*docs)[DOCID_MAX] = calloc((size_t)cap, sizeof *docs);
    /* rerank scratch: per-doc texts, score, and an index permutation. */
    const char  **texts   = rr ? calloc((size_t)cap, sizeof *texts)  : NULL;
    float        *rscore  = rr ? calloc((size_t)cap, sizeof *rscore) : NULL;
    int          *order   = rr ? calloc((size_t)cap, sizeof *order)  : NULL;
    if (!res || !scores || !hscores || !docs ||
        (rr && (!texts || !rscore || !order))) { perror("calloc"); return 1; }

    acc_t acc[M_COUNT] = {{0}};
    int skipped = 0, rr_failed = 0;

    for (int qi = 0; qi < g_nq; qi++) {
        qjudge_t *q = &g_q[qi];
        if (!q->text) { skipped++; continue; }

        float qvec[CRAG_EMBED_DIM_MAX]; int qdim = 0;
        int have_vec = 0;
        if (emb->embed_one(q->text, qvec, &qdim, emb->ctx) == 0) have_vec = 1;

        for (int m = 0; m < M_RERANK; m++) {
            if ((m == M_COSINE || m == M_HYBRID) && !have_vec) continue;
            int n = 0;
            if (m == M_COSINE)
                n = store_search(&store, qvec, qdim, k_chunks, res, scores);
            else if (m == M_BM25)
                n = store_search_bm25(&store, q->text, k_chunks, res, scores);
            else
                n = store_search_hybrid(&store, qvec, qdim, q->text,
                                        k_chunks, res, hscores);
            if (n < 0) n = 0;
            int nd = collapse_docs(res, n, docs, MAX_DOCS);
            score_query(q, docs, nd, &acc[m]);
            for (int i = 0; i < n; i++) { free(res[i].source); free(res[i].text); }
        }

        /* Rerank pass: pull a deep hybrid pool, let the cross-encoder reorder
           it, then collapse to docs in the reranked order. On any backend
           failure we fall back to the hybrid order (identity permutation) so
           the run still completes — it just shows no lift. */
        if (rr && have_vec) {
            int n = store_search_hybrid(&store, qvec, qdim, q->text,
                                        rr_pool, res, hscores);
            if (n < 0) n = 0;
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    texts[i] = res[i].text ? res[i].text : "";
                    order[i] = i;
                }
                int rc = rr->rerank(q->text, texts, n, rscore, rr->ctx);
                if (rc == 0) {
                    /* stable selection sort of order[] by rscore desc */
                    for (int a = 0; a < n; a++) {
                        int best = a;
                        for (int b = a + 1; b < n; b++)
                            if (rscore[order[b]] > rscore[order[best]]) best = b;
                        int t = order[a]; order[a] = order[best]; order[best] = t;
                    }
                } else {
                    rr_failed++;
                }
                int nd = collapse_docs_ordered(res, order, n, docs, MAX_DOCS);
                score_query(q, docs, nd, &acc[M_RERANK]);
            }
            for (int i = 0; i < n; i++) { free(res[i].source); free(res[i].text); }
        }
    }

    free(res); free(scores); free(hscores); free(docs);
    free(texts); free(rscore); free(order);
    if (rr) rerank_backend_free(rr);
    embed_backend_free(emb);
    store_close(&store);
    if (rr_failed)
        fprintf(stderr, "eval: %d queries fell back to hybrid order "
                "(rerank backend error)\n", rr_failed);

    int nprint = rr ? M_COUNT : M_RERANK;   /* hide rerank row unless active */

    int is_nfcorpus = dataset && strcmp(dataset, "nfcorpus") == 0;

    printf("\n=== cRAG retrieval quality — %s (BEIR) ===\n",
           dataset ? dataset : "scifact");
    printf("%-10s %9s %8s %9s %9s %10s %8s\n", "mode", "nDCG@10", "MRR@10",
           "R@10", "R@50", "R@100", "queries");
    for (int m = 0; m < nprint; m++) {
        int n = acc[m].n ? acc[m].n : 1;
        printf("%-10s %9.4f %8.4f %9.4f %9.4f %10.4f %8d\n", mode_name[m],
               acc[m].ndcg / n, acc[m].mrr / n,
               acc[m].recall[0] / n, acc[m].recall[1] / n, acc[m].recall[2] / n,
               acc[m].n);
    }
    if (skipped) printf("(%d judged queries skipped: no query text)\n", skipped);

    /* Reranker ceiling read-out: how much recall sits in the deep pool that a
       reranker could pull into the top-10 vs what is already there. */
    {
        int n = acc[M_COSINE].n ? acc[M_COSINE].n : 1;
        double r10  = acc[M_COSINE].recall[0] / n;
        double r100 = acc[M_COSINE].recall[2] / n;
        printf("\nreranker headroom (cosine pool): recall@10=%.4f -> recall@100=%.4f"
               " (+%.4f recoverable)\n", r10, r100, r100 - r10);
    }

    if (is_nfcorpus) {
        printf("\n--- published reference (BEIR/MTEB, NFCorpus nDCG@10) ---\n");
        printf("  BM25 (Anserini/BEIR paper) .............. 0.325\n");
        printf("  all-MiniLM-L6-v2 (dense) ................ ~0.305\n");
        printf("  Contriever (fine-tuned) ................. ~0.328\n");
        printf("  bge-small-en-v1.5 ....................... ~0.34\n");
        printf("  mxbai-embed-large-v1 (MTEB) ............. ~0.358\n");
        printf("  NFCorpus is a weak-dense medical task where BM25 is\n");
        printf("  competitive — the classic place a cross-encoder helps.\n");
        printf("  NB: this harness scores binary relevance, so absolute\n");
        printf("  nDCG reads below graded-gain official numbers; the\n");
        printf("  cosine/bm25/hybrid/rerank *deltas* are the signal.\n");
    } else {
        printf("\n--- published reference (BEIR/MTEB, SciFact nDCG@10) ---\n");
        printf("  BM25 (Anserini/BEIR paper) .............. 0.665\n");
        printf("  all-MiniLM-L6-v2 (dense) ................ 0.645\n");
        printf("  Contriever (fine-tuned) ................. 0.677\n");
        printf("  bge-small-en-v1.5 ....................... ~0.71\n");
        printf("  mxbai-embed-large-v1 (MTEB) ............. ~0.74\n");
        printf("  (cRAG's number depends on its configured embedder + fusion)\n");
    }

    if (csv_path) {
        int exists = 0;
        FILE *chk = fopen(csv_path, "r");
        if (chk) { exists = 1; fclose(chk); }
        FILE *csv = fopen(csv_path, "a");
        if (csv) {
            if (!exists) fprintf(csv, "mode,ndcg10,mrr10,recall10,recall50,recall100,queries\n");
            for (int m = 0; m < nprint; m++) {
                int n = acc[m].n ? acc[m].n : 1;
                fprintf(csv, "%s,%.4f,%.4f,%.4f,%.4f,%.4f,%d\n", mode_name[m],
                        acc[m].ndcg / n, acc[m].mrr / n,
                        acc[m].recall[0] / n, acc[m].recall[1] / n,
                        acc[m].recall[2] / n, acc[m].n);
            }
            fclose(csv);
            fprintf(stderr, "eval: wrote results to %s\n", csv_path);
        }
    }

    for (int i = 0; i < g_nq; i++) free(g_q[i].text);
    free(g_q);
    return 0;
}

/* -------------------------------- main ---------------------------------- */

int main(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[1], "materialize") == 0)
        return cmd_materialize(argv[2], argv[3]);

    if (argc >= 5 && strcmp(argv[1], "eval") == 0) {
        const char *db = argv[2], *queries = argv[3], *qrels = argv[4];
        int k = CHUNK_K;
        const char *csv = NULL;
        const char *dataset = "scifact";   /* drives header + reference block */
        const char *rr_name = NULL, *rr_url = NULL;
        int rr_pool = 100;            /* deep pool the cross-encoder reorders */
        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) k = atoi(argv[++i]);
            else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) csv = argv[++i];
            else if (strcmp(argv[i], "--dataset") == 0 && i + 1 < argc) dataset = argv[++i];
            else if (strcmp(argv[i], "--rerank") == 0) {
                if (!rr_name) rr_name = "llamacpp";
            }
            else if (strcmp(argv[i], "--rerank-backend") == 0 && i + 1 < argc)
                rr_name = argv[++i];
            else if (strcmp(argv[i], "--rerank-url") == 0 && i + 1 < argc)
                rr_url = argv[++i];
            else if (strcmp(argv[i], "--rerank-pool") == 0 && i + 1 < argc)
                rr_pool = atoi(argv[++i]);
        }
        if (k <= 0) k = CHUNK_K;
        if (rr_pool <= 0) rr_pool = 100;
        /* --rerank-url alone implies the default llama.cpp rerank backend. */
        if (rr_url && !rr_name) rr_name = "llamacpp";
        return run_eval(db, queries, qrels, k, csv, rr_name, rr_url, rr_pool,
                        dataset);
    }

    fprintf(stderr,
        "usage:\n"
        "  %s materialize <corpus.jsonl> <outdir>\n"
        "  %s eval <db> <queries.jsonl> <qrels.tsv> [-k N] [--csv path]\n"
        "          [--dataset NAME] [--rerank] [--rerank-backend NAME]\n"
        "          [--rerank-url URL] [--rerank-pool N]\n",
        argv[0], argv[0]);
    return 2;
}

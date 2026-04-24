#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ftw.h>
#include "crag.h"
#include "chunk.h"
#include "embed.h"
#include "store.h"

#define MAX_CHUNKS_PER_FILE 512
#define BATCH_SZ            256
#define COMMIT_EVERY_FILES  20

/* walk state passed via file-scope struct (nftw provides no user-data pointer) */
static struct {
    char          *exts[32];  /* owned by parse_exts / free_exts */
    int            next;
    crag_embed_backend_t *emb;
    crag_store_t  *store;
    int            max_chars;
    int            total_chunks;
    int            total_errors;
    int            total_files;
    int            files_since_commit;
    /* drop-reason aggregates for the final summary */
    int            drops_context;
    int            drops_other;
    int            files_fully_dropped;
    int            files_partial;
} g_walk;

static void free_exts(void) {
    for (int i = 0; i < g_walk.next; i++) { free(g_walk.exts[i]); g_walk.exts[i] = NULL; }
    g_walk.next = 0;
}

static int has_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    dot++;
    for (int i = 0; i < g_walk.next; i++) {
        if (strcasecmp(dot, g_walk.exts[i]) == 0) return 1;
    }
    return 0;
}

/* Embed one chunk's text and insert it into the store. If the embedder
   rejects the input as too long for its context window (CRAG_ECONTEXT),
   split the text roughly in half at a word boundary and recurse on each
   half. This fixes the char-vs-token mismatch: the chunker limits by
   character count, but real embedders limit by token count, and for
   symbol-dense text (URLs, markdown tables, code) 2048 chars can tokenize
   past small windows like mxbai-embed-large's 512. */
#define EMBED_SPLIT_MAX_DEPTH 3
#define EMBED_SPLIT_MIN_CHARS 200

/* Returns 1 if at least one (sub-)chunk landed in the store; 0 if the
   original chunk was fully dropped (all split attempts failed too). */
static int embed_and_insert(const char *source, const char *text, int depth) {
    crag_chunk_t chunk = {0};
    chunk.source = (char *)source;
    chunk.text   = (char *)text;

    int rc = g_walk.emb->embed_one(text, chunk.vec, &chunk.dim,
                                   g_walk.emb->ctx);
    if (rc == 0) {
        if (store_insert(g_walk.store, &chunk) == 0) {
            g_walk.total_chunks++;
            return 1;
        }
        g_walk.total_errors++;
        g_walk.drops_other++;
        return 0;
    }

    if (rc == CRAG_ECONTEXT && depth < EMBED_SPLIT_MAX_DEPTH) {
        size_t len = strlen(text);
        if (len >= EMBED_SPLIT_MIN_CHARS) {
            size_t half = len / 2;
            /* nudge the split to the next newline or space so we don't
               cut mid-word; cap the scan so we don't walk far */
            const char *split = text + half;
            for (const char *p = text + half; *p && p < text + half + 64; p++) {
                if (*p == '\n' || *p == ' ') { split = p + 1; break; }
            }
            size_t left_len = (size_t)(split - text);
            char *left  = strndup(text, left_len);
            char *right = strdup(split);
            if (left && right) {
                int l = embed_and_insert(source, left,  depth + 1);
                int r = embed_and_insert(source, right, depth + 1);
                free(left); free(right);
                return l || r;
            }
            free(left); free(right);
        }
    }

    /* Unrecoverable leaf drop — classify the reason for the summary. */
    if (rc == CRAG_ECONTEXT) {
        g_walk.drops_context++;
        fprintf(stderr, "index: chunk still too long for embedder (depth %d) in '%s' — dropping\n",
                depth, source);
    } else {
        g_walk.drops_other++;
        fprintf(stderr, "index: embed failed (rc=%d) for chunk in '%s'\n", rc, source);
    }
    g_walk.total_errors++;
    return 0;
}

static int walk_cb(const char *fpath, const struct stat *sb,
                   int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag != FTW_F || !has_ext(fpath)) return 0;

    char *texts[MAX_CHUNKS_PER_FILE];
    int n = chunk_file(fpath, g_walk.max_chars, texts, MAX_CHUNKS_PER_FILE);
    if (n < 0) {
        fprintf(stderr, "index: failed to chunk '%s'\n", fpath);
        return 0;
    }
    g_walk.total_files++;

    int file_dropped = 0;

    /* embed in batches */
    for (int start = 0; start < n; ) {
        int batch = n - start;
        if (batch > BATCH_SZ) batch = BATCH_SZ;
        const char **batch_texts = (const char **)texts + start;

        if (g_walk.emb->embed_many) {
            float *vecs = calloc((size_t)batch * CRAG_EMBED_DIM_MAX, sizeof(float));
            int   *dims = calloc((size_t)batch, sizeof(int));
            if (!vecs || !dims) {
                free(vecs); free(dims);
                for (int i = 0; i < n; i++) free(texts[i]);
                return 0;
            }
            if (g_walk.emb->embed_many(batch_texts, batch, vecs, dims, g_walk.emb->ctx) == 0) {
                for (int i = 0; i < batch; i++) {
                    if (dims[i] <= 0) {
                        g_walk.total_errors++; g_walk.drops_other++; file_dropped++;
                        continue;
                    }
                    crag_chunk_t chunk = {0};
                    chunk.source = (char *)fpath;
                    chunk.text   = texts[start + i];
                    chunk.dim    = dims[i];
                    memcpy(chunk.vec, vecs + (size_t)i * CRAG_EMBED_DIM_MAX,
                           (size_t)dims[i] * sizeof(float));
                    if (store_insert(g_walk.store, &chunk) == 0) {
                        g_walk.total_chunks++;
                    } else {
                        g_walk.total_errors++; g_walk.drops_other++; file_dropped++;
                    }
                }
            } else {
                fprintf(stderr, "index: batch embed failed for '%s'\n", fpath);
                g_walk.total_errors += batch;
                g_walk.drops_other  += batch;
                file_dropped        += batch;
            }
            free(vecs); free(dims);
        } else {
            /* fallback: one by one, with recursive split on CRAG_ECONTEXT */
            for (int i = 0; i < batch; i++) {
                if (embed_and_insert(fpath, batch_texts[i], 0) == 0)
                    file_dropped++;
            }
        }
        start += batch;
    }

    for (int i = 0; i < n; i++) free(texts[i]);

    if (n > 0 && file_dropped > 0) {
        if (file_dropped >= n) {
            fprintf(stderr, "partial: ALL %d chunks of '%s' dropped\n", n, fpath);
            g_walk.files_fully_dropped++;
        } else {
            fprintf(stderr, "partial: %d/%d chunks of '%s' dropped\n",
                    file_dropped, n, fpath);
            g_walk.files_partial++;
        }
    }

    /* Bound the blast radius of an interrupt: commit every N files and open
       a fresh transaction. Without this, a single final COMMIT wrapping the
       whole nftw() walk means a killed/crashed process loses all work. */
    if (++g_walk.files_since_commit >= COMMIT_EVERY_FILES) {
        if (store_commit(g_walk.store) == 0 && store_begin(g_walk.store) == 0)
            g_walk.files_since_commit = 0;
    }
    return 0;
}

static void parse_exts(const char *extstr) {
    char *dup = strdup(extstr);
    if (!dup) return;
    g_walk.next = 0;
    char *tok = strtok(dup, ",");
    while (tok && g_walk.next < 32) {
        char *s = strdup(tok);
        if (!s) break;
        g_walk.exts[g_walk.next++] = s;
        tok = strtok(NULL, ",");
    }
    free(dup);
}

int cmd_index(int argc, char **argv) {
    const char *dir      = NULL;
    const char *backend  = getenv("CRAG_EMBED_BACKEND") ? getenv("CRAG_EMBED_BACKEND") : "local";
    const char *db_path  = NULL;
    const char *url      = NULL;
    const char *extstr   = "txt,md";
    int max_chars = CHUNK_MAX_CHARS_DEFAULT;

    for (int i = 0; i < argc; i++) {
        if      (strcmp(argv[i], "--backend")   == 0 && i+1 < argc) backend   = argv[++i];
        else if (strcmp(argv[i], "--db")        == 0 && i+1 < argc) db_path   = argv[++i];
        else if ((strcmp(argv[i], "-o")         == 0) && i+1 < argc) db_path  = argv[++i];
        else if (strcmp(argv[i], "--url")       == 0 && i+1 < argc) url       = argv[++i];
        else if (strcmp(argv[i], "--ext")       == 0 && i+1 < argc) extstr    = argv[++i];
        else if (strcmp(argv[i], "--max-bytes") == 0 && i+1 < argc) max_chars = atoi(argv[++i]);
        else if (argv[i][0] != '-') dir = argv[i];
    }

    if (!dir) { fprintf(stderr, "index: missing <dir>\n"); return 2; }
    if (!db_path) db_path = getenv("CRAG_DB");
    if (!db_path) db_path = "./crag.db";

    parse_exts(extstr);

    crag_embed_backend_t *emb = embed_backend_create(backend, url, NULL);
    if (!emb) return 1;

    crag_store_t store;
    if (store_open(db_path, &store) != 0) { embed_backend_free(emb); return 3; }

    g_walk.emb                 = emb;
    g_walk.store               = &store;
    g_walk.max_chars           = max_chars;
    g_walk.total_chunks        = 0;
    g_walk.total_errors        = 0;
    g_walk.total_files         = 0;
    g_walk.files_since_commit  = 0;
    g_walk.drops_context       = 0;
    g_walk.drops_other         = 0;
    g_walk.files_fully_dropped = 0;
    g_walk.files_partial       = 0;

    store_begin(&store);
    nftw(dir, walk_cb, 64, FTW_PHYS);
    store_commit(&store);

    fprintf(stderr, "\n=== index summary ===\n");
    fprintf(stderr, "  files scanned:       %d\n", g_walk.total_files);
    fprintf(stderr, "  chunks stored:       %d\n", g_walk.total_chunks);
    fprintf(stderr, "  chunks dropped:      %d  (context=%d, other=%d)\n",
            g_walk.total_errors, g_walk.drops_context, g_walk.drops_other);
    fprintf(stderr, "  files partial:       %d\n", g_walk.files_partial);
    fprintf(stderr, "  files fully dropped: %d\n", g_walk.files_fully_dropped);
    fprintf(stderr, "  db:                  %s\n", db_path);

    store_close(&store);
    embed_backend_free(emb);
    free_exts();

    /* Exit codes: 4 = nothing indexed but errors; 5 = partial corpus hole
       (at least one file lost all its chunks). CI/automation can catch
       silent data loss by checking for non-zero. */
    if (g_walk.total_chunks == 0 && g_walk.total_errors > 0) return 4;
    if (g_walk.files_fully_dropped > 0) return 5;
    return 0;
}

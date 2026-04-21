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
                    if (dims[i] <= 0) { g_walk.total_errors++; continue; }
                    crag_chunk_t chunk = {0};
                    chunk.source = (char *)fpath;
                    chunk.text   = texts[start + i];
                    chunk.dim    = dims[i];
                    memcpy(chunk.vec, vecs + (size_t)i * CRAG_EMBED_DIM_MAX,
                           (size_t)dims[i] * sizeof(float));
                    if (store_insert(g_walk.store, &chunk) == 0)
                        g_walk.total_chunks++;
                    else
                        g_walk.total_errors++;
                }
            } else {
                fprintf(stderr, "index: batch embed failed for '%s'\n", fpath);
                g_walk.total_errors += batch;
            }
            free(vecs); free(dims);
        } else {
            /* fallback: one by one */
            for (int i = 0; i < batch; i++) {
                crag_chunk_t chunk = {0};
                chunk.source = (char *)fpath;
                chunk.text   = (char *)batch_texts[i];
                if (g_walk.emb->embed_one(batch_texts[i], chunk.vec, &chunk.dim,
                                          g_walk.emb->ctx) == 0) {
                    if (store_insert(g_walk.store, &chunk) == 0)
                        g_walk.total_chunks++;
                    else
                        g_walk.total_errors++;
                } else {
                    fprintf(stderr, "index: embed failed for chunk %d of '%s'\n",
                            start + i, fpath);
                    g_walk.total_errors++;
                }
            }
        }
        start += batch;
    }

    for (int i = 0; i < n; i++) free(texts[i]);
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

    g_walk.emb         = emb;
    g_walk.store       = &store;
    g_walk.max_chars   = max_chars;
    g_walk.total_chunks = 0;
    g_walk.total_errors = 0;
    g_walk.total_files  = 0;

    store_begin(&store);
    nftw(dir, walk_cb, 64, FTW_PHYS);
    store_commit(&store);

    fprintf(stderr, "indexed %d chunks from %d files into %s (%d errors)\n",
            g_walk.total_chunks, g_walk.total_files, db_path, g_walk.total_errors);

    store_close(&store);
    embed_backend_free(emb);
    free_exts();

    return g_walk.total_chunks == 0 && g_walk.total_errors > 0 ? 4 : 0;
}

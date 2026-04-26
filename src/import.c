#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crag.h"
#include "embed.h"
#include "store.h"

#define IMPORT_BATCH 64

/* Extract a JSON string field by key. Scans `line` for `"key":"`, then copies
   bytes into `out` (size out_sz) until an unescaped `"`, unescaping the
   subset of sequences that json_escape() emits: \" \\ \n \r \t and \uXXXX
   (only handles \u00XX since json_escape only emits ASCII control points).
   Returns 0 on success, -1 if the key is missing or the value is malformed. */
static int json_get_string(const char *line, const char *key,
                           char *out, size_t out_sz) {
    if (!line || !key || !out || out_sz == 0) return -1;
    /* Build the search needle: "key": (with optional whitespace, but our own
       exporter emits no spaces, so a strict match is enough for round-trip). */
    char needle[64];
    int nl = snprintf(needle, sizeof needle, "\"%s\":\"", key);
    if (nl < 0 || (size_t)nl >= sizeof needle) return -1;

    const char *p = strstr(line, needle);
    if (!p) return -1;
    p += nl;

    size_t wi = 0;
    while (*p && *p != '"') {
        if (wi + 1 >= out_sz) return -1;
        if (*p == '\\' && p[1]) {
            char esc = p[1];
            switch (esc) {
                case '"':  out[wi++] = '"';  p += 2; break;
                case '\\': out[wi++] = '\\'; p += 2; break;
                case '/':  out[wi++] = '/';  p += 2; break;
                case 'n':  out[wi++] = '\n'; p += 2; break;
                case 'r':  out[wi++] = '\r'; p += 2; break;
                case 't':  out[wi++] = '\t'; p += 2; break;
                case 'b':  out[wi++] = '\b'; p += 2; break;
                case 'f':  out[wi++] = '\f'; p += 2; break;
                case 'u': {
                    if (!p[2] || !p[3] || !p[4] || !p[5]) return -1;
                    char hex[5] = { p[2], p[3], p[4], p[5], 0 };
                    unsigned code = (unsigned)strtoul(hex, NULL, 16);
                    if (code == 0) return -1;
                    if (code < 0x80) {
                        out[wi++] = (char)code;
                    } else if (code < 0x800) {
                        if (wi + 2 >= out_sz) return -1;
                        out[wi++] = (char)(0xC0 | (code >> 6));
                        out[wi++] = (char)(0x80 | (code & 0x3F));
                    } else {
                        if (wi + 3 >= out_sz) return -1;
                        out[wi++] = (char)(0xE0 | (code >> 12));
                        out[wi++] = (char)(0x80 | ((code >> 6) & 0x3F));
                        out[wi++] = (char)(0x80 | (code & 0x3F));
                    }
                    p += 6;
                    break;
                }
                default: return -1;
            }
        } else {
            out[wi++] = *p++;
        }
    }
    if (*p != '"') return -1;
    out[wi] = '\0';
    return 0;
}

/* Hold one parsed JSONL row so we can batch through embed_many when the
   backend supports it. text is owned and freed by the caller. */
typedef struct { char *source; char *text; } row_t;

static void rows_free(row_t *rows, int n) {
    for (int i = 0; i < n; i++) { free(rows[i].source); free(rows[i].text); }
}

/* Embed and insert one batch of rows. Uses embed_many when available, falls
   back to per-chunk embed_one. Returns inserted count; updates *errors. */
static int flush_batch(crag_embed_backend_t *emb, crag_store_t *store,
                       row_t *rows, int n, int *errors) {
    int inserted = 0;
    if (emb->embed_many) {
        const char **texts = calloc((size_t)n, sizeof *texts);
        float *vecs = calloc((size_t)n * CRAG_EMBED_DIM_MAX, sizeof(float));
        int   *dims = calloc((size_t)n, sizeof(int));
        if (!texts || !vecs || !dims) { free(texts); free(vecs); free(dims); *errors += n; return 0; }
        for (int i = 0; i < n; i++) texts[i] = rows[i].text;
        if (emb->embed_many(texts, n, vecs, dims, emb->ctx) == 0) {
            for (int i = 0; i < n; i++) {
                if (dims[i] <= 0) { (*errors)++; continue; }
                crag_chunk_t chunk = {0};
                chunk.source = rows[i].source;
                chunk.text   = rows[i].text;
                chunk.dim    = dims[i];
                memcpy(chunk.vec, vecs + (size_t)i * CRAG_EMBED_DIM_MAX,
                       (size_t)dims[i] * sizeof(float));
                if (store_insert(store, &chunk) == 0) inserted++;
                else (*errors)++;
            }
        } else {
            *errors += n;
        }
        free(texts); free(vecs); free(dims);
    } else {
        for (int i = 0; i < n; i++) {
            crag_chunk_t chunk = {0};
            chunk.source = rows[i].source;
            chunk.text   = rows[i].text;
            if (emb->embed_one(rows[i].text, chunk.vec, &chunk.dim, emb->ctx) == 0
                && store_insert(store, &chunk) == 0) {
                inserted++;
            } else {
                (*errors)++;
            }
        }
    }
    return inserted;
}

int cmd_import(int argc, char **argv) {
    const char *in_path = NULL;
    const char *db_path = NULL;
    const char *backend = getenv("CRAG_EMBED_BACKEND") ? getenv("CRAG_EMBED_BACKEND") : "local";
    const char *url     = NULL;

    for (int i = 0; i < argc; i++) {
        if      (strcmp(argv[i], "--backend") == 0 && i+1 < argc) backend = argv[++i];
        else if ((strcmp(argv[i], "--db") == 0 || strcmp(argv[i], "-o") == 0) && i+1 < argc) db_path = argv[++i];
        else if (strcmp(argv[i], "--url") == 0 && i+1 < argc) url     = argv[++i];
        else if (argv[i][0] != '-') in_path = argv[i];
    }
    if (!in_path) { fprintf(stderr, "import: missing <file>\n"); return 2; }
    if (!db_path) db_path = getenv("CRAG_DB");
    if (!db_path) db_path = "./crag.db";

    FILE *fp = fopen(in_path, "r");
    if (!fp) { fprintf(stderr, "import: cannot open '%s'\n", in_path); return 3; }

    crag_embed_backend_t *emb = embed_backend_create(backend, url, NULL);
    if (!emb) { fclose(fp); return 1; }

    crag_store_t store;
    if (store_open(db_path, &store) != 0) {
        embed_backend_free(emb);
        fclose(fp);
        return 3;
    }

    store_begin(&store);

    row_t batch[IMPORT_BATCH];
    int n_batch = 0;
    int total_in = 0, total_inserted = 0, total_errors = 0;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, fp)) != -1) {
        if (len == 0 || line[0] == '\n' || line[0] == '\r') continue;
        total_in++;

        /* Allocate worst-case buffers (line length is the upper bound on each
           field after unescape, since unescape only ever shrinks the string). */
        char *src = malloc((size_t)len + 1);
        char *txt = malloc((size_t)len + 1);
        if (!src || !txt) { free(src); free(txt); total_errors++; continue; }

        if (json_get_string(line, "source", src, (size_t)len + 1) != 0 ||
            json_get_string(line, "text",   txt, (size_t)len + 1) != 0) {
            free(src); free(txt); total_errors++;
            fprintf(stderr, "import: skipping malformed line %d\n", total_in);
            continue;
        }

        batch[n_batch].source = src;
        batch[n_batch].text   = txt;
        n_batch++;

        if (n_batch == IMPORT_BATCH) {
            total_inserted += flush_batch(emb, &store, batch, n_batch, &total_errors);
            rows_free(batch, n_batch);
            n_batch = 0;
        }
    }
    if (n_batch > 0) {
        total_inserted += flush_batch(emb, &store, batch, n_batch, &total_errors);
        rows_free(batch, n_batch);
    }
    free(line);

    store_commit(&store);

    fprintf(stderr, "\n=== import summary ===\n");
    fprintf(stderr, "  lines read:      %d\n", total_in);
    fprintf(stderr, "  chunks inserted: %d\n", total_inserted);
    fprintf(stderr, "  errors:          %d\n", total_errors);
    fprintf(stderr, "  db:              %s\n", db_path);

    store_close(&store);
    embed_backend_free(emb);
    fclose(fp);

    if (total_inserted == 0 && total_errors > 0) return 4;
    return 0;
}

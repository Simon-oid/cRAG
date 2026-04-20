#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "crag.h"
#include "chunk.h"
#include "embed.h"
#include "store.h"

#define MAX_CHUNKS_PER_FILE 512

int cmd_index(int argc, char **argv) {
    const char *dir     = NULL;
    const char *backend = "openai";
    const char *db_path = NULL;
    const char *url     = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i+1 < argc) backend = argv[++i];
        else if (strcmp(argv[i], "--db") == 0 && i+1 < argc) db_path = argv[++i];
        else if (strcmp(argv[i], "--url") == 0 && i+1 < argc) url = argv[++i];
        else if (argv[i][0] != '-') dir = argv[i];
    }

    if (!dir) { fprintf(stderr, "index: missing <dir>\n"); return 1; }
    if (!db_path) db_path = getenv("CRAG_DB");
    if (!db_path) db_path = "./crag.db";

    crag_embed_backend_t *emb = embed_backend_create(backend, url, NULL);
    if (!emb) return 1;

    crag_store_t store;
    if (store_open(db_path, &store) != 0) { embed_backend_free(emb); return 1; }

    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "index: cannot open dir '%s'\n", dir);
        store_close(&store);
        embed_backend_free(emb);
        return 1;
    }

    char **texts = malloc(MAX_CHUNKS_PER_FILE * sizeof(char *));
    int total_chunks = 0, total_files = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || strcmp(ent->d_name + nlen - 4, ".txt") != 0) continue;

        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);

        int n = chunk_file(path, CHUNK_MAX_CHARS_DEFAULT, texts, MAX_CHUNKS_PER_FILE);
        if (n < 0) { fprintf(stderr, "index: failed to chunk '%s'\n", path); continue; }

        total_files++;
        for (int i = 0; i < n; i++) {
            crag_chunk_t chunk = {0};
            chunk.source = path;
            chunk.text   = texts[i];
            if (emb->embed_one(texts[i], chunk.vec, &chunk.dim, emb->ctx) == 0)
                store_insert(&store, &chunk);
            else
                fprintf(stderr, "index: embed failed for chunk %d of '%s'\n", i, path);
            free(texts[i]);
            total_chunks++;
        }
    }
    closedir(d);
    free(texts);

    fprintf(stderr, "indexed %d chunks from %d files into %s\n",
            total_chunks, total_files, db_path);

    store_close(&store);
    embed_backend_free(emb);
    return 0;
}

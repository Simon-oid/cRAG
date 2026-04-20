#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chunk.h"

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

int chunk_file(const char *path, int max_chars,
               char **texts_out, int max_chunks) {
    size_t len;
    char *buf = read_file(path, &len);
    if (!buf) return -1;

    int n = 0;
    char *p = buf;

    while (*p && n < max_chunks) {
        /* skip leading blank lines */
        while (*p == '\n' || *p == '\r') p++;
        if (!*p) break;

        /* find paragraph boundary (\n\n) */
        char *end = strstr(p, "\n\n");
        size_t para_len = end ? (size_t)(end - p) : strlen(p);

        /* split long paragraphs with stride */
        size_t off = 0;
        while (off < para_len && n < max_chunks) {
            size_t take = para_len - off;
            if ((int)take > max_chars) take = (size_t)max_chars;
            char *chunk = malloc(take + 1);
            if (!chunk) { free(buf); return n; }
            memcpy(chunk, p + off, take);
            chunk[take] = '\0';
            texts_out[n++] = chunk;
            off += take;
        }

        p = end ? end + 2 : p + para_len;
    }

    free(buf);
    return n;
}

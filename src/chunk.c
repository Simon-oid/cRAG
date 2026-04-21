#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "chunk.h"

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0 || sz > 16 * 1024 * 1024) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

static int is_md(const char *path) {
    size_t n = strlen(path);
    return n >= 3 && strcmp(path + n - 3, ".md") == 0;
}

/* Returns 1 if line starting at p (in buf) is an MD header and not inside a fence. */
static int is_md_header(const char *p) {
    if (*p != '#') return 0;
    const char *q = p;
    while (*q == '#') q++;
    return *q == ' ' || *q == '\t';
}

/* Scan fence boundaries into fence_starts/fence_ends arrays.
   Returns number of fences found (max max_fences). */
static int scan_fences(const char *buf, size_t len,
                       size_t *starts, size_t *ends, int max_fences) {
    int n = 0;
    const char *p = buf;
    const char *e = buf + len;
    int in_fence = 0;
    size_t fence_start = 0;

    while (p < e) {
        /* find start of line */
        if (p == buf || *(p-1) == '\n') {
            if (p + 3 <= e && p[0] == '`' && p[1] == '`' && p[2] == '`') {
                if (!in_fence) {
                    fence_start = (size_t)(p - buf);
                    in_fence = 1;
                } else {
                    if (n < max_fences) {
                        starts[n] = fence_start;
                        const char *nl = memchr(p, '\n', (size_t)(e - p));
                        ends[n] = nl ? (size_t)(nl - buf) + 1 : len;
                        n++;
                    }
                    in_fence = 0;
                }
            }
        }
        const char *nl = memchr(p, '\n', (size_t)(e - p));
        p = nl ? nl + 1 : e;
    }
    return n;
}

static int in_fence_range(size_t off, const size_t *starts, const size_t *ends, int nf) {
    for (int i = 0; i < nf; i++)
        if (off >= starts[i] && off < ends[i]) return 1;
    return 0;
}

/* Find the best split point before or at buf[max_off] (scanning backwards),
   preferring sentence endings then paragraph, then word. */
static size_t find_split(const char *buf, size_t start, size_t max_off) {
    if (max_off <= start) return max_off;
    /* scan backwards for sentence end */
    size_t i = max_off;
    while (i > start) {
        char c = buf[i];
        if ((c == '.' || c == '!' || c == '?') && buf[i+1] == ' ') return i + 2;
        if (c == '\n' && i > 0 && buf[i-1] == '\n') return i + 1;
        i--;
    }
    /* fallback: last space */
    i = max_off;
    while (i > start && buf[i] != ' ' && buf[i] != '\n') i--;
    return i > start ? i + 1 : max_off;
}

static int emit_chunk(const char *buf, size_t start, size_t end,
                      char **out, int max_n, int *n) {
    if (*n >= max_n) return 0;
    if (end <= start) return 0;
    /* trim leading/trailing whitespace */
    while (start < end && isspace((unsigned char)buf[start]))  start++;
    while (end > start && isspace((unsigned char)buf[end - 1])) end--;
    if (end <= start) return 0;
    size_t len = end - start;
    char *s = malloc(len + 1);
    if (!s) return -1;
    memcpy(s, buf + start, len);
    s[len] = '\0';
    out[(*n)++] = s;
    return 1;
}

/* Split section [sec_start, sec_end) into sliding-window chunks. */
static int sliding_split(const char *buf, size_t sec_start, size_t sec_end,
                         int max_chars, int overlap,
                         const size_t *fs, const size_t *fe, int nf,
                         char **out, int max_n, int *n) {
    size_t pos = sec_start;
    while (pos < sec_end && *n < max_n) {
        size_t take = (size_t)max_chars;
        if (pos + take > sec_end) take = sec_end - pos;

        /* if this window overlaps a fence, extend to include the full fence */
        for (int i = 0; i < nf; i++) {
            if (pos < fe[i] && pos + take > fs[i] && pos + take < fe[i])
                take = fe[i] - pos;
        }

        size_t split_at = pos + take < sec_end
            ? find_split(buf, pos, pos + take)
            : sec_end;

        if (emit_chunk(buf, pos, split_at, out, max_n, n) < 0) return -1;
        if (split_at <= pos) break;
        pos = split_at > (size_t)overlap ? split_at - (size_t)overlap : 0;
        if (pos < sec_start) pos = sec_start;
    }
    return 0;
}

int chunk_file(const char *path, int max_chars,
               char **texts_out, int max_chunks) {
    size_t len;
    char *buf = read_file(path, &len);
    if (!buf) return -1;

    int md = is_md(path);

    /* scan code fences */
    size_t fs[256], fe[256];
    int nf = scan_fences(buf, len, fs, fe, 256);

    int n = 0;

    if (!md) {
        /* plain text: paragraph split + sliding window */
        const char *p = buf;
        const char *end = buf + len;
        while (p < end && n < max_chunks) {
            while (p < end && (*p == '\n' || *p == '\r')) p++;
            if (p >= end) break;
            const char *para_end = strstr(p, "\n\n");
            size_t para_len = para_end ? (size_t)(para_end - p) : (size_t)(end - p);
            size_t sec_start = (size_t)(p - buf);
            size_t sec_end   = sec_start + para_len;
            if ((int)para_len <= max_chars) {
                emit_chunk(buf, sec_start, sec_end, texts_out, max_chunks, &n);
            } else {
                sliding_split(buf, sec_start, sec_end, max_chars,
                              CHUNK_OVERLAP_DEFAULT, fs, fe, nf,
                              texts_out, max_chunks, &n);
            }
            p = para_end ? para_end + 2 : end;
        }
    } else {
        /* markdown: primary split on header lines */
        const char *p = buf;
        const char *end = buf + len;
        size_t sec_start = 0;

        while (p <= end) {
            int is_header = (p < end) && !in_fence_range((size_t)(p - buf), fs, fe, nf)
                            && is_md_header(p);
            int is_end = (p == end);

            if ((is_header || is_end) && p > buf) {
                size_t sec_end = (size_t)(p - buf);
                if ((int)(sec_end - sec_start) <= max_chars) {
                    emit_chunk(buf, sec_start, sec_end, texts_out, max_chunks, &n);
                } else {
                    sliding_split(buf, sec_start, sec_end, max_chars,
                                  CHUNK_OVERLAP_DEFAULT, fs, fe, nf,
                                  texts_out, max_chunks, &n);
                }
                sec_start = (size_t)(p - buf);
            }

            if (is_end) break;
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            p = nl ? nl + 1 : end;
        }
    }

    free(buf);
    return n;
}

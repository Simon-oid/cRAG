#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

/* Length (1-4) of the well-formed UTF-8 sequence starting at p, or 0 if the
   lead/continuation bytes are malformed. Safe on NUL-terminated input: a '\0'
   reads as a non-continuation byte and fails validation before any over-read,
   because the continuation checks short-circuit left to right. */
static int utf8_seq_len(const unsigned char *p) {
    unsigned char c = p[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) {                       /* 110xxxxx, 2 bytes */
        if (c < 0xC2) return 0;                     /* overlong */
        return (p[1] & 0xC0) == 0x80 ? 2 : 0;
    }
    if ((c & 0xF0) == 0xE0) {                       /* 1110xxxx, 3 bytes */
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && p[1] < 0xA0) return 0;     /* overlong */
        if (c == 0xED && p[1] >= 0xA0) return 0;    /* UTF-16 surrogate */
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {                       /* 11110xxx, 4 bytes */
        if (c > 0xF4) return 0;
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
            (p[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && p[1] < 0x90) return 0;     /* overlong */
        if (c == 0xF4 && p[1] >= 0x90) return 0;    /* > U+10FFFF */
        return 4;
    }
    return 0;                                        /* stray continuation / 0xF5+ */
}

int json_escape(const char *in, char *out, size_t out_sz) {
    if (!in || !out || out_sz == 0) return -1;
    size_t wi = 0;
#define NEED(n) do { if (wi + (n) + 1 > out_sz) return -1; } while(0)
    for (const char *p = in; *p; ) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  { NEED(2); out[wi++] = '\\'; out[wi++] = '"';  p++; }
        else if (c == '\\') { NEED(2); out[wi++] = '\\'; out[wi++] = '\\'; p++; }
        else if (c == '\n') { NEED(2); out[wi++] = '\\'; out[wi++] = 'n';  p++; }
        else if (c == '\r') { NEED(2); out[wi++] = '\\'; out[wi++] = 'r';  p++; }
        else if (c == '\t') { NEED(2); out[wi++] = '\\'; out[wi++] = 't';  p++; }
        else if (c < 0x20) {
            NEED(6);
            snprintf(out + wi, 7, "\\u%04x", c);
            wi += 6; p++;
        } else if (c < 0x80) {
            NEED(1); out[wi++] = (char)c; p++;
        } else {
            int len = utf8_seq_len((const unsigned char *)p);
            if (len == 0) {                          /* replace bad byte w/ U+FFFD */
                NEED(3);
                out[wi++] = (char)0xEF; out[wi++] = (char)0xBF; out[wi++] = (char)0xBD;
                p++;
            } else {
                NEED((size_t)len);
                for (int i = 0; i < len; i++) out[wi++] = p[i];
                p += len;
            }
        }
    }
#undef NEED
    out[wi] = '\0';
    return (int)wi;
}

char *json_escape_dup(const char *in) {
    if (!in) return NULL;
    size_t n = strlen(in);
    size_t out_sz = n * 6 + 1;
    char *out = malloc(out_sz);
    if (!out) return NULL;
    if (json_escape(in, out, out_sz) < 0) { free(out); return NULL; }
    return out;
}

int json_extract_floats(const char *json, const char *key,
                        float *out, size_t max_n, size_t *out_n) {
    if (!json || !key || !out || !out_n) return -1;
    *out_n = 0;
    const char *p = strstr(json, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') p++;
    if (*p != '[') return -1;
    p++;

    size_t n = 0;
    while (*p && *p != ']' && n < max_n) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;
        if (*p == '[') { p++; continue; }
        char *end;
        float val = strtof(p, &end);
        if (end == p) break;
        out[n++] = val;
        p = end;
    }
    *out_n = n;
    return n > 0 ? 0 : -1;
}

/* Case-insensitive "does haystack contain needle?" — needle is ASCII (model
   name fragments), so a byte-wise lower-fold is enough. */
static int contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return 0;
    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen && h[i]) {
            unsigned char a = (unsigned char)h[i], b = (unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

/* Measured per-embedder generation-gate defaults. Only models whose cosine
   distribution has actually been measured belong here — a guessed value is
   worse than honestly returning the caller's fallback. */
float gen_min_score_resolve(const char *env_override, const char *embed_model,
                            float fallback) {
    /* 1. Explicit operator override wins, if it parses cleanly. */
    if (env_override && *env_override) {
        char *end = NULL;
        float v = strtof(env_override, &end);
        if (end != env_override && *end == '\0') return v;
        /* malformed: ignore and fall through (atof would've returned 0.0,
           silently disabling the gate — worse than the model default). */
    }
    /* 2. Measured per-model default. bge-m3's distribution sits ~0.2 below
       mxbai's, so the fallback (mxbai-era 0.60) would refuse genuine answers. */
    if (contains_ci(embed_model, "bge-m3")) return 0.38f;
    /* 3. mxbai and unknown models keep the conservative fallback. */
    return fallback;
}

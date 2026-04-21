#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

int json_escape(const char *in, char *out, size_t out_sz) {
    if (!in || !out || out_sz == 0) return -1;
    size_t wi = 0;
#define NEED(n) do { if (wi + (n) + 1 > out_sz) return -1; } while(0)
    for (const char *p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  { NEED(2); out[wi++] = '\\'; out[wi++] = '"';  }
        else if (c == '\\') { NEED(2); out[wi++] = '\\'; out[wi++] = '\\'; }
        else if (c == '\n') { NEED(2); out[wi++] = '\\'; out[wi++] = 'n';  }
        else if (c == '\r') { NEED(2); out[wi++] = '\\'; out[wi++] = 'r';  }
        else if (c == '\t') { NEED(2); out[wi++] = '\\'; out[wi++] = 't';  }
        else if (c < 0x20) {
            NEED(6);
            snprintf(out + wi, 7, "\\u%04x", c);
            wi += 6;
        } else {
            NEED(1); out[wi++] = (char)c;
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
        char *end;
        out[n++] = strtof(p, &end);
        if (end == p) break;
        p = end;
    }
    *out_n = n;
    return n > 0 ? 0 : -1;
}

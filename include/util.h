#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/* JSON-escape 'in' into caller-allocated 'out' (size out_sz).
   Escapes: \  "  \n  \r  \t  and control chars.
   Returns bytes written (not counting NUL), or -1 if out_sz too small. */
int json_escape(const char *in, char *out, size_t out_sz);

/* Returns a malloc'd JSON-escaped copy of 'in', or NULL on OOM. Caller frees. */
char *json_escape_dup(const char *in);

/* Find the first JSON array after 'key' (e.g. "\"embedding\":") in 'json',
   parse it as floats into out[0..max_n-1].  Sets *out_n to actual count.
   Returns 0 on success, -1 if key or array not found. */
int json_extract_floats(const char *json, const char *key,
                        float *out, size_t max_n, size_t *out_n);

#endif

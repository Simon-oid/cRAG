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

/* Resolve the generation confidence gate: the minimum top-1 cosine required
   before cRAG spends an LLM call (below it, query refuses with "Not in
   context."). The genuine-vs-irrelevant cosine boundary is a property of the
   EMBEDDING MODEL — bge-m3's whole distribution sits ~0.2 below mxbai's — so
   one fixed default silently refuses genuine answers on a low-floor model.
   Measured on the Italian fixture: bge-m3 genuine 0.47-0.77 / irrelevant
   0.20-0.34 -> 0.38; mxbai genuine 0.62-0.72 -> the 0.60 fallback fits.

   Precedence: 'env_override' (CRAG_GEN_MIN_SCORE) if it parses as a float
   with no trailing junk; else a measured per-model default keyed on
   'embed_model' (case-insensitive substring; bge-m3 -> 0.38); else
   'fallback'. Either string may be NULL/empty. Pure: no I/O, no globals. */
float gen_min_score_resolve(const char *env_override, const char *embed_model,
                            float fallback);

#endif

#ifndef SEARCH_H
#define SEARCH_H

/* Cosine similarity of two vectors of length dim.
   Returns a value in [-1, 1] (or close to it after the epsilon guard). */
float search_cosine(const float *a, const float *b, int dim);

/* The two sums a scan needs per stored vector v: q.v and v.v. The query's
   own norm is computed once per search with the same function, so
   dot / (sqrtf(q.q) * sqrtf(v.v) + 1e-9f) equals search_cosine(q, v) to the
   bit. */
void search_dot_norm(const float *q, const float *v, int dim, float *dot, float *norm2);

/* q.v alone, summed in the same lane order as search_dot_norm's dot. */
float search_dot(const float *q, const float *v, int dim);

#endif

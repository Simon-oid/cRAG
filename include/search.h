#ifndef SEARCH_H
#define SEARCH_H

/* Cosine similarity of two vectors of length dim.
   Returns a value in [-1, 1] (or close to it after the epsilon guard). */
float search_cosine(const float *a, const float *b, int dim);

#endif

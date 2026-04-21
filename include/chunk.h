#ifndef CHUNK_H
#define CHUNK_H

#define CHUNK_MAX_CHARS_DEFAULT 2048
#define CHUNK_OVERLAP_DEFAULT   200

/* Reads file at path, splits into chunks of at most max_chars.
   Fills chunks[] (caller must free each .text and .source).
   Returns number of chunks, or -1 on error. */
int chunk_file(const char *path, int max_chars,
               char **texts_out, int max_chunks);

#endif

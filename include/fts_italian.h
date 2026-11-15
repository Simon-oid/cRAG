#ifndef FTS_ITALIAN_H
#define FTS_ITALIAN_H

#include <sqlite3.h>

/* Registers the "italian" FTS5 tokenizer (unicode61 + a light Italian
   stemmer) on this connection. 0 on success. */
int fts_italian_register(sqlite3 *db);

/* The stemmer alone, in place on a lowercase ASCII token; for tests. */
int fts_italian_stem_for_test(char *s, int len);

#endif

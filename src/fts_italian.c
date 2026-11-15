/* FTS5 tokenizer "italian": unicode61 (diacritics removed), then a light
 * Italian stemmer on each token.
 *
 * The stemmer is Savoy's light stemmer for Italian, as in Lucene's
 * ItalianLightStemmer: it strips the gender and number endings only
 * (-o -a -e -i, and -ie -he -hi -ii -ia -io), on words of six letters or
 * more. "tumore" and "tumori" both become "tumor"; "casa" (4 letters) is
 * left alone. It does not touch derivational suffixes (-zione, -mente),
 * which a full Snowball stemmer would, at the cost of more collisions.
 *
 * Registered on every connection by store_open (fts_italian_register).
 * An index built with it can only be queried by a connection that has it:
 * the stock sqlite3 shell cannot read chunks_fts of such a database.
 */
#include <string.h>
#include <sqlite3.h>

#include "fts_italian.h"

#define MAX_TOKEN 256
#define MIN_STEM_LEN 6

/* In place on ASCII lowercase (unicode61 has folded case and diacritics).
   Returns the new length. */
static int italian_light_stem(char *s, int len) {
    if (len < MIN_STEM_LEN) return len;
    char last = s[len - 1], prev = s[len - 2];
    switch (last) {
    case 'e': return (prev == 'i' || prev == 'h') ? len - 2 : len - 1;
    case 'i': return (prev == 'h' || prev == 'i') ? len - 2 : len - 1;
    case 'a': return prev == 'i' ? len - 2 : len - 1;
    case 'o': return prev == 'i' ? len - 2 : len - 1;
    default:  return len;
    }
}

typedef struct {
    fts5_tokenizer base;      /* unicode61's methods */
    Fts5Tokenizer *inner;     /* unicode61 instance */
} italian_t;

typedef struct {
    void *ctx;
    int (*x_token)(void *, int, const char *, int, int, int);
} stem_ctx_t;

static int stem_cb(void *p, int flags, const char *tok, int n, int start, int end) {
    stem_ctx_t *c = p;
    if (n <= 0 || n > MAX_TOKEN) return c->x_token(c->ctx, flags, tok, n, start, end);
    char buf[MAX_TOKEN];
    memcpy(buf, tok, (size_t)n);
    for (int i = 0; i < n; i++)
        if ((unsigned char)buf[i] >= 0x80)          /* non-ASCII left: do not stem */
            return c->x_token(c->ctx, flags, tok, n, start, end);
    return c->x_token(c->ctx, flags, buf, italian_light_stem(buf, n), start, end);
}

static fts5_api *get_fts5_api(sqlite3 *db) {
    fts5_api *api = NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT fts5(?1)", -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_pointer(st, 1, (void *)&api, "fts5_api_ptr", NULL);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return api;
}

static int italian_create(void *user, const char **argv, int argc, Fts5Tokenizer **out) {
    fts5_api *api = user;
    italian_t *t = sqlite3_malloc(sizeof *t);
    if (!t) return SQLITE_NOMEM;
    void *uctx = NULL;
    int rc = api->xFindTokenizer(api, "unicode61", &uctx, &t->base);
    if (rc == SQLITE_OK) rc = t->base.xCreate(uctx, argv, argc, &t->inner);
    if (rc != SQLITE_OK) { sqlite3_free(t); return rc; }
    *out = (Fts5Tokenizer *)t;
    return SQLITE_OK;
}

static void italian_delete(Fts5Tokenizer *p) {
    italian_t *t = (italian_t *)p;
    t->base.xDelete(t->inner);
    sqlite3_free(t);
}

static int italian_tokenize(Fts5Tokenizer *p, void *ctx, int flags, const char *text, int n,
                            int (*x_token)(void *, int, const char *, int, int, int)) {
    italian_t *t = (italian_t *)p;
    stem_ctx_t c = { ctx, x_token };
    return t->base.xTokenize(t->inner, &c, flags, text, n, stem_cb);
}

int fts_italian_register(sqlite3 *db) {
    fts5_api *api = get_fts5_api(db);
    if (!api) return -1;
    fts5_tokenizer tok = { italian_create, italian_delete, italian_tokenize };
    return api->xCreateTokenizer(api, "italian", api, &tok, NULL) == SQLITE_OK ? 0 : -1;
}

int fts_italian_stem_for_test(char *s, int len) { return italian_light_stem(s, len); }

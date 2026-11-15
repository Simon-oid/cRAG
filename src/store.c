#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "store.h"
#include "search.h"
#include "fts_italian.h"
#include "crag.h"

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS chunks ("
    "  id     INTEGER PRIMARY KEY,"
    "  source TEXT NOT NULL,"
    "  text   TEXT NOT NULL,"
    "  dim    INTEGER NOT NULL,"
    "  vec    BLOB NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_source ON chunks(source);"
    "CREATE TABLE IF NOT EXISTS files ("
    "  path       TEXT PRIMARY KEY,"
    "  mtime      INTEGER NOT NULL,"
    "  size       INTEGER NOT NULL,"
    "  indexed_at INTEGER NOT NULL"
    ");"
    /* FTS5 index over chunks.text for BM25/hybrid retrieval. External-content
       mode keeps chunks.text as the source of truth and chunks_fts as a
       read-only index; triggers below keep them in sync on insert/delete. */
    "CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5("
    "  text, content='chunks', content_rowid='id'"
    ");"
    "CREATE TRIGGER IF NOT EXISTS chunks_ai AFTER INSERT ON chunks BEGIN"
    "  INSERT INTO chunks_fts(rowid, text) VALUES (new.id, new.text);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS chunks_ad AFTER DELETE ON chunks BEGIN"
    "  INSERT INTO chunks_fts(chunks_fts, rowid, text) VALUES('delete', old.id, old.text);"
    "END;";

/* CRAG_FTS_TOKENIZER picks the FTS5 tokenizer of chunks_fts. Unset: the
   database keeps the one it has (unicode61 for a new one). Set to a
   different one: the index is rebuilt from chunks.text, without
   re-embedding anything. */
static const struct { const char *name, *spec; } FTS_TOKENIZERS[] = {
    { "unicode61", "unicode61" },          /* no stemming: tumore != tumori */
    { "porter",    "porter unicode61" },   /* English stemming */
    { "trigram",   "trigram" },            /* substrings; CJK, code */
    { "italian",   "italian" },            /* unicode61 + light Italian stemmer */
};

static const char *fts_spec_for(const char *name) {
    for (size_t i = 0; i < sizeof FTS_TOKENIZERS / sizeof FTS_TOKENIZERS[0]; i++)
        if (strcmp(name, FTS_TOKENIZERS[i].name) == 0) return FTS_TOKENIZERS[i].spec;
    return NULL;
}

/* The tokenize= argument chunks_fts was created with ("unicode61" when it
   has none, FTS5's default). Caller frees. */
static char *fts_current_spec(sqlite3 *db) {
    sqlite3_stmt *st;
    char *out = NULL;
    if (sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE name='chunks_fts'",
                           -1, &st, NULL) != SQLITE_OK) return NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *sql = (const char *)sqlite3_column_text(st, 0);
        const char *t = sql ? strstr(sql, "tokenize='") : NULL;
        if (t) {
            t += strlen("tokenize='");
            const char *e = strchr(t, '\'');
            if (e) out = strndup(t, (size_t)(e - t));
        } else {
            out = strdup("unicode61");
        }
    }
    sqlite3_finalize(st);
    return out;
}

static int fts_apply_tokenizer(sqlite3 *db) {
    const char *name = getenv("CRAG_FTS_TOKENIZER");
    if (!name || !*name) return 0;
    const char *spec = fts_spec_for(name);
    if (!spec) {
        fprintf(stderr, "store: unknown CRAG_FTS_TOKENIZER '%s' (unicode61, porter, trigram, italian)\n",
                name);
        return -1;
    }
    char *cur = fts_current_spec(db);
    int same = cur && strcmp(cur, spec) == 0;
    free(cur);
    if (same) return 0;
    char sql[512];
    snprintf(sql, sizeof sql,
             "DROP TABLE IF EXISTS chunks_fts;"
             "CREATE VIRTUAL TABLE chunks_fts USING fts5("
             "  text, content='chunks', content_rowid='id', tokenize='%s');"
             "INSERT INTO chunks_fts(chunks_fts) VALUES('rebuild');", spec);
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "store: FTS tokenizer %s: %s\n", name, err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int store_open(const char *path, crag_store_t *s) {
    s->db = NULL;
    memset(&s->cache, 0, sizeof s->cache);
    if (sqlite3_open(path, &s->db) != SQLITE_OK) {
        fprintf(stderr, "store: %s\n", sqlite3_errmsg(s->db));
        sqlite3_close(s->db);
        s->db = NULL;
        return -1;
    }
    char *err = NULL;
    if (sqlite3_exec(s->db, "PRAGMA journal_mode=WAL;", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "store: WAL: %s\n", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(s->db);
        s->db = NULL;
        return -1;
    }
    /* Before the schema and the tokenizer check: a database indexed with
       "italian" cannot even be read without it. */
    if (fts_italian_register(s->db) != 0) {
        fprintf(stderr, "store: cannot register the italian FTS5 tokenizer\n");
        sqlite3_close(s->db);
        s->db = NULL;
        return -1;
    }
    if (sqlite3_exec(s->db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "store: schema: %s\n", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(s->db);
        s->db = NULL;
        return -1;
    }
    if (fts_apply_tokenizer(s->db) != 0) {
        sqlite3_close(s->db);
        s->db = NULL;
        return -1;
    }
    /* Migration for DBs created before FTS5 was enabled: if chunks has rows
       but chunks_fts is empty, rebuild the FTS index. This is a one-time
       cost on upgrade; the ai/ad triggers keep it fresh from here on. */
    sqlite3_stmt *st;
    long long nchunks = 0, nfts = 0;
    if (sqlite3_prepare_v2(s->db, "SELECT COUNT(*) FROM chunks", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) nchunks = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(s->db, "SELECT COUNT(*) FROM chunks_fts", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) nfts = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (nchunks > 0 && nfts == 0) {
        fprintf(stderr, "store: rebuilding FTS5 index for %lld existing chunks...\n", nchunks);
        const char *rebuild =
            "INSERT INTO chunks_fts(chunks_fts) VALUES('rebuild');";
        if (sqlite3_exec(s->db, rebuild, NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "store: FTS rebuild: %s\n", err ? err : "?");
            sqlite3_free(err);
            /* Non-fatal: cosine search still works. Hybrid will simply miss
               lexical hits until the next index --force. */
        }
    }
    return 0;
}

int store_begin(crag_store_t *s) {
    char *err = NULL;
    if (sqlite3_exec(s->db, "BEGIN", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "store: begin: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int store_commit(crag_store_t *s) {
    char *err = NULL;
    if (sqlite3_exec(s->db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "store: commit: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int store_insert(crag_store_t *s, const crag_chunk_t *chunk) {
    sqlite3_stmt *stmt;
    const char *sql =
        "INSERT INTO chunks(source,text,dim,vec) VALUES(?,?,?,?)";
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, chunk->source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, chunk->text,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 3, chunk->dim);
    sqlite3_bind_blob(stmt, 4, chunk->vec, chunk->dim * (int)sizeof(float), SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Fill results[i] (source, text, vector) for a chunk id already ranked. */
static int load_chunk(crag_store_t *s, int id, crag_chunk_t *out) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(s->db, "SELECT source,text,dim,vec FROM chunks WHERE id=?",
                           -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, id);
    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int dim = sqlite3_column_int(stmt, 2);
        const void *vec = sqlite3_column_blob(stmt, 3);
        char *src = strdup((const char *)sqlite3_column_text(stmt, 0));
        char *txt = strdup((const char *)sqlite3_column_text(stmt, 1));
        if (src && txt && vec && dim > 0 && dim <= CRAG_EMBED_DIM_MAX) {
            out->id = id;
            out->source = src;
            out->text = txt;
            out->dim = dim;
            memcpy(out->vec, vec, (size_t)dim * sizeof(float));
            rc = 0;
        } else {
            free(src);
            free(txt);
        }
    }
    sqlite3_finalize(stmt);
    return rc;
}

#define VEC_CACHE_DEFAULT_MB 2048

static void cache_free(crag_vec_cache_t *c) {
    free(c->ids);
    free(c->vecs);
    free(c->norms);
    memset(c, 0, sizeof *c);
}


static long long data_version(sqlite3 *db) {
    sqlite3_stmt *stmt;
    long long v = -1;
    if (sqlite3_prepare_v2(db, "PRAGMA data_version", -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

static size_t cache_cap_bytes(void) {
    const char *e = getenv("CRAG_VEC_CACHE_MB");
    long mb = e && *e ? strtol(e, NULL, 10) : VEC_CACHE_DEFAULT_MB;
    return mb > 0 ? (size_t)mb << 20 : 0;
}

/* 0: the cache holds every vector of `dim`; -1: no cache (too big, disabled,
   out of memory, or a read error), and the caller streams from SQLite. */
static int cache_ensure(crag_store_t *s, int dim) {
    crag_vec_cache_t *c = &s->cache;
    long long dv = data_version(s->db);
    long long tc = sqlite3_total_changes64(s->db);
    if (c->valid && c->dim == dim && c->data_version == dv && c->total_changes == tc) return 0;
    cache_free(c);
    size_t cap = cache_cap_bytes();
    if (cap == 0 || dv < 0) return -1;

    sqlite3_stmt *stmt;
    long long rows = 0;
    if (sqlite3_prepare_v2(s->db, "SELECT COUNT(*) FROM chunks WHERE dim=?", -1, &stmt, NULL)
            != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, dim);
    if (sqlite3_step(stmt) == SQLITE_ROW) rows = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    size_t bytes = (size_t)rows * ((size_t)dim * sizeof(float) + sizeof(float) + sizeof(int));
    if (rows <= 0 || bytes > cap) return -1;

    c->ids = malloc((size_t)rows * sizeof *c->ids);
    c->vecs = malloc((size_t)rows * (size_t)dim * sizeof *c->vecs);
    c->norms = malloc((size_t)rows * sizeof *c->norms);
    if (!c->ids || !c->vecs || !c->norms) { cache_free(c); return -1; }
    if (sqlite3_prepare_v2(s->db, "SELECT id,vec FROM chunks WHERE dim=?", -1, &stmt, NULL)
            != SQLITE_OK) { cache_free(c); return -1; }
    sqlite3_bind_int(stmt, 1, dim);
    int n = 0;
    while (n < rows && sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_bytes(stmt, 1) != dim * (int)sizeof(float)) continue;
        const float *v = sqlite3_column_blob(stmt, 1);
        if (!v) continue;
        float *dst = c->vecs + (size_t)n * dim;
        memcpy(dst, v, (size_t)dim * sizeof(float));
        float vv, norm2;
        search_dot_norm(dst, dst, dim, &vv, &norm2);
        c->norms[n] = sqrtf(norm2);
        c->ids[n] = sqlite3_column_int(stmt, 0);
        n++;
    }
    sqlite3_finalize(stmt);
    c->n = n;
    c->dim = dim;
    c->data_version = dv;
    c->total_changes = tc;
    c->valid = 1;
    return 0;
}

/* Enter (score, id) into the running top k (scores descending). Ties keep
   the earlier row ahead: a row enters only with a strictly higher score. */
static void topk_offer(float score, int id, int k, int *n, float *scores, int *ids) {
    if (*n == k && !(score > scores[k - 1])) return;
    int i = *n < k ? (*n)++ : k - 1;
    for (; i > 0 && score > scores[i - 1]; i--) {
        scores[i] = scores[i - 1];
        ids[i] = ids[i - 1];
    }
    scores[i] = score;
    ids[i] = id;
}

/* The scan keeps only (score, id) for the running top k and loads text and
   vector for the winners afterwards: before, every row entering the top k
   was strdup'd, and the insertion sort swapped whole crag_chunk_t values,
   16 KB each with their inline vector. It reads the vector cache when there
   is one, SQLite rows otherwise; both give the same scores to the bit. */
int store_search(crag_store_t *s, const float *qvec, int dim, int k,
                 crag_chunk_t *results, float *scores) {
    if (dim <= 0 || dim > CRAG_EMBED_DIM_MAX || k <= 0) return -1;
    int *ids = malloc((size_t)k * sizeof *ids);
    if (!ids) return -1;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(s->db, "SELECT id,dim,vec FROM chunks", -1, &stmt, NULL)
            != SQLITE_OK) { free(ids); return -1; }

    float qq, qnorm2;
    search_dot_norm(qvec, qvec, dim, &qq, &qnorm2);
    const float qnorm = sqrtf(qnorm2);
    int n = 0;
    if (cache_ensure(s, dim) == 0) {
        const crag_vec_cache_t *c = &s->cache;
        for (int r = 0; r < c->n; r++) {
            float dot = search_dot(qvec, c->vecs + (size_t)r * dim, dim);
            topk_offer(dot / (qnorm * c->norms[r] + 1e-9f), c->ids[r], k, &n, scores, ids);
        }
    } else {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (sqlite3_column_int(stmt, 1) != dim) continue;
            if (sqlite3_column_bytes(stmt, 2) != dim * (int)sizeof(float)) continue;
            const float *rvec = sqlite3_column_blob(stmt, 2);
            if (!rvec) continue;
            float dot, rnorm2;
            search_dot_norm(qvec, rvec, dim, &dot, &rnorm2);
            topk_offer(dot / (qnorm * sqrtf(rnorm2) + 1e-9f), sqlite3_column_int(stmt, 0),
                       k, &n, scores, ids);
        }
    }
    sqlite3_finalize(stmt);

    int out = 0;
    for (int i = 0; i < n; i++) {
        if (load_chunk(s, ids[i], &results[out]) != 0) continue;   /* deleted mid-search */
        scores[out++] = scores[i];
    }
    free(ids);
    return out;
}

/* Build an FTS5 MATCH expression from a raw user query. Strategy: walk the
   string as UTF-8 bytes, treat alphanumerics, byte >= 0x80 (UTF-8 non-ASCII),
   and `- _ .` as token bytes; everything else is a separator. Each token is
   wrapped in double quotes (with embedded `"` doubled per FTS5 rules) and
   tokens are joined with " OR ". This sidesteps FTS5 reserved words and
   special chars without losing Unicode letters. Returns a freshly malloc'd
   string (caller frees) or NULL if no usable tokens were found. */
#define FTS_MAX_TOKENS 32
#define FTS_MIN_TOKEN_LEN 2

static int is_token_byte(unsigned char c) {
    if (c >= 0x80) return 1;                     /* UTF-8 non-ASCII */
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c == '-' || c == '_' || c == '.') return 1;
    return 0;
}

static char *fts_match_from_query(const char *qtext) {
    if (!qtext) return NULL;
    size_t qlen = strlen(qtext);
    /* Worst case per token in output: original bytes + `""` + ` OR `. */
    size_t cap = qlen * 2 + 64;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t wp = 0;
    int tokens = 0;
    const unsigned char *p = (const unsigned char *)qtext;
    const unsigned char *end = p + qlen;
    while (p < end && tokens < FTS_MAX_TOKENS) {
        while (p < end && !is_token_byte(*p)) p++;
        const unsigned char *start = p;
        while (p < end && is_token_byte(*p)) p++;
        size_t tlen = (size_t)(p - start);
        if (tlen < FTS_MIN_TOKEN_LEN) continue;
        if (tokens > 0) { memcpy(out + wp, " OR ", 4); wp += 4; }
        out[wp++] = '"';
        for (size_t i = 0; i < tlen; i++) {
            unsigned char c = start[i];
            if (c == '"') { out[wp++] = '"'; out[wp++] = '"'; }
            else          { out[wp++] = (char)c; }
        }
        out[wp++] = '"';
        tokens++;
    }
    if (tokens == 0) { free(out); return NULL; }
    out[wp] = '\0';
    return out;
}

int store_search_bm25(crag_store_t *s, const char *qtext, int k,
                      crag_chunk_t *results, float *scores) {
    if (k <= 0) return 0;
    char *match = fts_match_from_query(qtext);
    if (!match) return 0;  /* no usable tokens → no BM25 hits */

    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT c.id, c.source, c.text, c.dim, c.vec, bm25(chunks_fts) "
        "FROM chunks_fts JOIN chunks c ON c.id = chunks_fts.rowid "
        "WHERE chunks_fts MATCH ? "
        "ORDER BY bm25(chunks_fts) LIMIT ?";
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(match); return -1;
    }
    sqlite3_bind_text(stmt, 1, match, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 2, k);

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < k) {
        int    rdim = sqlite3_column_int(stmt, 3);
        int    vbytes = sqlite3_column_bytes(stmt, 4);
        const float *rvec = sqlite3_column_blob(stmt, 4);
        if (rdim <= 0 || rdim > CRAG_EMBED_DIM_MAX) continue;
        if (vbytes != rdim * (int)sizeof(float) || !rvec) continue;
        char *src = strdup((const char *)sqlite3_column_text(stmt, 1));
        char *txt = strdup((const char *)sqlite3_column_text(stmt, 2));
        if (!src || !txt) { free(src); free(txt); continue; }
        results[n].id     = sqlite3_column_int(stmt, 0);
        results[n].source = src;
        results[n].text   = txt;
        results[n].dim    = rdim;
        memcpy(results[n].vec, rvec, (size_t)rdim * sizeof(float));
        /* SQLite's bm25() returns more-negative = better-match; negate so
           higher = better, matching cosine's convention. */
        scores[n] = -(float)sqlite3_column_double(stmt, 5);
        n++;
    }
    sqlite3_finalize(stmt);
    free(match);
    return n;
}

char *store_highlight_for_id(crag_store_t *s, const char *qtext, int id,
                             const char *open, const char *close) {
    if (!s || !qtext || !open || !close) return NULL;
    char *match = fts_match_from_query(qtext);
    if (!match) return NULL;

    /* sqlite3_mprintf %Q safely quotes/escapes the marker strings into the
       SQL text. The MATCH expression and id are bound as parameters. */
    char *sql = sqlite3_mprintf(
        "SELECT highlight(chunks_fts, 0, %Q, %Q) "
        "FROM chunks_fts WHERE chunks_fts MATCH ? AND rowid = ?",
        open, close);
    if (!sql) { free(match); return NULL; }

    sqlite3_stmt *stmt;
    char *result = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, match, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 2, id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            if (t) result = strdup(t);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_free(sql);
    free(match);
    return result;
}

#define RRF_K 60
#define HYBRID_POOL_MIN 20

int store_search_hybrid(crag_store_t *s,
                        const float *qvec, int dim,
                        const char *qtext, int k,
                        crag_chunk_t *results, crag_score_t *scores) {
    if (k <= 0) return 0;
    int pool = k * 4;
    if (pool < HYBRID_POOL_MIN) pool = HYBRID_POOL_MIN;

    crag_chunk_t *cos_res  = calloc((size_t)pool, sizeof(crag_chunk_t));
    float        *cos_sc   = calloc((size_t)pool, sizeof(float));
    crag_chunk_t *bm25_res = calloc((size_t)pool, sizeof(crag_chunk_t));
    float        *bm25_sc  = calloc((size_t)pool, sizeof(float));
    if (!cos_res || !cos_sc || !bm25_res || !bm25_sc) {
        free(cos_res); free(cos_sc); free(bm25_res); free(bm25_sc);
        return -1;
    }
    int n_cos  = store_search(s, qvec, dim, pool, cos_res, cos_sc);
    int n_bm25 = store_search_bm25(s, qtext, pool, bm25_res, bm25_sc);
    if (n_cos  < 0) n_cos  = 0;
    if (n_bm25 < 0) n_bm25 = 0;

    int out_n = 0;
    int   *ids = NULL;
    float *rrf = NULL;

    int total = n_cos + n_bm25;
    if (total == 0) goto cleanup;

    /* RRF fusion. Build a unique-id table scoring each doc by
       sum(1/(RRF_K + rank_i)) over retrievers that returned it. Pool is
       small (<=2 * pool), so O(total^2) linear dedupe is fine. */
    ids = calloc((size_t)total, sizeof(int));
    rrf = calloc((size_t)total, sizeof(float));
    if (!ids || !rrf) { out_n = -1; goto cleanup; }

    int nu = 0;
    #define RRF_ADD(id_, rank_) do {                                   \
        float add = 1.0f / (float)(RRF_K + (rank_));                   \
        int found = -1;                                                \
        for (int j = 0; j < nu; j++) if (ids[j] == (id_)) { found = j; break; } \
        if (found < 0) { ids[nu] = (id_); rrf[nu] = add; nu++; }       \
        else           { rrf[found] += add; }                          \
    } while (0)

    for (int i = 0; i < n_cos;  i++) RRF_ADD(cos_res[i].id,  i + 1);
    for (int i = 0; i < n_bm25; i++) RRF_ADD(bm25_res[i].id, i + 1);
    #undef RRF_ADD

    /* Selection sort (top-k of nu). Transfer chunk ownership from whichever
       retriever's pool already holds it. */
    out_n = nu < k ? nu : k;
    for (int slot = 0; slot < out_n; slot++) {
        int best = slot;
        for (int j = slot + 1; j < nu; j++) if (rrf[j] > rrf[best]) best = j;
        if (best != slot) {
            float ts = rrf[best]; rrf[best] = rrf[slot]; rrf[slot] = ts;
            int   ti = ids[best]; ids[best] = ids[slot]; ids[slot] = ti;
        }
        int id = ids[slot];
        crag_chunk_t *src = NULL;
        /* Recover the raw per-retriever scores for this doc. cosine defaults
           to NA and bm25 to 0 (no lexical hit) until found in a pool. */
        float cos_s = CRAG_SCORE_NA, bm25_s = 0.0f;
        int have_cos = 0;
        for (int j = 0; j < n_cos; j++) if (cos_res[j].id == id) {
            src = &cos_res[j]; cos_s = cos_sc[j]; have_cos = 1; break;
        }
        for (int j = 0; j < n_bm25; j++) if (bm25_res[j].id == id) {
            if (!src) src = &bm25_res[j];
            bm25_s = bm25_sc[j];
            break;
        }
        if (!src) { out_n = slot; break; }
        /* A doc found only via BM25 was never cosine-scored in the pool.
           Score it now from its stored vector so every result carries a
           real cosine relevance signal, not just the fusion number. */
        if (!have_cos && qvec && dim > 0 && dim <= CRAG_EMBED_DIM_MAX)
            cos_s = search_cosine(qvec, src->vec, dim);
        results[slot] = *src;
        src->source = NULL; src->text = NULL;  /* ownership transferred */
        scores[slot].fusion = rrf[slot];
        scores[slot].cosine = cos_s;
        scores[slot].bm25   = bm25_s;
    }

cleanup:
    free(ids); free(rrf);
    /* Any pool entries whose strings weren't stolen get freed here. */
    for (int i = 0; i < n_cos;  i++) { free(cos_res[i].source);  free(cos_res[i].text); }
    for (int i = 0; i < n_bm25; i++) { free(bm25_res[i].source); free(bm25_res[i].text); }
    free(cos_res); free(cos_sc); free(bm25_res); free(bm25_sc);
    return out_n;
}

int store_stats(crag_store_t *s, crag_store_stats_t *out) {
    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT COUNT(*), COUNT(DISTINCT source), MAX(dim) FROM chunks";
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return -1; }
    out->chunks = sqlite3_column_int(stmt, 0);
    out->files  = sqlite3_column_int(stmt, 1);
    out->dim    = sqlite3_column_int(stmt, 2);
    sqlite3_finalize(stmt);
    return 0;
}

int store_file_is_current(crag_store_t *s, const char *path,
                          long long mtime, long long size) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT mtime,size FROM files WHERE path = ?";
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    int hit = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        long long rmtime = sqlite3_column_int64(stmt, 0);
        long long rsize  = sqlite3_column_int64(stmt, 1);
        if (rmtime == mtime && rsize == size) hit = 1;
    }
    sqlite3_finalize(stmt);
    return hit;
}

int store_file_upsert(crag_store_t *s, const char *path,
                      long long mtime, long long size, long long run_ts) {
    sqlite3_stmt *stmt;
    const char *sql =
        "INSERT INTO files(path,mtime,size,indexed_at) VALUES(?,?,?,?) "
        "ON CONFLICT(path) DO UPDATE SET "
        "  mtime=excluded.mtime, size=excluded.size, indexed_at=excluded.indexed_at";
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text (stmt, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, mtime);
    sqlite3_bind_int64(stmt, 3, size);
    sqlite3_bind_int64(stmt, 4, run_ts);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int store_file_touch(crag_store_t *s, const char *path, long long run_ts) {
    sqlite3_stmt *stmt;
    const char *sql = "UPDATE files SET indexed_at = ? WHERE path = ?";
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, run_ts);
    sqlite3_bind_text (stmt, 2, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int store_delete_chunks_for(crag_store_t *s, const char *path) {
    sqlite3_stmt *stmt;
    const char *sql = "DELETE FROM chunks WHERE source = ?";
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

long long store_next_run_ts(crag_store_t *s, long long now) {
    sqlite3_stmt *stmt;
    long long last = 0;
    if (sqlite3_prepare_v2(s->db, "SELECT COALESCE(MAX(indexed_at), 0) FROM files",
                           -1, &stmt, NULL) != SQLITE_OK) return now;
    if (sqlite3_step(stmt) == SQLITE_ROW) last = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return last >= now ? last + 1 : now;
}

int store_sweep_missing(crag_store_t *s, long long run_ts,
                        int *files_removed, int *chunks_removed) {
    if (files_removed)  *files_removed  = 0;
    if (chunks_removed) *chunks_removed = 0;

    /* Count what's about to go so the caller can report it. SQLite has no
       portable RETURNING-with-CTE we can rely on across vendored versions,
       so two selects + two deletes is the clearest path. */
    sqlite3_stmt *stmt;
    int frm = 0, crm = 0;

    if (sqlite3_prepare_v2(s->db,
            "SELECT COUNT(*) FROM files WHERE indexed_at < ?",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, run_ts);
    if (sqlite3_step(stmt) == SQLITE_ROW) frm = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (sqlite3_prepare_v2(s->db,
            "SELECT COUNT(*) FROM chunks WHERE source IN "
            "(SELECT path FROM files WHERE indexed_at < ?)",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, run_ts);
    if (sqlite3_step(stmt) == SQLITE_ROW) crm = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (sqlite3_prepare_v2(s->db,
            "DELETE FROM chunks WHERE source IN "
            "(SELECT path FROM files WHERE indexed_at < ?)",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, run_ts);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    if (sqlite3_prepare_v2(s->db,
            "DELETE FROM files WHERE indexed_at < ?",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, run_ts);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    if (files_removed)  *files_removed  = frm;
    if (chunks_removed) *chunks_removed = crm;
    return 0;
}

void store_close(crag_store_t *s) {
    if (!s) return;
    cache_free(&s->cache);
    if (s->db) { sqlite3_close(s->db); s->db = NULL; }
}

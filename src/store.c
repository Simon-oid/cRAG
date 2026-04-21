#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sqlite3.h>
#include "store.h"
#include "crag.h"

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS chunks ("
    "  id     INTEGER PRIMARY KEY,"
    "  source TEXT NOT NULL,"
    "  text   TEXT NOT NULL,"
    "  dim    INTEGER NOT NULL,"
    "  vec    BLOB NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_source ON chunks(source);";

int store_open(const char *path, crag_store_t *s) {
    if (sqlite3_open(path, &s->db) != SQLITE_OK) {
        fprintf(stderr, "store: %s\n", sqlite3_errmsg(s->db));
        return -1;
    }
    char *err = NULL;
    if (sqlite3_exec(s->db, "PRAGMA journal_mode=WAL;", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "store: WAL: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    if (sqlite3_exec(s->db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "store: schema: %s\n", err);
        sqlite3_free(err);
        return -1;
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

    sqlite3_bind_text(stmt, 1, chunk->source, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, chunk->text,   -1, SQLITE_STATIC);
    sqlite3_bind_int (stmt, 3, chunk->dim);
    sqlite3_bind_blob(stmt, 4, chunk->vec, chunk->dim * (int)sizeof(float), SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static float cosine(const float *a, const float *b, int dim) {
    float dot = 0, na = 0, nb = 0;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    return dot / (sqrtf(na) * sqrtf(nb) + 1e-9f);
}

int store_search(crag_store_t *s, const float *qvec, int dim, int k,
                 crag_chunk_t *results, float *scores) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT id,source,text,dim,vec FROM chunks";
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int rdim = sqlite3_column_int(stmt, 3);
        if (rdim != dim) continue;

        const float *rvec = sqlite3_column_blob(stmt, 4);
        float score = cosine(qvec, rvec, dim);

        if (n < k) {
            results[n].id     = sqlite3_column_int(stmt, 0);
            results[n].source = strdup((const char *)sqlite3_column_text(stmt, 1));
            results[n].text   = strdup((const char *)sqlite3_column_text(stmt, 2));
            results[n].dim    = rdim;
            memcpy(results[n].vec, rvec, (size_t)rdim * sizeof(float));
            scores[n] = score;
            /* insertion sort */
            for (int i = n; i > 0 && scores[i] > scores[i-1]; i--) {
                float ts = scores[i]; scores[i] = scores[i-1]; scores[i-1] = ts;
                crag_chunk_t tc = results[i]; results[i] = results[i-1]; results[i-1] = tc;
            }
            n++;
        } else if (score > scores[n-1]) {
            free(results[n-1].source);
            free(results[n-1].text);
            results[n-1].id     = sqlite3_column_int(stmt, 0);
            results[n-1].source = strdup((const char *)sqlite3_column_text(stmt, 1));
            results[n-1].text   = strdup((const char *)sqlite3_column_text(stmt, 2));
            results[n-1].dim    = rdim;
            memcpy(results[n-1].vec, rvec, (size_t)rdim * sizeof(float));
            scores[n-1] = score;
            for (int i = n-1; i > 0 && scores[i] > scores[i-1]; i--) {
                float ts = scores[i]; scores[i] = scores[i-1]; scores[i-1] = ts;
                crag_chunk_t tc = results[i]; results[i] = results[i-1]; results[i-1] = tc;
            }
        }
    }
    sqlite3_finalize(stmt);
    return n;
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

void store_close(crag_store_t *s) {
    if (s && s->db) { sqlite3_close(s->db); s->db = NULL; }
}

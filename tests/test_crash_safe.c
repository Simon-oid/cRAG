/* test_crash_safe.c — Tier 4.4 crash safety (offline, deterministic).
 *
 * `crag index` bounds the blast radius of an interrupt by committing every N
 * files inside an explicit transaction, and writes each file's chunks AND its
 * `files` row in the SAME transaction (index.c). A SIGKILL therefore leaves
 * every file either fully indexed (chunks + files row durable) or fully absent
 * (uncommitted WAL frames discarded on the next open) — never half. Resume then
 * skips durable files (store_file_is_current) and retries the rest.
 *
 * This test reproduces that invariant WITHOUT an embedder or a forked process:
 * store_close() is a bare sqlite3_close(), so closing a store with an open,
 * uncommitted transaction is the exact WAL outcome of a SIGKILL mid-batch
 * (the frames were never committed). We assert: uncommitted work rolls back
 * whole, the DB passes PRAGMA integrity_check, chunks<->files stay consistent
 * (no orphan chunk, no empty file row), resume completes, and reindex is
 * idempotent. The real fork+SIGKILL integration counterpart lives in
 * tests/manual/crash_safe.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "crag.h"
#include "store.h"
#include "test.h"

static void cleanup_db(const char *p) {
    unlink(p);
    char wal[256]; snprintf(wal, sizeof wal, "%s-wal", p); unlink(wal);
    char shm[256]; snprintf(shm, sizeof shm, "%s-shm", p); unlink(shm);
}

/* One file's worth of index work, mirroring index.c: clear old chunks, insert
   the new ones, then record the files row — all on the caller's transaction. */
static void index_file(crag_store_t *s, const char *path, long long mtime,
                       long long size, long long run_ts, int nchunks) {
    store_delete_chunks_for(s, path);
    for (int i = 0; i < nchunks; i++) {
        crag_chunk_t c = {0};
        char text[96];
        snprintf(text, sizeof text, "chunk %d of %s lorem ipsum", i, path);
        c.source = (char *)path; c.text = text; c.dim = 4;
        c.vec[0] = 0.5f; c.vec[1] = 0.25f; c.vec[2] = 0.1f; c.vec[3] = 0.05f;
        TEST_ASSERT(store_insert(s, &c) == 0);
    }
    store_file_upsert(s, path, mtime, size, run_ts);
}

static int integrity_cb(void *got, int ncol, char **vals, char **names) {
    (void)ncol; (void)names;
    if (vals && vals[0] && strcmp(vals[0], "ok") == 0) *(int *)got = 1;
    return 0;
}
static int integrity_ok(crag_store_t *s) {
    int ok = 0;
    sqlite3_exec(s->db, "PRAGMA integrity_check;", integrity_cb, &ok, NULL);
    return ok;
}

static int count_cb(void *got, int ncol, char **vals, char **names) {
    (void)ncol; (void)names;
    if (vals && vals[0]) *(int *)got = atoi(vals[0]);
    return 0;
}
static int scalar(crag_store_t *s, const char *sql) {
    int v = -1;
    sqlite3_exec(s->db, sql, count_cb, &v, NULL);
    return v;
}
/* chunks<->files consistency: a healthy store has no chunk whose source is not
   a files row, and no files row with zero chunks. */
static int orphan_chunks(crag_store_t *s) {
    return scalar(s,
        "SELECT count(*) FROM chunks WHERE source NOT IN (SELECT path FROM files)");
}
static int empty_files(crag_store_t *s) {
    return scalar(s,
        "SELECT count(*) FROM files WHERE path NOT IN (SELECT DISTINCT source FROM chunks)");
}

static void test_crash_mid_batch_rolls_back_then_resumes(void) {
    const char *p = "/tmp/crag_crash_resume.db";
    cleanup_db(p);
    crag_store_t s;

    /* --- run 1: commit A,B; then start C and "crash" (close, no commit) --- */
    TEST_ASSERT(store_open(p, &s) == 0);
    TEST_ASSERT(store_begin(&s) == 0);
    index_file(&s, "A.md", 100, 10, 1000, 2);
    index_file(&s, "B.md", 200, 20, 1000, 3);
    TEST_ASSERT(store_commit(&s) == 0);          /* A,B now durable: 5 chunks */

    TEST_ASSERT(store_begin(&s) == 0);
    index_file(&s, "C.md", 300, 30, 1000, 2);    /* uncommitted */
    store_close(&s);                              /* == SIGKILL mid-batch */

    /* --- reopen: C must be gone entirely, A,B intact --- */
    TEST_ASSERT(store_open(p, &s) == 0);
    crag_store_stats_t st = {0};
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.chunks == 5);                 /* C's 2 chunks rolled back */
    TEST_ASSERT(st.files  == 2);
    TEST_ASSERT(integrity_ok(&s) == 1);
    TEST_ASSERT(orphan_chunks(&s) == 0);
    TEST_ASSERT(empty_files(&s)   == 0);

    /* resume signal: A,B current (skip), C not current (retry) */
    TEST_ASSERT(store_file_is_current(&s, "A.md", 100, 10) == 1);
    TEST_ASSERT(store_file_is_current(&s, "B.md", 200, 20) == 1);
    TEST_ASSERT(store_file_is_current(&s, "C.md", 300, 30) == 0);

    /* --- run 2 (resume): index only C, commit --- */
    TEST_ASSERT(store_begin(&s) == 0);
    index_file(&s, "C.md", 300, 30, 2000, 2);
    TEST_ASSERT(store_commit(&s) == 0);
    memset(&st, 0, sizeof st);
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.chunks == 7);                 /* A2 + B3 + C2 */
    TEST_ASSERT(st.files  == 3);
    TEST_ASSERT(store_file_is_current(&s, "C.md", 300, 30) == 1);
    TEST_ASSERT(integrity_ok(&s) == 1);
    TEST_ASSERT(orphan_chunks(&s) == 0);
    TEST_ASSERT(empty_files(&s)   == 0);

    store_close(&s);
    cleanup_db(p);
}

static void test_reindex_is_idempotent(void) {
    const char *p = "/tmp/crag_crash_idem.db";
    cleanup_db(p);
    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);
    TEST_ASSERT(store_begin(&s) == 0);
    index_file(&s, "A.md", 100, 10, 1000, 2);
    index_file(&s, "B.md", 200, 20, 1000, 3);
    TEST_ASSERT(store_commit(&s) == 0);

    crag_store_stats_t st = {0};
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.chunks == 5);

    /* Re-index A (e.g. a resumed run re-touching an already-done file): the
       delete_chunks_for step must prevent chunk duplication. */
    TEST_ASSERT(store_begin(&s) == 0);
    index_file(&s, "A.md", 100, 10, 2000, 2);
    TEST_ASSERT(store_commit(&s) == 0);
    memset(&st, 0, sizeof st);
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.chunks == 5);                 /* still 5, not 7 */
    TEST_ASSERT(st.files  == 2);
    TEST_ASSERT(orphan_chunks(&s) == 0);
    TEST_ASSERT(empty_files(&s)   == 0);

    store_close(&s);
    cleanup_db(p);
}

/* Many crash points: for each file, randomly (deterministic PRNG) either commit
   or crash before committing, then reopen and assert the store is ALWAYS
   integrity-clean and consistent — surfacing any boundary that leaves orphans. */
static void test_repeated_crash_points_stay_consistent(void) {
    const char *p = "/tmp/crag_crash_loop.db";
    cleanup_db(p);
    unsigned rng = 0x9e3779b9u;
    int committed_files = 0;

    crag_store_t s;
    TEST_ASSERT(store_open(p, &s) == 0);
    for (int it = 0; it < 30; it++) {
        rng = rng * 1664525u + 1013904223u;
        int crash = (rng >> 16) & 1;
        char path[32];
        snprintf(path, sizeof path, "f%02d.md", it);

        TEST_ASSERT(store_begin(&s) == 0);
        index_file(&s, path, 1000 + it, 10 + it, 5000 + it, 1 + (it % 3));
        if (crash) {
            store_close(&s);                     /* crash: drop this file */
            TEST_ASSERT(store_open(p, &s) == 0);
        } else {
            TEST_ASSERT(store_commit(&s) == 0);
            committed_files++;
        }
        /* Invariant after every step, committed or crashed: */
        TEST_ASSERT(integrity_ok(&s) == 1);
        TEST_ASSERT(orphan_chunks(&s) == 0);
        TEST_ASSERT(empty_files(&s)   == 0);
    }
    crag_store_stats_t st = {0};
    TEST_ASSERT(store_stats(&s, &st) == 0);
    TEST_ASSERT(st.files == committed_files);     /* only committed files survive */
    store_close(&s);
    cleanup_db(p);
}

int main(void) {
    fprintf(stderr, "test_crash_safe\n");
    TEST_RUN(test_crash_mid_batch_rolls_back_then_resumes);
    TEST_RUN(test_reindex_is_idempotent);
    TEST_RUN(test_repeated_crash_points_stay_consistent);
    TEST_DONE();
}

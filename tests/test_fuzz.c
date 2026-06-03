/* Adversarial-input fuzzer for cRAG's three hand-rolled parsers.
 *
 * These run on attacker-influenced bytes: corpus chunks (often invalid
 * UTF-8) flow through json_escape on every embed/gen/rerank request, and
 * the *responses* json_extract_floats / rerank_parse_scores chew on come
 * straight off the wire. A single missed bound is a heap overwrite. The
 * live-server UTF-8 bug that corrupted the rerank eval (raw bad bytes
 * smuggled into a JSON body, HTTP 500) was exactly this class — a fuzzer
 * over json_escape asserts it can never recur for *any* input.
 *
 * The value is under sanitizers: `make test-asan` makes every buffer an
 * ASan red-zoned heap allocation, so an off-by-one OOB write aborts here
 * instead of silently corrupting a neighbour. Plain `make test` still runs
 * it as a fast smoke check of the logical invariants below.
 *
 * Determinism: seed defaults to a fixed constant so a failure reproduces.
 * Override with CRAG_FUZZ_SEED / CRAG_FUZZ_ITERS to widen a hunt or to
 * minimise a discovered crash.
 *
 * Invariants asserted (beyond "ASan saw no OOB"):
 *   json_escape         — on success, strlen(out)==rc, output is *clean*
 *                         (no raw byte <0x20, every >=0x80 run is valid
 *                         UTF-8); an exact-sized buffer succeeds and a
 *                         one-byte-short buffer fails with -1, never a
 *                         partial overflow.
 *   json_extract_floats — *out_n never exceeds max_n.
 *   rerank_parse_scores — never writes an index outside [0,n) (enforced by
 *                         an exact-sized scores[] under ASan); return is
 *                         -1 or a positive parse count.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "test.h"

/* non-static in src/rerank_llamacpp.c */
int rerank_parse_scores(const char *json, int n, float *scores);

/* ---- deterministic PRNG (xorshift32) ---------------------------------- */
static uint32_t g_rng = 0x9e3779b9u;
static uint32_t xs(void) {
    uint32_t x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_rng = x;
}
static uint32_t xs_below(uint32_t n) { return n ? xs() % n : 0; }

/* ---- independent UTF-8 validator (cross-checks src/util.c's own) ------- */
/* Returns 1 if s is "clean": no raw control byte (<0x20) and every byte
   >=0x80 begins a well-formed, minimal, in-range UTF-8 sequence. This is
   precisely the property json_escape must guarantee for *any* input. */
static int is_clean_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        if (c < 0x20) return 0;                 /* raw control leaked */
        if (c < 0x80) { p++; continue; }
        int len, min_lo = 0, min_hi = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; if (c < 0xC2) return 0; }
        else if ((c & 0xF0) == 0xE0) { len = 3;
            if (c == 0xE0) min_lo = 0xA0;        /* reject overlong */
            if (c == 0xED) min_hi = 0x9F;        /* reject surrogate */
        }
        else if ((c & 0xF8) == 0xF0) { len = 4;
            if (c > 0xF4) return 0;
            if (c == 0xF0) min_lo = 0x90;
            if (c == 0xF4) min_hi = 0x8F;
        }
        else return 0;                          /* stray continuation / 0xF5+ */
        for (int i = 1; i < len; i++)
            if ((p[i] & 0xC0) != 0x80) return 0; /* '\0' fails here too */
        if (min_lo && p[1] < min_lo) return 0;
        if (min_hi && p[1] > min_hi) return 0;
        p += len;
    }
    return 1;
}

/* ---- adversarial input generator -------------------------------------- */
/* Build a NUL-terminated buffer biased toward the bytes that stress these
   parsers: high/continuation bytes and truncated multibyte leads (UTF-8
   edge cases), plus JSON structure and number tokens so the float/score
   parsers actually reach their inner loops instead of bailing at byte 0. */
static const char *const TOKENS[] = {
    "\"index\"", "\"score\"", "\"relevance_score\"", "\"embedding\"",
    "\"vec\"", "{", "}", "[", "]", ":", ",", "\"", "results",
    "-", "+", ".", "e", "E", "0", "1", "9",
    "1e999", "-1e-999", "nan", "inf", "1.5e3", "99999999999999999999",
    "\xC3", "\xE2\x82", "\xF0\x9F", "\xFF", "\x80", "\xED\xA0\x80",
    "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80",
};
#define NTOK ((int)(sizeof TOKENS / sizeof TOKENS[0]))

static char *gen_input(size_t *out_len) {
    size_t cap = 1 + xs_below(512);
    char *b = malloc(cap + 1);
    if (!b) { perror("malloc"); exit(2); }
    size_t w = 0;
    while (w < cap) {
        uint32_t r = xs_below(3);
        if (r == 0) {                            /* a structured token */
            const char *t = TOKENS[xs_below(NTOK)];
            size_t tl = strlen(t);
            if (w + tl > cap) break;
            memcpy(b + w, t, tl); w += tl;
        } else {                                 /* a raw byte, never NUL */
            b[w++] = (char)(1 + xs_below(255));
        }
    }
    b[w] = '\0';
    *out_len = w;
    return b;
}

/* ---- the fuzz drivers -------------------------------------------------- */

static void fuzz_json_escape(const char *in) {
    size_t n = strlen(in);
    size_t big = n * 6 + 1;
    char *out = malloc(big);                     /* exact worst case */
    if (!out) { perror("malloc"); exit(2); }

    int rc = json_escape(in, out, big);
    TEST_ASSERT(rc >= 0);                         /* worst-case buf always fits */
    TEST_ASSERT((size_t)rc == strlen(out));       /* no embedded NUL, NUL-term'd */
    TEST_ASSERT(is_clean_utf8(out));              /* THE property: output is safe */

    /* Exact-sized buffer (rc+1) must succeed; one byte short must fail with
       -1 and — under ASan — must not write past its end. Heap-exact sizing
       turns any off-by-one into an ASan abort. */
    char *exact = malloc((size_t)rc + 1);
    int r2 = json_escape(in, exact, (size_t)rc + 1);
    TEST_ASSERT(r2 == rc);
    free(exact);

    if (rc > 0) {
        char *shortb = malloc((size_t)rc);        /* one byte too small */
        int r3 = json_escape(in, shortb, (size_t)rc);
        TEST_ASSERT(r3 == -1);
        free(shortb);
    }
    free(out);

    /* json_escape_dup must agree and stay clean. */
    char *dup = json_escape_dup(in);
    TEST_ASSERT(dup != NULL);
    TEST_ASSERT(is_clean_utf8(dup));
    free(dup);
}

static void fuzz_extract_floats(const char *in) {
    size_t max_n = 1 + xs_below(32);
    float *out = malloc(max_n * sizeof *out);     /* exact: ASan guards the tail */
    if (!out) { perror("malloc"); exit(2); }
    const char *keys[] = { "\"embedding\"", "\"vec\"", "\"data\"",
                           "\"index\"", "\"results\"", "\"\"" };
    const char *key = keys[xs_below((uint32_t)(sizeof keys / sizeof keys[0]))];

    size_t got = 12345;
    int rc = json_extract_floats(in, key, out, max_n, &got);
    if (rc == 0) {
        TEST_ASSERT(got >= 1 && got <= max_n);    /* success ⇒ 1..max_n written */
    } else {
        TEST_ASSERT(rc == -1);
        TEST_ASSERT(got == 0);                    /* failure ⇒ count zeroed */
    }
    free(out);
}

static void fuzz_rerank_parse(const char *in) {
    int n = 1 + (int)xs_below(16);
    float *s = malloc((size_t)n * sizeof *s);     /* exact: index>=n ⇒ ASan abort */
    if (!s) { perror("malloc"); exit(2); }
    for (int i = 0; i < n; i++) s[i] = -1e30f;

    int got = rerank_parse_scores(in, n, s);
    TEST_ASSERT(got == -1 || got >= 1);           /* contract: -1 or parse count */
    free(s);
}

/* ---- harness ---------------------------------------------------------- */

static void test_fuzz_all_parsers(void) {
    const char *s_seed  = getenv("CRAG_FUZZ_SEED");
    const char *s_iters = getenv("CRAG_FUZZ_ITERS");
    if (s_seed && *s_seed) g_rng = (uint32_t)strtoul(s_seed, NULL, 0) | 1u;
    long iters = (s_iters && *s_iters) ? strtol(s_iters, NULL, 10) : 10000;
    if (iters < 1) iters = 1;

    fprintf(stderr, "    (seed=0x%08x, iters=%ld)\n", g_rng, iters);
    for (long i = 0; i < iters; i++) {
        size_t len;
        char *in = gen_input(&len);
        fuzz_json_escape(in);
        fuzz_extract_floats(in);
        fuzz_rerank_parse(in);
        free(in);
    }
}

/* A few hand-picked regressions that must always stay clean, independent
   of the random stream — including the exact byte shapes that broke the
   live reranker. */
static void test_fuzz_known_nasties(void) {
    const char *cases[] = {
        "\xFF\xFE\xFD",                           /* all-invalid */
        "valid \xF0\x9F\x98\x80 then \xC3 trunc", /* good emoji + dangling lead */
        "\xED\xA0\x80",                           /* UTF-16 surrogate, illegal */
        "\xC0\x80",                               /* overlong NUL */
        "a\x01\x02\x1f" "b",                       /* raw control run */
        "\"\\\n\r\t\b\f",                         /* every escape trigger */
        "",                                       /* empty */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *dup = json_escape_dup(cases[i]);
        TEST_ASSERT(dup != NULL);
        TEST_ASSERT(is_clean_utf8(dup));
        free(dup);
    }
}

int main(void) {
    fprintf(stderr, "test_fuzz:\n");
    TEST_RUN(test_fuzz_known_nasties);
    TEST_RUN(test_fuzz_all_parsers);
    TEST_DONE();
}

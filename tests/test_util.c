#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "test.h"

static void test_escape_plain_ascii_is_identity(void) {
    char out[64];
    int n = json_escape("hello world", out, sizeof out);
    TEST_ASSERT(n == 11);
    TEST_ASSERT(strcmp(out, "hello world") == 0);
}

static void test_escape_special_chars(void) {
    char out[128];
    int n = json_escape("say \"hi\"\nline\tend\\done", out, sizeof out);
    TEST_ASSERT(n > 0);
    TEST_ASSERT(strcmp(out, "say \\\"hi\\\"\\nline\\tend\\\\done") == 0);
}

static void test_escape_carriage_return(void) {
    char out[16];
    int n = json_escape("a\rb", out, sizeof out);
    TEST_ASSERT(n == 4);
    TEST_ASSERT(strcmp(out, "a\\rb") == 0);
}

static void test_escape_control_char_u_encoded(void) {
    /* 0x01 must become  (6 bytes) */
    char in[4]  = { 'a', 0x01, 'b', '\0' };
    char out[32];
    int n = json_escape(in, out, sizeof out);
    TEST_ASSERT(n == 8);
    TEST_ASSERT(strcmp(out, "a\\u0001b") == 0);
}

static void test_escape_buffer_too_small_returns_error(void) {
    char out[4];
    /* "hi\"" needs 4 bytes of content + NUL = 5, out holds only 4 */
    int n = json_escape("hi\"", out, sizeof out);
    TEST_ASSERT(n == -1);
}

static void test_escape_empty_string(void) {
    char out[8];
    int n = json_escape("", out, sizeof out);
    TEST_ASSERT(n == 0);
    TEST_ASSERT(out[0] == '\0');
}

static void test_escape_null_inputs_return_error(void) {
    char out[8];
    TEST_ASSERT(json_escape(NULL, out,  sizeof out) == -1);
    TEST_ASSERT(json_escape("x",  NULL, sizeof out) == -1);
    TEST_ASSERT(json_escape("x",  out,  0)          == -1);
}

static void test_escape_dup_roundtrip(void) {
    char *s = json_escape_dup("a\"b\nc");
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(strcmp(s, "a\\\"b\\nc") == 0);
    free(s);
}

static void test_escape_dup_null_input(void) {
    TEST_ASSERT(json_escape_dup(NULL) == NULL);
}

/* Well-formed UTF-8 (é = C3 A9, € = E2 82 AC, 😀 = F0 9F 98 80) passes
   through byte-for-byte — the escaper must not mangle multi-byte text. */
static void test_escape_valid_utf8_passthrough(void) {
    char *s = json_escape_dup("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(strcmp(s, "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80") == 0);
    free(s);
}

/* Malformed UTF-8 must become the U+FFFD replacement char (EF BF BD) so the
   emitted JSON stays valid — strict parsers (e.g. llama.cpp) reject raw bad
   bytes. 0xFF is never legal; 0xC3 followed by a non-continuation is a broken
   2-byte lead; a stray 0x80 continuation has no lead. */
static void test_escape_invalid_utf8_replaced(void) {
    char *s = json_escape_dup("a\xFF" "b");          /* lone 0xFF */
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(strcmp(s, "a\xEF\xBF\xBD" "b") == 0);
    free(s);

    char *t = json_escape_dup("x\xC3" "y");          /* truncated 2-byte seq */
    TEST_ASSERT(t != NULL);
    TEST_ASSERT(strcmp(t, "x\xEF\xBF\xBD" "y") == 0);
    free(t);

    char *u = json_escape_dup("\x80z");              /* stray continuation */
    TEST_ASSERT(u != NULL);
    TEST_ASSERT(strcmp(u, "\xEF\xBF\xBD" "z") == 0);
    free(u);
}

static void test_extract_floats_basic(void) {
    const char *j = "{\"embedding\":[1.0, 2.5, -3.0, 0.5]}";
    float v[8] = {0};
    size_t got = 0;
    int r = json_extract_floats(j, "\"embedding\"", v, 8, &got);
    TEST_ASSERT(r == 0);
    TEST_ASSERT(got == 4);
    TEST_ASSERT(v[0] ==  1.0f);
    TEST_ASSERT(v[1] ==  2.5f);
    TEST_ASSERT(v[2] == -3.0f);
    TEST_ASSERT(v[3] ==  0.5f);
}

static void test_extract_floats_missing_key(void) {
    const char *j = "{\"other\":[1.0, 2.0]}";
    float v[4] = {0};
    size_t got = 99;
    int r = json_extract_floats(j, "\"embedding\"", v, 4, &got);
    TEST_ASSERT(r == -1);
    TEST_ASSERT(got == 0);
}

static void test_extract_floats_respects_max_n(void) {
    const char *j = "\"vec\":[0.1, 0.2, 0.3, 0.4, 0.5]";
    float v[3] = {0};
    size_t got = 0;
    int r = json_extract_floats(j, "\"vec\"", v, 3, &got);
    TEST_ASSERT(r == 0);
    TEST_ASSERT(got == 3);
    TEST_ASSERT(v[0] == 0.1f);
    TEST_ASSERT(v[2] == 0.3f);
}

static void test_extract_floats_tolerates_whitespace(void) {
    const char *j = "\"embedding\"  :  [ 1.25 ,\n 2.5 ,\t -0.5 ]";
    float v[4] = {0};
    size_t got = 0;
    int r = json_extract_floats(j, "\"embedding\"", v, 4, &got);
    TEST_ASSERT(r == 0);
    TEST_ASSERT(got == 3);
    TEST_ASSERT(v[0] == 1.25f);
    TEST_ASSERT(v[2] == -0.5f);
}

static void test_extract_floats_first_match_wins(void) {
    /* Parser uses first strstr hit; confirm it stops at that array. */
    const char *j = "\"data\":[\"x\"],\"vec\":[7.0,8.0],\"vec\":[99.0]";
    float v[4] = {0};
    size_t got = 0;
    int r = json_extract_floats(j, "\"vec\"", v, 4, &got);
    TEST_ASSERT(r == 0);
    TEST_ASSERT(got == 2);
    TEST_ASSERT(v[0] == 7.0f);
    TEST_ASSERT(v[1] == 8.0f);
}

static void test_extract_floats_empty_array_is_error(void) {
    const char *j = "\"vec\":[]";
    float v[2] = {0};
    size_t got = 42;
    int r = json_extract_floats(j, "\"vec\"", v, 2, &got);
    /* Contract: returns -1 when nothing parsed (n == 0). */
    TEST_ASSERT(r == -1);
    TEST_ASSERT(got == 0);
}

static void test_extract_floats_null_inputs(void) {
    float v[2] = {0};
    size_t got = 0;
    TEST_ASSERT(json_extract_floats(NULL, "\"k\"", v,    2, &got) == -1);
    TEST_ASSERT(json_extract_floats("{}", NULL,   v,    2, &got) == -1);
    TEST_ASSERT(json_extract_floats("{}", "\"k\"", NULL, 2, &got) == -1);
    TEST_ASSERT(json_extract_floats("{}", "\"k\"", v,    2, NULL) == -1);
}

/* ---- gen_min_score_resolve: embedder-aware generation gate ------------- */
static int feq(float a, float b) { float d = a - b; return (d < 0 ? -d : d) < 1e-6f; }

static void test_gen_gate_env_override_wins(void) {
    /* A clean float overrides everything, including the model default. */
    TEST_ASSERT(feq(gen_min_score_resolve("0.42", "bge-m3", 0.60f), 0.42f));
    TEST_ASSERT(feq(gen_min_score_resolve("0.0", "mxbai", 0.60f), 0.0f));
    TEST_ASSERT(feq(gen_min_score_resolve("-1", NULL, 0.60f), -1.0f));
}

static void test_gen_gate_bge_default(void) {
    /* bge-m3's low cosine floor -> 0.38, regardless of the fallback, and
       robust to ollama/llama.cpp decorations + case. */
    TEST_ASSERT(feq(gen_min_score_resolve(NULL, "bge-m3", 0.60f), 0.38f));
    TEST_ASSERT(feq(gen_min_score_resolve(NULL, "bge-m3:latest", 0.60f), 0.38f));
    TEST_ASSERT(feq(gen_min_score_resolve(NULL, "registry/BGE-M3", 0.60f), 0.38f));
    TEST_ASSERT(feq(gen_min_score_resolve("", "bge-m3", 0.60f), 0.38f));
}

static void test_gen_gate_fallback_preserved(void) {
    /* mxbai and unknown/unset models keep the caller's fallback unchanged —
       no behaviour regression for existing deployments. */
    TEST_ASSERT(feq(gen_min_score_resolve(NULL, "mxbai-embed-large", 0.60f), 0.60f));
    TEST_ASSERT(feq(gen_min_score_resolve(NULL, "nomic-embed-text", 0.60f), 0.60f));
    TEST_ASSERT(feq(gen_min_score_resolve(NULL, NULL, 0.60f), 0.60f));
}

static void test_gen_gate_invalid_env_falls_through(void) {
    /* Garbage/trailing-junk env must NOT silently become 0.0 (the old atof
       bug that disabled the gate) — it falls through to model/fallback. */
    TEST_ASSERT(feq(gen_min_score_resolve("garbage", "bge-m3", 0.60f), 0.38f));
    TEST_ASSERT(feq(gen_min_score_resolve("0.4x", "mxbai", 0.60f), 0.60f));
    TEST_ASSERT(feq(gen_min_score_resolve("0.4x", NULL, 0.55f), 0.55f));
}

int main(void) {
    fprintf(stderr, "test_util:\n");
    TEST_RUN(test_escape_plain_ascii_is_identity);
    TEST_RUN(test_escape_special_chars);
    TEST_RUN(test_escape_carriage_return);
    TEST_RUN(test_escape_control_char_u_encoded);
    TEST_RUN(test_escape_buffer_too_small_returns_error);
    TEST_RUN(test_escape_empty_string);
    TEST_RUN(test_escape_null_inputs_return_error);
    TEST_RUN(test_escape_dup_roundtrip);
    TEST_RUN(test_escape_dup_null_input);
    TEST_RUN(test_escape_valid_utf8_passthrough);
    TEST_RUN(test_escape_invalid_utf8_replaced);
    TEST_RUN(test_extract_floats_basic);
    TEST_RUN(test_extract_floats_missing_key);
    TEST_RUN(test_extract_floats_respects_max_n);
    TEST_RUN(test_extract_floats_tolerates_whitespace);
    TEST_RUN(test_extract_floats_first_match_wins);
    TEST_RUN(test_extract_floats_empty_array_is_error);
    TEST_RUN(test_extract_floats_null_inputs);
    TEST_RUN(test_gen_gate_env_override_wins);
    TEST_RUN(test_gen_gate_bge_default);
    TEST_RUN(test_gen_gate_fallback_preserved);
    TEST_RUN(test_gen_gate_invalid_env_falls_through);
    TEST_DONE();
}

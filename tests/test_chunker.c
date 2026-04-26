#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "chunk.h"
#include "test.h"

static int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

static void free_chunks(char **texts, int n) {
    for (int i = 0; i < n; i++) { free(texts[i]); texts[i] = NULL; }
}

static void test_missing_file_returns_error(void) {
    char *texts[4] = {0};
    int n = chunk_file("/tmp/crag_test_nope_xyz_does_not_exist", 512, texts, 4);
    TEST_ASSERT(n == -1);
}

static void test_empty_file_produces_zero_chunks(void) {
    const char *p = "/tmp/crag_test_empty.md";
    write_file(p, "");
    char *texts[4] = {0};
    int n = chunk_file(p, 512, texts, 4);
    TEST_ASSERT(n == 0);
    free_chunks(texts, n);
    unlink(p);
}

static void test_markdown_coalesces_short_sections(void) {
    /* Mirror of the plain-text coalesce: a short md file with multiple
       headers should land in one chunk if the whole file fits. Without
       this, READMEs fragmented per-header even at 70 bytes total. */
    const char *p = "/tmp/crag_test_md_coalesce.md";
    write_file(p,
        "# One\n"
        "alpha alpha\n\n"
        "## Two\n"
        "beta beta\n\n"
        "### Three\n"
        "gamma gamma\n");
    char *texts[8] = {0};
    int n = chunk_file(p, 2048, texts, 8);
    TEST_ASSERT(n == 1);
    if (n == 1) {
        TEST_ASSERT(strstr(texts[0], "# One")     != NULL);
        TEST_ASSERT(strstr(texts[0], "## Two")    != NULL);
        TEST_ASSERT(strstr(texts[0], "### Three") != NULL);
    }
    free_chunks(texts, n);
    unlink(p);
}

static void test_markdown_splits_on_headers_when_oversize(void) {
    /* Three substantial sections with bodies sized so two-but-not-three
       sections fit: coalescing keeps adjacent sections together but still
       flushes at a header boundary when the next one would overflow. */
    const char *p = "/tmp/crag_test_md_split.md";
    char body[401];
    for (int i = 0; i < 400; i++) body[i] = (i % 80 == 79) ? ' ' : 'a';
    body[400] = '\0';

    FILE *f = fopen(p, "w");
    TEST_ASSERT(f != NULL);
    fprintf(f, "# One\n%s\n\n",   body);
    fprintf(f, "## Two\n%s\n\n",  body);
    fprintf(f, "### Three\n%s\n", body);
    fclose(f);

    /* each section ~410 chars; max=900 → two sections (~820) fit, the
       third forces a flush at the header boundary → 2 chunks. */
    char *texts[8] = {0};
    int n = chunk_file(p, 900, texts, 8);
    TEST_ASSERT(n == 2);
    if (n == 2) {
        TEST_ASSERT(strstr(texts[0], "# One")     != NULL);
        TEST_ASSERT(strstr(texts[0], "## Two")    != NULL);
        TEST_ASSERT(strstr(texts[1], "### Three") != NULL);
        /* split happens at header — chunk 0 must NOT contain Three */
        TEST_ASSERT(strstr(texts[0], "### Three") == NULL);
    }
    free_chunks(texts, n);
    unlink(p);
}

static void test_code_fence_is_not_header(void) {
    /* '#' inside a fenced code block must not trigger a section break. */
    const char *p = "/tmp/crag_test_fence.md";
    write_file(p,
        "# Title\n"
        "intro\n\n"
        "```c\n"
        "# not a header\n"
        "int main(void) { return 0; }\n"
        "```\n"
        "outro\n");
    char *texts[8] = {0};
    int n = chunk_file(p, 4096, texts, 8);
    TEST_ASSERT(n == 1);
    if (n == 1) {
        TEST_ASSERT(strstr(texts[0], "# not a header") != NULL);
        TEST_ASSERT(strstr(texts[0], "outro")          != NULL);
    }
    free_chunks(texts, n);
    unlink(p);
}

static void test_large_paragraph_forces_sliding_split(void) {
    const char *p = "/tmp/crag_test_slide.txt";
    /* ~1500 chars, no paragraph breaks */
    char buf[1501];
    for (int i = 0; i < 1500; i++) buf[i] = (i % 80 == 79) ? ' ' : 'a';
    buf[1500] = '\0';
    write_file(p, buf);

    char *texts[16] = {0};
    int n = chunk_file(p, 400, texts, 16);
    TEST_ASSERT(n >= 2);
    for (int i = 0; i < n; i++) TEST_ASSERT(strlen(texts[i]) > 0);
    free_chunks(texts, n);
    unlink(p);
}

static void test_chunks_are_trimmed(void) {
    const char *p = "/tmp/crag_test_trim.md";
    write_file(p, "# Header\n\n\n   body text   \n\n\n");
    char *texts[4] = {0};
    int n = chunk_file(p, 2048, texts, 4);
    TEST_ASSERT(n >= 1);
    if (n >= 1) {
        size_t L = strlen(texts[0]);
        TEST_ASSERT(L > 0);
        TEST_ASSERT(texts[0][0]     != ' ');
        TEST_ASSERT(texts[0][0]     != '\n');
        TEST_ASSERT(texts[0][L - 1] != ' ');
        TEST_ASSERT(texts[0][L - 1] != '\n');
    }
    free_chunks(texts, n);
    unlink(p);
}

static void test_text_coalesces_short_paragraphs(void) {
    /* A short plain-text file split into 3 paragraphs by blank lines
       should produce ONE chunk when the whole file fits in max_chars,
       not one chunk per paragraph. Previously this was 3 chunks of
       37/75/310 chars each — too little context for the embedder. */
    const char *p = "/tmp/crag_test_coalesce.txt";
    write_file(p,
        "Documento 1 - Categoria: Architettura\n\n"
        "Titolo: Approfondimento su Sistema distribuito.\n\n"
        "Questo documento analizza dati tecnici per Smart City OS "
        "e performance.\n");
    char *texts[8] = {0};
    int n = chunk_file(p, 2048, texts, 8);
    TEST_ASSERT(n == 1);
    if (n == 1) {
        TEST_ASSERT(strstr(texts[0], "Documento 1")       != NULL);
        TEST_ASSERT(strstr(texts[0], "Titolo")            != NULL);
        TEST_ASSERT(strstr(texts[0], "Smart City OS")     != NULL);
    }
    free_chunks(texts, n);
    unlink(p);
}

static void test_text_splits_when_paragraphs_exceed_max(void) {
    /* Two substantial paragraphs together exceed max_chars → 2 chunks,
       split at the paragraph boundary (not mid-sentence). */
    const char *p = "/tmp/crag_test_coalesce_split.txt";
    char para[801];
    for (int i = 0; i < 800; i++) para[i] = (i % 80 == 79) ? ' ' : 'x';
    para[800] = '\0';

    FILE *f = fopen(p, "w");
    TEST_ASSERT(f != NULL);
    fputs(para, f); fputs("\n\n", f);
    fputs(para, f); fputs("\n", f);
    fclose(f);

    char *texts[8] = {0};
    int n = chunk_file(p, 1000, texts, 8);
    TEST_ASSERT(n == 2);
    free_chunks(texts, n);
    unlink(p);
}

static void test_unbreakable_tail_does_not_loop(void) {
    /* Regression: libsamplerate/index.md ends with a long PGP base64 block
       inside a code fence. The tail had no spaces, no `. `, no `\n\n`, so
       sliding_split's find_split() returned sec_end every iteration. With
       split_at == sec_end and pos = sec_end - overlap, pos never advanced
       past the previous iteration → the same trailing chunk got emitted
       MAX_CHUNKS_PER_FILE times. The fix is to break when split_at >=
       sec_end (the section is fully covered). */
    const char *p = "/tmp/crag_test_unbreakable_tail.md";
    FILE *f = fopen(p, "w");
    TEST_ASSERT(f != NULL);
    /* prose body, then a fence containing 600 chars of unbreakable base64-ish
       content (no spaces; some newlines so it's not a single mega-line) */
    fputs("# Header\n\n", f);
    for (int i = 0; i < 5; i++) fputs("ordinary sentence text. ", f);
    fputs("\n\n```\n", f);
    for (int row = 0; row < 10; row++) {
        for (int c = 0; c < 60; c++) fputc('A' + (c % 26), f);
        fputc('\n', f);
    }
    fputs("```\n", f);
    fclose(f);

    char *texts[600] = {0};
    int n = chunk_file(p, 400, texts, 600);
    TEST_ASSERT(n > 0);
    TEST_ASSERT(n < 30);   /* a sane upper bound; pre-fix emitted ~512 */
    /* No two consecutive chunks should be byte-identical — a duplicate was
       the smoking gun for the loop. */
    for (int i = 1; i < n; i++)
        TEST_ASSERT(strcmp(texts[i - 1], texts[i]) != 0);
    free_chunks(texts, n);
    unlink(p);
}

static void test_respects_max_chunks_cap(void) {
    const char *p = "/tmp/crag_test_cap.md";
    char big[8192];
    int off = 0;
    for (int i = 0; i < 20; i++)
        off += snprintf(big + off, sizeof(big) - off, "## H%d\npara %d\n\n", i, i);
    write_file(p, big);

    char *texts[3] = {0};
    int n = chunk_file(p, 2048, texts, 3);
    TEST_ASSERT(n <= 3);
    free_chunks(texts, n);
    unlink(p);
}

int main(void) {
    fprintf(stderr, "test_chunker:\n");
    TEST_RUN(test_missing_file_returns_error);
    TEST_RUN(test_empty_file_produces_zero_chunks);
    TEST_RUN(test_markdown_coalesces_short_sections);
    TEST_RUN(test_markdown_splits_on_headers_when_oversize);
    TEST_RUN(test_code_fence_is_not_header);
    TEST_RUN(test_large_paragraph_forces_sliding_split);
    TEST_RUN(test_chunks_are_trimmed);
    TEST_RUN(test_text_coalesces_short_paragraphs);
    TEST_RUN(test_text_splits_when_paragraphs_exceed_max);
    TEST_RUN(test_unbreakable_tail_does_not_loop);
    TEST_RUN(test_respects_max_chunks_cap);
    TEST_DONE();
}

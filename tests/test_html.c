#include <stdlib.h>
#include <string.h>
#include "html.h"
#include "test.h"

static char *conv(const char *in) {
    size_t n = 0;
    return html_to_text(in, strlen(in), &n);
}

static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

static void test_headings_become_markdown(void) {
    char *t = conv("<html><body><h1>Title</h1><p>Intro text.</p><h3>Sub</h3><p>More.</p></body></html>");
    TEST_ASSERT(t && has(t, "# Title\n") && has(t, "### Sub\n"));
    TEST_ASSERT(has(t, "Intro text.") && has(t, "More."));
    TEST_ASSERT(!has(t, "<") && !has(t, ">"));
    free(t);
}

static void test_script_style_and_comments_are_dropped(void) {
    char *t = conv("<head><style>p{color:red}</style><script>var x = '<p>';</script></head>"
                   "<!-- secret --><p>Kept</p><SCRIPT type=x>alert(1)</SCRIPT>");
    TEST_ASSERT(t && has(t, "Kept"));
    TEST_ASSERT(!has(t, "color") && !has(t, "var x") && !has(t, "secret") && !has(t, "alert"));
    free(t);
}

static void test_entities_are_decoded(void) {
    char *t = conv("<p>a &amp; b &lt;c&gt; &quot;d&quot; &#39;e&#39; caf&eacute; &#233; &#xE9; &nbsp;x</p>");
    TEST_ASSERT(t && has(t, "a & b <c> \"d\" 'e'"));
    TEST_ASSERT(has(t, "\xc3\xa9 \xc3\xa9"));            /* numeric é, decimal and hex */
    TEST_ASSERT(has(t, "&eacute;"));                       /* unknown name: kept literally */
    free(t);
}

static void test_invalid_code_points_become_replacement(void) {
    char *t = conv("<p>&#0; &#xD800; &#x110000; &#99999999999;</p>");
    TEST_ASSERT(t && !has(t, "\xed\xa0\x80"));             /* no encoded surrogate */
    TEST_ASSERT(has(t, "\xef\xbf\xbd"));                   /* U+FFFD instead */
    free(t);
}

static void test_whitespace_collapses_outside_pre(void) {
    char *t = conv("<p>one\n   two\t\tthree</p><pre>keep   this\n  indent</pre>");
    TEST_ASSERT(t && has(t, "one two three"));
    TEST_ASSERT(has(t, "keep   this\n  indent"));
    free(t);
}

static void test_lists_and_breaks(void) {
    char *t = conv("<ul><li>first</li><li>second</li></ul>line<br>next");
    TEST_ASSERT(t && has(t, "- first") && has(t, "- second") && has(t, "line\nnext"));
    free(t);
}

static void test_attributes_with_angle_brackets_do_not_leak(void) {
    char *t = conv("<a href=\"x>y\" title='a>b'>link</a> after");
    TEST_ASSERT(t && has(t, "link after") && !has(t, "y\"") && !has(t, "b'"));
    free(t);
}

static void test_truncated_input_terminates(void) {
    const char *cases[] = { "<", "<p", "<!--", "<script>", "&", "&#", "&#x", "&amp", "<a href=\"" };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *t = conv(cases[i]);
        TEST_ASSERT(t != NULL);
        free(t);
    }
}

int main(void) {
    fprintf(stderr, "test_html:\n");
    TEST_RUN(test_headings_become_markdown);
    TEST_RUN(test_script_style_and_comments_are_dropped);
    TEST_RUN(test_entities_are_decoded);
    TEST_RUN(test_invalid_code_points_become_replacement);
    TEST_RUN(test_whitespace_collapses_outside_pre);
    TEST_RUN(test_lists_and_breaks);
    TEST_RUN(test_attributes_with_angle_brackets_do_not_leak);
    TEST_RUN(test_truncated_input_terminates);
    TEST_DONE();
}

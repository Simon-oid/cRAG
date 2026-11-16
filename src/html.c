#include "html.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTITY 32
#define MAX_TAG_NAME 16
#define REPLACEMENT "\xef\xbf\xbd"

typedef struct {
    char  *p;
    size_t n, cap;
    int    oom;
    int    pending_space;   /* whitespace seen since the last character */
    int    in_pre;
} out_t;

static void put(out_t *o, const char *s, size_t n) {
    if (o->oom) return;
    if (o->n + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 256;
        while (cap < o->n + n + 1) cap *= 2;
        char *p = realloc(o->p, cap);
        if (!p) { o->oom = 1; return; }
        o->p = p;
        o->cap = cap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
}

/* A visible character, preceded by one space if whitespace came before. */
static void put_text(out_t *o, const char *s, size_t n) {
    if (o->pending_space && o->n > 0 && o->p[o->n - 1] != '\n' && o->p[o->n - 1] != ' ')
        put(o, " ", 1);
    o->pending_space = 0;
    put(o, s, n);
}

/* End the current block: no trailing spaces, then `lines` newlines. */
static void block_break(out_t *o, int lines) {
    o->pending_space = 0;
    while (o->n > 0 && o->p[o->n - 1] == ' ') o->n--;
    if (o->n == 0) return;
    int have = 0;
    while (have < (int)o->n && o->p[o->n - 1 - have] == '\n') have++;
    for (; have < lines; have++) put(o, "\n", 1);
}

static void put_utf8(out_t *o, unsigned long cp) {
    char b[4];
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        put_text(o, REPLACEMENT, 3);
    } else if (cp < 0x80) {
        b[0] = (char)cp;
        put_text(o, b, 1);
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        put_text(o, b, 2);
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        put_text(o, b, 3);
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        put_text(o, b, 4);
    }
}

static const struct { const char *name, *text; } ENTITIES[] = {
    { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" },
    { "nbsp", " " }, { "ndash", "\xe2\x80\x93" }, { "mdash", "\xe2\x80\x94" },
    { "hellip", "\xe2\x80\xa6" }, { "copy", "\xc2\xa9" }, { "reg", "\xc2\xae" },
};

/* At in[i] == '&'. Returns the bytes consumed (1 if left as a literal). */
static size_t entity(out_t *o, const char *in, size_t n, size_t i) {
    size_t end = i + 1;
    while (end < n && end - i <= MAX_ENTITY && in[end] != ';' && in[end] != '&' &&
           !isspace((unsigned char)in[end]))
        end++;
    if (end >= n || in[end] != ';' || end == i + 1) {
        put_text(o, "&", 1);
        return 1;
    }
    const char *name = in + i + 1;
    size_t len = end - i - 1;
    if (name[0] == '#') {
        int hex = len > 1 && (name[1] == 'x' || name[1] == 'X');
        size_t d = hex ? 2 : 1;
        if (d >= len) { put_text(o, "&", 1); return 1; }
        unsigned long cp = 0;
        for (; d < len; d++) {
            int c = (unsigned char)name[d], v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (hex && c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else { put_text(o, "&", 1); return 1; }
            cp = cp * (hex ? 16 : 10) + (unsigned long)v;
            if (cp > 0x10FFFF) cp = 0x110000;          /* saturate: invalid either way */
        }
        if (cp == 0x20 || cp == 0xA0) o->pending_space = 1;
        else put_utf8(o, cp);
        return end - i + 1;
    }
    for (size_t k = 0; k < sizeof ENTITIES / sizeof ENTITIES[0]; k++) {
        if (strlen(ENTITIES[k].name) == len && memcmp(ENTITIES[k].name, name, len) == 0) {
            if (strcmp(ENTITIES[k].name, "nbsp") == 0) o->pending_space = 1;
            else put_text(o, ENTITIES[k].text, strlen(ENTITIES[k].text));
            return end - i + 1;
        }
    }
    put_text(o, "&", 1);
    return 1;
}

static const char *find_ci(const char *in, size_t n, size_t from, const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = from; i + k <= n; i++) {
        size_t j = 0;
        while (j < k && tolower((unsigned char)in[i + j]) == needle[j]) j++;
        if (j == k) return in + i;
    }
    return NULL;
}

static int is_one_of(const char *name, const char *const *list) {
    for (; *list; list++) if (strcmp(name, *list) == 0) return 1;
    return 0;
}

static const char *const BLOCKS[] = { "p", "div", "section", "article", "header", "footer",
    "nav", "aside", "main", "ul", "ol", "dl", "table", "blockquote", "form", "hr",
    "figure", "figcaption", NULL };

/* Apply a tag's effect on the text. */
static void tag(out_t *o, const char *name, int closing) {
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        block_break(o, 2);
        if (!closing) {
            put(o, "######", (size_t)(name[1] - '0'));
            put(o, " ", 1);
        }
    } else if (strcmp(name, "pre") == 0) {
        block_break(o, 2);
        o->in_pre += closing ? (o->in_pre > 0 ? -1 : 0) : 1;
    } else if (strcmp(name, "li") == 0) {
        block_break(o, 1);
        if (!closing) put(o, "- ", 2);
    } else if (is_one_of(name, BLOCKS)) {
        block_break(o, 2);
    } else if (strcmp(name, "br") == 0 || strcmp(name, "tr") == 0 ||
               strcmp(name, "dt") == 0 || strcmp(name, "dd") == 0) {
        block_break(o, 1);
    } else if (strcmp(name, "td") == 0 || strcmp(name, "th") == 0) {
        o->pending_space = 1;
    }
}

/* At in[i] == '<'. Returns the index just past what was consumed. */
static size_t markup(out_t *o, const char *in, size_t n, size_t i) {
    if (n - i >= 4 && memcmp(in + i, "<!--", 4) == 0) {
        const char *e = find_ci(in, n, i + 4, "-->");
        return e ? (size_t)(e - in) + 3 : n;
    }
    size_t j = i + 1;
    if (j < n && (in[j] == '!' || in[j] == '?')) {
        const char *e = memchr(in + j, '>', n - j);
        return e ? (size_t)(e - in) + 1 : n;
    }
    int closing = j < n && in[j] == '/';
    if (closing) j++;
    char name[MAX_TAG_NAME + 1];
    size_t len = 0;
    /* A tag name starts with an ASCII letter ("<3" is text), then
       letters or digits ("h1"). */
    while (j < n && (len == 0 ? isalpha((unsigned char)in[j]) : isalnum((unsigned char)in[j]))) {
        if (len < MAX_TAG_NAME) name[len++] = (char)tolower((unsigned char)in[j]);
        j++;
    }
    name[len] = '\0';
    if (len == 0) {                       /* "a < b": a literal bracket */
        put_text(o, "<", 1);
        return i + 1;
    }
    char quote = 0;                       /* find the '>' outside attribute quotes */
    for (; j < n; j++) {
        if (quote) { if (in[j] == quote) quote = 0; }
        else if (in[j] == '"' || in[j] == '\'') quote = in[j];
        else if (in[j] == '>') break;
    }
    if (j >= n) return n;                 /* unterminated tag: drop the rest */
    j++;
    if (!closing && (strcmp(name, "script") == 0 || strcmp(name, "style") == 0)) {
        const char *e = find_ci(in, n, j, strcmp(name, "script") == 0 ? "</script" : "</style");
        if (!e) return n;
        const char *gt = memchr(e, '>', (size_t)(in + n - e));
        return gt ? (size_t)(gt - in) + 1 : n;
    }
    tag(o, name, closing);
    return j;
}

char *html_to_text(const char *in, size_t n, size_t *out_len) {
    out_t o = { 0 };
    put(&o, "", 0);
    for (size_t i = 0; i < n && !o.oom;) {
        char c = in[i];
        if (c == '<') {
            i = markup(&o, in, n, i);
        } else if (c == '&') {
            i += entity(&o, in, n, i);
        } else if (o.in_pre) {
            put(&o, &c, 1);
            i++;
        } else if (isspace((unsigned char)c)) {
            o.pending_space = 1;
            i++;
        } else {
            put_text(&o, &c, 1);
            i++;
        }
    }
    while (o.n > 0 && (o.p[o.n - 1] == ' ' || o.p[o.n - 1] == '\n')) o.n--;
    if (o.oom) { free(o.p); return NULL; }
    o.p[o.n] = '\0';
    if (out_len) *out_len = o.n;
    return o.p;
}

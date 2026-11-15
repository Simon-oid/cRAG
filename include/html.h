#ifndef HTML_H
#define HTML_H

#include <stddef.h>

/* HTML to the text cRAG indexes. Tags are removed; <script>, <style>,
   comments and doctypes are dropped with their content; <h1>..<h6>
   become Markdown headings ("## Title") so the Markdown chunker splits on
   them; block elements become blank lines and <li> a "- " item;
   whitespace collapses except inside <pre>; character references are
   decoded to UTF-8 (an invalid code point becomes U+FFFD, an unknown
   name stays as written). Not a full HTML5 parser: no DOM, no CSS.

   Returns a malloc'd NUL-terminated string (length in *out_len), or NULL
   when out of memory. */
char *html_to_text(const char *in, size_t n, size_t *out_len);

#endif

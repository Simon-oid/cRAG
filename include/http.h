#ifndef HTTP_H
#define HTTP_H

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} http_buf_t;

/* Returns HTTP status code, or -1 on curl error. Response written to buf (caller frees buf->data). */
int http_post_json(const char *url, const char **headers, const char *body,
                   http_buf_t *buf);

#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <curl/curl.h>
#include "http.h"

#define HTTP_MAX_RESPONSE (256u * 1024u * 1024u)   /* 256 MB hard cap */

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    http_buf_t *b = userdata;
    if (size && nmemb > SIZE_MAX / size) return 0;
    size_t n = size * nmemb;
    if (n > SIZE_MAX - b->len - 1) return 0;
    if (b->len + n > HTTP_MAX_RESPONSE) return 0;
    if (b->len + n + 1 > b->cap) {
        size_t newcap = b->cap ? b->cap : 4096;
        while (newcap < b->len + n + 1) {
            if (newcap > SIZE_MAX / 2) return 0;
            newcap *= 2;
        }
        char *tmp = realloc(b->data, newcap);
        if (!tmp) return 0;
        b->data = tmp;
        b->cap  = newcap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

int http_post_json(const char *url, const char **headers, const char *body,
                   http_buf_t *buf) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    buf->data = NULL;
    buf->len  = 0;
    buf->cap  = 0;

    struct curl_slist *slist = NULL;
    slist = curl_slist_append(slist, "Content-Type: application/json");
    if (headers) {
        for (int i = 0; headers[i]; i++)
            slist = curl_slist_append(slist, headers[i]);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);
    long status = -1;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(slist);
    curl_easy_cleanup(curl);
    return (int)status;
}

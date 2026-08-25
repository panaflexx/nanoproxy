#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include "stringbuf.h"
#ifndef INCLUDE_STB_DS_H
#  include "stb_ds.h"
#endif
#include "logger.h"

#define MAX_HEADERS 128
#ifndef MAX_BODY_SIZE
#define MAX_BODY_SIZE (64 << 20)  // 64 MiB default (override with -DMAX_BODY_SIZE=...)
#endif
#ifndef MAX_CHUNK_SIZE
#define MAX_CHUNK_SIZE (1 << 20)  // 1 MiB (override with -DMAX_CHUNK_SIZE=...)
#endif
#define MAX_URL_SIZE 4096
#define MAX_METHOD_SIZE 16
#define MAX_HTTP_VERSION_SIZE 8
#define MAX_HEADER_NAME_SIZE 256
#define MAX_HEADER_VALUE_SIZE 4096
#define MAX_HEADER_LINE_SIZE 8192

#ifdef HTTP_PARSE_DEBUG
#  define PARSE_ERROR(msg) do { size_t __bl = stringbuf_size(&parser->buffer); LOG_ERROR(HTTP, "parse: %s (fd=%d pos=%zu buflen=%zu)", (msg), parser->fd, parser->pos, __bl); parser->state = HP_ERROR; return -1; } while(0)
#else
#  define PARSE_ERROR(msg) do { LOG_DEBUG(HTTP, "parse: %s (fd=%d)", (msg), parser->fd); parser->state = HP_ERROR; return -1; } while(0)
#endif

struct http_header {
    char *key;
    char *value;
};

struct http_request {
    char *method;
    char *uri;
    char *version;
    struct http_header *headers;
    char *body;
    size_t body_len;
};

struct http_response {
    int status_code;
    char *reason_phrase;
    struct http_header *headers;
    char *body;
    size_t body_len;
};

struct http_response_log {
    int status_code;
	size_t br, bw;
};

enum http_parser_state {
    HP_REQUEST_LINE,
    HP_HEADERS,
    HP_BODY,
    HP_DONE,
    HP_ERROR
};

typedef struct http_parser {
    enum http_parser_state state;
    StringBuf buffer;
    size_t pos;
    struct http_request req;
    size_t content_length;
    bool chunked;
    bool streaming_body;  // when true, return 2 at body start instead of buffering
	int fd; // for sockets
} http_p;


static inline void http_parser_init(struct http_parser *parser, int fd) {
    memset(parser, 0, sizeof(*parser));
    parser->state = HP_REQUEST_LINE;
    stringbuf_init(&parser->buffer, 0);
	parser->fd = fd;
}

/* shputs(parser->req.headers, ...) stores the exact key/value pointers we
 * strndup'd while parsing (no sh_new_strdup was ever called on this table),
 * so shfree() below -- which only releases stb_ds's own array/hash-index
 * memory -- leaves every header's key and value permanently leaked unless
 * we free them here first. */
static inline void http_parser_free_req(struct http_request *req) {
    for (ptrdiff_t i = 0; i < shlen(req->headers); i++) {
        free(req->headers[i].key);
        free(req->headers[i].value);
    }
    shfree(req->headers);
    free(req->method);
    free(req->uri);
    free(req->version);
    free(req->body);
}

static inline void http_parser_reset(struct http_parser *parser) {
    http_parser_free_req(&parser->req);
    stringbuf_free(&parser->buffer);
    http_parser_init(parser, parser->fd);
}

/* Final teardown (parser struct will not be reused) -- must NOT call
 * http_parser_init/http_parser_reset afterward, since that allocates a
 * fresh STRINGBUF_DEFAULT_CAP (64 KB) buffer that would then never be
 * freed: previously this just called http_parser_reset(), leaking exactly
 * that buffer on every proxied request (proxy_cleanup owns a per-request
 * proxy_state_t with its own parser) and on every closed connection. */
static inline void http_parser_destroy(struct http_parser *parser) {
    http_parser_free_req(&parser->req);
    stringbuf_free(&parser->buffer);
}

/* Drain any unconsumed data from the parser buffer (used for streaming body) */
static inline size_t http_parser_drain_body(struct http_parser *parser, const char **out) {
    size_t buflen = stringbuf_size(&parser->buffer);
    size_t avail = buflen - parser->pos;
    if (avail == 0) { *out = NULL; return 0; }
    *out = stringbuf_cstr(&parser->buffer) + parser->pos;
    parser->pos = buflen;  /* mark as consumed */
    return avail;
}

static inline int http_parser_feed(struct http_parser *parser, const char *data, size_t len) {
    if (!parser || !data) return -1;

    // Append incoming data to buffer
    if (!stringbuf_append(&parser->buffer, data, len)) {
        PARSE_ERROR("stringbuf_append failed");
    }

    const char *buf = stringbuf_cstr(&parser->buffer);
    size_t buflen = stringbuf_size(&parser->buffer);

    while (parser->state != HP_DONE && parser->state != HP_ERROR && parser->pos < buflen) {
        switch (parser->state) {
            case HP_REQUEST_LINE: {
                ssize_t line_end_pos = stringbuf_memchr(&parser->buffer, '\n', parser->pos);
                if (line_end_pos == -1) return 0; // Need more data

                size_t line_len = line_end_pos - parser->pos;
                if (line_len > MAX_HEADER_LINE_SIZE) {
                    PARSE_ERROR("request line too long");
                }
                const char *line_start = buf + parser->pos;
                if (line_end_pos > 0 && buf[line_end_pos - 1] == '\r') line_len--;

                ssize_t space1_pos = stringbuf_find(&parser->buffer, parser->pos, " ", 1);
                if (space1_pos == -1) {
                    PARSE_ERROR("missing space after method");
                }
                size_t method_len = space1_pos - parser->pos;
                if (method_len == 0 || method_len > MAX_METHOD_SIZE) {
                    PARSE_ERROR("bad method length");
                }
                parser->req.method = strndup(line_start, method_len);
                if (!parser->req.method) {
                    PARSE_ERROR("strndup method failed");
                }

                ssize_t space2_pos = stringbuf_find(&parser->buffer, space1_pos + 1, " ", 1);
                if (space2_pos == -1) {
                    PARSE_ERROR("missing space after URI");
                }
                size_t uri_len = space2_pos - (space1_pos + 1);
                if (uri_len == 0 || uri_len > MAX_URL_SIZE) {
                    PARSE_ERROR("bad URI length");
                }
                parser->req.uri = strndup(buf + space1_pos + 1, uri_len);
                if (!parser->req.uri) {
                    PARSE_ERROR("strndup URI failed");
                }

                size_t version_len = (parser->pos + line_len) - (space2_pos + 1);
                if (version_len == 0 || version_len > MAX_HTTP_VERSION_SIZE) {
                    PARSE_ERROR("bad HTTP version length");
                }
                parser->req.version = strndup(buf + space2_pos + 1, version_len);
                if (!parser->req.version) {
                    PARSE_ERROR("strndup version failed");
                }

                parser->pos = line_end_pos + 1;
                parser->state = HP_HEADERS;
                break;
            }
            case HP_HEADERS: {
                ssize_t line_end_pos = stringbuf_memchr(&parser->buffer, '\n', parser->pos);
                if (line_end_pos == -1) return 0; // Need more data

                size_t line_len = line_end_pos - parser->pos;
                if (line_len > MAX_HEADER_LINE_SIZE) {
                    PARSE_ERROR("header line too long");
                }
                const char *line_start = buf + parser->pos;
                if (line_end_pos > 0 && buf[line_end_pos - 1] == '\r') line_len--;

                if (line_len == 0) {
                    // End of headers
                    parser->pos = line_end_pos + 1;

                    ptrdiff_t cl_idx = shgeti(parser->req.headers, "content-length");
                    if (cl_idx >= 0) {
                        char *endptr;
                        errno = 0;
                        unsigned long long cl = strtoull(parser->req.headers[cl_idx].value, &endptr, 10);
                        if (errno == ERANGE || *endptr != '\0' || cl > MAX_BODY_SIZE) {
                            PARSE_ERROR("Content-Length invalid or too large");
                        }
                        parser->content_length = (size_t)cl;
                        parser->chunked = false;
                    } else {
                        ptrdiff_t te_idx = shgeti(parser->req.headers, "transfer-encoding");
                        if (te_idx >= 0 && strcasecmp(parser->req.headers[te_idx].value, "chunked") == 0) {
                            parser->chunked = true;
                            parser->content_length = 0;
                        } else {
                            parser->content_length = 0;
                        }
                    }

                    if (parser->content_length > 0 || parser->chunked) {
                        parser->state = HP_BODY;
                        if (parser->streaming_body) {
                            return 2;  // headers complete, body available for streaming
                        }
                    } else {
                        parser->state = HP_DONE;
                    }
                    break;
                }

                ssize_t colon_pos = stringbuf_memchr(&parser->buffer, ':', parser->pos);
                if (colon_pos == -1) {
                    PARSE_ERROR("missing colon in header");
                }

                size_t name_start = parser->pos;
                size_t name_end = colon_pos;
                while (name_start < name_end && isspace((unsigned char)buf[name_start])) ++name_start;
                while (name_end > name_start && isspace((unsigned char)buf[name_end - 1])) --name_end;

                size_t value_start = colon_pos + 1;
                size_t value_end = line_end_pos;
                while (value_start < value_end && isspace((unsigned char)buf[value_start])) ++value_start;
                while (value_end > value_start && isspace((unsigned char)buf[value_end - 1])) --value_end;

                size_t name_len = name_end - name_start;
                if (name_len == 0 || name_len > MAX_HEADER_NAME_SIZE) {
                    PARSE_ERROR("bad header name length");
                }
                char *key = strndup(buf + name_start, name_len);
                if (!key) {
                    PARSE_ERROR("strndup header name failed");
                }
                /* RFC 7230: header field names are case-insensitive — normalize to lowercase */
                for (size_t k = 0; k < name_len; k++) key[k] = tolower((unsigned char)key[k]);

                size_t value_len = value_end - value_start;
                if (value_len > MAX_HEADER_VALUE_SIZE) {
                    free(key);
                    PARSE_ERROR("header value too long");
                }
                char *value = strndup(buf + value_start, value_len);
                if (!value) {
                    free(key);
                    PARSE_ERROR("strndup header value failed");
                }

                struct http_header h = {.key = key, .value = value};
                shputs(parser->req.headers, h);
                if (shlen(parser->req.headers) > MAX_HEADERS) {
                    PARSE_ERROR("too many headers");
                }

                parser->pos = line_end_pos + 1;
                break;
            }
            case HP_BODY: {
                if (!parser->chunked) {
                    size_t remaining = parser->content_length - parser->req.body_len;
                    size_t avail = buflen - parser->pos;
                    if (avail == 0) return 0;

                    size_t consume = (remaining < avail) ? remaining : avail;
                    if (parser->req.body_len + consume > MAX_BODY_SIZE) {
                        PARSE_ERROR("body exceeds MAX_BODY_SIZE");
                    }
                    char *new_body = (char *)realloc(parser->req.body, parser->req.body_len + consume);
                    if (!new_body) {
                        PARSE_ERROR("realloc body failed");
                    }
                    parser->req.body = new_body;
                    memcpy(parser->req.body + parser->req.body_len, buf + parser->pos, consume);
                    parser->req.body_len += consume;
                    parser->pos += consume;

                    if (parser->req.body_len == parser->content_length) {
                        parser->state = HP_DONE;
                    }
                } else {
                    // Basic chunked parsing (no extensions, trailers)
                    while (parser->pos < buflen) {
                        if (parser->content_length == 0) { // Parse chunk size
                            ssize_t line_end_pos = stringbuf_memchr(&parser->buffer, '\n', parser->pos);
                            if (line_end_pos == -1) return 0;

                            size_t line_len = line_end_pos - parser->pos;
                            if (line_end_pos > 0 && buf[line_end_pos - 1] == '\r') line_len--;

                            char line[32];
                            if (line_len >= sizeof(line)) {
                                PARSE_ERROR("chunk-size line too long");
                            }
                            memcpy(line, buf + parser->pos, line_len);
                            line[line_len] = '\0';

                            char *semi = strchr(line, ';');
                            if (semi) *semi = '\0';

                            char *endptr;
                            errno = 0;
                            unsigned long long chunk_size = strtoull(line, &endptr, 16);
                            if (errno == ERANGE || *endptr != '\0' || chunk_size > MAX_CHUNK_SIZE || parser->req.body_len + chunk_size > MAX_BODY_SIZE) {
                                PARSE_ERROR("chunk size invalid or too large");
                            }
                            parser->content_length = (size_t)chunk_size;

                            if (parser->content_length == 0) {
                                parser->state = HP_DONE;
                                parser->pos = line_end_pos + 1;
                                break;
                            }
                            parser->pos = line_end_pos + 1;
                        } else { // Parse chunk data
                            size_t avail = buflen - parser->pos;
                            if (avail < parser->content_length + 2) return 0;

                            if (memcmp(buf + parser->pos + parser->content_length, "\r\n", 2) != 0) {
                                PARSE_ERROR("missing CRLF after chunk data");
                            }

                            char *new_body = (char *)realloc(parser->req.body, parser->req.body_len + parser->content_length);
                            if (!new_body) {
                                PARSE_ERROR("realloc chunk body failed");
                            }
                            parser->req.body = new_body;
                            memcpy(parser->req.body + parser->req.body_len, buf + parser->pos, parser->content_length);
                            parser->req.body_len += parser->content_length;
                            parser->pos += parser->content_length + 2;
                            parser->content_length = 0;
                        }
                    }
                }
                // Shift remaining buffer data left if any
                if (parser->pos > 0 && parser->pos < buflen) {
                    stringbuf_consume(&parser->buffer, parser->pos);
                    parser->pos = 0;
                }
                break;
            }
            default:
                break;
        }
    }
    if (parser->state == HP_DONE) return 1;
    if (parser->state == HP_ERROR) return -1;
    return 0;
}

static inline size_t http_build_response(const struct http_response *resp, char *buf, size_t buf_size) {
    char status_line[64];
    snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n", resp->status_code, resp->reason_phrase ? resp->reason_phrase : "");
    size_t len = strlen(status_line);
    if (len >= buf_size) return 0;
    memcpy(buf, status_line, len);

    struct http_header *headers = resp->headers;
    for (ptrdiff_t i = 0; i < shlen(headers); ++i) {
        char header_line[512];
        size_t hlen = snprintf(header_line, sizeof(header_line), "%s: %s\r\n", headers[i].key, headers[i].value);
        if (len + hlen >= buf_size) return 0;
        memcpy(buf + len, header_line, hlen);
        len += hlen;
    }

    char cl_header[64];
    size_t cl_len = snprintf(cl_header, sizeof(cl_header), "Content-Length: %zu\r\n", resp->body_len);
    if (len + cl_len >= buf_size) return 0;
    memcpy(buf + len, cl_header, cl_len);
    len += cl_len;

    if (len + 2 >= buf_size) return 0;
    memcpy(buf + len, "\r\n", 2);
    len += 2;

    if (resp->body_len > 0 && resp->body) {
        if (len + resp->body_len >= buf_size) return 0;
        memcpy(buf + len, resp->body, resp->body_len);
        len += resp->body_len;
    }

    return len;
}

static inline bool http_should_keep_alive(struct http_request *req) {
    ptrdiff_t conn_idx = shgeti(req->headers, "connection");
    bool is_http_11 = req->version && strcmp(req->version, "HTTP/1.1") == 0;
    if (is_http_11) {
        /* HTTP/1.1 defaults to keep-alive unless the client opts out. */
        if (conn_idx >= 0 && strcasecmp(req->headers[conn_idx].value, "close") == 0) return false;
        return true;
    }
    /* HTTP/1.0 defaults to close, but an explicit "Connection: keep-alive"
     * (e.g. ApacheBench's default request) asks to keep the socket open. */
    return conn_idx >= 0 && strcasecmp(req->headers[conn_idx].value, "keep-alive") == 0;
}

#endif // HTTP_PARSER_H


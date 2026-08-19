#ifndef REQUEST_H
#define REQUEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include "http.h"
#include "socket_server.h"
#ifdef HAVE_ZLIB
#  include <zlib.h>
#endif

/*
 * Proxy/dispatch debug logging — now uses the structured logger.
 * Set proxy log level to DEBUG or TRACE via config to enable.
 * The old -DPROXY_DEBUG flag is no longer needed.
 */
#define PROXY_LOG(fmt, ...) LOG_DEBUG(PROXY, fmt, ##__VA_ARGS__)

#ifndef INCLUDE_STB_DS_H
#  include "stb_ds.h"
#endif
#ifdef __linux__
#  include <sys/sendfile.h>
#endif

#ifndef DEFAULT_PROXY_TIMEOUT
#define DEFAULT_PROXY_TIMEOUT 5       /* seconds to wait for upstream connect */
#endif
#ifndef DEFAULT_RETRY_TIMEOUT
#define DEFAULT_RETRY_TIMEOUT 30      /* seconds before retrying a dead target */
#endif

//#define CHUNK_SIZE 4096

#ifdef HAVE_ZLIB
#ifndef GZIP_MAX_SIZE
#define GZIP_MAX_SIZE (5 * 1024 * 1024)  /* max file size for in-memory gzip (5 MiB) */
#endif
#ifndef GZIP_MIN_SIZE
#define GZIP_MIN_SIZE 256                 /* skip gzip for tiny responses */
#endif

/* Portable NOSIGNAL flag */
#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

static inline bool is_compressible_mime(const char *mime) {
    if (strncmp(mime, "text/", 5) == 0) return true;
    if (strcmp(mime, "application/javascript") == 0) return true;
    if (strcmp(mime, "application/json") == 0) return true;
    if (strcmp(mime, "application/xml") == 0) return true;
    if (strcmp(mime, "image/svg+xml") == 0) return true;
    return false;
}

/* Compress buf into gzip format.  Returns malloc'd output; caller frees.
   Returns NULL on any failure (caller should fall back to uncompressed). */
static inline unsigned char *gzip_compress_buf(const unsigned char *in, size_t in_len, size_t *out_len) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    /* windowBits 15+16 = gzip wrapper */
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return NULL;
    size_t bound = deflateBound(&strm, in_len);
    unsigned char *out = (unsigned char *)malloc(bound);
    if (!out) { deflateEnd(&strm); return NULL; }
    strm.next_in  = (unsigned char *)in;
    strm.avail_in = in_len;
    strm.next_out = out;
    strm.avail_out = bound;
    int ret = deflate(&strm, Z_FINISH);
    *out_len = strm.total_out;
    deflateEnd(&strm);
    if (ret != Z_STREAM_END) { free(out); return NULL; }
    return out;
}
#endif /* HAVE_ZLIB */

struct path_entry {
    char *key;
    http_handler_t value;
};

struct base_entry {
    char *key;
    struct path_entry *value;
};

struct socket_handler_entry {
    char *key;
    socket_handler_t value;
};

struct Location {
    char *real_path;
    uid_t user;
    gid_t group;
};

typedef struct {
    char *key;
    struct Location value;
} LocationEntry;

extern struct base_entry *base_handlers;
extern struct path_entry *global_http_handlers;
extern struct socket_handler_entry *socket_handler_map;
extern LocationEntry *locations;
extern struct event_handlers handlers;

static inline void resume_send(int loopfd, int fd);

static inline void addLocation(const char *prefix, const char *real_path, uid_t user, gid_t group) {
    char *p = strdup(prefix);
    if (!p) return;
    char *rp = strdup(real_path);
    if (!rp) {
        free(p);
        return;
    }
    struct Location loc = {.real_path = rp, .user = user, .group = group};
    hmputs(locations, ((LocationEntry){.key = p, .value = loc}) );
}

static inline void default_access_log(struct client_info *info, const char *action, int status, size_t br, size_t bw) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%d/%b/%Y:%H:%M:%S %z", tm);
    np_access_log("%s - - [%s] \"%s\" %d %zu %zu\n", info->addr, timebuf, action, status, br, bw);
}

static inline void default_error_log(const char *msg) {
    LOG_ERROR(SERVER, "%s", msg);
}


static inline void http_add_connection_header(struct http_response *resp, bool keep_alive) {
    struct http_header conn = {
        .key = "Connection",
        .value = keep_alive ? "keep-alive" : "close"
    };
    // Avoid duplicates
    ptrdiff_t idx = shgeti(resp->headers, "Connection");
    if (idx >= 0) {
        free(resp->headers[idx].value);
        resp->headers[idx].value = strdup(conn.value);
    } else {
        shputs(resp->headers, conn);
    }
}

static inline struct http_response_log http_ok(http_p *p, struct http_request *req, const char *content,
												ssize_t content_length, const char *content_type)
{
    struct http_response resp = {
        .status_code = 200,
        .reason_phrase = "OK",
        .body = NULL,
        .body_len = content_length
    };
    struct http_header ct = {.key = "Content-Type", .value = (char *)content_type};
    shputs(resp.headers, ct);
    struct http_header server = {.key = "Server", .value = "NanoServer/0.1"};
    shputs(resp.headers, server);
	http_add_connection_header(&resp, http_should_keep_alive(req));

    char resp_buf[2048];
    size_t resp_len = http_build_response(&resp, resp_buf, sizeof(resp_buf));
    if (resp_len > 0) {
        socket_write(p->fd, resp_buf, resp_len);
        if (content_length > 0 && content) {
            socket_write(p->fd, content, content_length);
        }
    }
    shfree(resp.headers);
	return ((struct http_response_log){ .status_code = 200, .br=0, .bw=resp_len});
}

static inline struct http_response_log http_error(http_p *p, int status_code, const char *reason_phrase) {
    struct http_response resp = {
        .status_code = status_code,
        .reason_phrase = (char *)reason_phrase,
        .body = NULL,
        .body_len = 0
    };
    struct http_header server = {.key = "Server", .value = "NanoServer/0.1"};
    shputs(resp.headers, server);

    char resp_buf[2048];
    size_t resp_len = http_build_response(&resp, resp_buf, sizeof(resp_buf));
    if (resp_len > 0) {
        socket_write(p->fd, resp_buf, resp_len);
    }
    shfree(resp.headers);

	return ((struct http_response_log){ .status_code = status_code, .br=0, .bw=resp_len});
}

static inline void default_http_handler(http_p *p, struct http_request *req, struct client_data *cd) {
    char body[512];
    ssize_t body_len = snprintf(body, sizeof(body),
							"Welcome! Received request:\nMethod: %s\nURI: %s\nVersion: %s\nBody length: %zu\n",
                            req->method ? req->method : "", req->uri ? req->uri : "",
							req->version ? req->version : "", req->body_len);
    http_ok(p, req, body, body_len, "text/plain");
}

static inline void hello_http_handler(http_p *p, struct http_request *req, struct client_data *cd) {
    char body[512];
    ssize_t body_len = snprintf(body, sizeof(body),
							"HELLO! Received request:\nMethod: %s\nURI: %s\nVersion: %s\nBody length: %zu\n",
                            req->method ? req->method : "", req->uri ? req->uri : "",
							req->version ? req->version : "", req->body_len);
    http_ok(p, req, body, body_len, "text/plain");
}

static inline const char *get_mime_type(const char *path) {
    static struct { const char *ext; const char *type; } mimes[] = {
        {".html", "text/html"},
        {".htm", "text/html"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".json", "application/json"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".svg", "image/svg+xml"},
        {".txt", "text/plain"},
        {".mp4", "video/mp4"},
        {NULL, NULL}
    };

    size_t path_len = strlen(path);
    for (int i = 0; mimes[i].ext; ++i) {
        size_t ext_len = strlen(mimes[i].ext);
        if (path_len >= ext_len && strcasecmp(path + path_len - ext_len, mimes[i].ext) == 0) {
            return mimes[i].type;
        }
    }
    return "application/octet-stream";
}

static inline void http_static_dir(http_p *p, struct http_request *req, struct client_data *cd) {
    bool is_head = (strcmp(req->method, "HEAD") == 0);
    if (strcmp(req->method, "GET") != 0 && !is_head) {
        cd->log_status = 405;
        http_error(p, 405, "Method Not Allowed");
        return;
    }
    const char *prefix = cd->matched_prefix;
    if (!prefix) {
        cd->log_status = 500;
        http_error(p, 500, "Internal Server Error");
        return;
    }
    /* Find location by string content (locations uses pointer-keyed hm map) */
    ptrdiff_t loc_idx = -1;
    for (int i = 0; i < hmlen(locations); i++) {
        if (strcmp(locations[i].key, prefix) == 0) {
            loc_idx = i;
            break;
        }
    }
    const char *root;
    if (loc_idx >= 0) {
        root = locations[loc_idx].value.real_path;
    } else {
        root = "./public";
    }
    const char *subpath = req->uri + strlen(prefix);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", root, subpath);
    LOG_DEBUG(HTTP, "prefix=%s root=%s subpath=%s -> path=%s", prefix, root, subpath, path);
    if (strlen(path) >= PATH_MAX - 1) {
        cd->log_status = 414;
        http_error(p, 414, "URI Too Long");
        return;
    }
    /* Resolve root to an absolute path for the traversal check */
    char real_root[PATH_MAX];
    if (realpath(root, real_root) == NULL) {
        cd->log_status = 500;
        http_error(p, 500, "Internal Server Error");
        return;
    }
    size_t real_root_len = strlen(real_root);

    char real_path[PATH_MAX];
    if (realpath(path, real_path) == NULL) {
        cd->log_status = 404;
        http_error(p, 404, "Not Found");
        if (cd->log_action) {
            default_access_log(&cd->info, cd->log_action, cd->log_status, 0, 0);
            free(cd->log_action);
            cd->log_action = NULL;
        }
        return;
    }

    /* Path traversal guard: resolved path must be inside the root directory */
    if (strncmp(real_path, real_root, real_root_len) != 0 ||
        (real_path[real_root_len] != '/' && real_path[real_root_len] != '\0')) {
        LOG_WARN(HTTP, "path traversal blocked: %s is outside %s", real_path, real_root);
        cd->log_status = 403;
        http_error(p, 403, "Forbidden");
        if (cd->log_action) {
            default_access_log(&cd->info, cd->log_action, cd->log_status, 0, 0);
            free(cd->log_action);
            cd->log_action = NULL;
        }
        return;
    }

    struct stat st;
    if (stat(real_path, &st) < 0) {
        cd->log_status = 404;
        http_error(p, 404, "Not Found");
        if (cd->log_action) {
            default_access_log(&cd->info, cd->log_action, cd->log_status, 0, 0);
            free(cd->log_action);
            cd->log_action = NULL;
        }
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        char index_path[PATH_MAX+128];
        snprintf(index_path, sizeof(index_path), "%s/index.html", real_path);
        if (stat(index_path, &st) < 0) {
            http_error(p, 404, "Not Found");
            return;
        }
        strncpy(real_path, index_path, sizeof(real_path) - 1);
        real_path[sizeof(real_path) - 1] = '\0';
    }
    if (!S_ISREG(st.st_mode)) {
        cd->log_status = 403;
        http_error(p, 403, "Forbidden");
        return;
    }
    int file_fd = open(real_path, O_RDONLY);
    if (file_fd < 0) {
        cd->log_status = 500;
        http_error(p, 500, "Internal Server Error");
        return;
    }
    const char *mime = get_mime_type(real_path);

    // Range request support (simple single-range bytes=START-END)
    off_t range_start = 0;
    off_t range_end = st.st_size - 1;
    bool is_range = false;
    const char *range_val = NULL;
    {
        ptrdiff_t ri = shgeti(req->headers, "range");
        if (ri >= 0) range_val = req->headers[ri].value;
    }
    if (range_val) {
        if (strncmp(range_val, "bytes=", 6) == 0) {
            char *dash = strchr(range_val + 6, '-');
            if (dash) {
                range_start = atoll(range_val + 6);
                if (dash[1]) range_end = atoll(dash + 1);
                if (range_start >= 0 && range_end >= range_start && range_end < st.st_size) {
                    is_range = true;
                }
            }
        }
    }

    if (is_range) {
        if (lseek(file_fd, range_start, SEEK_SET) == (off_t)-1) {
            close(file_fd);
            cd->log_status = 500;
            http_error(p, 500, "Internal Server Error");
            return;
        }
    }
    size_t content_len = is_range ? (range_end - range_start + 1) : st.st_size;
    struct http_response resp = {
        .status_code = is_range ? 206 : 200,
        .reason_phrase = is_range ? "Partial Content" : "OK",
        .body = NULL,
        .body_len = content_len
    };
    struct http_header ct = {.key = "Content-Type", .value = (char *)mime};
    shputs(resp.headers, ct);
    struct http_header server = {.key = "Server", .value = "NanoServer/0.1"};
    shputs(resp.headers, server);
    struct http_header ar = {.key = "Accept-Ranges", .value = "bytes"};
    shputs(resp.headers, ar);
    char *cr_copy = NULL;
    if (is_range) {
        char cr[128];
        snprintf(cr, sizeof(cr), "bytes %lld-%lld/%lld", (long long)range_start, (long long)range_end, (long long)st.st_size);
        cr_copy = strdup(cr);
        if (cr_copy) {
            struct http_header crh = {.key = "Content-Range", .value = cr_copy};
            shputs(resp.headers, crh);
        }
    }
    http_add_connection_header(&resp, http_should_keep_alive(req));
    char resp_buf[2048];
    size_t resp_len = http_build_response(&resp, resp_buf, sizeof(resp_buf));
    if (cr_copy) free(cr_copy);
    shfree(resp.headers);
    if (resp_len == 0) {
        close(file_fd);
        cd->log_status = 500;
        http_error(p, 500, "Internal Server Error");
        return;
    }
#ifdef HAVE_ZLIB
    /* ── Gzip compression for compressible MIME types ─────────────────── */
    bool do_gzip = false;
    if (!is_range && !is_head && is_compressible_mime(mime)
        && (size_t)st.st_size >= GZIP_MIN_SIZE && (size_t)st.st_size <= GZIP_MAX_SIZE) {
        ptrdiff_t ae_idx = shgeti(req->headers, "accept-encoding");
        if (ae_idx >= 0 && strstr(req->headers[ae_idx].value, "gzip"))
            do_gzip = true;
    }

    if (do_gzip) {
        unsigned char *file_buf = (unsigned char *)malloc(st.st_size);
        if (file_buf) {
            ssize_t nread = read(file_fd, file_buf, st.st_size);
            if (nread == st.st_size) {
                size_t gz_len = 0;
                unsigned char *gz = gzip_compress_buf(file_buf, st.st_size, &gz_len);
                if (gz && gz_len < (size_t)st.st_size) {
                    free(file_buf);
                    close(file_fd);

                    /* Rebuild response with compressed Content-Length */
                    shfree(resp.headers); resp.headers = NULL;
                    resp.body_len = gz_len;
                    struct http_header ct2 = {.key = "Content-Type", .value = (char *)mime};
                    shputs(resp.headers, ct2);
                    struct http_header sv2 = {.key = "Server", .value = "NanoServer/0.1"};
                    shputs(resp.headers, sv2);
                    struct http_header ar2 = {.key = "Accept-Ranges", .value = "bytes"};
                    shputs(resp.headers, ar2);
                    struct http_header ce  = {.key = "Content-Encoding", .value = "gzip"};
                    shputs(resp.headers, ce);
                    struct http_header vy  = {.key = "Vary", .value = "Accept-Encoding"};
                    shputs(resp.headers, vy);
                    http_add_connection_header(&resp, http_should_keep_alive(req));

                    char gz_resp_buf[2048];
                    size_t gz_resp_len = http_build_response(&resp, gz_resp_buf, sizeof(gz_resp_buf));
                    shfree(resp.headers);
                    if (gz_resp_len == 0) {
                        free(gz);
                        cd->log_status = 500;
                        http_error(p, 500, "Internal Server Error");
                        return;
                    }
                    socket_write(p->fd, gz_resp_buf, gz_resp_len);

                    /* Async-send the compressed body from memory (no file fd) */
                    cd->sending_body = true;
                    cd->send_file_fd = -1;
                    cd->send_offset  = 0;
                    cd->send_remaining = gz_len;
                    cd->use_sendfile = false;
                    cd->send_buffer  = (char *)gz;  /* takes ownership */
                    cd->send_buf_len = gz_len;
                    cd->send_buf_pos = 0;
                    resume_send(cd->loopfd, cd->fd);
                    return;
                }
                free(gz); /* compressed was larger — fall through to uncompressed */
            }
            free(file_buf);
        }
        /* On any failure, fall through to the normal uncompressed path */
        lseek(file_fd, 0, SEEK_SET);
    }
#endif /* HAVE_ZLIB */

    socket_write(p->fd, resp_buf, resp_len);

    /* HEAD responses: headers already sent, skip the body */
    if (is_head) {
        close(file_fd);
        if (http_should_keep_alive(&cd->parser.req)) {
            http_parser_reset(&cd->parser);
            event_mod(cd->loopfd, cd->fd, EV_READ);
        } else {
            conn_del(cd->fd);
        }
        return;
    }

    // Setup async sending
    cd->sending_body = true;
    cd->send_file_fd = file_fd;
    cd->send_offset = is_range ? range_start : 0;
    cd->send_remaining = content_len;
    cd->use_sendfile = (cd->ssl == NULL);  // Use sendfile only if no SSL
    if (is_range) cd->log_status = 206;

    cd->send_buffer = malloc(CHUNK_SIZE);
    if (!cd->send_buffer) {
        cd->log_status = 500;
        http_error(p, 500, "Internal Server Error");
        close(file_fd);
        return;
    }
    cd->send_buf_len = 0;
    cd->send_buf_pos = 0;
    // Initial attempt to send
    resume_send(cd->loopfd, cd->fd);
}




static inline void http_dispatcher(http_p *p, struct http_request *req, struct client_data *info) {
    char action[512];
    snprintf(action, sizeof(action), "%s %s %s", req->method ? req->method : "", req->uri ? req->uri : "", req->version ? req->version : "");

    bool keep_alive = http_should_keep_alive(req);
	//printf("keep_alive = %s\n", keep_alive?"TRUE":"FALSE");

    /* Longest-prefix match across per-listener and global handlers (nginx/caddy style) */
    char *best_key = NULL;
    size_t best_len = 0;
    http_handler_t best_h = NULL;

    PROXY_LOG("[DISPATCH] uri=%s listen=%s\n", req->uri ? req->uri : "(null)", info->listen_uri ? info->listen_uri : "(null)");

    /* Per-listener handlers */
    ptrdiff_t base_idx = shgeti(base_handlers, info->listen_uri);
    if (base_idx >= 0) {
        struct path_entry *paths = base_handlers[base_idx].value;
        for (ptrdiff_t j = 0; j < shlen(paths); ++j) {
            char *key = paths[j].key;
            size_t klen = strlen(key);
            bool match = (key[klen-1] == '/') ? (strncmp(req->uri, key, klen) == 0)
                                              : (strcmp(req->uri, key) == 0);
            PROXY_LOG("[DISPATCH]   base[%s] key=%s klen=%zu match=%d\n", info->listen_uri, key, klen, match);
            if (match && klen > best_len) {
                best_len = klen;
                best_key = key;
                best_h = paths[j].value;
            }
        }
    }

    /* Global handlers (e.g. reverse proxy) */
    for (ptrdiff_t j = 0; j < shlen(global_http_handlers); ++j) {
        char *key = global_http_handlers[j].key;
        size_t klen = strlen(key);
        bool match;
        if (key[klen - 1] == '/') {
            match = (strncmp(req->uri, key, klen) == 0);
        } else {
            match = (strncmp(req->uri, key, klen) == 0) &&
                    (req->uri[klen] == '/' || req->uri[klen] == '?' || req->uri[klen] == '\0');
        }
        PROXY_LOG("[DISPATCH]   global key=%s klen=%zu match=%d\n", key, klen, match);
        if (match && klen > best_len) {
            best_len = klen;
            best_key = key;
            best_h = global_http_handlers[j].value;
        }
    }
    PROXY_LOG("[DISPATCH]   -> best=%s len=%zu handler=%p\n", best_key ? best_key : "(null)", best_len, best_h);

    if (best_h) {
        info->matched_prefix = best_key;
        info->log_action = strdup(action);
        info->log_status = 200;
        best_h(p, req, info);
    } else {
        info->matched_prefix = NULL;
        info->log_action = strdup(action);
        info->log_status = 200;
        default_http_handler(p, req, info);
    }

    // Post-process: add Connection header and decide on persistence
    // Note: This assumes the handler has already sent the response via http_ok/http_error.
    // Since handlers build/send immediately, we can't modify the response here.
    // Instead, rely on handlers to use keep-alive if needed, or add to next step.

    // For persistence: reset parser if keep-alive, else close.
    // Skip if proxy streaming is active — the proxy manages its own lifecycle.
    if (info->proxy_active) {
        /* Proxy owns this connection — don't reset parser or close */
    } else if (keep_alive) {
        http_parser_reset(p);
    } else if (!info->sending_body || info->send_remaining == 0) {
        conn_del(p->fd);
    }
}

static inline void resume_send(int loopfd, int fd) {
    int idx = get_conn(fd);
    if (idx == -1) return;
    struct client_data *cd = &clients[idx];

    if (!cd->sending_body || cd->send_remaining == 0) return;

    ssize_t sent = 0;

    if (cd->use_sendfile) {
#ifdef __APPLE__
        off_t len = cd->send_remaining;
        int ret = sendfile(cd->send_file_fd, cd->fd, cd->send_offset, &len, NULL, 0);
        sent = len;                    // macOS returns bytes sent in len
        if (ret == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                my_on_error(cd->loopfd, cd->fd, errno, NULL);
                cleanup_send_state(cd);
                conn_del(cd->fd);
                return;
            }
            sent = 0;  // EAGAIN
        }
        // Always update offset on macOS
        cd->send_offset += sent;
#else
        // Linux
        sent = sendfile(cd->fd, cd->send_file_fd, &cd->send_offset, cd->send_remaining);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                sent = 0;
            } else {
                if (cd->log_action) {
                    default_access_log(&cd->info, cd->log_action, cd->log_status, 0, cd->bytes_written);
                    free(cd->log_action);
                    cd->log_action = NULL;
                }
                my_on_error(cd->loopfd, cd->fd, errno, NULL);
                cleanup_send_state(cd);
                conn_del(cd->fd);
                return;
            }
        }
#endif
    } else {
        // ... existing non-sendfile (gzip / buffered) code remains unchanged ...
        if (cd->send_buf_pos == cd->send_buf_len) {
            if (cd->send_file_fd < 0) {
                // in-memory buffer complete
                if (cd->log_action) {
                    default_access_log(&cd->info, cd->log_action, cd->log_status, 0, cd->bytes_written);
                    free(cd->log_action);
                    cd->log_action = NULL;
                }
                cleanup_send_state(cd);
                event_mod(cd->loopfd, cd->fd, EV_READ);
                if (http_should_keep_alive(&cd->parser.req)) {
                    http_parser_reset(&cd->parser);
                } else {
                    conn_del(cd->fd);
                }
                return;
            }
            ssize_t r = read(cd->send_file_fd, cd->send_buffer, CHUNK_SIZE);
            if (r <= 0) {
                if (cd->log_action) {
                    default_access_log(&cd->info, cd->log_action, cd->log_status, 0, cd->bytes_written);
                    free(cd->log_action);
                    cd->log_action = NULL;
                }
                if (r < 0) my_on_error(cd->loopfd, cd->fd, errno, NULL);
                cleanup_send_state(cd);
                return;
            }
            cd->send_buf_len = r;
            cd->send_buf_pos = 0;
        }
        sent = socket_write(cd->fd, cd->send_buffer + cd->send_buf_pos, cd->send_buf_len - cd->send_buf_pos);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                sent = 0;
            } else {
                if (cd->log_action) {
                    default_access_log(&cd->info, cd->log_action, cd->log_status, 0, cd->bytes_written);
                    free(cd->log_action);
                    cd->log_action = NULL;
                }
                my_on_error(cd->loopfd, cd->fd, errno, NULL);
                cleanup_send_state(cd);
                conn_del(cd->fd);
                return;
            }
        }
        cd->send_buf_pos += sent;
    }

    cd->bytes_written += sent;
    cd->send_remaining -= sent;

    if (cd->send_remaining == 0) {
        if (cd->log_action) {
            default_access_log(&cd->info, cd->log_action, cd->log_status, 0, cd->bytes_written);
            free(cd->log_action);
            cd->log_action = NULL;
        }
        cleanup_send_state(cd);
        event_mod(cd->loopfd, cd->fd, EV_READ);
        if (http_should_keep_alive(&cd->parser.req)) {
            http_parser_reset(&cd->parser);
        } else {
            conn_del(cd->fd);
        }
    } else {
        event_mod(cd->loopfd, cd->fd, EV_WRITE);
    }
}

static inline void resume_send_orig(int loopfd, int fd) {
	int idx = get_conn(fd);
    if (idx == -1) return;
    struct client_data *cd = &clients[idx];

    if (!cd->sending_body || cd->send_remaining == 0) return;
    //printf("resume_send: sendfile=%s remaining=%zu    \r", cd->use_sendfile ? "TRUE" : "FALSE", cd->send_remaining);

    ssize_t sent = 0;
    if (cd->use_sendfile) {
#ifdef __APPLE__
        off_t len = cd->send_remaining;
        int ret = sendfile(cd->send_file_fd, cd->fd, cd->send_offset, &len, NULL, 0);
        sent = len;
        if (ret == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                my_on_error(cd->loopfd, cd->fd, errno, NULL);
                cleanup_send_state(cd);
                conn_del(cd->fd);
                return;
            }
            // For EAGAIN, proceed with sent = len (possibly 0 or partial)
        }
        // For success (ret == 0), sent = len == remaining
#else
        sent = sendfile(cd->fd, cd->send_file_fd, &cd->send_offset, cd->send_remaining);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                sent = 0;
                // Proceed
				LOG_DEBUG(HTTP, "sendfile EAGAIN on fd=%d", cd->fd);
            } else {
                if (cd->log_action) {
                    default_access_log(&cd->info, cd->log_action, cd->log_status, 0, cd->bytes_written);
                    free(cd->log_action);
                    cd->log_action = NULL;
                }
                my_on_error(cd->loopfd, cd->fd, errno, NULL);
                cleanup_send_state(cd);
                conn_del(cd->fd);
                return;
            }
        }
#endif
    } else {
        if (cd->send_buf_pos == cd->send_buf_len) {
            /* Pre-filled in-memory buffer (e.g. gzip): no file to refill from */
            if (cd->send_file_fd < 0) {
                if (cd->log_action) {
                    default_access_log(&cd->info, cd->log_action, cd->log_status, 0, cd->bytes_written);
                    free(cd->log_action);
                    cd->log_action = NULL;
                }
                cleanup_send_state(cd);
                event_mod(cd->loopfd, cd->fd, EV_READ);
                if (http_should_keep_alive(&cd->parser.req)) {
                    http_parser_reset(&cd->parser);
                } else {
                    conn_del(cd->fd);
                }
                return;
            }
            ssize_t r = read(cd->send_file_fd, cd->send_buffer, CHUNK_SIZE);
            if (r <= 0) {
                if (cd->log_action) {
                    default_access_log(&cd->info, cd->log_action, cd->log_status, 0, cd->bytes_written);
                    free(cd->log_action);
                    cd->log_action = NULL;
                }
                if (r < 0) my_on_error(cd->loopfd, cd->fd, errno, NULL);
                cleanup_send_state(cd);
                return;
            }
            cd->send_buf_len = r;
            cd->send_buf_pos = 0;
        }
        sent = socket_write(cd->fd, cd->send_buffer + cd->send_buf_pos, cd->send_buf_len - cd->send_buf_pos);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                sent = 0;
				LOG_DEBUG(HTTP, "socket_write EAGAIN on fd=%d", cd->fd);
            } else {
                if (cd->log_action) {
                    default_access_log(&cd->info, cd->log_action, cd->log_status, 0, cd->bytes_written);
                    free(cd->log_action);
                    cd->log_action = NULL;
                }
                my_on_error(cd->loopfd, cd->fd, errno, NULL);
                cleanup_send_state(cd);
                conn_del(cd->fd);
                return;
            }
        }
        cd->send_buf_pos += sent;
        //printf("Buffered sent: %zd bytes\n", sent);
    }
    cd->bytes_written += sent;
    cd->send_remaining -= sent;
    #ifndef __linux__
    cd->send_offset += sent;
    #endif

    if (cd->send_remaining == 0) {
        if (cd->log_action) {
            default_access_log(&cd->info, cd->log_action, cd->log_status, 0, cd->bytes_written);
            free(cd->log_action);
            cd->log_action = NULL;
        }
        cleanup_send_state(cd);
        event_mod(cd->loopfd, cd->fd, EV_READ);
        if (http_should_keep_alive(&cd->parser.req)) {
			//printf("http_parser_reset\n");
            http_parser_reset(&cd->parser);
        } else {
			//printf("conn_del\n");
            conn_del(cd->fd);
        }
    } else {
        event_mod(cd->loopfd, cd->fd, EV_WRITE);
    }
}

static inline void default_socket_handler(int fd, const char *data, size_t len, struct client_info *info) {
    if (info->type == CONN_UDPV4 || info->type == CONN_UDPV6) {
        socket_write(fd, data, len);
    } else {
        const char *msg = "Hello world!\n";
        socket_write(fd, msg, strlen(msg));
    }
}

/* Reverse proxy state per client (non-blocking upstream) */
typedef struct {
    int upstream_fd;
    struct http_parser parser;
    bool connected;
    bool headers_sent;
    char *upstream_host;
    int upstream_port;
    char *proxy_prefix;  /* original mount prefix, e.g. "/api" */
    char *pending;       /* buffered data to send to upstream (headers + body overflow) */
    size_t pending_len;
    size_t pending_sent;
    /* Downstream buffer: upstream->client (for backpressure) */
    char *downstream_buf;
    size_t downstream_len;
    size_t downstream_sent;
    /* Backpressure flags */
    bool client_paused;    /* stopped reading from client fd */
    bool upstream_paused;  /* stopped reading from upstream fd */
    /* Body streaming tracking */
    size_t body_remaining; /* Content-Length countdown for client->upstream */
    bool body_complete;    /* all body data forwarded to upstream */
    bool body_chunked;     /* client is sending chunked body */
    /* Upstream pool tracking (for load-balanced proxying) */
    char *pool_name;       /* non-NULL when target came from an upstream pool */
    char *pool_target_url; /* the resolved target URL, for dec_conn on cleanup */
    bool is_websocket;     /* true after Upgrade: websocket — connection is a raw tunnel */
    time_t connect_start;  /* timestamp when upstream connect was initiated */
    int proxy_timeout;     /* seconds before connect is considered timed out */
} proxy_state_t;

typedef struct { int key; proxy_state_t value; } ProxyEntry;
static ProxyEntry *proxy_states = NULL;          /* client_fd -> proxy_state_t */
typedef struct { int key; int value; } UpstreamFdEntry;
static UpstreamFdEntry *upstream_fd_to_client = NULL;  /* upstream_fd -> client_fd (fast lookup) */

/* Forward decl for the proxy handler */
static inline void proxy_handler(struct http_parser *p, struct http_request *req, struct client_data *cd);

static inline void addHandler(const char *uri, http_handler_t handler) {
    size_t uri_len = strlen(uri);
    if (uri_len > MAX_URL_SIZE) return;

    if (uri[0] == '/') {
        char *key = strndup(uri, MAX_URL_SIZE);
        if (!key) return;
        struct path_entry pe = {.key = key, .value = handler};
        shputs(global_http_handlers, pe);
        return;
    }

    const char *scheme_end = strstr(uri, "://");
    if (!scheme_end) return;
    size_t scheme_len = scheme_end - uri;
    if (scheme_len == 0 || scheme_len >= 16) return;
    char scheme[16];
    strncpy(scheme, uri, scheme_len);
    scheme[scheme_len] = '\0';

    const char *path_start = strchr(scheme_end + 3, '/');
    size_t base_len = path_start ? path_start - uri : uri_len;
    if (base_len > MAX_URL_SIZE) return;
    char *base_uri = strndup(uri, base_len);
    if (!base_uri) return;

    char *path;
    if (path_start) {
        size_t path_len = uri_len - (path_start - uri);
        if (path_len > MAX_URL_SIZE) {
            free(base_uri);
            return;
        }
        path = strndup(path_start, path_len);
    } else {
        path = strdup("/");
    }
    if (!path) {
        free(base_uri);
        return;
    }

    bool is_http = (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0);

    if (is_http) {
        ptrdiff_t base_idx = shgeti(base_handlers, base_uri);
        if (base_idx < 0) {
            struct path_entry *new_paths = NULL;
            struct base_entry be = {.key = base_uri, .value = new_paths};
            shputs(base_handlers, be);
            base_idx = shgeti(base_handlers, base_uri);
        }
        struct path_entry *paths = base_handlers[base_idx].value;
        struct path_entry pe = {.key = path, .value = (http_handler_t)handler};
        shputs(paths, pe);
    } else {
        char *key = strndup(uri, MAX_URL_SIZE);
        if (!key) {
            free(base_uri);
            free(path);
            return;
        }
        struct socket_handler_entry she = {.key = key, .value = (socket_handler_t)handler};
        shputs(socket_handler_map, she);
        free(base_uri);
        free(path);
    }
}

/* ==================== Reverse Proxy Implementation ==================== */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* proxy_on_upstream_data removed: body streaming is now handled by proxy_on_body_data */
static inline void proxy_cleanup(int client_fd);
static inline void upstream_pool_mark_down(const char *pool_name, const char *target_url);
static inline void proxy_on_body_data(int loopfd, int client_fd, const char *data, size_t len);
static inline void proxy_on_client_writable(int loopfd, int client_fd);

/* Called from the main event loop when the upstream becomes readable */
static inline void proxy_on_upstream_write(int loopfd, int upstream_fd);
static inline void proxy_on_upstream_read(int loopfd, int upstream_fd);

/* Called when upstream fd becomes writable (connection complete or drain) */
static inline void proxy_on_upstream_write(int loopfd, int upstream_fd) {
    PROXY_LOG("[PROXY-UP] write event on upstream_fd=%d\n", upstream_fd);
    ptrdiff_t cidx = hmgeti(upstream_fd_to_client, upstream_fd);
    if (cidx < 0) return;
    int client_fd = upstream_fd_to_client[cidx].value;
    ProxyEntry *entry = NULL;
    ptrdiff_t pidx = hmgeti(proxy_states, client_fd);
    if (pidx >= 0) entry = &proxy_states[pidx];
    if (!entry) return;

    proxy_state_t *ps = &entry->value;
    bool need_write = false;

    if (!ps->connected) {
        int soerr = 0;
        socklen_t olen = sizeof(soerr);
        getsockopt(upstream_fd, SOL_SOCKET, SO_ERROR, &soerr, &olen);
        if (soerr != 0) {
            PROXY_LOG("[PROXY-UP] connect failed soerr=%d\n", soerr);
            /* Mark pool target as temporarily down */
            if (ps->pool_name && ps->pool_target_url)
                upstream_pool_mark_down(ps->pool_name, ps->pool_target_url);
            /* Send 502 to client before cleanup */
            {
                int ci = get_conn(client_fd);
                if (ci >= 0) {
                    http_error(&clients[ci].parser, 502, "Bad Gateway");
                    clients[ci].log_status = 502;
                }
            }
            proxy_cleanup(client_fd);
            return;
        }
        ps->connected = true;
        PROXY_LOG("[PROXY-UP] upstream connected\n");
    }

    /* Drain pending outbound data (headers + any buffered body chunks) */
    if (ps->pending && ps->pending_sent < ps->pending_len) {
        ssize_t n = write(upstream_fd, ps->pending + ps->pending_sent,
                        ps->pending_len - ps->pending_sent);
        if (n > 0) {
            ps->pending_sent += n;
            if (ps->pending_sent == ps->pending_len) {
                free(ps->pending);
                ps->pending = NULL;
                ps->pending_len = ps->pending_sent = 0;
            } else {
                need_write = true;  /* more to send */
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            PROXY_LOG("[PROXY-UP] write pending failed errno=%d\n", errno);
            proxy_cleanup(client_fd);
            return;
        } else {
            need_write = true;  /* EAGAIN, keep waiting for writable */
        }
    }

    /* Resume reading from client if we were applying backpressure and buffer is drained */
    if (!need_write && ps->client_paused) {
        ps->client_paused = false;
        event_set(loopfd, client_fd, EV_READ);
        PROXY_LOG("[PROXY-UP] resumed client reads on fd=%d\n", client_fd);
    }

    /* Transition to read-only once everything is sent */
    if (loopfd >= 0) {
        if (need_write) {
            /* keep EV_WRITE; EV_READ already registered */
        } else {
            event_set(loopfd, upstream_fd, EV_READ);
        }
    }
}

/* Dispatch from event loop */
static inline void proxy_on_upstream_event(int loopfd, int fd, int events) {
    if (events & EV_WRITE) proxy_on_upstream_write(loopfd, fd);
    if (events & EV_READ)  proxy_on_upstream_read(loopfd, fd);
}

/* Called from the main event loop when the upstream becomes readable */
static inline void proxy_on_upstream_read(int loopfd, int upstream_fd) {
    PROXY_LOG("[PROXY-UP] read event on upstream_fd=%d\n", upstream_fd);
    ptrdiff_t cidx = hmgeti(upstream_fd_to_client, upstream_fd);
    if (cidx < 0) {
        PROXY_LOG("[PROXY-UP]   unknown upstream_fd\n");
        return;
    }
    int client_fd = upstream_fd_to_client[cidx].value;
    ptrdiff_t pidx = hmgeti(proxy_states, client_fd);
    ProxyEntry *entry = (pidx >= 0) ? &proxy_states[pidx] : NULL;
    if (!entry) {
        PROXY_LOG("[PROXY-UP]   no proxy state for client %d\n", client_fd);
        return;
    }
    proxy_state_t *ps = &entry->value;

    char buf[CHUNK_SIZE];
    ssize_t n = read(upstream_fd, buf, sizeof(buf));
    PROXY_LOG("[PROXY-UP]   read %zd bytes from upstream (client_fd=%d)\n", n, client_fd);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0) PROXY_LOG("[PROXY-UP]   read errno=%d (%s)\n", errno, strerror(errno));
        proxy_cleanup(client_fd);
        return;
    }

    /* Try writing to client */
    ssize_t w = socket_write(client_fd, buf, n);
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) w = 0;
    if (w < 0) {
        PROXY_LOG("[PROXY-UP]   client write failed errno=%d\n", errno);
        proxy_cleanup(client_fd);
        return;
    }

    if (w < n) {
        /* Partial write to client — buffer remainder, apply backpressure */
        size_t rem = n - w;
        ps->downstream_buf = (char *)realloc(ps->downstream_buf, rem);
        if (!ps->downstream_buf) {
            PROXY_LOG("[PROXY-UP]   downstream_buf alloc failed\n");
            proxy_cleanup(client_fd);
            return;
        }
        memcpy(ps->downstream_buf, buf + w, rem);
        ps->downstream_len = rem;
        ps->downstream_sent = 0;
        ps->upstream_paused = true;
        /* Stop reading from upstream, wait for client to become writable */
        event_del(loopfd, upstream_fd, EV_READ, NULL);
        event_set(loopfd, client_fd, EV_READ | EV_WRITE);
        PROXY_LOG("[PROXY-UP]   backpressure: buffered %zu bytes, paused upstream\n", rem);
    }

    /* Log on first response bytes from upstream (standard access log) */
    {
        int ci = get_conn(client_fd);
        if (ci >= 0) {
            struct client_data *cd = &clients[ci];
            if (cd->log_action == NULL) {
                char action[512];
                snprintf(action, sizeof(action), "GET %s HTTP/1.1", "proxy");
                cd->log_action = strdup(action);
                cd->log_status = 200;
            }
        }
    }
}

static inline void proxy_cleanup(int client_fd);
static inline void upstream_pool_dec_conn(const char *pool_name, const char *target_url);
static inline void upstream_pool_inc_conn(const char *pool_name, const char *target_url);
extern int g_loopfd;
static inline void proxy_cleanup(int client_fd) {
    ptrdiff_t idx = hmgeti(proxy_states, client_fd);
    if (idx < 0) return;
    proxy_state_t *ps = &proxy_states[idx].value;
    if (ps->upstream_fd >= 0) {
        if (g_loopfd >= 0) {
            event_del(g_loopfd, ps->upstream_fd, EV_READ, NULL);
            event_del(g_loopfd, ps->upstream_fd, EV_WRITE, NULL);
        }
        hmdel(upstream_fd_to_client, ps->upstream_fd);
        close(ps->upstream_fd);
    }
    /* Emit standard access log and restore client state */
    {
        int cidx = get_conn(client_fd);
        if (cidx >= 0) {
            struct client_data *cd = &clients[cidx];
            cd->proxy_active = false;
            cd->proxy_streaming = false;
            cd->sending_body = false;
            if (cd->log_action) {
                default_access_log(&cd->info, cd->log_action, cd->log_status, 0, cd->bytes_written);
                free(cd->log_action);
                cd->log_action = NULL;
            }
            /* Always close the client-facing side after a proxied response.
             * The upstream leg is always told "Connection: close" (no
             * upstream connection pooling — see upstream request building)
             * and its response bytes, including that header, are relayed to
             * the client verbatim. Deciding keep-alive independently here
             * from the client's own request would contradict what was
             * already sent to the client and leave a socket open that a
             * client obeying its "Connection: close" header won't reuse. */
            conn_del(client_fd);
        }
    }
    http_parser_destroy(&ps->parser);
    free(ps->upstream_host);
    free(ps->proxy_prefix);
    free(ps->pending);
    free(ps->downstream_buf);
    /* Decrement upstream pool connection counter */
    if (ps->pool_name && ps->pool_target_url) {
        upstream_pool_dec_conn(ps->pool_name, ps->pool_target_url);
    }
    free(ps->pool_name);
    free(ps->pool_target_url);
    hmdel(proxy_states, client_fd);
}

/* ---------------------------------------------------------------------------
 * Upstream pool system for load-balanced reverse proxying.
 * Pools are registered by name and resolved at request time using the
 * configured balancing strategy.
 * --------------------------------------------------------------------------- */

enum {
    BALANCE_ROUND_ROBIN = 0,
    BALANCE_RANDOM      = 1,
    BALANCE_IP_HASH     = 2,
    BALANCE_CONN_HASH   = 3,
    BALANCE_LEAST_CONN  = 4
};

typedef struct {
    char  host[256];
    int   port;
    char *full_url;           /* e.g. "http://127.0.0.1:8001" */
    time_t unhealthy_until;   /* 0 = healthy, >0 = unhealthy until this timestamp */
    int   active_connections;
} UpstreamTarget;

typedef struct {
    char           *name;
    UpstreamTarget  targets[16];
    int             num_targets;
    int             balance_mode;
    unsigned int    rr_counter;
    const char     *health_path;
    int             health_interval;
    int             retry_timeout;   /* seconds before retrying a dead target */
    int             proxy_timeout;   /* seconds to wait for upstream connect */
} UpstreamPool;

typedef struct { char *key; UpstreamPool value; } UpstreamPoolEntry;
static UpstreamPoolEntry *upstream_pools = NULL;

static inline void upstream_pool_register(const char *name,
                                          const char **target_urls,
                                          int num_targets,
                                          const char *balance,
                                          const char *health_path,
                                          int health_interval,
                                          int retry_timeout,
                                          int proxy_timeout) {
    UpstreamPool pool;
    memset(&pool, 0, sizeof(pool));
    pool.name = (char *)name;
    pool.num_targets = num_targets > 16 ? 16 : num_targets;
    pool.rr_counter = 0;
    pool.health_path = health_path;
    pool.health_interval = health_interval;
    pool.retry_timeout = retry_timeout > 0 ? retry_timeout : DEFAULT_RETRY_TIMEOUT;
    pool.proxy_timeout = proxy_timeout > 0 ? proxy_timeout : DEFAULT_PROXY_TIMEOUT;

    /* Determine balance mode from string. */
    if      (!balance || strcmp(balance, "round-robin") == 0) pool.balance_mode = BALANCE_ROUND_ROBIN;
    else if (strcmp(balance, "random")     == 0)              pool.balance_mode = BALANCE_RANDOM;
    else if (strcmp(balance, "ip-hash")    == 0)              pool.balance_mode = BALANCE_IP_HASH;
    else if (strcmp(balance, "conn-hash")  == 0)              pool.balance_mode = BALANCE_CONN_HASH;
    else if (strcmp(balance, "least-conn") == 0)              pool.balance_mode = BALANCE_LEAST_CONN;
    else                                                      pool.balance_mode = BALANCE_ROUND_ROBIN;

    /* Parse each target URL into host + port. */
    for (int i = 0; i < pool.num_targets; i++) {
        const char *url = target_urls[i];
        pool.targets[i].full_url = strdup(url);
        pool.targets[i].unhealthy_until = 0;
        pool.targets[i].active_connections = 0;

        /* Skip scheme ("http://" or "https://"). */
        const char *host_start = url;
        const char *scheme_end = strstr(url, "://");
        if (scheme_end) host_start = scheme_end + 3;

        /* Find port separator, stop at path or end. */
        if (host_start[0] == '[') {
            const char *close = strchr(host_start, ']');
            if (close) {
                size_t hlen = (size_t)(close - host_start - 1);
                if (hlen >= sizeof(pool.targets[i].host)) hlen = sizeof(pool.targets[i].host) - 1;
                memcpy(pool.targets[i].host, host_start + 1, hlen);
                pool.targets[i].host[hlen] = '\0';
                if (close[1] == ':') {
                    pool.targets[i].port = atoi(close + 2);
                } else {
                    pool.targets[i].port = (url[4] == 's') ? 443 : 80;
                }
            }
        } else {
            const char *colon = NULL;
            const char *p = host_start;
            while (*p && *p != '/' && *p != '?') {
                if (*p == ':') colon = p;
                p++;
            }

            if (colon) {
                size_t hlen = (size_t)(colon - host_start);
                if (hlen >= sizeof(pool.targets[i].host)) hlen = sizeof(pool.targets[i].host) - 1;
                memcpy(pool.targets[i].host, host_start, hlen);
                pool.targets[i].host[hlen] = '\0';
                pool.targets[i].port = atoi(colon + 1);
            } else {
                size_t hlen = (size_t)(p - host_start);
                if (hlen >= sizeof(pool.targets[i].host)) hlen = sizeof(pool.targets[i].host) - 1;
                memcpy(pool.targets[i].host, host_start, hlen);
                pool.targets[i].host[hlen] = '\0';
                /* Default port based on scheme. */
                pool.targets[i].port = (url[4] == 's') ? 443 : 80;
            }
        }
    }

    shput(upstream_pools, name, pool);
}

static inline const char *upstream_pool_resolve(const char *pool_name,
                                                const char *client_ip,
                                                int client_fd) {
    ptrdiff_t idx = shgeti(upstream_pools, pool_name);
    if (idx < 0) return NULL;
    UpstreamPool *pool = &upstream_pools[idx].value;
    if (pool->num_targets == 0) return NULL;

    int n = pool->num_targets;
    int start, chosen;

    /* Helper macro: a target is considered healthy when unhealthy_until is
     * zero (never marked down) or the current time has passed it. */
    time_t now = time(NULL);
    #define TARGET_HEALTHY(t) ((t).unhealthy_until == 0 || now >= (t).unhealthy_until)

    switch (pool->balance_mode) {

    case BALANCE_ROUND_ROBIN: {
        unsigned int c = pool->rr_counter++;
        for (int tries = 0; tries < n; tries++) {
            int i = (int)((c + (unsigned int)tries) % (unsigned int)n);
            if (TARGET_HEALTHY(pool->targets[i])) return pool->targets[i].full_url;
        }
        return NULL;
    }

    case BALANCE_RANDOM: {
        start = rand() % n;
        for (int tries = 0; tries < n; tries++) {
            int i = (start + tries) % n;
            if (TARGET_HEALTHY(pool->targets[i])) return pool->targets[i].full_url;
        }
        return NULL;
    }

    case BALANCE_IP_HASH: {
        /* djb2 hash of client IP. */
        unsigned long h = 5381;
        if (client_ip) {
            for (const char *s = client_ip; *s; s++)
                h = ((h << 5) + h) + (unsigned char)*s;
        }
        start = (int)(h % (unsigned long)n);
        for (int tries = 0; tries < n; tries++) {
            int i = (start + tries) % n;
            if (TARGET_HEALTHY(pool->targets[i])) return pool->targets[i].full_url;
        }
        return NULL;
    }

    case BALANCE_CONN_HASH: {
        /* Hash the file descriptor for per-connection stickiness. */
        unsigned int h = (unsigned int)client_fd;
        h ^= h >> 16;
        h *= 0x45d9f3b;
        h ^= h >> 16;
        start = (int)(h % (unsigned int)n);
        for (int tries = 0; tries < n; tries++) {
            int i = (start + tries) % n;
            if (TARGET_HEALTHY(pool->targets[i])) return pool->targets[i].full_url;
        }
        return NULL;
    }

    case BALANCE_LEAST_CONN: {
        chosen = -1;
        int min_conn = INT_MAX;
        for (int i = 0; i < n; i++) {
            if (TARGET_HEALTHY(pool->targets[i]) &&
                pool->targets[i].active_connections < min_conn) {
                min_conn = pool->targets[i].active_connections;
                chosen = i;
            }
        }
        return (chosen >= 0) ? pool->targets[chosen].full_url : NULL;
    }

    default:
        return NULL;
    }
    #undef TARGET_HEALTHY
}

static inline void upstream_pool_inc_conn(const char *pool_name,
                                          const char *target_url) {
    ptrdiff_t idx = shgeti(upstream_pools, pool_name);
    if (idx < 0) return;
    UpstreamPool *pool = &upstream_pools[idx].value;
    for (int i = 0; i < pool->num_targets; i++) {
        if (strcmp(pool->targets[i].full_url, target_url) == 0) {
            pool->targets[i].active_connections++;
            return;
        }
    }
}

static inline void upstream_pool_dec_conn(const char *pool_name,
                                          const char *target_url) {
    ptrdiff_t idx = shgeti(upstream_pools, pool_name);
    if (idx < 0) return;
    UpstreamPool *pool = &upstream_pools[idx].value;
    for (int i = 0; i < pool->num_targets; i++) {
        if (strcmp(pool->targets[i].full_url, target_url) == 0) {
            if (pool->targets[i].active_connections > 0)
                pool->targets[i].active_connections--;
            return;
        }
    }
}

/* Mark an upstream target as temporarily unhealthy. */
static inline void upstream_pool_mark_down(const char *pool_name,
                                           const char *target_url) {
    ptrdiff_t idx = shgeti(upstream_pools, pool_name);
    if (idx < 0) return;
    UpstreamPool *pool = &upstream_pools[idx].value;
    for (int i = 0; i < pool->num_targets; i++) {
        if (strcmp(pool->targets[i].full_url, target_url) == 0) {
            pool->targets[i].unhealthy_until = time(NULL) + pool->retry_timeout;
            LOG_WARN(PROXY, "upstream %s marked DOWN for %ds",
                     target_url, pool->retry_timeout);
            return;
        }
    }
}

/* Get the proxy connect timeout for a named pool (0 = use default). */
static inline int upstream_pool_get_proxy_timeout(const char *pool_name) {
    ptrdiff_t idx = shgeti(upstream_pools, pool_name);
    if (idx < 0) return DEFAULT_PROXY_TIMEOUT;
    return upstream_pools[idx].value.proxy_timeout;
}

/* Streaming non-blocking reverse proxy handler.
 * Only buffers the HTTP request headers (~2KB). The request body is streamed
 * in CHUNK_SIZE (64KB) increments via proxy_on_body_data, with backpressure
 * applied when the upstream write buffer fills up. */
static inline void proxy_handler(struct http_parser *p, struct http_request *req, struct client_data *cd) {
    const char *prefix = cd->matched_prefix ? cd->matched_prefix : "/";
    /* Find location by string content (locations uses pointer-keyed hm map) */
    ptrdiff_t loc_idx = -1;
    for (int i = 0; i < hmlen(locations); i++) {
        if (strcmp(locations[i].key, prefix) == 0) {
            loc_idx = i;
            break;
        }
    }
    if (loc_idx < 0) {
        PROXY_LOG("[PROXY] ERROR: no location for prefix=%s\n", prefix);
        http_error(p, 502, "Bad Gateway");
        return;
    }

    const char *proxy_pass = locations[loc_idx].value.real_path;
    PROXY_LOG("[PROXY] matched proxy_pass=%s\n", proxy_pass);

    /* Resolve upstream:// pool references to a concrete target URL */
    const char *resolved_url = proxy_pass;
    char *pool_name_resolved = NULL;
    char *pool_target_resolved = NULL;
    if (proxy_pass && strncmp(proxy_pass, "upstream://", 11) == 0) {
        const char *pname = proxy_pass + 11;
        const char *chosen = upstream_pool_resolve(pname, cd->info.addr, cd->fd);
        if (!chosen) {
            PROXY_LOG("[PROXY] upstream pool '%s' has no healthy targets\n", pname);
            http_error(p, 502, "Bad Gateway");
            return;
        }
        resolved_url = chosen;
        pool_name_resolved = strdup(pname);
        pool_target_resolved = strdup(chosen);
        upstream_pool_inc_conn(pname, chosen);
        PROXY_LOG("[PROXY] upstream pool '%s' resolved -> %s\n", pname, chosen);
    }

    if (!resolved_url || strncmp(resolved_url, "http://", 7) != 0) {
        free(pool_name_resolved);
        free(pool_target_resolved);
        http_error(p, 502, "Bad Gateway");
        return;
    }

    /* Parse upstream host:port from resolved URL (IPv4/IPv6/hostname)
     * Supports: http://1.2.3.4:8000  http://[::1]:8000  http://host:8000 */
    const char *host_start = resolved_url + 7;
    char host[256] = {0};
    char portstr[8] = "80";
    int port = 80;

    if (host_start[0] == '[') {
        /* Bracketed IPv6: [addr]:port */
        const char *close_bracket = strchr(host_start, ']');
        if (!close_bracket) {
            free(pool_name_resolved); free(pool_target_resolved);
            http_error(p, 502, "Bad Gateway"); return;
        }
        size_t hlen = (size_t)(close_bracket - host_start - 1);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, host_start + 1, hlen);
        host[hlen] = '\0';
        if (close_bracket[1] == ':') {
            port = atoi(close_bracket + 2);
            snprintf(portstr, sizeof(portstr), "%d", port);
        }
    } else {
        /* IPv4 or hostname: find last colon before path */
        const char *path_start = strchr(host_start, '/');
        const char *end = path_start ? path_start : host_start + strlen(host_start);
        const char *colon = NULL;
        for (const char *s = host_start; s < end; s++)
            if (*s == ':') colon = s;
        if (colon) {
            size_t hlen = (size_t)(colon - host_start);
            if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
            memcpy(host, host_start, hlen);
            host[hlen] = '\0';
            port = atoi(colon + 1);
            snprintf(portstr, sizeof(portstr), "%d", port);
        } else {
            size_t hlen = (size_t)(end - host_start);
            if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
            memcpy(host, host_start, hlen);
            host[hlen] = '\0';
        }
    }
    PROXY_LOG("[PROXY] connecting to %s:%d (from proxy_pass)\n", host, port);

    /* Resolve host via getaddrinfo (AF_UNSPEC: works for IPv4, IPv6, hostnames) */
    struct addrinfo proxy_hints = {0}, *proxy_res = NULL;
    proxy_hints.ai_family = AF_UNSPEC;
    proxy_hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &proxy_hints, &proxy_res) != 0 || !proxy_res) {
        PROXY_LOG("[PROXY] getaddrinfo failed for %s:%s\n", host, portstr);
        if (pool_name_resolved) { upstream_pool_dec_conn(pool_name_resolved, pool_target_resolved); free(pool_name_resolved); free(pool_target_resolved); }
        http_error(p, 502, "Bad Gateway");
        return;
    }

    /* Try each resolved address until connect succeeds or returns EINPROGRESS */
    int upstream = -1;
    for (struct addrinfo *ai = proxy_res; ai; ai = ai->ai_next) {
        upstream = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (upstream < 0) continue;
        fcntl(upstream, F_SETFL, O_NONBLOCK);
        int c = connect(upstream, ai->ai_addr, ai->ai_addrlen);
        if (c == 0 || errno == EINPROGRESS) {
            PROXY_LOG("[PROXY] connect to %s:%d fd=%d (family=%d)\n", host, port, upstream, ai->ai_family);
            break;
        }
        close(upstream);
        upstream = -1;
    }
    freeaddrinfo(proxy_res);

    if (upstream < 0) {
        PROXY_LOG("[PROXY] all connect attempts failed for %s:%d\n", host, port);
        if (pool_name_resolved) { upstream_pool_dec_conn(pool_name_resolved, pool_target_resolved); free(pool_name_resolved); free(pool_target_resolved); }
        http_error(p, 502, "Bad Gateway");
        return;
    }

    /* Register upstream in the event loop for BOTH read and write (need write until connected) */
    if (g_loopfd >= 0) {
        event_add(g_loopfd, upstream, EV_READ | EV_WRITE, NULL);
        PROXY_LOG("[PROXY] registered upstream_fd=%d for EV_READ|EV_WRITE\n", upstream);
    }

    /* Determine whether body is expected and how large it is */
    bool has_body = (p->content_length > 0 || p->chunked);
    bool is_streaming = (p->state == HP_BODY);  /* headers-only parse (res==2) */

    /* Detect WebSocket upgrade (Upgrade: websocket + Connection: Upgrade) */
    bool is_websocket = false;
    {
        ptrdiff_t ui = shgeti(req->headers, "upgrade");
        if (ui >= 0 && strcasecmp(req->headers[ui].value, "websocket") == 0)
            is_websocket = true;
    }

    /* Store proxy state */
    proxy_state_t ps = {0};
    ps.upstream_fd = upstream;
    ps.connected = false;
    ps.headers_sent = false;
    ps.upstream_host = strdup(host);
    ps.upstream_port = port;
    ps.proxy_prefix = strdup(prefix);
    ps.body_remaining = p->content_length;  /* 0 if chunked or no body */
    ps.body_chunked = p->chunked;
    ps.body_complete = !has_body;            /* no body = already complete */
    ps.pool_name = pool_name_resolved;       /* NULL when not from a pool */
    ps.pool_target_url = pool_target_resolved;
    ps.is_websocket = is_websocket;
    ps.connect_start = time(NULL);
    ps.proxy_timeout = pool_name_resolved
        ? upstream_pool_get_proxy_timeout(pool_name_resolved)
        : DEFAULT_PROXY_TIMEOUT;
    http_parser_init(&ps.parser, upstream);
    hmputs(proxy_states, ((ProxyEntry){cd->fd, ps}));
    hmputs(upstream_fd_to_client, ((UpstreamFdEntry){upstream, cd->fd}));
    PROXY_LOG("[PROXY] stored proxy state client_fd=%d -> upstream=%d streaming=%d\n", cd->fd, upstream, is_streaming);

    /* Set up standard access log for this proxy request */
    {
        char action[512];
        snprintf(action, sizeof(action), "%s %s %s", req->method ? req->method : "GET", req->uri ? req->uri : "/", req->version ? req->version : "HTTP/1.1");
        cd->log_action = strdup(action);
        cd->log_status = 200;
    }

    /* Build ONLY the outbound request headers (body is streamed separately) */
    StringBuf sb;
    stringbuf_init(&sb, 2048);
    stringbuf_appendf(&sb, "%s %s HTTP/1.1\r\n", req->method, req->uri);
    PROXY_LOG("[PROXY] building request headers: %s %s HTTP/1.1 content_length=%zu chunked=%d\n",
             req->method, req->uri, p->content_length, p->chunked);

    for (int i = 0; i < shlen(req->headers); i++) {
        /* Strip headers we rewrite ourselves (keys are lowercased at parse time) */
        if (strcmp(req->headers[i].key, "host") == 0) continue;
        if (strcmp(req->headers[i].key, "connection") == 0) continue;
        if (strcmp(req->headers[i].key, "transfer-encoding") == 0) continue;
        if (strcmp(req->headers[i].key, "content-length") == 0) continue;
        if (strcmp(req->headers[i].key, "x-forwarded-for") == 0) continue;
        if (strcmp(req->headers[i].key, "x-real-ip") == 0) continue;
        if (strcmp(req->headers[i].key, "x-forwarded-proto") == 0) continue;
        stringbuf_appendf(&sb, "%s: %s\r\n", req->headers[i].key, req->headers[i].value);
    }
    /* Set Host to the backend so the upstream sees a correct virtual host */
    stringbuf_appendf(&sb, "Host: %s:%d\r\n", host, port);
    if (is_websocket) {
        stringbuf_append(&sb, "Connection: Upgrade\r\n", 21);
    } else {
        stringbuf_append(&sb, "Connection: close\r\n", 19);
    }
    /* Client identity headers */
    const char *client_ip = cd->info.addr;
    if (client_ip[0]) {
        /* Append to existing X-Forwarded-For chain if the client sent one */
        ptrdiff_t xff_idx = shgeti(req->headers, "x-forwarded-for");
        if (xff_idx >= 0) {
            stringbuf_appendf(&sb, "X-Forwarded-For: %s, %s\r\n", req->headers[xff_idx].value, client_ip);
        } else {
            stringbuf_appendf(&sb, "X-Forwarded-For: %s\r\n", client_ip);
        }
        stringbuf_appendf(&sb, "X-Real-IP: %s\r\n", client_ip);
    }
    {
        const char *proto = "http";
#ifdef HAVE_OPENSSL
        if (cd->ssl != NULL) proto = "https";
#endif
        stringbuf_appendf(&sb, "X-Forwarded-Proto: %s\r\n", proto);
    }

    if (is_streaming) {
        /* Streaming mode: forward Content-Length or Transfer-Encoding from client */
        if (p->chunked) {
            stringbuf_append(&sb, "Transfer-Encoding: chunked\r\n", 28);
        } else if (p->content_length > 0) {
            stringbuf_appendf(&sb, "Content-Length: %zu\r\n", p->content_length);
        }
    } else {
        /* Full-body mode (small body already buffered by parser, e.g. res==1) */
        if (req->body_len > 0) {
            stringbuf_appendf(&sb, "Content-Length: %zu\r\n", req->body_len);
        }
    }
    stringbuf_append(&sb, "\r\n", 2);

    /* For non-streaming requests (no body, or body already fully buffered), append body */
    if (!is_streaming && req->body && req->body_len > 0) {
        stringbuf_append(&sb, req->body, req->body_len);
    }

    /* Debug: dump first 512 bytes of the request headers we are sending */
    PROXY_LOG("[PROXY] outbound request bytes (first 512):\n%.*s\n", (int)(sb.size>512?512:sb.size), sb.data);

    ptrdiff_t pidx = hmgeti(proxy_states, cd->fd);
    proxy_state_t *state = (pidx >= 0) ? &proxy_states[pidx].value : NULL;
    if (state) {
        state->pending = sb.data;
        state->pending_len = sb.size;
        state->pending_sent = 0;
    } else {
        free(sb.data);
    }

    /* Mark connection as proxy-owned so http_dispatcher doesn't reset the parser.
     * For streaming bodies (is_streaming), my_on_read will also use the fast path
     * to forward body chunks directly via on_body_data.
     * For WebSocket upgrades, enable the streaming fast path immediately so that
     * post-handshake frames bypass the HTTP parser entirely. */
    cd->proxy_active = true;
    if (is_streaming || is_websocket) {
        cd->proxy_streaming = true;
        p->streaming_body = true;
    }

    /* Mark that we are now waiting for upstream response */
    cd->sending_body = true;
}

/* ==================== Proxy Body Streaming ==================== */

/* Called from my_on_read when client_data.proxy_streaming is true.
 * Forwards raw body data from the client to the upstream backend,
 * applying backpressure by pausing client reads when upstream is full. */
static inline void proxy_on_body_data(int loopfd, int client_fd,
                                       const char *data, size_t len) {
    ptrdiff_t pidx = hmgeti(proxy_states, client_fd);
    if (pidx < 0) return;
    proxy_state_t *ps = &proxy_states[pidx].value;

    PROXY_LOG("[PROXY-BODY] client_fd=%d len=%zu connected=%d pending=%zu/%zu\n",
             client_fd, len, ps->connected, ps->pending_sent, ps->pending_len);

    /* If upstream not yet connected or we still have pending data, append to buffer */
    if (!ps->connected || (ps->pending && ps->pending_sent < ps->pending_len)) {
        char *newbuf = (char *)realloc(ps->pending, ps->pending_len + len);
        if (!newbuf) {
            PROXY_LOG("[PROXY-BODY] realloc failed\n");
            proxy_cleanup(client_fd);
            return;
        }
        memcpy(newbuf + ps->pending_len, data, len);
        ps->pending = newbuf;
        ps->pending_len += len;
        return;
    }

    /* Try direct write to upstream */
    ssize_t n = write(ps->upstream_fd, data, len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) n = 0;
    if (n < 0) {
        PROXY_LOG("[PROXY-BODY] upstream write failed errno=%d\n", errno);
        proxy_cleanup(client_fd);
        return;
    }

    /* Track body progress for Content-Length bodies (skip for WebSocket tunnel) */
    if (!ps->is_websocket && ps->body_remaining > 0 && n > 0) {
        ps->body_remaining -= (size_t)n;
        if (ps->body_remaining == 0) {
            ps->body_complete = true;
            PROXY_LOG("[PROXY-BODY] body complete\n");
        }
    }

    if ((size_t)n < len) {
        /* Partial write — buffer remainder, pause client reads */
        size_t rem = len - (size_t)n;
        ps->pending = (char *)malloc(rem);
        if (!ps->pending) {
            PROXY_LOG("[PROXY-BODY] malloc failed\n");
            proxy_cleanup(client_fd);
            return;
        }
        memcpy(ps->pending, data + n, rem);
        ps->pending_len = rem;
        ps->pending_sent = 0;
        ps->client_paused = true;
        /* Stop reading from client, wait for upstream to become writable */
        event_set(loopfd, client_fd, 0);  /* remove EV_READ */
        event_set(loopfd, ps->upstream_fd, EV_READ | EV_WRITE);
        PROXY_LOG("[PROXY-BODY] backpressure: buffered %zu bytes, paused client\n", rem);
    }
}

/* Called from my_on_write when client_data.proxy_streaming is true.
 * Drains the downstream buffer (upstream->client response data) and
 * resumes reading from upstream once the buffer is fully sent. */
static inline void proxy_on_client_writable(int loopfd, int client_fd) {
    ptrdiff_t pidx = hmgeti(proxy_states, client_fd);
    if (pidx < 0) return;
    proxy_state_t *ps = &proxy_states[pidx].value;

    if (!ps->downstream_buf || ps->downstream_sent >= ps->downstream_len) {
        /* Nothing to drain — remove EV_WRITE */
        event_set(loopfd, client_fd, EV_READ);
        return;
    }

    ssize_t w = socket_write(client_fd,
                              ps->downstream_buf + ps->downstream_sent,
                              ps->downstream_len - ps->downstream_sent);
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) w = 0;
    if (w < 0) {
        PROXY_LOG("[PROXY-DOWN] client write failed errno=%d\n", errno);
        proxy_cleanup(client_fd);
        return;
    }
    if (w > 0) ps->downstream_sent += (size_t)w;

    PROXY_LOG("[PROXY-DOWN] drained %zd bytes (%zu/%zu)\n", w, ps->downstream_sent, ps->downstream_len);

    if (ps->downstream_sent >= ps->downstream_len) {
        /* Buffer fully drained */
        free(ps->downstream_buf);
        ps->downstream_buf = NULL;
        ps->downstream_len = ps->downstream_sent = 0;

        /* Resume reading from upstream */
        if (ps->upstream_paused && ps->upstream_fd >= 0) {
            ps->upstream_paused = false;
            event_set(loopfd, ps->upstream_fd, EV_READ);
            PROXY_LOG("[PROXY-DOWN] resumed upstream reads on fd=%d\n", ps->upstream_fd);
        }
        /* Remove EV_WRITE from client since buffer is drained */
        event_set(loopfd, client_fd, EV_READ);
    }
}

/* Sweep all active proxy connections for connect timeouts.
 * Called once per second from the event loop timer. */
static inline void proxy_timeout_sweep(int loopfd) {
    if (hmlen(proxy_states) == 0) return;
    time_t now = time(NULL);

    /* Collect fds to clean up (can't modify hash map while iterating) */
    int stale[128];
    int nstale = 0;
    for (ptrdiff_t i = 0; i < hmlen(proxy_states) && nstale < 128; i++) {
        proxy_state_t *ps = &proxy_states[i].value;
        if (!ps->connected && ps->proxy_timeout > 0 &&
            now - ps->connect_start >= ps->proxy_timeout) {
            stale[nstale++] = proxy_states[i].key;
        }
    }

    for (int i = 0; i < nstale; i++) {
        int client_fd = stale[i];
        ptrdiff_t pidx = hmgeti(proxy_states, client_fd);
        if (pidx < 0) continue;
        proxy_state_t *ps = &proxy_states[pidx].value;

        LOG_WARN(TIMEOUT, "proxy upstream %s:%d connect timed out (%ds)",
                 ps->upstream_host ? ps->upstream_host : "?",
                 ps->upstream_port, ps->proxy_timeout);

        /* Mark pool target as temporarily down */
        if (ps->pool_name && ps->pool_target_url)
            upstream_pool_mark_down(ps->pool_name, ps->pool_target_url);

        /* Send 504 Gateway Timeout to the client */
        int ci = get_conn(client_fd);
        if (ci >= 0) {
            http_error(&clients[ci].parser, 504, "Gateway Timeout");
            clients[ci].log_status = 504;
        }

        proxy_cleanup(client_fd);
    }
}

/* ── Connection timeout sweep (Slowloris / idle / DoS defense) ──────── */
static inline void connection_timeout_sweep(int loopfd) {
    time_t now = time(NULL);

    int stale_fds[256];
    int stale_reason[256];  /* 0=header, 1=body, 2=idle */
    int nstale = 0;

    for (int i = 0; i < NUM_CLIENTS && nstale < 256; i++) {
        if (clients[i].fd == 0) continue;
        if (clients[i].is_listen) continue;

        struct client_data *cd = &clients[i];

        /* Skip connections managed by other subsystems */
        if (cd->forward_active) continue;
        if (cd->proxy_active || cd->proxy_streaming) continue;

        if (cd->is_http) {
            /* Header timeout: accepted but headers not yet complete */
            if (cd->parser.state < HP_BODY && cd->parser.state != HP_DONE &&
                g_conn_timeouts.header_timeout > 0 &&
                (int)(now - cd->accept_time) >= g_conn_timeouts.header_timeout) {
                stale_fds[nstale] = cd->fd;
                stale_reason[nstale] = 0;
                nstale++;
                continue;
            }
            /* Body timeout: receiving body but no data for too long */
            if (cd->parser.state == HP_BODY &&
                g_conn_timeouts.body_timeout > 0 &&
                (int)(now - cd->last_activity) >= g_conn_timeouts.body_timeout) {
                stale_fds[nstale] = cd->fd;
                stale_reason[nstale] = 1;
                nstale++;
                continue;
            }
        }

        /* Idle timeout: no activity for too long (keep-alive, etc.) */
        if (g_conn_timeouts.idle_timeout > 0 &&
            (int)(now - cd->last_activity) >= g_conn_timeouts.idle_timeout) {
            stale_fds[nstale] = cd->fd;
            stale_reason[nstale] = 2;
            nstale++;
        }
    }

    for (int i = 0; i < nstale; i++) {
        int fd = stale_fds[i];
        int idx = get_conn(fd);
        if (idx < 0) continue;
        struct client_data *cd = &clients[idx];

        switch (stale_reason[i]) {
        case 0: /* header timeout */
            LOG_WARN(TIMEOUT, "header timeout fd=%d %s (%ds)",
                     fd, cd->info.addr, g_conn_timeouts.header_timeout);
            if (cd->is_http) {
                /* Try to send 408 — parser may not be ready, so use raw write as fallback */
                const char *resp408 =
                    "HTTP/1.1 408 Request Timeout\r\n"
                    "Connection: close\r\nContent-Length: 0\r\n"
                    "Server: NanoServer/0.1\r\n\r\n";
                socket_write(fd, resp408, strlen(resp408));
            }
            break;
        case 1: /* body timeout */
            LOG_WARN(TIMEOUT, "body timeout fd=%d %s (%ds idle)",
                     fd, cd->info.addr, g_conn_timeouts.body_timeout);
            if (cd->is_http) {
                const char *resp408 =
                    "HTTP/1.1 408 Request Timeout\r\n"
                    "Connection: close\r\nContent-Length: 0\r\n"
                    "Server: NanoServer/0.1\r\n\r\n";
                socket_write(fd, resp408, strlen(resp408));
            }
            break;
        case 2: /* idle timeout */
            LOG_INFO(TIMEOUT, "idle timeout fd=%d %s (%ds)",
                     fd, cd->info.addr, g_conn_timeouts.idle_timeout);
            break;
        }
        conn_del(fd);
    }
}

/* Combined timer: proxy timeouts + connection timeouts.
 * Called once per second from the event loop. */
static inline void server_timeout_sweep(int loopfd) {
    proxy_timeout_sweep(loopfd);
    connection_timeout_sweep(loopfd);
}

/* Global loopfd used by proxy (set from server.c) */
extern int g_loopfd;
static inline void proxy_set_loopfd(int fd) { g_loopfd = fd; }

/* ── Port forwarding / raw relay ─────────────────────────────────────── */

static ForwardEntry *forward_states = NULL;           /* downstream_fd -> forward_state_t */
static ForwardUpstreamEntry *forward_upstream_map = NULL; /* upstream_fd -> downstream_fd */

/* Parse a forward_to URI ("tcp://host:port", "udp://host:port", "unix:///path")
 * into a forward_target struct. Returns 1 on success, 0 on error. */
static inline int parse_forward_target(const char *uri, struct forward_target *ft) {
    if (!uri || !ft) return 0;
    memset(ft, 0, sizeof(*ft));
    ft->buffer_size = CHUNK_SIZE;

    if (strncmp(uri, "tcp://", 6) == 0) {
        ft->proto = FWD_TCP;
        const char *hoststart = uri + 6;
        if (hoststart[0] == '[') {
            const char *close = strchr(hoststart, ']');
            if (!close) return 0;
            size_t hlen = (size_t)(close - hoststart - 1);
            if (hlen >= sizeof(ft->host)) hlen = sizeof(ft->host) - 1;
            memcpy(ft->host, hoststart + 1, hlen);
            ft->host[hlen] = '\0';
            if (close[1] == ':') ft->port = atoi(close + 2);
            else return 0;
        } else {
            const char *colon = strrchr(hoststart, ':');
            if (!colon) return 0;
            size_t hlen = (size_t)(colon - hoststart);
            if (hlen >= sizeof(ft->host)) hlen = sizeof(ft->host) - 1;
            memcpy(ft->host, hoststart, hlen);
            ft->host[hlen] = '\0';
            ft->port = atoi(colon + 1);
        }
    } else if (strncmp(uri, "udp://", 6) == 0) {
        ft->proto = FWD_UDP;
        const char *hoststart = uri + 6;
        if (hoststart[0] == '[') {
            const char *close = strchr(hoststart, ']');
            if (!close) return 0;
            size_t hlen = (size_t)(close - hoststart - 1);
            if (hlen >= sizeof(ft->host)) hlen = sizeof(ft->host) - 1;
            memcpy(ft->host, hoststart + 1, hlen);
            ft->host[hlen] = '\0';
            if (close[1] == ':') ft->port = atoi(close + 2);
            else return 0;
        } else {
            const char *colon = strrchr(hoststart, ':');
            if (!colon) return 0;
            size_t hlen = (size_t)(colon - hoststart);
            if (hlen >= sizeof(ft->host)) hlen = sizeof(ft->host) - 1;
            memcpy(ft->host, hoststart, hlen);
            ft->host[hlen] = '\0';
            ft->port = atoi(colon + 1);
        }
    } else if (strncmp(uri, "unix://", 7) == 0) {
        ft->proto = FWD_UNIX;
        const char *path = uri + 7;
        size_t plen = strlen(path);
        if (plen >= sizeof(ft->path)) plen = sizeof(ft->path) - 1;
        memcpy(ft->path, path, plen);
        ft->path[plen] = '\0';
    } else {
        return 0;
    }
    ft->mode = FWD_MODE_AUTO;
    return 1;
}

/* Create the upstream socket for a forward target and initiate connect.
 * Returns the upstream fd on success, -1 on error.
 * Supports IPv4, IPv6, and Unix sockets via getaddrinfo(AF_UNSPEC). */
static inline int forward_connect(struct forward_target *ft, struct sockaddr_storage *out_addr, socklen_t *out_len) {
    int fd = -1;

    if (ft->proto == FWD_TCP || ft->proto == FWD_UDP) {
        int socktype = (ft->proto == FWD_TCP) ? SOCK_STREAM : SOCK_DGRAM;
        char portstr[8];
        snprintf(portstr, sizeof(portstr), "%d", ft->port);

        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_UNSPEC;  /* IPv4 or IPv6 */
        hints.ai_socktype = socktype;
        if (getaddrinfo(ft->host, portstr, &hints, &res) != 0 || !res)
            return -1;

        /* Try each resolved address until one works */
        for (struct addrinfo *p = res; p; p = p->ai_next) {
            fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd < 0) continue;
            fcntl(fd, F_SETFL, O_NONBLOCK);

#ifdef __APPLE__
            /* macOS equivalent of MSG_NOSIGNAL */
            int nosigpipe = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

            if (out_addr) {
                memcpy(out_addr, p->ai_addr, p->ai_addrlen);
                *out_len = p->ai_addrlen;
            }

            if (ft->proto == FWD_UDP) {
                /* UDP: no connect, just store the address for sendto */
                freeaddrinfo(res);
                return fd;
            }

            int c = connect(fd, p->ai_addr, p->ai_addrlen);
            if (c == 0 || errno == EINPROGRESS) {
                freeaddrinfo(res);
                return fd;
            }
            close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
        return -1;

    } else if (ft->proto == FWD_UNIX) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, ft->path, sizeof(addr.sun_path) - 1);
        if (out_addr) { memcpy(out_addr, &addr, sizeof(addr)); *out_len = sizeof(addr); }
        int c = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (c < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    }
    return fd;
}

/* Clean up a forward relay (both directions) */
static inline void forward_cleanup(int downstream_fd) {
    ptrdiff_t idx = hmgeti(forward_states, downstream_fd);
    if (idx < 0) return;
    forward_state_t *fs = &forward_states[idx].value;

    if (fs->upstream_fd >= 0) {
        event_del(g_loopfd, fs->upstream_fd, 0, NULL);
        close(fs->upstream_fd);
        hmdel(forward_upstream_map, fs->upstream_fd);
    }
    free(fs->up_buf);
    free(fs->down_buf);
    hmdel(forward_states, downstream_fd);

    /* Mark client_data as no longer forwarding */
    int cidx = get_conn(downstream_fd);
    if (cidx >= 0) {
        clients[cidx].forward_active = false;
        clients[cidx].forward_peer_fd = -1;
    }
}

/* Relay data between downstream and upstream for a TCP forward.
 * Called from the event loop on upstream fd read/write events. */
static inline void forward_on_upstream_event(int loopfd, int upstream_fd, int events) {
    ptrdiff_t uidx = hmgeti(forward_upstream_map, upstream_fd);
    if (uidx < 0) return;
    int downstream_fd = forward_upstream_map[uidx].value;
    ptrdiff_t fidx = hmgeti(forward_states, downstream_fd);
    if (fidx < 0) return;
    forward_state_t *fs = &forward_states[fidx].value;

    if (events & EV_WRITE) {
        if (!fs->connected) {
            /* Check if connect() completed */
            int soerr = 0;
            socklen_t olen = sizeof(soerr);
            getsockopt(upstream_fd, SOL_SOCKET, SO_ERROR, &soerr, &olen);
            if (soerr != 0) {
                forward_cleanup(downstream_fd);
                conn_del(downstream_fd);
                return;
            }
            fs->connected = true;
            event_set(loopfd, upstream_fd, EV_READ);
        }
        /* Flush up_buf (downstream->upstream pending data) */
        if (fs->up_buf && fs->up_sent < fs->up_len) {
            ssize_t n = send(upstream_fd, fs->up_buf + fs->up_sent,
                             fs->up_len - fs->up_sent, MSG_NOSIGNAL);
            if (n > 0) fs->up_sent += (size_t)n;
            if (fs->up_sent >= fs->up_len) {
                free(fs->up_buf);
                fs->up_buf = NULL;
                fs->up_len = fs->up_sent = 0;
            }
        }
    }

    if (events & EV_READ) {
        /* Read from upstream, write to downstream */
        char buf[CHUNK_SIZE];
        ssize_t n = recv(upstream_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            forward_cleanup(downstream_fd);
            conn_del(downstream_fd);
            return;
        }
        fs->bytes_relayed_down += (size_t)n;
        fs->last_activity = time(NULL);
        socket_write(downstream_fd, buf, (size_t)n);
    }
}

/* Relay data from downstream (client) to upstream (target).
 * Called from the event loop when client fd has data and forward_active is set. */
static inline void forward_on_downstream_data(int loopfd, int downstream_fd,
                                               const char *data, size_t len) {
    ptrdiff_t fidx = hmgeti(forward_states, downstream_fd);
    if (fidx < 0) return;
    forward_state_t *fs = &forward_states[fidx].value;
    fs->last_activity = time(NULL);

    if (fs->upstream_proto == FWD_UDP) {
        /* TCP->UDP: send each read as a datagram */
        sendto(fs->upstream_fd, data, len, 0,
               (struct sockaddr *)&fs->upstream_addr, fs->upstream_addr_len);
        fs->bytes_relayed_up += len;
        return;
    }

    if (!fs->connected) {
        /* Buffer until connect completes */
        fs->up_buf = realloc(fs->up_buf, fs->up_len + len);
        if (fs->up_buf) {
            memcpy(fs->up_buf + fs->up_len, data, len);
            fs->up_len += len;
        }
        return;
    }

    ssize_t n = send(fs->upstream_fd, data, len, MSG_NOSIGNAL);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) n = 0;
    if (n < 0) {
        forward_cleanup(downstream_fd);
        return;
    }
    fs->bytes_relayed_up += (size_t)n;

    /* Buffer unsent remainder */
    if ((size_t)n < len) {
        size_t rem = len - (size_t)n;
        fs->up_buf = realloc(fs->up_buf, fs->up_len + rem);
        if (fs->up_buf) {
            memcpy(fs->up_buf + fs->up_len, data + n, rem);
            fs->up_len += rem;
        }
        event_set(loopfd, fs->upstream_fd, EV_READ | EV_WRITE);
    }
}

/* Called on accept for a forward-mode listener. Sets up the relay. */
static inline void forward_on_accept(int loopfd, int client_fd, struct forward_target *target) {
    struct sockaddr_storage upstream_addr = {0};
    socklen_t upstream_len = 0;
    int upstream_fd = forward_connect(target, &upstream_addr, &upstream_len);
    if (upstream_fd < 0) {
        close(client_fd);
        return;
    }

    /* Register upstream fd in event loop */
    int ev_flags = (target->proto == FWD_UDP) ? EV_READ : (EV_READ | EV_WRITE);
    event_add(loopfd, upstream_fd, ev_flags, NULL);

    /* Create relay state */
    forward_state_t fs = {0};
    fs.downstream_fd = client_fd;
    fs.upstream_fd = upstream_fd;
    fs.connected = (target->proto == FWD_UDP); /* UDP is "connected" immediately */
    fs.upstream_proto = target->proto;
    fs.downstream_proto = FWD_TCP; /* listener proto, determined at bind time */
    fs.mode = target->mode;
    fs.upstream_addr = upstream_addr;
    fs.upstream_addr_len = upstream_len;
    fs.buffer_size = target->buffer_size ? target->buffer_size : CHUNK_SIZE;
    fs.timeout = target->timeout;
    fs.last_activity = time(NULL);

    hmput(forward_states, client_fd, fs);
    hmput(forward_upstream_map, upstream_fd, client_fd);

    /* Mark client as forwarding */
    int cidx = get_conn(client_fd);
    if (cidx >= 0) {
        clients[cidx].forward_active = true;
        clients[cidx].forward_peer_fd = upstream_fd;
    }

    LOG_INFO(FORWARD, "%s:%d fd=%d -> %s(%s:%d) upstream_fd=%d",
            "client", 0, client_fd,
            target->proto == FWD_TCP ? "tcp" : target->proto == FWD_UDP ? "udp" : "unix",
            target->host, target->port, upstream_fd);
}

#endif /* REQUEST_H */

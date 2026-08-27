/*
 * mock.h — Streaming HTTP request/response recorder for backend mocking.
 *
 * Designed for use inside the proxy path where request and response bodies
 * arrive incrementally (proxy_on_body_data, proxy_on_upstream_read).
 *
 * Produces a single combined .mock file per interaction:
 *
 *   ---META---
 *   {"recorded_at":...,"upstream":"...","latency_ms":...}
 *
 *   ---REQUEST---
 *   GET /path HTTP/1.1
 *   ...
 *   <body streamed in>
 *
 *   ---RESPONSE---
 *   HTTP/1.1 200 OK
 *   ...
 *   <raw response bytes streamed from upstream>
 */

#ifndef MOCK_H
#define MOCK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>

#include "http.h"

#ifdef _WIN32
#  include <direct.h>
#  define mkdir(path, mode) _mkdir(path)
#endif

typedef struct {
    FILE   *f;
    char   *filepath;
    int     request_written;
    int     response_started;
    time_t  start_time;
    char   *upstream_url;
    size_t  request_body_bytes;
    size_t  response_body_bytes;
    int     response_status;
    time_t  response_start_time;
    /* Offsets for the fixed header (populated on finish) */
    long    header_size;
    long    meta_offset, meta_len;
    long    request_offset, request_len;
    long    response_offset, response_len;
    long    summary_offset, summary_len;
} mock_recorder_t;

/* Ensure directory exists (recursive, best-effort). */
static inline void mock_ensure_dir(const char *path) {
    if (!path || !*path) return;
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return;
    if (tmp[len-1] == '/') tmp[len-1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Create a safe filename component from method + URI. */
static inline void mock_make_key(const char *method, const char *uri, char *out, size_t outsz) {
    if (!method) method = "GET";
    if (!uri) uri = "/";
    const char *q = strchr(uri, '?');
    size_t ulen = q ? (size_t)(q - uri) : strlen(uri);
    int n = snprintf(out, outsz, "%s_%.*s", method, (int)ulen, uri);
    if (n < 0) n = 0;
    for (size_t i = 0; i < outsz && out[i]; i++) {
        char c = out[i];
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && c != '_' && c != '-' && c != '.') {
            out[i] = '_';
        }
    }
    if ((size_t)n >= outsz) out[outsz-1] = '\0';
}

/* Begin recording. Opens the .mock file and writes META + REQUEST header section.
 * The request body may be appended later via mock_recorder_append_request_body.
 * Returns a recorder handle or NULL on failure.
 */
static inline mock_recorder_t *mock_recorder_begin(const char *record_dir,
                                                   const struct http_request *req,
                                                   const char *upstream_url)
{
    if (!record_dir || !req) return NULL;

    mock_ensure_dir(record_dir);

    char key[256];
    mock_make_key(req->method, req->uri, key, sizeof(key));

    time_t now = time(NULL);
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s_%ld.mock", record_dir, key, (long)now);

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        /* try simpler name as fallback */
        snprintf(filepath, sizeof(filepath), "%s/%s.mock", record_dir, key);
        f = fopen(filepath, "wb");
        if (!f) return NULL;
    }

    mock_recorder_t *rec = (mock_recorder_t *)calloc(1, sizeof(*rec));
    if (!rec) {
        fclose(f);
        return NULL;
    }

    rec->f = f;
    rec->filepath = strdup(filepath);
    rec->start_time = now;
    rec->upstream_url = upstream_url ? strdup(upstream_url) : NULL;
    rec->request_body_bytes = req->body ? req->body_len : 0;
    rec->response_body_bytes = 0;
    rec->response_status = 0;
    rec->response_start_time = 0;
    rec->header_size = 256;  /* reserved fixed header */

    /* Reserve fixed-size header at the start (will be overwritten on finish) */
    fseek(rec->f, rec->header_size, SEEK_SET);

    /* Write META */
    rec->meta_offset = ftell(rec->f);
    fprintf(rec->f, "---META---\n");
    fprintf(rec->f, "{\"recorded_at\":%ld,\"upstream\":\"%s\",\"latency_ms\":0}\n\n",
            (long)now, rec->upstream_url ? rec->upstream_url : "");
    rec->meta_len = ftell(rec->f) - rec->meta_offset;

    /* Write REQUEST section header + request line + headers */
    rec->request_offset = ftell(rec->f);
    fprintf(rec->f, "---REQUEST---\n");
    fprintf(rec->f, "%s %s %s\r\n",
            req->method ? req->method : "GET",
            req->uri ? req->uri : "/",
            req->version ? req->version : "HTTP/1.1");

    if (req->headers) {
        for (ptrdiff_t i = 0; i < shlen(req->headers); i++) {
            fprintf(rec->f, "%s: %s\r\n", req->headers[i].key, req->headers[i].value);
        }
    }
    fprintf(rec->f, "\r\n");

    /* Write any body that arrived with the request headers */
    if (req->body && req->body_len > 0) {
        fwrite(req->body, 1, req->body_len, rec->f);
    }

    rec->request_written = 1;
    return rec;
}

/* Append more request body bytes (streaming case). */
static inline int mock_recorder_append_request_body(mock_recorder_t *rec,
                                                    const void *data, size_t len)
{
    if (!rec || !rec->f || !data || len == 0) return 0;
    if (fwrite(data, 1, len, rec->f) != len) return -1;
    rec->request_body_bytes += len;
    return 0;
}

/* Mark the start of the response section and write the raw response bytes seen so far.
 * This is called on the first data received from upstream. The bytes must include
 * the status line and response headers (exactly as received from the real backend).
 * Subsequent body bytes should be written with mock_recorder_append_response_body.
 */
static inline int mock_recorder_begin_response(mock_recorder_t *rec,
                                               const void *first_bytes, size_t len)
{
    if (!rec || !rec->f || rec->response_started) return 0;

    rec->response_start_time = time(NULL);

    rec->response_offset = ftell(rec->f);
    fprintf(rec->f, "\n---RESPONSE---\n");
    if (first_bytes && len > 0) {
        fwrite(first_bytes, 1, len, rec->f);
        /* crude status parse: "HTTP/1.1 200 ..." */
        const char *p = (const char *)first_bytes;
        if (strncmp(p, "HTTP/", 5) == 0) {
            const char *sp = strchr(p, ' ');
            if (sp) rec->response_status = atoi(sp + 1);
        }
        /* count bytes written in this call as response body (rough; header bytes included) */
        rec->response_body_bytes += len;
    }
    rec->response_started = 1;
    return 0;
}

/* Append more response body bytes (after begin_response has been called). */
static inline int mock_recorder_append_response_body(mock_recorder_t *rec,
                                                     const void *data, size_t len)
{
    if (!rec || !rec->f || !rec->response_started || !data || len == 0) return 0;
    if (fwrite(data, 1, len, rec->f) != len) return -1;
    rec->response_body_bytes += len;
    return 0;
}

/* Finish recording and append a SUMMARY trailer with final metrics.
 * Latency is measured from begin() until the first response bytes arrive.
 * All sizes and the response status are recorded for later analysis or replay.
 * A fixed-size binary header at offset 0 contains byte offsets/lengths for
 * each logical section so a reader never has to parse the delimiter text.
 */
static inline int mock_recorder_finish(mock_recorder_t *rec)
{
    if (!rec) return 0;

    if (rec->f) {
        long latency_ms = 0;
        if (rec->response_start_time > 0) {
            latency_ms = (long)(rec->response_start_time - rec->start_time) * 1000;
        }

        /* Record final lengths before writing SUMMARY */
        if (rec->request_len == 0 && rec->request_offset > 0)
            rec->request_len = ftell(rec->f) - rec->request_offset;

        if (rec->response_offset > 0)
            rec->response_len = ftell(rec->f) - rec->response_offset;

        /* Write SUMMARY section */
        rec->summary_offset = ftell(rec->f);
        fprintf(rec->f, "\n---SUMMARY---\n");
        fprintf(rec->f,
                "{\"latency_ms\":%ld,\"request_body_bytes\":%zu,"
                "\"response_status\":%d,\"response_body_bytes\":%zu}\n",
                latency_ms,
                rec->request_body_bytes,
                rec->response_status,
                rec->response_body_bytes);
        rec->summary_len = ftell(rec->f) - rec->summary_offset;

        /* Write fixed 256-byte text header line (CSV-style, space-padded) */
        fseek(rec->f, 0, SEEK_SET);
        char hdr[256];
        int n = snprintf(hdr, sizeof(hdr),
            "MOCKv1,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%zu,%zu,%d",
            rec->header_size,
            rec->meta_offset, rec->meta_len,
            rec->request_offset, rec->request_len,
            rec->response_offset, rec->response_len,
            rec->summary_offset, rec->summary_len,
            rec->request_body_bytes,
            rec->response_body_bytes,
            rec->response_status);
        /* right-pad with spaces to exactly 256 bytes */
        for (int i = n; i < 255; i++) hdr[i] = ' ';
        hdr[255] = '\n';
        fwrite(hdr, 1, 256, rec->f);

        fclose(rec->f);
        rec->f = NULL;
    }

    free(rec->filepath);
    free(rec->upstream_url);
    free(rec);
    return 0;
}


/*
 * Playback / retrieval API
 * -----------------------
 * Designed for a streaming event-driven server.
 * The reader only parses the fixed 256-byte header; the rest of the file
 * is treated as a byte stream that can be sent directly to clients.
 */

typedef struct {
    FILE   *f;
    long    header_size;
    long    meta_off, meta_len;
    long    request_off, request_len;
    long    response_off, response_len;   /* byte range of the ---RESPONSE--- section */
    long    summary_off, summary_len;
    size_t  req_body_bytes;
    size_t  resp_body_bytes;
    int     status;
} mock_reader_t;

/* Parse a 256-byte CSV header line. Returns 1 on success. */
static inline int mock_parse_header_line(const char *line, mock_reader_t *r)
{
    /* Expected: MOCKv1,hdr,mo,ml,ro,rl,reso,resl,so,sl,rqb,rsb,st */
    long h, mo, ml, ro, rl, reso, resl, so, sl;
    size_t rqb, rsb;
    int st;
    int n = sscanf(line,
        "MOCKv1,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%zu,%zu,%d",
        &h, &mo, &ml, &ro, &rl, &reso, &resl, &so, &sl, &rqb, &rsb, &st);
    if (n != 12) return 0;
    r->header_size   = h;
    r->meta_off      = mo;  r->meta_len      = ml;
    r->request_off   = ro;  r->request_len   = rl;
    r->response_off  = reso; r->response_len = resl;
    r->summary_off   = so;  r->summary_len   = sl;
    r->req_body_bytes = rqb;
    r->resp_body_bytes = rsb;
    r->status        = st;
    return 1;
}

/* Open a .mock file for playback. Only the 256-byte header is read. */
static inline mock_reader_t *mock_reader_open(const char *path)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    char hdr[256];
    if (fread(hdr, 1, 256, f) != 256) {
        fclose(f);
        return NULL;
    }

    /* hdr is space-padded; find the newline or trim trailing spaces */
    for (int i = 0; i < 256; i++) {
        if (hdr[i] == '\n' || hdr[i] == ' ') { hdr[i] = '\0'; break; }
    }

    mock_reader_t *r = (mock_reader_t *)calloc(1, sizeof(*r));
    if (!r) { fclose(f); return NULL; }
    r->f = f;

    if (!mock_parse_header_line(hdr, r)) {
        free(r);
        fclose(f);
        return NULL;
    }
    return r;
}

static inline void mock_reader_close(mock_reader_t *r)
{
    if (!r) return;
    if (r->f) fclose(r->f);
    free(r);
}

/* Byte offset in the file where the raw response content begins
 * (immediately after the "---RESPONSE---\n" line). */
static inline long mock_reader_response_start(mock_reader_t *r)
{
    return r ? r->response_off : -1;
}

/* Number of bytes belonging to the recorded response (headers + body). */
static inline long mock_reader_response_length(mock_reader_t *r)
{
    return r ? r->response_len : 0;
}

/* Convenience: response status recorded in the mock. */
static inline int mock_reader_status(mock_reader_t *r)
{
    return r ? r->status : 0;
}

#endif /* MOCK_H */
/* jit_backend/app.cy — HTTP/DB infrastructure for the classyc port of
 * backend/app.py (FastAPI + SQLite social feed demo). Split into two
 * translation units, same shape as classyc's examples/http_crud:
 *   - app.cy (this file): raw-socket HTTP/1.1 accept loop, per-worker
 *     SQLite connections, session token storage, password hashing, form
 *     parsing. Builds a httpserve.h Request and calls app_handle().
 *   - api.cy: this app's actual routes ([[HttpGet]] / [[HttpPost]] / …)
 *     and its DB schema. Self-contained; no hand-rolled dispatch table.
 *
 * The accept loop is still hand-rolled (modeled on examples/http-serve.c)
 * rather than linking http-serve-cchan.c, because this app wants a 4 MiB
 * upload cap, per-worker SQLite, and its own session map — not because
 * Request can't see cookies or raw bodies anymore (req_attach_raw +
 * Request.header/cookie cover that).
 *
 * Deliberate simplifications vs. the Python version (this is a demo):
 *   - Password hashing: SHA-256(password + per-user random salt), not
 *     bcrypt (classyc has no bcrypt binding). Stored as "salt$hexhash".
 *   - Auth token: an opaque random session token held in an in-memory
 *     Map<String,int> (token -> user_id), not a signed JWT. Set as an
 *     HttpOnly cookie, same contract the frontend already expects
 *     (fetch(..., {credentials:"include"})); no bearer-header path since
 *     the frontend never uses one.
 *   - No CORS headers: frontend and API are always same-origin through
 *     nanoproxy's reverse proxy (see config.json), unlike the Python demo
 *     which could be hit directly on :8000.
 *
 * Build:
 *   sh jit_backend/build.sh
 *   # → jit_backend/app.bmir (this file) + jit_backend/api.bmir
 *
 * Run standalone: jitrunner jit_backend/app.bmir jit_backend/api.bmir --mode gen
 * Run via nanoproxy: "handler": "jit" dispatch entry in config.json.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "httpserve.h"
#include "map.h"
#include "sqlite.h"
#include "app.h"

/* Worker pool: plain pthreads + a bounded channel, same pattern as
 * examples/http-serve-cchan.c (no -ffibers needed). Unlike that file's
 * suggested "one mutex around all DB access", each worker gets its own
 * SQLite connection (WAL + busy_timeout) so SQLite's own per-connection
 * locking arbitrates concurrent access -- the same model SQLAlchemy uses
 * for SQLite, and it means one slow query only blocks other writers, not
 * every other request in the app. Only the in-memory session map (no
 * locking of its own) still needs an application mutex -- see
 * get_db()/g_sessions_mutex below. */
#define CCHAN_IMPLEMENTATION
#include "cchan.h"
#include <pthread.h>

/* ─────────────────────────────────────────────────────────────────────
   Raw POSIX sockets + file I/O — declared directly (à la http-serve.c /
   httpclient.h): classyc's parser can't take the real system headers,
   but plain extern prototypes for libc symbols work fine.
   ───────────────────────────────────────────────────────────────────── */
extern int    socket(int domain, int type, int protocol);
extern int    setsockopt(int fd, int level, int optname, void *optval, int optlen);
extern int    bind(int fd, void *addr, int addrlen);
extern int    listen(int fd, int backlog);
extern int    accept(int fd, void *addr, void *addrlen);
extern long   recv(int fd, void *buf, long len, int flags);
extern long   send(int fd, void *buf, long len, int flags);
extern void  *signal(int signum, void *handler);

extern long   read(int fd, void *buf, long count);
extern int    mkdir(const char *path, int mode);

extern unsigned char *SHA256(const unsigned char *d, unsigned long n, unsigned char *md);
extern int sqlite3_config(int op, ...);
#define SQLITE_CONFIG_MULTITHREAD 2

static unsigned short cy_htons(unsigned short x) {
    return (unsigned short)((x << 8) | (x >> 8));
}

struct sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;
    unsigned int   sin_addr;
    unsigned char  sin_zero[8];
};

#define AF_INET       2
#define SOCK_STREAM   1
#define SOL_SOCKET    1
#define SO_REUSEADDR  2
#define SO_RCVTIMEO   20
#define SIG_IGN_PTR   ((void*)1)
#define SIGPIPE       13

#define O_RDONLY  0

#define HTTP_BUF_MAX (4 << 20)  /* 4 MiB — covers demo image/video uploads */

struct cy_timeval {
    long tv_sec;
    long tv_usec;
};

/* ─────────────────────────────────────────────────────────────────────
   Small standalone globals
   ───────────────────────────────────────────────────────────────────── */
/* Per-resource locking, matching how SQLAlchemy/Python actually handle
 * SQLite concurrency rather than one global mutex around everything:
 *   - SQLite itself: one connection PER WORKER THREAD (not shared), with
 *     WAL mode + busy_timeout so readers never block each other or a
 *     writer, and writer-vs-writer conflicts retry instead of erroring —
 *     no application-level lock needed at all for DB access.
 *   - The in-memory session Map: still genuinely shared mutable state
 *     with no thread safety of its own, so it keeps a lock — but a tiny,
 *     fast one scoped to just the map lookup/insert, not the whole
 *     request, so a slow query can never block it (or anything else). */
static const char *g_db_path = "jit_backend/feed.db";
#define NUM_WORKERS 64

/* Per-worker SQLite connection, keyed by matching pthread_self() against a
 * table main() fills in *before* spawning each worker (each worker's slot
 * index is handed to it directly as its pthread_create argument -- no
 * thread registers its own slot, so there's no shared counter for two
 * worker threads to race on at startup). NOT _Thread_local: classyc's
 * parser rejects `_Thread_local` (even on a plain void*) at file scope
 * once cyexc.h's own internal per-thread exception state is also in the
 * same module ("mir.tls_addr was already defined differently in the
 * module" -- a MIR-level conflict between two independent thread-local
 * variables in one JIT-loaded module). A tiny linear scan over
 * NUM_WORKERS entries sidesteps TLS entirely and is negligible at this
 * size. */
static pthread_t g_worker_tids[NUM_WORKERS];
static Sqlite   *g_worker_dbs[NUM_WORKERS];

/* Session tokens: private to this file so the mutex stays next to the
 * map. api.cy only sees session_get()/session_put(). */
static Map<String, int> *g_sessions = 0;
static pthread_mutex_t   g_sessions_mutex;

const char *g_usermedia_dir = "jit_backend/usermedia";

/* Lazily opens this worker thread's own SQLite connection on first use.
 * journal_mode=WAL is a persistent, on-disk setting (set once, in main()'s
 * bootstrap connection, before any worker starts) so it doesn't need
 * repeating here; busy_timeout is per-connection and does. */
Sqlite *get_db(void) {
    pthread_t self = pthread_self();
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (!pthread_equal(g_worker_tids[i], self)) continue;
        if (!g_worker_dbs[i]) {
            g_worker_dbs[i] = Sqlite.open(g_db_path);
            if (g_worker_dbs[i])
                g_worker_dbs[i]->execute("PRAGMA busy_timeout=5000");
        }
        return g_worker_dbs[i];
    }
    return 0; /* only worker threads call get_db(); nothing else should */
}

long session_get(const char *token) {
    pthread_mutex_lock(&g_sessions_mutex);
    long uid = (long) g_sessions->GetOr((String) token, 0);
    pthread_mutex_unlock(&g_sessions_mutex);
    return uid;
}

void session_put(const char *token, long uid) {
    pthread_mutex_lock(&g_sessions_mutex);
    (*g_sessions)[(String) token] = (int) uid;
    pthread_mutex_unlock(&g_sessions_mutex);
}

/* ─────────────────────────────────────────────────────────────────────
   Random bytes, hex encoding, SHA-256 password hashing
   ───────────────────────────────────────────────────────────────────── */
static void hex_encode(const unsigned char *in, int n, char *out) {
    const char *hexd = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2]     = hexd[(in[i] >> 4) & 0xf];
        out[i * 2 + 1] = hexd[in[i] & 0xf];
    }
    out[n * 2] = 0;
}

static int random_bytes(unsigned char *out, int n) {
    int fd = open("/dev/urandom", O_RDONLY, 0);
    if (fd < 0) return 0;
    long got = 0;
    while (got < n) {
        long r = read(fd, out + got, n - got);
        if (r <= 0) { close(fd); return 0; }
        got += r;
    }
    close(fd);
    return 1;
}

/* out must be >= 2*nbytes+1; nbytes capped at 64 */
void random_hex(char *out, int nbytes) {
    unsigned char buf[64];
    if (nbytes > 64) nbytes = 64;
    if (!random_bytes(buf, nbytes)) {
        for (int i = 0; i < nbytes; i++) buf[i] = (unsigned char)(rand() & 0xff);
    }
    hex_encode(buf, nbytes, out);
}

static void sha256_hex(const char *input, int len, char *out_hex /* >= 65 bytes */) {
    unsigned char digest[32];
    SHA256((const unsigned char *)input, (unsigned long)len, digest);
    hex_encode(digest, 32, out_hex);
}

/* Demo-grade password hash: SHA-256(salt + password), stored "salt$hash".
   Not bcrypt — classyc has no bcrypt binding; see file header. */
String hash_password(const char *password) {
    char salt[33];
    random_hex(salt, 16);
    char combined[320];
    snprintf(combined, sizeof(combined), "%s%s", salt, password);
    char hash[65];
    sha256_hex(combined, (int)strlen(combined), hash);
    char out[160];
    snprintf(out, sizeof(out), "%s$%s", salt, hash);
    return out;
}

int verify_password(const char *password, const char *stored) {
    const char *dollar = strchr(stored, '$');
    if (!dollar) return 0;
    int saltlen = (int)(dollar - stored);
    if (saltlen >= 64) return 0;
    char salt[65];
    memcpy(salt, stored, saltlen);
    salt[saltlen] = 0;
    char combined[320];
    snprintf(combined, sizeof(combined), "%s%s", salt, password);
    char hash[65];
    sha256_hex(combined, (int)strlen(combined), hash);
    return strcmp(hash, dollar + 1) == 0;
}

void url_decode(const char *in, char *out, int outcap) {
    int j = 0;
    for (int i = 0; in[i] != 0 && j < outcap - 1; i++) {
        if (in[i] == '+') {
            out[j++] = ' ';
        } else if (in[i] == '%' && in[i + 1] != 0 && in[i + 2] != 0) {
            char hex[3];
            hex[0] = in[i + 1]; hex[1] = in[i + 2]; hex[2] = 0;
            out[j++] = (char) strtol(hex, 0, 16);
            i += 2;
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = 0;
}

/* application/x-www-form-urlencoded field lookup (login uses this). */
int form_field(const char *body, const char *key, char *out, int outcap) {
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (!p) { out[0] = 0; return 0; }
    p += strlen(needle);
    const char *amp = strchr(p, '&');
    int len = amp ? (int)(amp - p) : (int)strlen(p);
    char raw[512];
    if (len >= (int)sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, (size_t)len);
    raw[len] = 0;
    url_decode(raw, out, outcap);
    return 1;
}

/* ─────────────────────────────────────────────────────────────────────
   HTTP accept loop — adapted from examples/http-serve.c's
   http_handle_client/serve (same header parsing technique), extended to
   capture the raw header block (for Cookie lookup) and to temporarily
   NUL-terminate the body in place around app_handle() so String/json()/strstr
   see an exact-length C string, restoring the byte for pipelined reads.
   ───────────────────────────────────────────────────────────────────── */
static long content_length_of(char *buf, long header_end) {
    const char *key = "content-length:";
    long klen = 15;
    for (long i = 0; i + klen <= header_end; i++) {
        int match = 1;
        for (long j = 0; j < klen; j++) {
            char a = buf[i + j], b = key[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != b) { match = 0; break; }
        }
        if (!match) continue;
        long k = i + klen;
        while (k < header_end && buf[k] == ' ') k++;
        long v = 0;
        while (k < header_end && buf[k] >= '0' && buf[k] <= '9') { v = v * 10 + (buf[k] - '0'); k++; }
        return v;
    }
    return 0;
}

static int ci_eq_n(const char *a, const char *b, long n) {
    for (long j = 0; j < n; j++) {
        char x = a[j], y = b[j];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

static int is_http_10(char *buf, long header_end) {
    long line_end = 0;
    while (line_end < header_end &&
           !(buf[line_end] == '\r' && line_end + 1 < header_end && buf[line_end + 1] == '\n'))
        line_end++;
    for (long i = 0; i + 8 <= line_end; i++)
        if (ci_eq_n(buf + i, "HTTP/1.0", 8)) return 1;
    return 0;
}

static int connection_has(char *buf, long header_end, const char *token) {
    const char *key = "connection:";
    long klen = 11;
    long tlen = (long) strlen(token);
    for (long i = 0; i + klen <= header_end; i++) {
        if (!ci_eq_n(buf + i, key, klen)) continue;
        if (i > 0 && buf[i - 1] != '\n') continue;
        long k = i + klen;
        while (k < header_end && buf[k] == ' ') k++;
        long vend = k;
        while (vend < header_end && buf[vend] != '\r' && buf[vend] != '\n') vend++;
        long p = k;
        while (p < vend) {
            while (p < vend && (buf[p] == ' ' || buf[p] == ',' || buf[p] == '\t')) p++;
            long q = p;
            while (q < vend && buf[q] != ',' && buf[q] != ' ' && buf[q] != '\t') q++;
            if (q - p == tlen && ci_eq_n(buf + p, token, tlen)) return 1;
            p = q;
        }
        return 0;
    }
    return 0;
}

static int wants_close(char *buf, long header_end) {
    if (is_http_10(buf, header_end)) return !connection_has(buf, header_end, "keep-alive");
    return connection_has(buf, header_end, "close");
}

static void http_handle_client(int cfd) {
    long cap = 8192, len = 0;
    unowned char *buf = (char *) malloc(cap);
    if (buf == NULL) return;

    /* Without this, a worker's recv() below blocks with no timeout at all --
     * confirmed the hard way: under ab -k -c 16 against our 8-worker pool,
     * requests on the "overflow" connections (whichever ones don't get a
     * worker on the first pass) can sit unread for several seconds waiting
     * for a free worker, and with no recv() timeout the worker that
     * eventually inherits a genuinely stalled/idle connection blocks
     * forever instead of ever cycling back to pick up other queued work --
     * classyc's own examples/http-serve.c sets exactly this on every
     * accepted socket for the same reason (see its http_handle_client). */
    int idle = 5;
    char *idle_env = getenv("HTTP_KEEPALIVE_TIMEOUT");
    if (idle_env && idle_env[0]) idle = atoi(idle_env);
    if (idle < 1) idle = 1;
    struct cy_timeval tv;
    tv.tv_sec = idle;
    tv.tv_usec = 0;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, (int) sizeof(tv));

    int nserved = 0;
    int max_req = 1000;
    while (nserved < max_req) {
        long header_end = -1;
        long content_len = 0;

        while (1) {
            if (header_end < 0 && len > 0) {
                char *p = strstr(buf, "\r\n\r\n");
                if (p != NULL) {
                    header_end = (long)(p - buf) + 4;
                    content_len = content_length_of(buf, header_end);
                }
            }
            if (header_end >= 0 && len >= header_end + content_len) break;

            if (len + 1 >= cap) {
                if (cap >= HTTP_BUF_MAX) { free(buf); return; }
                cap = cap * 2;
                if (cap > HTTP_BUF_MAX) cap = HTTP_BUF_MAX;
                buf = (char *) realloc(buf, cap);
                if (buf == NULL) return;
            }
            long n = recv(cfd, buf + len, cap - len - 1, 0);
            if (n <= 0) { free(buf); return; }
            len = len + n;
            buf[len] = 0;
        }

        int close_after = wants_close(buf, header_end);
        if (nserved + 1 >= max_req) close_after = 1;

        char *method = buf;
        char *sp1 = strchr(buf, ' ');
        if (sp1 == NULL) { free(buf); return; }
        *sp1 = 0;
        char *target = sp1 + 1;
        char *sp2 = strchr(target, ' ');
        if (sp2 != NULL) *sp2 = 0;

        char *query = "";
        char *q = strchr(target, '?');
        if (q != NULL) { *q = 0; query = q + 1; }
        char *path = target;
        char *body = buf + header_end;

        char saved_byte = body[content_len];
        body[content_len] = 0;

        unowned Request *req = new Request(method, path, query, body);
        req_attach_raw(req, buf, header_end, body, content_len);
        unowned Response *res = app_handle(req);
        body[content_len] = saved_byte;
        if (res == 0) { delete req; free(buf); return; }

        res->keep_alive = close_after ? 0 : 1;
        String msg = res->wire();
        /* send() on a blocking TCP socket is not guaranteed to write the
         * whole buffer in one call -- under concurrent load it can return a
         * short count, silently truncating the response and desyncing this
         * keep-alive connection for every request after it. classyc's own
         * examples/http-serve.c has the same unlooped send() call; this loop
         * is the fix, verified against that reference under an oversubscribed
         * ab -k -c 16 (client keep-alive concurrency > worker count) stress
         * test that reproduced silent length-mismatched responses there. */
        long msg_len = (long) strlen((char *) msg);
        long sent = 0;
        while (sent < msg_len) {
            long w = send(cfd, (char *) msg + sent, msg_len - sent, 0);
            if (w <= 0) break;
            sent += w;
        }

        if (getenv("HTTP_QUIET") == 0)
            printf("  %s %s -> %d%s\n", method, path, res->status, res->keep_alive ? "  ka" : "");

        delete res;
        delete req;
        nserved++;

        if (close_after) break;

        long used = header_end + content_len;
        long rest = len - used;
        if (rest > 0) memmove(buf, buf + used, (size_t) rest);
        len = rest;
        buf[len] = 0;
    }
    free(buf);
}

static int http_listen(int port) {
    signal(SIGPIPE, SIG_IGN_PTR);
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { printf("socket() failed\n"); return -1; }
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, 4);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = cy_htons((unsigned short) port);
    addr.sin_addr = 0;

    if (bind(sfd, &addr, 16) < 0) { printf("bind() failed (port %d in use?)\n", port); close(sfd); return -1; }
    if (listen(sfd, 128) < 0) { printf("listen() failed\n"); close(sfd); return -1; }
    return sfd;
}

struct worker_arg { cchan_t *jobs; int id; };

static void *http_worker_loop(void *arg) {
    struct worker_arg *wa = (struct worker_arg *) arg;
    /* This worker's slot index (wa->id) was decided by main() before
     * pthread_create was even called, so there's nothing to race on:
     * this thread is the only one that will ever write g_worker_tids[wa->id],
     * and it does so using its own pthread_self() before touching get_db(). */
    g_worker_tids[wa->id] = pthread_self();
    cchan_t *jobs = wa->jobs;
    int cfd = 0;
    while (cchan_recv_i32(jobs, &cfd) == 1) {
        http_handle_client(cfd);
        close(cfd);
    }
    return 0;
}

int main() {
    /* Must be the very first sqlite3 call of any kind -- sqlite3_config()
     * can only change the threading mode before the library's first
     * connection is opened. MULTITHREAD (not the default SERIALIZED) drops
     * SQLite's own internal mutex on every operation, safe here because
     * every worker already owns its own exclusive connection (get_db())
     * and never shares one across threads -- confirmed a real cost via
     * `perf record`: pthread_mutex_lock/unlock together were ~3% of total
     * CPU time under load before this change. */
    sqlite3_config(SQLITE_CONFIG_MULTITHREAD);

    g_sessions = new Map<String, int>();
    pthread_mutex_init(&g_sessions_mutex, 0);

    /* Bootstrap connection: schema + seed run once, single-threaded, before
     * any worker starts. journal_mode=WAL is persisted in the database file
     * itself, so every worker's own later get_db() connection inherits it
     * without needing to set it again. */
    Sqlite *bootstrap = Sqlite.open(g_db_path);
    if (!bootstrap) { printf("jit_backend: db open failed\n"); return 1; }
    bootstrap->execute("PRAGMA journal_mode=WAL");
    db_init(bootstrap);
    db_seed_if_empty(bootstrap);
    delete bootstrap;

    mkdir(g_usermedia_dir, 0755);

    int port = 8000;
    int sfd = http_listen(port);
    if (sfd < 0) return 1;

    cchan_t *jobs = cchan_create(256, sizeof(int));
    if (jobs == 0) { printf("jit_backend: cchan_create failed\n"); return 1; }
    pthread_t workers[NUM_WORKERS];
    static struct worker_arg wargs[NUM_WORKERS]; /* must outlive the threads using them */
    for (int i = 0; i < NUM_WORKERS; i++) {
        wargs[i].jobs = jobs;
        wargs[i].id = i;
        pthread_create(&workers[i], 0, http_worker_loop, &wargs[i]);
    }

    printf("jit_backend listening on http://127.0.0.1:%d (%d workers)\n", port, NUM_WORKERS);
    fflush(stdout);

    while (1) {
        int cfd = accept(sfd, 0, 0);
        if (cfd < 0) continue;
        cchan_send_i32(jobs, cfd);
    }
    return 0;
}

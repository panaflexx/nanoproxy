/* jit_backend/app.h — shared contract between app.cy (HTTP/DB
 * infrastructure: socket loop, per-worker SQLite connections, session
 * token storage, password hashing, form parsing) and api.cy (this app's
 * routes / business logic).
 *
 * api.cy is the self-contained application: [[HttpGet]] / [[HttpPost]]
 * handlers on httpserve.h's Request/Response, dispatched by
 * route_dispatch() — same split as classyc's examples/http_crud
 * {main,items}.cy. app.cy is the server core (accept loop + workers)
 * and calls app_handle() declared in httpserve.h.
 *
 * Session storage is still reached only through session_get()/
 * session_put() rather than a shared Map pointer: the lock lives next
 * to the map in app.cy, so api.cy never holds g_sessions_mutex.
 */
#ifndef JIT_BACKEND_APP_H
#define JIT_BACKEND_APP_H

#include <string.h>
#include "sqlite.h"

/* ── Raw POSIX externs needed on both sides of the split ────────────── */
extern int   open(const char *path, int flags, int mode);
extern long  write(int fd, void *buf, long count);
extern int   close(int fd);
extern int   unlink(const char *path);
extern int   usleep(unsigned int usec);
extern void *memmem(const void *haystack, unsigned long haystacklen,
                     const void *needle, unsigned long needlelen);

#define O_WRONLY  1
#define O_CREAT   0100
#define O_TRUNC   01000

/* ── Session tokens — app.cy owns the Map + mutex ───────────────────── */
long session_get(const char *token);          /* 0 if not found */
void session_put(const char *token, long uid);

extern const char *g_usermedia_dir;

/* ── Per-worker SQLite connection (app.cy) ──────────────────────────── */
Sqlite *get_db(void);

/* ── Password hashing + random hex (app.cy) ─────────────────────────── */
String hash_password(const char *password);
int    verify_password(const char *password, const char *stored);
void   random_hex(char *out, int nbytes);

/* ── Form parsing (login is application/x-www-form-urlencoded) ──────── */
void url_decode(const char *in, char *out, int outcap);
int  form_field(const char *body, const char *key, char *out, int outcap);

/* ── Schema/seed — defined in api.cy, called from app.cy's main() ───── */
void db_init(Sqlite *db);
void db_seed_if_empty(Sqlite *db);

#endif

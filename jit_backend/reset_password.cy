/* reset_password.cy — passwd-style password reset for jit_backend's feed.db.
 *
 * The Python backend (backend/app.py) stores bcrypt hashes, but jit_backend's
 * verify_password() (app.cy) expects its own demo-grade format
 * "salt$SHA256hex(salt + password)". Copying the Python feed.db into
 * jit_backend therefore breaks logins. This resets ONE user's password,
 * prompting for it twice (echo off, like passwd).
 *
 * Usage (from the nanoproxy repo root):
 *   classyc -l sqlite3 -l crypto jit_backend/reset_password.cy -eg <username> [db_path]
 * or AOT:
 *   classyc -l sqlite3 -l crypto -o reset_password.bmir -c jit_backend/reset_password.cy
 *   b2obj reset_password.bmir reset_password.o && gcc -o reset_password reset_password.o -lsqlite3 -lcrypto
 *   ./reset_password <username> [db_path]
 *
 * db_path defaults to jit_backend/feed.db (repo-root-relative).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite.h"

extern unsigned char *SHA256(const unsigned char *d, unsigned long n, unsigned char *md);
extern char  *getpass(const char *prompt);   /* libc: reads /dev/tty, echo off */
extern int    open(const char *path, int flags, int mode);
extern long   read(int fd, void *buf, long count);
extern int    close(int fd);

#define DEFAULT_DB "jit_backend/feed.db"

static void hex_encode(const unsigned char *in, int n, char *out) {
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[in[i] & 0xf];
    }
    out[n * 2] = 0;
}

static int random_bytes(unsigned char *out, int n) {
    int fd = open("/dev/urandom", 0 /* O_RDONLY */, 0);
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

static void sha256_hex(const char *input, int len, char *out_hex /* >= 65 bytes */) {
    unsigned char digest[32];
    SHA256((const unsigned char *)input, (unsigned long)len, digest);
    hex_encode(digest, 32, out_hex);
}

/* Same format as app.cy's hash_password(): salt$SHA256hex(salt + password),
 * salt = 16 random bytes as 32 hex chars. */
static void hash_password(const char *password, char *out /* >= 160 */) {
    unsigned char raw[16];
    if (!random_bytes(raw, 16)) {
        for (int i = 0; i < 16; i++) raw[i] = (unsigned char)(rand() & 0xff);
    }
    char salt[33];
    hex_encode(raw, 16, salt);
    char combined[320];
    snprintf(combined, sizeof(combined), "%s%s", salt, password);
    char hash[65];
    sha256_hex(combined, (int)strlen(combined), hash);
    snprintf(out, 160, "%s$%s", salt, hash);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: reset_password <username> [db_path]\n");
        return 2;
    }
    char *uname = argv[1];
    const char *dbpath = argc > 2 ? argv[2] : DEFAULT_DB;

    Sqlite *db = Sqlite.open(dbpath);
    if (!db) { fprintf(stderr, "reset_password: cannot open %s\n", dbpath); return 1; }
    defer delete db;

    dict row = db->query_one("SELECT username FROM users WHERE username = ?", "s", uname);
    if (!row) {
        fprintf(stderr, "reset_password: no such user: %s\n", uname);
        return 1;
    }

    char *g1 = getpass("New password: ");
    if (!g1) { fprintf(stderr, "reset_password: getpass failed\n"); return 1; }
    /* getpass returns a static buffer — copy before the second call overwrites it */
    char p1[256];
    snprintf(p1, sizeof(p1), "%s", g1);
    char *p2 = getpass("Retype new password: ");
    if (!p2) { fprintf(stderr, "reset_password: getpass failed\n"); return 1; }
    if (strcmp(p1, p2) != 0) {
        fprintf(stderr, "reset_password: passwords do not match\n");
        return 1;
    }
    if (strlen(p1) == 0) {
        fprintf(stderr, "reset_password: empty password not allowed\n");
        return 1;
    }

    char stored[160];
    hash_password(p1, stored);
    db->execute("UPDATE users SET password_hash = ? WHERE username = ?",
                "ss", stored, uname);
    printf("reset_password: password for '%s' updated in %s\n", uname, dbpath);
    return 0;
}

/* jit_backend/api.cy — this app's REST API, self-contained.
 *
 * Controllers register with [[HttpGet]] / [[HttpPost]] / … the same way
 * classyc's examples/http_crud/items.cy does. No central switch / ROUTE
 * table. HTTP/DB infrastructure (sockets, workers, sessions, hashing)
 * lives in app.cy; this file is the developer-facing code: schema, seed,
 * and every handler.
 *
 *   [[HttpGet("/api/posts/{id}/comments")]]
 *   static Response *h_get_comments(Request *req) {
 *       long post_id = (long) req->argInt("id");
 *       ...
 *   }
 *
 * Mirrors backend/schema.sql + backend/app.py's routes, minus the unused
 * "friends" table (no endpoint reads or writes it in either version).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "httpserve.h"
#include "list.h"
#include "sqlite.h"
#include "app.h"
#include "jsonbuild.h"

/* Expands to separate statements (not a function call), which is the
 * point: jb_key() must run and complete BEFORE valexpr is evaluated, and
 * a plain function jb_field(jb, obj, key, valexpr) can't guarantee that
 * -- valexpr would already be evaluated as an argument before the
 * function body (which creates the key) ever runs. See jsonbuild.h's
 * comment on jb_key/jb_attach for what breaks if this order is violated
 * (found the hard way: every key/value pair in a response silently
 * swapped). */
#define JB_FIELD(jb, obj, key, valexpr) do { \
    JBNode *_jbf_k = jb_key((jb), (key)); \
    JBNode *_jbf_v = (valexpr); \
    jb_attach((jb), (obj), _jbf_k, _jbf_v); \
} while (0)

/* ─────────────────────────────────────────────────────────────────────
   Database: schema + demo seed data.
   ───────────────────────────────────────────────────────────────────── */
void db_init(Sqlite *db) {
    db->execute(
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT UNIQUE NOT NULL,"
        "  display_name TEXT,"
        "  avatar_url TEXT,"
        "  password_hash TEXT,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP)");
    db->execute(
        "CREATE TABLE IF NOT EXISTS posts ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  user_id INTEGER NOT NULL,"
        "  type TEXT NOT NULL,"
        "  text TEXT,"
        "  quote TEXT,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE)");
    db->execute(
        "CREATE TABLE IF NOT EXISTS media ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  post_id INTEGER NOT NULL,"
        "  type TEXT NOT NULL,"
        "  url TEXT NOT NULL,"
        "  position INTEGER DEFAULT 0,"
        "  FOREIGN KEY (post_id) REFERENCES posts(id) ON DELETE CASCADE)");
    db->execute(
        "CREATE TABLE IF NOT EXISTS comments ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  post_id INTEGER NOT NULL,"
        "  user_id INTEGER NOT NULL,"
        "  text TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY (post_id) REFERENCES posts(id) ON DELETE CASCADE,"
        "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE)");
    db->execute(
        "CREATE TABLE IF NOT EXISTS likes ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  post_id INTEGER NOT NULL,"
        "  user_id INTEGER NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE(post_id, user_id),"
        "  FOREIGN KEY (post_id) REFERENCES posts(id) ON DELETE CASCADE,"
        "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE)");
    db->execute("CREATE INDEX IF NOT EXISTS idx_posts_created ON posts(created_at DESC)");
    db->execute("CREATE INDEX IF NOT EXISTS idx_comments_post ON comments(post_id, created_at)");
    db->execute("CREATE INDEX IF NOT EXISTS idx_likes_post ON likes(post_id)");
}

void db_seed_if_empty(Sqlite *db) {
    dict row = db->query_one("SELECT COUNT(*) AS n FROM users");
    defer delete row;
    long n = row ? (long)row["n"] : 0;
    if (n > 0) return;

    const char *names[4][2] = {
        {"alice", "Alice Chen"}, {"bob", "Bob Rivera"},
        {"carol", "Carol Kim"},  {"dave", "Dave Patel"}
    };
    for (int i = 0; i < 4; i++) {
        String ph = hash_password("demo");
        char avatar[64];
        snprintf(avatar, sizeof(avatar), "https://i.pravatar.cc/150?img=%d", i + 1);
        db->execute(
            "INSERT INTO users (username, display_name, avatar_url, password_hash) "
            "VALUES (?, ?, ?, ?)",
            "ssss", names[i][0], names[i][1], avatar, (char *)ph);
    }

    db->execute("INSERT INTO posts (user_id, type, text, quote) VALUES "
                  "(1, 'text', 'Just deployed the new nanoserver! Loving the performance.', NULL),"
                  "(2, 'single-media', 'Check out this sunset from my hike today.', NULL),"
                  "(3, 'big-text', NULL, 'The only way to do great work is to love what you do. — Steve Jobs'),"
                  "(1, 'multi-media', 'Some photos from the weekend trip', NULL),"
                  "(4, 'text', 'Anyone else obsessed with the new Zed editor?', NULL)");

    db->execute("INSERT INTO media (post_id, type, url, position) VALUES "
                  "(2, 'image', 'https://picsum.photos/id/1018/600/400', 0),"
                  "(4, 'image', 'https://picsum.photos/id/1005/600/400', 0),"
                  "(4, 'image', 'https://picsum.photos/id/1016/600/400', 1)");

    db->execute("INSERT INTO comments (post_id, user_id, text) VALUES "
                  "(1, 2, 'Congrats! How''s the new architecture working out?'),"
                  "(1, 3, 'Need to check that out.'),"
                  "(2, 1, 'Gorgeous shot!'),"
                  "(4, 3, 'Completely agree. The collab features are insane.')");

    db->execute("INSERT OR IGNORE INTO likes (post_id, user_id) VALUES "
                  "(1, 2), (1, 3), (2, 1), (3, 1), (3, 2), (4, 3)");

    printf("db: seeded demo data\n");
}

static dict get_user_row(long id) {
    return get_db()->query_one("SELECT id, username, display_name FROM users WHERE id=?", "l", id);
}

/* FastAPI-shaped errors (`{"detail":...}`) — login.html reads data.detail.
 *
 * Frees `body` after serializing it: dict.h's own header comment admits its
 * allocations "don't survive the ownership checker", and confirmed the hard
 * way under load -- every response tree built via dict_create_object/
 * dict_create_array (resp/arr/d, and everything nested into them via
 * dict_object_set/dict_array_append) was leaking ~20KB/request on the
 * heaviest route (/api/posts), growing jit_backend's RSS by over 1GB in a
 * few minutes of benchmarking and visibly degrading throughput as the heap
 * grew (6-7k req/s new, ~2.6k req/s after). `delete body` here calls
 * dict_destroy, which recurses through the whole tree in one call, so this
 * single fix point covers every caller. */
static Response *json_resp(int status, const char *status_text, dict body) {
    String b = body ? (String) body.json() : (String) "null";
    if (body) delete body;
    return new Response(status, status_text, b);
}

static Response *err_resp(int status, const char *status_text, const char *detail) {
    dict d = dict_create_object();
    dict_object_set(d, "detail", dict_create_string(detail));
    return json_resp(status, status_text, d);
}

static long auth_uid(Request *req) {
    String tok = req->cookie("access_token");
    if ((char *)tok == 0 || ((char *)tok)[0] == 0) return 0;
    return session_get((char *)tok);
}

/* ─────────────────────────────────────────────────────────────────────
   Post serialization (mirrors Python's PostResponse shape).

   Built on jsonbuild.h (cejson-backed) instead of classyc's dict, since
   this is the hottest response path in the app (called once per post,
   ~5-15 field writes each) and profiling (perf record -g under ab -k
   load) showed ~10% of all CPU in malloc/free machinery, consistent with
   dict's one-heap-allocation-per-node model. cejson bump-allocates all
   nodes from one pre-sized array per request; only leaf string/number
   values still need their own small malloc (see jsonbuild.cy).

   `out_array`: if non-NULL, the built post object is appended to it
   (h_get_posts's shape: many posts sharing one JBuilder + one outer
   array). If NULL, the post object itself becomes the JBuilder's root
   (h_create_post's shape: one post, nothing wrapping it) -- cejson always
   treats whichever node was created first in a JBuilder as node 0, so
   this only works correctly when post_to_json's own jb_object() call is
   the very first builder call made against `jb`.
   ───────────────────────────────────────────────────────────────────── */
static JBNode *post_to_json(JBuilder *jb, JBNode *out_array, dict prow, long viewer_id) {
    long post_id = (long) prow["id"];
    JBNode *d = jb_object(jb);
    JB_FIELD(jb,d, "id", jb_int(jb, post_id));

    const char *disp  = (char *) prow["display_name"];
    const char *uname = (char *) prow["username"];
    JB_FIELD(jb,d, "user", jb_string(jb, (disp && disp[0]) ? disp : uname));
    JB_FIELD(jb,d, "timestamp", jb_string(jb, "Just now"));
    JB_FIELD(jb,d, "type", jb_string(jb, (char *) prow["type"]));

    const char *text = (char *) prow["text"];
    JB_FIELD(jb,d, "text", jb_string(jb, text ? text : ""));
    const char *quote = (char *) prow["quote"];
    JB_FIELD(jb,d, "quote", jb_string(jb, quote ? quote : ""));

    /* NOT JB_FIELD here: JB_FIELD only orders correctly when the value is
     * a single fresh jb_TYPE(...) call. For a container value, the key
     * must be created BEFORE the container's entire subtree (array header
     * + every element), not just before the final attach -- otherwise the
     * key node ends up positioned AFTER the whole subtree in cejson's flat
     * array, breaking adjacency for this pair and desyncing every field
     * that follows it (found the hard way: "media" vanished and the next
     * field printed as an empty-string key). So: key first, then build the
     * value completely, then jb_attach() at the end once both exist. */
    JBNode *media_key = jb_key(jb, "media");
    JBNode *media = jb_array(jb);
    List<dict> *media_rows = get_db()->query(
        "SELECT type, url FROM media WHERE post_id=? ORDER BY position", "l", post_id);
    defer delete media_rows;
    for (auto m in media_rows) {
        JBNode *mi = jb_object(jb);
        JB_FIELD(jb,mi, "type", jb_string(jb, (char *) m["type"]));
        JB_FIELD(jb,mi, "url", jb_string(jb, (char *) m["url"]));
        jb_append(jb, media, mi);
        delete m; /* List<dict>'s own destructor doesn't free dict-typed
                     elements (dict isn't a real classyc class with a
                     destructor __destroy can dispatch to) -- confirmed
                     under load: each row here was leaking independently
                     of the List's own backing-array cleanup. */
    }
    jb_attach(jb, d, media_key, media);

    JBNode *comments_key = jb_key(jb, "comments");
    JBNode *comments = jb_array(jb);
    List<dict> *crows = get_db()->query(
        "SELECT c.text, u.username FROM comments c JOIN users u ON c.user_id=u.id "
        "WHERE c.post_id=? ORDER BY c.created_at ASC LIMIT 3", "l", post_id);
    defer delete crows;
    for (auto c in crows) {
        JBNode *ci = jb_object(jb);
        JB_FIELD(jb,ci, "author", jb_string(jb, (char *) c["username"]));
        JB_FIELD(jb,ci, "text", jb_string(jb, (char *) c["text"]));
        jb_append(jb, comments, ci);
        delete c; /* see media_rows loop above */
    }
    jb_attach(jb, d, comments_key, comments);

    /* One round trip instead of three: likes count, "did viewer like this",
     * and comment count as scalar subqueries in a single query_one(). Each
     * query_one/query call is a fresh sqlite3_prepare_v2 (no statement
     * caching in classyc's Sqlite class), so cutting 3 prepares down to 1
     * here directly cuts post_to_json's per-post query count from 5 to 3. */
    dict counts = get_db()->query_one(
        "SELECT "
        "  (SELECT COUNT(*) FROM likes WHERE post_id=?) AS like_count, "
        "  (SELECT COUNT(*) FROM likes WHERE post_id=? AND user_id=?) AS liked, "
        "  (SELECT COUNT(*) FROM comments WHERE post_id=?) AS comment_count",
        "llll", post_id, post_id, viewer_id, post_id);
    defer delete counts;
    JB_FIELD(jb,d, "likes", jb_int(jb, counts ? (long) counts["like_count"] : 0));
    JB_FIELD(jb,d, "liked", jb_bool(jb, counts && (long) counts["liked"] > 0));
    JB_FIELD(jb,d, "comment_count", jb_int(jb, counts ? (long) counts["comment_count"] : 0));

    long owner_id = (long) prow["user_id"];
    JB_FIELD(jb,d, "owned_by_me", jb_bool(jb, viewer_id > 0 && owner_id == viewer_id));

    if (out_array) jb_append(jb, out_array, d);
    return d;
}

/* ─────────────────────────────────────────────────────────────────────
   Multipart/form-data upload — hand-rolled (classyc has no multipart
   support). Uses memmem (explicit lengths) rather than strstr so binary
   file content with embedded NUL bytes can't truncate the boundary scan.
   ───────────────────────────────────────────────────────────────────── */
static long find_bytes(const char *hay, long haylen, const char *needle, long needlelen, long start) {
    if (start < 0 || start > haylen - needlelen) return -1;
    void *p = memmem(hay + start, (unsigned long)(haylen - start), needle, (unsigned long)needlelen);
    if (!p) return -1;
    return (long)((char *) p - hay);
}

/* Writes one part's data to usermedia/ and returns {"url","type"} — or 0
   if the part's Content-Type isn't image/* or video/* (skipped, not an
   error, unlike the stricter Python version — see file header). */
static dict save_upload_part(const char *part_headers, const char *data, long data_len) {
    const char *fn_key = "filename=\"";
    const char *fp = strstr(part_headers, fn_key);
    char filename[256];
    filename[0] = 0;
    if (fp) {
        fp += strlen(fn_key);
        const char *end = strchr(fp, '"');
        int len = end ? (int)(end - fp) : 0;
        if (len > 0 && len < (int)sizeof(filename)) {
            memcpy(filename, fp, (size_t)len);
            filename[len] = 0;
        }
    }

    const char *ct_key = "Content-Type:";
    const char *ctp = strstr(part_headers, ct_key);
    char parttype[128];
    strcpy(parttype, "application/octet-stream");
    if (ctp) {
        ctp += strlen(ct_key);
        while (*ctp == ' ') ctp++;
        const char *end = strstr(ctp, "\r\n");
        int len = end ? (int)(end - ctp) : (int)strlen(ctp);
        if (len > 0 && len < (int)sizeof(parttype)) {
            memcpy(parttype, ctp, (size_t)len);
            parttype[len] = 0;
        }
    }

    int is_video = (strncmp(parttype, "video/", 6) == 0);
    int is_image = (strncmp(parttype, "image/", 6) == 0);
    if (!is_video && !is_image) return 0;

    const char *ext = strrchr(filename, '.');
    if (!ext) ext = ".bin";
    char unique[64];
    random_hex(unique, 16);
    char destpath[512];
    snprintf(destpath, sizeof(destpath), "%s/%s%s", g_usermedia_dir, unique, ext);

    int fd = open(destpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    long off = 0;
    while (off < data_len) {
        long w = write(fd, (void *)(data + off), data_len - off);
        if (w <= 0) break;
        off += w;
    }
    close(fd);

    dict item = dict_create_object();
    char publicurl[128];
    snprintf(publicurl, sizeof(publicurl), "/usermedia/%s%s", unique, ext);
    dict_object_set(item, "url", dict_create_string(publicurl));
    dict_object_set(item, "type", dict_create_string(is_video ? "video" : "image"));
    return item;
}

/* ─────────────────────────────────────────────────────────────────────
   Route handlers — self-register via [[HttpGet]] / [[HttpPost]] / …
   ───────────────────────────────────────────────────────────────────── */

[[HttpPost("/api/register")]]
static Response *h_register(Request *req) {
    if (req->body == 0) return err_resp(400, "Bad Request", "invalid JSON");

    const char *username = (char *) req->body["username"];
    const char *password = (char *) req->body["password"];
    if (!username || !username[0] || !password || !password[0])
        return err_resp(400, "Bad Request", "username and password required");

    dict existing = get_db()->query_one("SELECT id FROM users WHERE username=?", "s", username);
    defer delete existing;
    if (existing) return err_resp(409, "Conflict", "Username already taken");

    const char *display_name = (char *) req->body["display_name"];
    String ph = hash_password(password);
    get_db()->execute(
        "INSERT INTO users (username, display_name, password_hash) VALUES (?, ?, ?)",
        "sss", username, (display_name && display_name[0]) ? display_name : username, (char *) ph);

    dict resp = dict_create_object();
    dict_object_set(resp, "message", dict_create_string("User created successfully"));
    return json_resp(201, "Created", resp);
}

[[HttpPost("/api/login")]]
static Response *h_login(Request *req) {
    char username[128], password[128];
    const char *b = req->raw_body ? req->raw_body : "";
    form_field(b, "username", username, sizeof(username));
    form_field(b, "password", password, sizeof(password));
    if (!username[0] || !password[0]) return err_resp(401, "Unauthorized", "Invalid credentials");

    dict row = get_db()->query_one(
        "SELECT id, password_hash FROM users WHERE username=?", "s", username);
    defer delete row;
    if (!row) return err_resp(401, "Unauthorized", "Invalid credentials");
    const char *stored = (char *) row["password_hash"];
    if (!stored || !verify_password(password, stored))
        return err_resp(401, "Unauthorized", "Invalid credentials");

    long uid = (long) row["id"];
    char token[65];
    random_hex(token, 32);
    session_put(token, uid);

    dict resp = dict_create_object();
    dict_object_set(resp, "access_token", dict_create_string(token));
    dict_object_set(resp, "token_type", dict_create_string("bearer"));
    Response *r = json_resp(200, "OK", resp);
    r->setCookie = f"access_token={token}; Path=/; HttpOnly; SameSite=Lax";
    return r;
}

[[HttpPost("/api/logout")]]
static Response *h_logout(Request *req) {
    (void) req;
    dict resp = dict_create_object();
    dict_object_set(resp, "message", dict_create_string("Logged out"));
    return json_resp(200, "OK", resp);
}

[[HttpGet("/api/me")]]
static Response *h_me(Request *req) {
    long uid = auth_uid(req);
    if (uid == 0) return err_resp(401, "Unauthorized", "Not authenticated");
    dict row = get_user_row(uid);
    defer delete row;
    if (!row) return err_resp(401, "Unauthorized", "Not authenticated");
    dict resp = dict_create_object();
    dict_object_set(resp, "id", dict_create_int64(uid));
    dict_object_set(resp, "username", dict_create_string((char *) row["username"]));
    const char *disp = (char *) row["display_name"];
    dict dispval;
    if (disp) dispval = dict_create_string(disp);
    else dispval = dict_create_null();
    dict_object_set(resp, "display_name", dispval);
    return json_resp(200, "OK", resp);
}

[[HttpGet("/api/posts")]]
static Response *h_get_posts(Request *req) {
    long uid = auth_uid(req);
    if (uid == 0) return err_resp(401, "Unauthorized", "Not authenticated");
    int page = req->argInt("page");
    int count = req->argInt("count");
    if (page < 1) page = 1;
    if (count < 1) count = 10;
    if (count > 50) count = 50;
    int offset = (page - 1) * count;

    List<dict> *rows = get_db()->query(
        "SELECT p.id, p.type, p.text, p.quote, p.created_at, p.user_id, u.username, u.display_name "
        "FROM posts p JOIN users u ON p.user_id=u.id ORDER BY p.created_at DESC LIMIT ? OFFSET ?",
        "ii", count, offset);
    defer delete rows;

    /* Generous per-post node budget (~120 covers the base ~13 fields plus
     * several media/comment rows at ~5 nodes each with room to spare) --
     * one right-sized malloc up front beats dict's one-malloc-per-node
     * model even at the max page size (count capped at 50 above). */
    JBuilder *jb = jb_create(count * 120 + 64);
    JBNode *arr = jb_array(jb);
    for (auto r in rows) {
        post_to_json(jb, arr, r, uid);
        delete r;
    }
    char *raw = jb_finish(jb, arr);
    /* String.attach() (c2m_str_attach) transfers ownership of an
     * externally-malloc'd buffer into String's own scope-exit cleanup --
     * the correct way to hand jb_finish's malloc'd buffer to a String.
     * A plain `String body = raw;` looked fine immediately (same bytes,
     * same pointer) but was a ticking use-after-free once anything
     * downstream freed or outlived raw, and a manual `free(raw)` right
     * after the assignment was an immediate one (both found the hard
     * way). Do not free `raw` after this call -- attach() owns it now. */
    String body = String.attach(raw);
    jb_destroy(jb);
    return new Response(200, "OK", body);
}

[[HttpPost("/api/posts")]]
static Response *h_create_post(Request *req) {
    long uid = auth_uid(req);
    if (uid == 0) return err_resp(401, "Unauthorized", "Not authenticated");
    if (req->body == 0) return err_resp(400, "Bad Request", "invalid JSON");

    const char *type = (char *) req->body["type"];
    if (!type) type = "text";
    if (strcmp(type, "text") != 0 && strcmp(type, "big-text") != 0 &&
        strcmp(type, "single-media") != 0 && strcmp(type, "multi-media") != 0)
        return err_resp(400, "Bad Request", "Invalid post type");

    const char *text  = (char *) req->body["text"];
    const char *quote = (char *) req->body["quote"];
    get_db()->execute("INSERT INTO posts (user_id, type, text, quote) VALUES (?, ?, ?, ?)",
                  "isss", (int) uid, type, text ? text : "", quote ? quote : "");
    long post_id = get_db()->lastInsertRowId();

    dict media = req->body["media"];
    if (media && media.type() == DICT_ARRAY) {
        int mn = (int) media.length();
        for (int i = 0; i < mn; i++) {
            dict mi = media[i];
            const char *mtype = (char *) mi["type"];
            const char *murl  = (char *) mi["url"];
            get_db()->execute("INSERT INTO media (post_id, type, url, position) VALUES (?, ?, ?, ?)",
                          "lssi", post_id, mtype ? mtype : "", murl ? murl : "", (int) i);
        }
    }

    dict prow = get_db()->query_one(
        "SELECT p.id, p.type, p.text, p.quote, p.created_at, p.user_id, u.username, u.display_name "
        "FROM posts p JOIN users u ON p.user_id=u.id WHERE p.id=?", "l", post_id);
    defer delete prow;

    JBuilder *jb = jb_create(256);
    JBNode *post = post_to_json(jb, 0, prow, uid);
    char *raw = jb_finish(jb, post);
    String body = String.attach(raw); /* see h_get_posts's comment on jb_finish */
    jb_destroy(jb);
    return new Response(201, "Created", body);
}

[[HttpPost("/api/upload")]]
static Response *h_upload(Request *req) {
    if (auth_uid(req) == 0) return err_resp(401, "Unauthorized", "Not authenticated");

    String ct = req->header("Content-Type");
    if ((char *)ct == 0 || ((char *)ct)[0] == 0)
        return err_resp(400, "Bad Request", "missing Content-Type");
    const char *content_type = (char *)ct;

    const char *bkey = "boundary=";
    const char *bp = strstr(content_type, bkey);
    if (!bp) return err_resp(400, "Bad Request", "missing multipart boundary");
    bp += strlen(bkey);
    char boundary[256];
    if (*bp == '"') {
        bp++;
        const char *end = strchr(bp, '"');
        int len = end ? (int)(end - bp) : 0;
        if (len >= (int)sizeof(boundary)) len = (int)sizeof(boundary) - 1;
        memcpy(boundary, bp, (size_t)len);
        boundary[len] = 0;
    } else {
        strncpy(boundary, bp, sizeof(boundary) - 1);
        boundary[sizeof(boundary) - 1] = 0;
        char *sc = strpbrk(boundary, "\r\n;");
        if (sc) *sc = 0;
    }

    char delim[300];
    snprintf(delim, sizeof(delim), "--%s", boundary);
    long dlen = (long) strlen(delim);

    dict files = dict_create_array();

    long pos = find_bytes(req->raw_body, req->body_len, delim, dlen, 0);
    while (pos >= 0) {
        long part_start = pos + dlen;
        if (part_start + 1 < req->body_len &&
            req->raw_body[part_start] == '-' && req->raw_body[part_start + 1] == '-')
            break; /* closing "--boundary--" */
        if (part_start + 1 < req->body_len &&
            req->raw_body[part_start] == '\r' && req->raw_body[part_start + 1] == '\n')
            part_start += 2;

        long hdr_end = find_bytes(req->raw_body, req->body_len, "\r\n\r\n", 4, part_start);
        if (hdr_end < 0) break;
        long data_start = hdr_end + 4;

        long next = find_bytes(req->raw_body, req->body_len, delim, dlen, data_start);
        long data_end = (next >= 0) ? next : req->body_len;
        if (data_end - 2 >= data_start &&
            req->raw_body[data_end - 2] == '\r' && req->raw_body[data_end - 1] == '\n')
            data_end -= 2;

        char part_headers[1024];
        long hlen = hdr_end - part_start;
        if (hlen >= (long) sizeof(part_headers)) hlen = (long) sizeof(part_headers) - 1;
        memcpy(part_headers, req->raw_body + part_start, (size_t)hlen);
        part_headers[hlen] = 0;

        dict item = save_upload_part(part_headers, req->raw_body + data_start, data_end - data_start);
        if (item) dict_array_append(files, item);

        if (next < 0) break;
        pos = next;
    }

    dict resp = dict_create_object();
    dict_object_set(resp, "files", files);
    return json_resp(200, "OK", resp);
}

[[HttpPost("/api/posts/{id}/like")]]
static Response *h_like(Request *req) {
    long uid = auth_uid(req);
    if (uid == 0) return err_resp(401, "Unauthorized", "Not authenticated");
    long post_id = (long) req->argInt("id");
    dict exists = get_db()->query_one("SELECT 1 AS x FROM posts WHERE id=?", "l", post_id);
    defer delete exists;
    if (!exists) return err_resp(404, "Not Found", "Post not found");
    get_db()->execute("INSERT OR IGNORE INTO likes (post_id, user_id) VALUES (?, ?)", "ll", post_id, uid);
    dict c = get_db()->query_one("SELECT COUNT(*) AS n FROM likes WHERE post_id=?", "l", post_id);
    defer delete c;
    dict resp = dict_create_object();
    dict_object_set(resp, "likes", dict_create_int64(c ? (long) c["n"] : 0));
    dict_object_set(resp, "liked", dict_create_bool(1));
    return json_resp(200, "OK", resp);
}

[[HttpDelete("/api/posts/{id}/like")]]
static Response *h_unlike(Request *req) {
    long uid = auth_uid(req);
    if (uid == 0) return err_resp(401, "Unauthorized", "Not authenticated");
    long post_id = (long) req->argInt("id");
    get_db()->execute("DELETE FROM likes WHERE post_id=? AND user_id=?", "ll", post_id, uid);
    dict c = get_db()->query_one("SELECT COUNT(*) AS n FROM likes WHERE post_id=?", "l", post_id);
    defer delete c;
    dict resp = dict_create_object();
    dict_object_set(resp, "likes", dict_create_int64(c ? (long) c["n"] : 0));
    dict_object_set(resp, "liked", dict_create_bool(0));
    return json_resp(200, "OK", resp);
}

[[HttpGet("/api/posts/{id}/comments")]]
static Response *h_get_comments(Request *req) {
    long uid = auth_uid(req);
    if (uid == 0) return err_resp(401, "Unauthorized", "Not authenticated");
    long post_id = (long) req->argInt("id");
    int page = req->argInt("page");
    int count = req->argInt("count");
    if (page < 1) page = 1;
    if (count < 1) count = 15;
    int offset = (page - 1) * count;

    List<dict> *rows = get_db()->query(
        "SELECT c.text, u.username FROM comments c JOIN users u ON c.user_id=u.id "
        "WHERE c.post_id=? ORDER BY c.created_at ASC LIMIT ? OFFSET ?",
        "lii", post_id, count, offset);
    defer delete rows;
    dict arr = dict_create_array();
    for (auto r in rows) {
        dict ci = dict_create_object();
        dict_object_set(ci, "author", dict_create_string((char *) r["username"]));
        dict_object_set(ci, "text", dict_create_string((char *) r["text"]));
        dict_array_append(arr, ci);
        delete r;
    }

    dict total = get_db()->query_one("SELECT COUNT(*) AS n FROM comments WHERE post_id=?", "l", post_id);
    defer delete total;
    long tn = total ? (long) total["n"] : 0;
    int has_more = (page * count) < tn;

    dict resp = dict_create_object();
    dict_object_set(resp, "comments", arr);
    dict_object_set(resp, "has_more", dict_create_bool(has_more));
    dict_object_set(resp, "page", dict_create_int64(page));
    return json_resp(200, "OK", resp);
}

[[HttpPost("/api/posts/{id}/comments")]]
static Response *h_create_comment(Request *req) {
    long uid = auth_uid(req);
    if (uid == 0) return err_resp(401, "Unauthorized", "Not authenticated");
    long post_id = (long) req->argInt("id");
    dict exists = get_db()->query_one("SELECT 1 AS x FROM posts WHERE id=?", "l", post_id);
    defer delete exists;
    if (!exists) return err_resp(404, "Not Found", "Post not found");

    if (req->body == 0) return err_resp(400, "Bad Request", "invalid JSON");
    const char *text = (char *) req->body["text"];
    if (!text || !text[0]) return err_resp(400, "Bad Request", "text required");

    get_db()->execute("INSERT INTO comments (post_id, user_id, text) VALUES (?, ?, ?)",
                  "lls", post_id, uid, text);
    dict resp = dict_create_object();
    dict_object_set(resp, "message", dict_create_string("Comment added"));
    return json_resp(201, "Created", resp);
}

[[HttpDelete("/api/posts/{id}")]]
static Response *h_delete_post(Request *req) {
    long uid = auth_uid(req);
    if (uid == 0) return err_resp(401, "Unauthorized", "Not authenticated");
    long post_id = (long) req->argInt("id");
    dict prow = get_db()->query_one("SELECT user_id FROM posts WHERE id=?", "l", post_id);
    defer delete prow;
    if (!prow) return err_resp(404, "Not Found", "Post not found");
    if ((long) prow["user_id"] != uid)
        return err_resp(403, "Forbidden", "Not allowed to delete this post");

    List<dict> *media_rows = get_db()->query("SELECT url FROM media WHERE post_id=?", "l", post_id);
    defer delete media_rows;
    for (auto m in media_rows) {
        const char *url = (char *) m["url"];
        const char *base = strrchr(url, '/');
        if (base) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", g_usermedia_dir, base + 1);
            unlink(path);
        }
        delete m;
    }

    get_db()->execute("DELETE FROM posts WHERE id=?", "l", post_id);
    return new Response(204, "No Content", "");
}

/* Test-only endpoint simulating a slow/large DB query: GET /api/slow?ms=500.
 * Sleeps for the requested duration (default 500ms, capped at 10s), then
 * does a real query on this worker's own connection — with per-worker
 * SQLite connections (see app.cy's get_db()), this only ties up the ONE
 * worker handling it (and, briefly, any writer that lands mid-sleep, since
 * SQLite still allows only one writer at a time even in WAL mode); it no
 * longer blocks every other request in the app the way a single shared
 * connection + one global mutex would have. */
[[HttpGet("/api/slow")]]
static Response *h_slow(Request *req) {
    if (auth_uid(req) == 0) return err_resp(401, "Unauthorized", "Not authenticated");
    int ms = req->argInt("ms");
    if (ms <= 0) ms = 500;
    if (ms > 10000) ms = 10000;
    usleep((unsigned int)ms * 1000);
    dict row = get_db()->query_one("SELECT COUNT(*) AS n FROM posts");
    defer delete row;
    dict resp = dict_create_object();
    dict_object_set(resp, "message", dict_create_string("slow query complete"));
    dict_object_set(resp, "simulated_ms", dict_create_int64(ms));
    dict_object_set(resp, "posts_count", dict_create_int64(row ? (long)row["n"] : 0));
    return json_resp(200, "OK", resp);
}

Response *app_handle(Request *req) {
    return route_dispatch(req);
}

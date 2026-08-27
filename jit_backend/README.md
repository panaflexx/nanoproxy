# jit_backend

A classyc port of `backend/app.py` — same
social feed API (users/auth, posts, comments, likes, media uploads), same
SQLite persistence, but JIT-compiled and run through classyc's `jitrunner`
instead of FastAPI/uvicorn.

Two files, sharing `app.h`, same shape as classyc's `examples/http_crud`:

- **`app.cy`** — HTTP/DB infrastructure: the raw-socket HTTP/1.1 accept
  loop (modeled on classyc's `examples/http-serve.c`), per-worker SQLite
  connections, session token storage, password hashing, and form
  parsing. Builds a `httpserve.h` `Request` and calls `app_handle()`.
- **`api.cy`** — this app's actual developer-facing code: the DB schema/
  seed data, and every route handler, registered with `[[HttpGet]]` /
  `[[HttpPost]]` / `[[HttpDelete]]` (no hand-rolled dispatch table).

The accept loop is still this file's rather than `http-serve-cchan.c`,
because the demo wants a 4 MiB upload cap, per-worker SQLite, and its
own session map. Cookies and raw (including multipart) bodies go through
`req_attach_raw` + `Request.header()` / `Request.cookie()` — the
original reason not to use `httpserve.h` is gone.

**Multi-TU `.bmir` loading works** — jitrunner resolves the
`[[HttpGet]]` registry's `__start_cyreg_routes` symbol, and permits
identical header-inline function definitions across TUs
(`MIR_set_func_redef_permission`, matching `classyc -eg`). So
`Sqlite.execute(const char*)` can be called from both `app.cy` (PRAGMA
setup) and `api.cy` (CREATE TABLE), and both TUs can include
`httpserve.h`. Session storage is still `session_get()`/`session_put()`
rather than a shared `Map<String,int>*`, but that's so the lock lives
next to the map, not a remaining compiler constraint.

Also worth knowing if you see it again: a `Response*` (or any class pointer)
returned across a TU boundary from an `extern` function needs `unowned`
at the receiving call site for classyc's ownership checker to accept a
later manual `delete` — same pattern `examples/http-serve.c` uses for
`app_handle()`'s return value. Without it, the checker raises a false
"`use-after-free: was released earlier on this path`" even though the
code is correct.

## Build

```bash
sh jit_backend/build.sh
```

Needs `classyc` on `PATH` (or `~/.classyc/bin`, or `/usr/local/bin`), linked
against `sqlite3` and `crypto` (OpenSSL, for SHA-256 password hashing).
Compiles `app.cy` and `api.cy` each to their own `.bmir` (`classyc -c -o
jit_backend/app.bmir …app.cy` and the same for `api.cy`). jitrunner
runtime-links them:

```bash
jitrunner jit_backend/app.bmir jit_backend/api.bmir --mode gen
```

**jitrunner needs a matching fix to run it.** jitrunner only resolves
external symbols against a hardcoded `libc`/`libm` list — no `-l` flags are
forwarded from how the `.bmir` was built, so `sqlite3_*`/`SHA256` fail to
resolve at load time ("cannot resolve symbol"). Fixed by adding
`libsqlite3.so`/`libcrypto.so` to `std_libs[]` in
`~/src/MIR/classyc/src/jitrunner/mir-bridge.c`, then rebuilding
(`bash src/jitrunner/build.sh`) and reinstalling (`cp bin/jitrunner
~/.classyc/bin/jitrunner`, or wherever your classyc is installed). If a
future jit-backed app needs some other library, it'll need the same
treatment (or a real `-l` passthrough added to jitrunner) until jitrunner
grows a general mechanism for this.

## Concurrency

`NUM_WORKERS` (64) pthread workers pull accepted connections off a bounded
`cchan_t` queue (the acceptor is the only thread that calls `accept()`),
same pattern as classyc's `examples/http-serve-cchan.c` — plain pthreads,
no `-ffibers`. As with any blocking-worker-per-connection server: keep
client-side keep-alive concurrency at or below the worker count, or excess
connections queue for a free worker.

**Load-test this claim before trusting it, not just this endpoint.**
Earlier testing here (with the old default of 8 workers, before an
`httpserve.h` rewrite made each request measurably heavier) genuinely
believed "over the worker count degrades gracefully" — a handful of `ab`
timeouts, nothing worse. Re-verified under `ab -k -c 16` after that
rewrite and got a **permanent hang** instead (792/800 complete, the
remaining 8 never respond, confirmed via `ss` that their request bytes
sat completely unread while every worker sat idle on `cchan`'s condvar).
Root cause: this file's hand-rolled accept loop was missing the
`SO_RCVTIMEO` that classyc's own `examples/http-serve.c` sets on every
accepted socket specifically to prevent this — a worker whose connection
goes idle with no free peer now blocks forever instead of self-healing
after 5s. Fixed by adding the same timeout here. Separately (found in the
same investigation, same upstream file): `http-serve.c`'s `send()` call
isn't looped, so a legitimate short write under load can silently
truncate a response — fixed here too. Neither bug was ours originally;
both are in classyc's own reference example, just needed real concurrent
load to surface. **Verified after both fixes + raising `NUM_WORKERS`
8→64 + `sqlite3_config(SQLITE_CONFIG_MULTITHREAD)`** (see below): zero
failures at sustained `ab -k` concurrency up to at least 64, throughput
actually *rising* with concurrency (3,957 req/s @ c=8 → 5,013 req/s @
c=64) instead of collapsing.

**Per-resource locking, not one global mutex.** The first version wrapped
every request — DB or not — in one mutex around a single shared SQLite
connection, matching the "single mutex around all DB access" pattern
`http-serve-cchan.c` itself suggests. That has a bigger blast radius than
it sounds: with only one shared connection, a single slow query (see
`/api/slow?ms=<n>` below) stalls *every other request in the app*, since
even lightweight non-DB handlers were serialized behind the same lock.
Measured under a mixed `ab` workload (fast + DB-backed + one artificially
slow endpoint running concurrently): the global-mutex version pushed the
fast endpoint's worst-case latency to 5.25s (vs. a ~1ms median) and the
slow endpoint's own requests queued up behind *each other* to as long as
5.68s for a "500ms" query. This mirrors how SQLAlchemy/Python actually
handle SQLite instead:
- **One SQLite connection per worker thread** (`get_db()`), not shared —
  each opened lazily on first use, with `PRAGMA busy_timeout=5000` so a
  writer-vs-writer conflict retries instead of erroring immediately.
- **`PRAGMA journal_mode=WAL`**, set once on the bootstrap connection
  before any worker starts (WAL is persisted in the database file itself,
  so every worker's own later connection inherits it automatically) — lets
  any number of readers run fully in parallel with a single writer; only
  writer-vs-writer actually serializes.
- **The in-memory session `Map`** is the one thing left with no safety of
  its own, so it keeps a dedicated mutex (`g_sessions_mutex`) — but scoped
  to just the map lookup/insert (`current_user_id`, login's token store),
  not the whole request, so it can never be held for anywhere near as long
  as a slow query.

Retested the same mixed workload after this change: the fast endpoint's
worst case dropped from 5.25s to **82ms**, the DB-backed endpoint's from
518ms to **55ms**, and ten "500ms" slow requests each completed in
**~502–504ms** (i.e. actually running in parallel across workers) instead
of visibly queueing behind each other.

Not `_Thread_local`: classyc's parser rejects a bare `_Thread_local`
declaration at file scope without an explicit `static` ("declared as
thread local" / treated as invalid `auto` storage), and even
`static _Thread_local` triggered a separate MIR-level load error once
combined with `cyexc.h`'s own internal per-thread exception state in the
same module (`mir.tls_addr was already defined differently in the
module`). `get_db()` instead keeps a small `pthread_t[8]` table that each
worker fills in with its own `pthread_self()` before it starts pulling
jobs (slot index decided by `main()` before `pthread_create`, so there's
no shared counter for two workers to race on), and looks itself up in it
by `pthread_equal` — a tiny linear scan, no TLS involved at all.

**`GET /api/slow?ms=<n>`** (default 500, capped at 10000) is a test-only
endpoint that sleeps for `n` milliseconds before running a real query —
used to simulate a large/slow DB query and demonstrate the above.

**Must run with `jitrunner --mode gen`, not the default `--mode lazy`.**
jitrunner's default per-function lazy JIT has no cross-thread lock around
first-call compilation — two worker threads racing to first-call the same
function can hit a MIR assertion (`generate_func_code: ... func_item->data
== NULL`) and crash the whole process. `--mode gen` generates all code
upfront, once, on the main thread, before any worker starts, which avoids
the race entirely. `config.json`'s dispatch entry already sets
`"mode": "gen"`; do the same if running standalone.

## Performance tuning

Found with `perf record -g` against the worker process under sustained
`ab -k` load (needs `kernel.perf_event_paranoid` low enough to profile a
non-root process — not this repo's concern, but worth knowing before
assuming profiling is unavailable in a given environment):

- **~8% of all CPU cycles were in SQLite's own SQL parser**
  (`sqlite3Parser`/`sqlite3GetToken`/`sqlite3RunParser`) — expected, since
  classyc's `Sqlite` class calls `sqlite3_prepare_v2` fresh on every
  `query()`/`query_one()`/`execute()` call with no statement-cache, so
  identical SQL text gets recompiled from scratch every time. Not fixed
  at the `Sqlite`-class level (would need a real prepared-statement
  cache keyed by SQL text, per connection); partially mitigated instead
  by cutting `post_to_dict()`'s query count directly — its 3 separate
  `COUNT(*)`/`EXISTS`-style `query_one()` calls (likes count, "did
  viewer like this", comment count) collapsed into 1 query using scalar
  subqueries, dropping this function's per-post query count from 5 to 3.
- **~3% in `pthread_mutex_lock`/`unlock`** — SQLite defaults to
  `SQLITE_CONFIG_SERIALIZED` threading mode, which takes an internal
  mutex on every operation even though every worker here already has its
  own exclusive connection (`get_db()`) and never shares one across
  threads. Switched to `SQLITE_CONFIG_MULTITHREAD` (`sqlite3_config()`,
  called once in `main()` before the bootstrap connection opens — it can
  only change threading mode before SQLite's first connection). Safe
  specifically because of this app's per-worker-connection design; not a
  safe default change for code that shares connections across threads.
  Raw CPU time in the mutex was only ~3%, but the real-world effect was
  much bigger than that number suggests: lock *contention* (threads
  blocking on each other) scales worse than raw time-in-lock as
  concurrency rises, so removing a serialization point pays off more at
  higher thread counts than a flat 3% would imply — don't extrapolate
  "safe to skip" from a small percentage in a single-threaded-looking
  profile line.
- `dict_serialize_value_pretty`'s 0.74% is **not** wasted pretty-printing
  — checked: `.json()` already compiles to `dict_serialize_json_heap(val,
  0)` (pretty=0, compact output) by default. The function name is just
  generic/historical; both modes share it.
- **`NUM_WORKERS` raised 8→64** after the `SO_RCVTIMEO`/`send()` fixes
  above made higher concurrency actually safe (zero failures verified up
  to `ab -k -c 64`) — each worker only costs one OS thread + one lazily
  opened SQLite connection, both cheap, so there's no real downside
  short of very large worker counts.
- **~10% of CPU was in malloc/free machinery** (`_int_malloc`, `_int_free`,
  `malloc_consolidate`, ...) — consistent with this app's dict-tree-based
  JSON building allocating many small nodes per response. Not addressed
  here (would need a different response-building strategy, e.g. streaming
  JSON output instead of building a full `dict` tree first); noted for
  anyone picking this up next.

Related but separate finding from the same investigation: `dict` and
`List<dict>` don't free themselves in classyc — every `dict` this app
built for a JSON response, and every row `dict` pulled out of a
`List<dict>*` query result, was leaking (~20KB/request on `/api/posts`,
confirmed via RSS growth) rather than just being slow. Fixed in
`api.cy`'s `json_resp()` (frees its `body` argument after serializing —
`dict_destroy` recurses through the whole tree in one call) and in every
`for (auto row in someList)` loop (`delete row;` per iteration, since
`List<dict>`'s own destructor doesn't free `dict`-typed elements).

### `/api/posts`'s response building: cejson instead of dict

Followed up on the "~10% CPU in malloc/free" finding above by switching
`post_to_json()` (was `post_to_dict()`) from classyc's `dict` to cejson
(a separate local project, `~/src/GUI/cejson`; vendored here as
`jit_backend/cejson.h` + `stringbuf.h` — header-only, so a plain copy
avoids a build dependency on that repo) via a small wrapper,
`jsonbuild.h`/`jsonbuild.cy`.
cejson bump-allocates all JSON nodes from one pre-sized array per
request instead of one heap allocation per node, which is what the
malloc/free profiling pointed at.

**Why a separate TU (`jsonbuild.cy`), not `api.cy` including cejson.h
directly**: cejson's builder functions (`json_create_int`/`float`/
`string`) trip classyc's ownership checker with false-positive
"double-free risk" errors on their malloc-then-maybe-free-on-capacity-
failure pattern (same category as classyc's own `dict.h`). The fix is
`-fno-ownership`, which is a whole-compile-unit flag — turning it on for
all of `api.cy` would disable ownership checking for every handler's own
`dict`/`List<dict>` cleanup too, exactly the class of bug just described
above. So `jsonbuild.cy` is built with `-fno-ownership` in isolation
(see `build.sh`) and exposes only opaque `JBuilder`/`JBNode` pointer
types via `jsonbuild.h` — `api.cy` never sees cejson's actual struct
layout and keeps full ownership checking.

Three real bugs found and fixed while wiring this up, each the kind that
compiles clean and passes a quick functional smoke test, only showing up
under closer inspection or real load — see `jsonbuild.h`'s header comment
for the full writeup of the first two:

1. **Key-before-value ordering.** cejson's flat node array requires an
   object's key node to be created *immediately* before its value's node
   — a single convenience call like `jb_set(jb, obj, "id", jb_int(jb,
   42))` is unsafe because `jb_int(...)` is evaluated as an argument
   before the call happens, silently swapping every key/value pair in the
   output. Fixed with a `JB_FIELD(jb, obj, key, valexpr)` macro (expands
   to separate statements, so order is by construction) for scalar
   values, and an explicit `jb_key()`-then-build-the-whole-subtree-then-
   `jb_attach()` pattern for container values (arrays/objects), since a
   macro can't help there either — the value is a whole pre-built
   subtree, not a single expression.
2. **`String.attach()`, not a plain assignment, for a `char*` you got
   from `malloc()`.** `jsonbuild.cy`'s `jb_finish()` returns a plain
   `char*` (the char*-to-String conversion has to happen in
   ownership-checked code — doing it inside `-fno-ownership` code
   corrupted the result by the time `api.cy` read it back). The first fix
   attempt, `String body = raw; free(raw);`, crashed/corrupted (a plain
   assignment doesn't deep-copy — it holds the same pointer, so the
   explicit `free()` is a use-after-free). The second attempt, dropping
   the `free()`, avoided the crash but leaked ~1.8KB/request (nothing
   was freeing `raw` anymore either). The actual fix: `String body =
   String.attach(raw);` — `c2m_str_attach` transfers ownership of an
   externally-`malloc`'d buffer into `String`'s own scope-exit cleanup.
   Confirmed flat (<1 byte/request, pure noise) over an 80,000-request
   follow-up run.
3. **String escaping was implemented but never wired in.** cejson's
   `StringBuf`-based dump path (`json_dump_node_buf`) had a real,
   separately-defined `json_dump_escape_buf()` sitting right next to the
   `JSON_STRING` case that was supposed to call it — but that case did a
   raw `stringbuf_append` between quotes instead, with the escaping call
   commented out. Any post text containing `"` or `\` or a control
   character would have produced invalid JSON. Fixed by wiring
   `json_dump_escape_buf()` into both the value case and the
   object-key-writing path (which had the identical bug), and batching
   its run of "no escaping needed" bytes into one `stringbuf_append`
   (memcpy) instead of one `stringbuf_append_char` per byte, while at it.

**Net performance effect was a wash, and the *reason* it started out
worse is the more useful finding than the final number.** The first
working version (correctness-fixed, using `open_memstream` + cejson's
original `FILE*`-based `json_dump_node`) was measurably *slower* than
the `dict`-based version it replaced -- 4% slower at `ab -k -c 8`,
12% slower at `c=64`. Root cause: `json_dump_escape`'s `FILE*` path
calls `fputc()` once per character for ordinary (non-escaped) string
content, and glibc's regular `fputc`/`fputs`/`fwrite` each take an
internal lock by default -- pure overhead here, since each
`open_memstream` stream is only ever touched by the single thread that
created it, never shared. Switching `jb_finish()` to build directly into
a `StringBuf` (plain `memcpy`-based appends, no `FILE*`/stdio at all)
closed the entire gap: c=8 3,783→3,990 req/s, c=64 4,401→5,010 req/s,
now essentially matching or slightly beating the original `dict`-based
numbers at every concurrency level tested. The lesson: switching
allocation *strategy* (bump-allocated array vs. per-node heap
allocation) doesn't help if the *output* path still goes through
lock-taking stdio calls -- profile the whole pipeline, not just the part
you set out to change.

### 1:1 `dict` vs `cejson` micro-benchmark, and the real bottleneck

The "net effect was a wash" result above prompted a closer, controlled
comparison: a standalone micro-benchmark pairing identical `dict`- and
cejson-built structures (tiny/small/POST-shaped/100-field-wide/50-post
array), timed in isolation from HTTP/DB/network overhead. First result
was the opposite of what the bump-allocation intuition predicted --
**`dict` was 2-4x faster than cejson at every size**, and the gap widened
with complexity. Splitting the timing into build/serialize/free phases
found why: **build was already comparable** (cejson's bump allocation
genuinely is a bit cheaper than `dict`'s malloc-per-node, exactly as
expected), but **serialize was 68-88x slower** -- all the gap was in one
phase, not spread across the algorithm generally.

Root cause, confirmed by direct A/B: `dict.h` can only be `#include`d by
classyc's own compiler-internal C sources (gated behind
`DICT_CLASSYC_INTERNAL`, with an `#error` blocking any other include) --
its functions are compiled **once, ahead-of-time, by a real optimizing C
compiler**, baked permanently into the `classyc`/`jitrunner` binary, and
resolved via a direct function pointer (`jitrunner`'s import resolver:
`return (void*)dict_serialize_json_heap;`). `cejson.h`, being a
`static inline` header meant for user `.cy` code, gets **parsed and
JIT-compiled by MIR** every time a program that includes it loads.
Compiling the exact same cejson POST-shape benchmark with real `gcc -O2`
instead of running it through `jitrunner --mode gen` dropped its total
time from ~8,590ns to ~2,390ns -- **faster overall than `dict`** despite
`dict` always running as precompiled code. The lesson for classyc's own
architecture: a JSON builder's bump-allocation strategy is sound and
would win outright, but only if it's compiled into classyc's runtime the
way `dict.h` is -- left as a JIT-compiled header library, better
algorithms can't outrun worse-optimized machine code.

### Two more real cejson bugs found chasing the remaining gap

Even with fair (gcc `-O2`) compilation, cejson's serialize phase was
still ~17x slower than `dict`'s. Chasing that down surfaced a genuinely
critical bug, plus a solid further optimization:

1. **Stack overflow / heap corruption in the builder API, confirmed
   crashing a real server.** `json_create_object()`/`json_create_array()`
   pushed onto `p->stack` with **no bounds check**, and nothing in the
   builder API (`json_object_set`/`json_array_append`) ever popped it --
   every container-creation call permanently consumed one stack slot for
   the `JsonParser`'s entire lifetime (the stack is meant to track
   nesting *depth*, correctly popped on `}`/`]` during parsing; the
   builder API has no such closing event). `h_get_posts`'s shape --
   one `JBuilder` shared across up to 50 posts, each needing several
   container creates -- silently exceeded a 128-slot cap and corrupted
   adjacent heap memory. Reproduced directly against the real server:
   `GET /api/posts?count=50` (a value the API itself allows) crashed the
   **entire process** with `free(): invalid size` / `SIGABRT`. Root-cause
   fixed in `cejson.h`: confirmed nothing downstream (`json_object_set`,
   `json_array_append`, `json_first_child`/`json_next_sibling`,
   `json_dump_node_buf`) ever reads `p->stack` for builder-created nodes
   (only `json_feed()`'s own properly-paired parser push/pop, and
   `json_finish()`'s unrelated "everything got closed" check, do) -- so
   `json_create_object`/`json_create_array` simply don't touch the stack
   at all anymore. Verified: `jsonbuild.cy`'s stack buffer shrunk back
   from a 4096-slot stopgap to 32 slots, and `count=50` still works fine,
   proving the fix (not just a bigger buffer) is what's protecting it.
2. **`json_create_string()` hashes every string unconditionally**, purely
   to make `json_get_object_value()`'s key lookup fast later -- a cost a
   pure builder-then-serialize caller (never looks a key back up by name)
   pays for nothing. Added an opt-in `p->skip_string_hash` flag (default
   false, so existing callers are unaffected) that `jsonbuild.cy` sets
   right after `json_init()`.

### Making `stringbuf_append_char` unnecessary, not just faster

`stringbuf_reserve`/`stringbuf_append`/`stringbuf_append_str`/
`stringbuf_append_char` were plain extern-linkage functions gated behind
`#ifdef STRINGBUF_IMPLEMENTATION` (fine for functions meant to be
compiled once and linked; not inlinable at their many call sites in a
dump loop). Moved to `static inline`, defined unconditionally (matching
`stringbuf_data`/`stringbuf_cstr`/etc.'s existing pattern) so they're
correctly inlinable *and* still safe for any TU to include without
opting into the single-definition `STRINGBUF_IMPLEMENTATION` path.
Beyond that, per the profiling: every `stringbuf_append_char` call pays
its own capacity-check regardless of inlining, so the real fix was
reducing *how many* separate calls the dump path makes, not just making
each one cheaper:

- `json_dump_escape_buf()` now pre-scans a string once for "does
  anything here need escaping at all" (cheap, simple byte comparisons);
  for the common case (nothing does), it reserves once and writes the
  opening quote + content + closing quote as a single direct buffer
  write, instead of three separate `stringbuf_append_char`/
  `stringbuf_append`/`stringbuf_append_char` calls (three capacity-checks
  instead of one).
- Adjacent fixed punctuation (`"[\n"`, `",\n"`, `"{\n"`) that pretty-mode
  used to write as two separate `stringbuf_append_char` calls now goes
  out as one literal via `stringbuf_append`. Compact mode (this app's own
  hot path) was already a single isolated byte either way, so unaffected
  by this specific change.
- Added `json_dump_indent_buf()`, writing pretty-mode indentation in
  `ceil(n/64)` calls instead of one `stringbuf_append_char` per space.

Combined effect, measured via the same phase-split benchmark: serialize
phase dropped from ~7,127ns to ~3,550ns under `jitrunner --mode gen`
(~50% reduction), ~1,446ns to ~974ns compiled natively with gcc `-O2`
(~33% reduction, the fairer apples-to-apples number given the
JIT-vs-AOT finding above). End to end, on the real server: `/api/posts`
went from ~3,957-5,013 req/s (the original `dict`-based numbers) to
~5,500-9,300 req/s across `ab -k -c 8..64` -- a real, reproducible
30-70%+ improvement over the `dict` baseline this whole investigation
started from, not just a wash.

## Run

Standalone:

```bash
jitrunner jit_backend/app.bmir jit_backend/api.bmir jit_backend/jsonbuild.bmir --mode gen
```

Through nanoproxy (the normal way): the `config.json` dispatch entry with
`"handler": "jit"` on the `web` listener spawns this automatically and
reverse-proxies `/api/**` to it. See the `jit` section of the main
[README](../README.md#jit-run-a-classyc-http-api-behind-npserver).

## Deliberate differences from the Python version

This is a demo; these are honest simplifications, not bugs:

- **Password hashing:** `SHA-256(password + per-user random salt)`, not
  bcrypt — classyc has no bcrypt binding. Stored as `salt$hexhash`.
- **Auth token:** an opaque random session token in an in-memory
  `Map<String,int>` (token → user_id), not a signed JWT. Set as an
  `HttpOnly` cookie — the same contract the frontend already uses
  (`fetch(..., {credentials:"include"})`); there's no bearer-header path
  since the frontend never sends one.
- **No CORS headers.** Frontend and API are always same-origin through
  nanoproxy's reverse proxy; the Python version needed CORS because it
  could also be hit directly on `:8000`.
- **The `friends` table** from `backend/schema.sql` isn't recreated —
  no endpoint reads or writes it in either version.
- **Multipart upload** is hand-rolled (boundary scanning via `memmem` on
  the raw body, not `strstr`, so binary file bytes containing `\0` can't
  truncate the scan). Non-image/video parts are silently skipped rather
  than rejecting the whole request with a 400, unlike the Python version.

## Data

`feed.db` (SQLite) and `usermedia/` are created next to `app.cy` on first
run, seeded with the same four demo users (`alice`/`bob`/`carol`/`dave`,
password `demo` for all) and sample posts as the Python version. Both are
gitignored — delete `feed.db` to reseed from scratch.

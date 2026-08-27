# npserver (NanoProxyServer)

**One binary. Every protocol. Zero dependencies.**

A ~5,000-line C server that replaces nginx + HAProxy + CoreDNS + socat — all from a single process with a single JSON config file. Static files, reverse proxy, WebSocket passthrough, TCP/UDP forwarding, protocol translation, built-in authoritative DNS, TLS termination, gzip compression, and five load-balancing algorithms. Built on epoll/kqueue with `sendfile(2)` zero-copy I/O.

Because sometimes you don't need a container orchestrator. You need a Swiss Army knife.

```
            ┌─────────────────────────────────────┐
            │             npserver                │
            │                                     │
  HTTP ───▶ │ static files  ·  reverse proxy      │
 HTTPS ───▶ │ WebSocket     ·  gzip compression   │
   TCP ───▶ │ port forward  ·  load balancing     │
   UDP ───▶ │ DNS server    ·  protocol translate │
  Unix ───▶ │ TLS terminate ·  sendfile(2)        │
            │                                     │
            └─────────────────────────────────────┘
```

---

## Quick Start

```bash
# Build
cmake . && make

# Serve static files on port 8080
./npserver http://:8080

# HTTPS + HTTP with TLS termination
sh make_test_cert.sh
./npserver https://:443 http://:80 -cert testsite.crt -key testsite.key

# Port forward (ssh -L style)
./npserver tcp://:3307 -to dbserver.internal:3306

# Full config-driven setup
./npserver -config config.json
```

---

## Features

### Static File Serving
- `sendfile(2)` zero-copy on Linux and macOS (fallback for TLS)
- Byte-range requests (`Range: bytes=START-END`, `206 Partial Content`)
- `HEAD` method support (headers only, no body)
- Gzip compression for text, JS, CSS, JSON, SVG (via zlib, compile-time optional)
- `Vary: Accept-Encoding` for correct cache behavior
- `index.html` directory fallback
- Path traversal protection (`realpath` + prefix check)
- Per-location root mapping

### Reverse Proxy
- Non-blocking upstream connect with full bidirectional backpressure
- **WebSocket passthrough** — `Upgrade: websocket` is detected, `Connection: Upgrade` is forwarded, and the connection becomes a raw bidirectional tunnel
- Header rewriting: `Host`, `X-Forwarded-For`, `X-Real-IP`, `X-Forwarded-Proto`
- Streaming request bodies (chunked and Content-Length)
- Named upstream pools with five load-balancing algorithms:
  - `round-robin` — sequential rotation
  - `random` — random selection
  - `ip-hash` — sticky sessions by client IP (djb2 hash)
  - `conn-hash` — per-connection stickiness
  - `least-conn` — route to the backend with fewest active connections
- Per-pool health check configuration
- IPv4 + IPv6 upstream resolution

### TCP / UDP / Unix Forwarding
- Bidirectional relay with buffered backpressure
- **Protocol translation**: TCP↔UDP, TCP↔Unix, UDP↔TCP
- Framing modes: `stream` (byte-oriented) and `datagram` (message-oriented)
- Configurable idle timeout, max connections, and buffer size
- Like `ssh -L` or `socat`, but declarative

### Built-in DNS Server
- Authoritative DNS responder over UDP
- Record types: **A**, **AAAA**, **CNAME**, **MX**, **TXT**, **SRV**, **NS**, **PTR**
- `ANY` query support (returns all records for a name)
- CNAME pass-through (included in answers regardless of query type)
- Per-record TTL overrides
- DNS name compression on responses
- Case-insensitive matching with compression-loop protection
- All configured in JSON — no zone files

### TLS
- OpenSSL-based TLS 1.2/1.3 termination
- Scheme-driven: `https://` and `ssl://` listeners get TLS automatically
- Non-blocking handshake integrated into the event loop

### HTTP Parser
- Hand-rolled incremental state-machine parser (no library dependencies)
- HTTP/1.1 with keep-alive and chunked transfer encoding
- Streaming body support for large uploads
- Configurable limits: `MAX_BODY_SIZE` (64 MiB), `MAX_URL_SIZE` (4096), `MAX_HEADERS` (128)

### Architecture
- **Single process, single thread** — no locking, no shared state
- **epoll** (Linux) + **kqueue** (macOS/BSD) dual backend
- Header-only libraries: `http.h`, `socket_server.h`, `dns.h`, `config.h`, `stringbuf.h`, `dict.h`
- Everything compiles from one `server.c` — total build time under 1 second
- URI-based socket configuration: `http://`, `https://`, `tcp://`, `udp://`, `ssl://`, `unix://`, `unixgram://`
- IPv4 + IPv6 dual-stack on all listeners

---

## CLI One-Liners

```bash
# Serve files from ./public on port 8080
./npserver http://:8080

# HTTPS on 443 + HTTP on 80
./npserver https://:443 http://:80 -cert fullchain.pem -key privkey.pem

# TCP port forward: local 3307 → remote MySQL 3306
./npserver tcp://:3307 -to dbserver.internal:3306

# UDP relay: local 5353 → Google DNS
./npserver udp://127.0.0.1:5353 -to udp://8.8.8.8:53

# Unix socket listener
./npserver unix:///tmp/myapp.sock

# Listen on a specific interface
./npserver http://192.168.1.10:8080

# Custom backlog
./npserver http://:8080 -c 4096

# Use a config file
./npserver -config production.json

# JIT-run a compiled classyc (.bmir) program — passthrough to `jitrunner`
./npserver jitrunner /tmp/prog.bmir
./npserver jitrunner /tmp/prog.bmir --watch
```

`npserver jitrunner` is a thin exec passthrough: it looks for a `jitrunner`
binary on `PATH`, then `~/.classyc/bin/jitrunner`, then
`/usr/local/bin/jitrunner`, and execs it with the rest of the arguments.
npserver does not link classyc or MIR — this just saves typing the full
path when classyc (or its `jitrunner` tool) is installed. See classyc's
own docs for `.bmir` compilation, `--watch` hot-reload, and
`--dap`/`--dap-stdio` debugging.

---

## Configuration

npserver uses a single JSON config file. All features — listeners, static serving, proxying, forwarding, DNS, upstream pools — are declared in one place.

```jsonc
{
  // ── Listeners ─────────────────────────────────────────────
  "listen": [
    { "name": "web",       "uri": "http://:8080" },
    { "name": "tls",       "uri": "https://:8443" },
    { "name": "dns-in",    "uri": "udp://127.0.0.1:5353" },
    { "name": "db-local",  "uri": "tcp://:3307" },
    { "name": "sidecar",   "uri": "tcp://:9000" },
    { "name": "gameproxy", "uri": "udp://:27015" },
    "unix:///tmp/app.sock"
  ],

  // ── TLS ───────────────────────────────────────────────────
  "tls": { "cert": "fullchain.pem", "key": "privkey.pem" },
  "backlog": 2048,

  // ── DNS Zone ──────────────────────────────────────────────
  "dns_ttl": 300,
  "dns": {
    "redis.local":    { "A": "192.168.11.2", "AAAA": "fd00::2" },
    "postgres.local": { "A": "192.168.11.3" },
    "web.local":      { "A": ["10.0.0.10", "10.0.0.11"], "AAAA": "fd00::10" },
    "app.local":      { "CNAME": "web.local" },
    "cache.local":    { "A": "192.168.11.4", "TTL": 60 },
    "example.com": {
      "A": "93.184.216.34",
      "MX": [
        { "priority": 10, "host": "mx1.example.com" },
        { "priority": 20, "host": "mx2.example.com" }
      ],
      "TXT": ["v=spf1 include:example.com ~all"],
      "NS":  ["ns1.example.com", "ns2.example.com"]
    },
    "_http._tcp.web.local": {
      "SRV": { "priority": 0, "weight": 100, "port": 8080, "target": "web.local" }
    }
  },

  // ── Upstream Pools (load balancing) ───────────────────────
  "upstreams": {
    "app_pool": {
      "targets": [
        "http://127.0.0.1:8001",
        "http://127.0.0.1:8002",
        "http://127.0.0.1:8003"
      ],
      "balance": "round-robin",
      "health_check": { "path": "/healthz", "interval": 10 }
    },
    "ws_pool": {
      "targets": ["http://127.0.0.1:8010", "http://127.0.0.1:8011"],
      "balance": "least-conn"
    }
  },

  // ── Dispatch (routing rules) ──────────────────────────────
  "dispatch": [
    // Static files
    { "listener": "web", "handler": "static", "root": "./public", "index": "index.html" },
    { "listener": "tls", "handler": "static", "root": "./public" },

    // Reverse proxy to backend API
    { "listener": "web", "handler": "proxy", "path": "/api/**",
      "proxy_pass": "http://127.0.0.1:8000" },
    { "listener": "tls", "handler": "proxy", "path": "/api/**",
      "proxy_pass": "http://127.0.0.1:8000" },

    // Load-balanced proxy via named upstream pool
    { "listener": "web", "handler": "proxy", "path": "/app/**",
      "proxy_pass": "upstream://app_pool" },

    // WebSocket proxy (detected automatically from Upgrade header)
    { "listener": "web", "handler": "proxy", "path": "/ws/**",
      "proxy_pass": "upstream://ws_pool" },

    // TCP forwarding: local :3307 → remote MySQL
    { "listener": "db-local", "handler": "forward",
      "forward_to": "tcp://dbserver.internal:3306",
      "timeout": 300, "max_connections": 20, "buffer_size": 65536 },

    // TCP → UDP protocol translation (StatsD sidecar)
    { "listener": "sidecar", "handler": "forward",
      "forward_to": "udp://statsd.internal:8125", "mode": "datagram" },

    // UDP → TCP protocol translation (game traffic)
    { "listener": "gameproxy", "handler": "forward",
      "forward_to": "tcp://gameserver.internal:27015", "mode": "stream" },

    // Unix socket forwarding (PHP-FPM, etc.)
    { "listener": "sidecar", "handler": "forward",
      "forward_to": "unix:///var/run/php-fpm.sock" },

    // Built-in DNS
    { "listener": "dns-in", "handler": "dns" }
  ]
}
```

### Handler Types

| Handler    | What it does                                  | Key fields                                  |
|------------|-----------------------------------------------|---------------------------------------------|
| `static`   | Serve files from a directory                  | `root`, `index`                             |
| `proxy`    | HTTP reverse proxy (+ WebSocket auto-detect)  | `proxy_pass`, `path`, `keepalive`           |
| `forward`  | Raw TCP/UDP/Unix bidirectional relay          | `forward_to`, `mode`, `timeout`, `buffer_size` |
| `dns`      | Authoritative DNS from the `dns` config block | *(none — uses the top-level `dns` section)* |
| `jit`      | Spawn a classyc-JIT'd HTTP API via `jitrunner`, reverse-proxy to it | `bmir`, `port`, `watch`, `mode`, `path`, `keepalive` |

#### `keepalive`: pool upstream connections instead of one per request

`{ "handler": "proxy", "proxy_pass": "http://127.0.0.1:8000", "path": "/api/**", "keepalive": true, "keepalive_pool_size": 4, "keepalive_idle_timeout": 60 }`

Off by default: every proxied request opens a fresh upstream connection
and tells the upstream `Connection: close` (see "No Proxy Retry /
Failover" below — there's still no retry on a failed upstream). Turning
`keepalive` on for a route pools idle upstream connections per target
(`host:port`) instead, reusing them across requests — this matters a lot
under load, since opening a fresh TCP connection per request is a fixed
cost on top of whatever the request itself takes (measured: proxying a
sub-millisecond endpoint through a fresh connection each time was ~3x
slower than reusing one; a 15ms database-backed endpoint saw a much
smaller relative hit, since the fixed per-connection cost is a smaller
fraction of a slower request).

**Only pools responses framed with `Content-Length`.** Reusing a
connection safely requires knowing exactly where one response ends and
the next begins; chunked or unknown-length responses (or an upstream
that explicitly sends `Connection: close`) always fall back to a fresh
per-request connection rather than risk corrupting a reused one.
Pooled connections are validated optimistically on reuse (a quick
non-blocking `MSG_PEEK`, catching the common case of an upstream that
already closed an idle connection on its own timeout) — a connection
that goes stale in the instant between that check and actually being
used isn't retried, same as any other upstream failure.

**Size `keepalive_pool_size` (default 4) below the upstream's own
concurrency capacity, not up to it.** A backend typically allocates one
worker/thread per *open* connection, including ones sitting idle in
npserver's pool — so a pool sized at or above the backend's own worker
count can tie up every worker just idling for npserver, starving
anything else that hits the backend (a direct request, a health check,
another listener's own `proxy_pass` to the same target). `jit_backend`
runs 8 workers; its `config.json` entry deliberately pools only 4.
`keepalive_idle_timeout` (default 60s) closes a pooled connection that's
sat unused that long — keep it comfortably above the backend's own
idle-connection timeout if it has one, or every pooled connection will
already be stale by the time npserver tries to reuse it.

#### `jit`: run a classyc HTTP API behind npserver

`{ "handler": "jit", "bmir": "jit/app.bmir", "port": 18099, "watch": true, "path": "/jit/**" }`

`bmir` may be a string or an array of `.bmir` files — jitrunner loads
them all into one MIR context and runtime-links them (HTTP core + app):

`{ "handler": "jit", "bmir": ["jit_backend/app.bmir", "jit_backend/api.bmir"], "port": 8000, "mode": "gen", "path": "/api/**" }`

At startup npserver execs `jitrunner <bmir...> [--watch] [--mode <mode>]` as
a child process (its own process group, so shutdown kills it cleanly) and
wires the matched `path` to reverse-proxy to `http://127.0.0.1:<port>` —
the same "proxy" machinery used everywhere else, just pointed at a
process npserver itself launched. If `bmir` doesn't exist at startup the
route is skipped with a logged error (no spawn) rather than looping —
with `--watch`, jitrunner busy-retries a missing file, so npserver
refuses to start that.

**`port` must match what the app itself binds.** `jitrunner` runs the
JIT'd program's `main()` with no `argv`, so npserver has no way to pass
`--port=` through — set `port` to whatever the compiled program hardcodes
(or reads from its own config/env).

**`mode`** passes `--mode <value>` to jitrunner (`lazy` (default) / `gen`
/ `interp`). Any classyc app that runs its own worker threads (e.g.
`jit_backend`, or `http-serve-cchan.c`'s `serve_workers_cchan`) needs
`"mode": "gen"` — jitrunner's default lazy per-function JIT has no
cross-thread lock around first-call compilation, so two worker threads
racing to first-call the same function can hit a MIR assertion and crash.
`gen` compiles everything upfront on the main thread before any worker
starts, which avoids that race.

Build each `.bmir` with classyc `-c` (one TU per file), then pass them
all to jitrunner / `"bmir": [...]`. Combining sources into one `.bmir`
(`classyc -o app.bmir a.cy b.cy`, no `-c`) still works if you prefer.
See classyc's `examples/http_crud` for what a real routed HTTP API looks
like (`[[HttpGet]]` + SQLite) and `jit_backend/` in this repo for a
worked single-file example with its own worker pool.

### Load Balancing Algorithms

| Algorithm     | Config value    | Behavior                                       |
|---------------|-----------------|-------------------------------------------------|
| Round-robin   | `round-robin`   | Sequential rotation across healthy targets      |
| Random        | `random`        | Random selection with health-aware fallback      |
| IP hash       | `ip-hash`       | Sticky sessions — same client IP → same backend |
| Conn hash     | `conn-hash`     | Per-connection stickiness via fd hash            |
| Least connections | `least-conn` | Route to the backend with fewest active conns   |

---

## Use Cases

### 🏠 Home Lab / Self-Hosting

One binary replaces your entire ingress stack. Serve your blog, proxy Jellyfin, forward SSH, and resolve `.local` hostnames — all from one config:

```jsonc
{
  "listen": [
    { "name": "web", "uri": "https://:443" },
    { "name": "dns", "uri": "udp://192.168.1.1:53" }
  ],
  "tls": { "cert": "/etc/letsencrypt/live/mysite/fullchain.pem",
           "key":  "/etc/letsencrypt/live/mysite/privkey.pem" },
  "dns": {
    "jellyfin.home": { "A": "192.168.1.50" },
    "nas.home":      { "A": "192.168.1.10" },
    "pi.home":       { "A": "192.168.1.20" }
  },
  "dispatch": [
    { "listener": "web", "handler": "static", "root": "/var/www/blog" },
    { "listener": "web", "handler": "proxy", "path": "/jellyfin/**",
      "proxy_pass": "http://192.168.1.50:8096" },
    { "listener": "dns", "handler": "dns" }
  ]
}
```

### 🐳 Container Sidecar

Deploy as a sidecar that terminates TLS, proxies HTTP to your app, and forwards metrics to StatsD — no service mesh required:

```bash
./npserver -config sidecar.json
# Listens: https://:443 → http://localhost:8000 (your app)
#          tcp://:9000 → udp://statsd:8125 (metrics)
#          unix:///tmp/app.sock → your app's unix socket
```

### 🎮 Game Server Proxy

Translate between UDP game clients and a TCP game server, with a web dashboard on the side:

```jsonc
{
  "listen": [
    { "name": "game",  "uri": "udp://:27015" },
    { "name": "admin", "uri": "http://:8080" }
  ],
  "dispatch": [
    { "listener": "game", "handler": "forward",
      "forward_to": "tcp://gameserver.internal:27015", "mode": "stream" },
    { "listener": "admin", "handler": "static", "root": "./dashboard" }
  ]
}
```

### 🔬 Dev/Test Database Tunnel

Expose a remote database locally — like `ssh -L` but declarative and persistent:

```bash
# MySQL on a remote host, available locally on :3307
./npserver tcp://:3307 -to dbserver.staging:3306
```

### ⚡ WebSocket Load Balancer

Distribute WebSocket connections across a pool of backend servers with least-connections balancing:

```jsonc
{
  "listen": [{ "name": "web", "uri": "http://:8080" }],
  "upstreams": {
    "ws_pool": {
      "targets": ["http://10.0.0.1:3000", "http://10.0.0.2:3000", "http://10.0.0.3:3000"],
      "balance": "least-conn"
    }
  },
  "dispatch": [
    { "listener": "web", "handler": "proxy", "path": "/ws/**",
      "proxy_pass": "upstream://ws_pool" }
  ]
}
```

### 🏢 Split-Horizon DNS + Web Server

Serve different DNS answers on your internal network while also hosting the website:

```jsonc
{
  "listen": [
    { "name": "web",  "uri": "http://:80" },
    { "name": "dns",  "uri": "udp://10.0.0.1:53" }
  ],
  "dns": {
    "api.company.com":    { "A": "10.0.0.50" },
    "db.company.com":     { "A": "10.0.0.51" },
    "search.company.com": { "A": ["10.0.0.60", "10.0.0.61"] },
    "company.com": {
      "A": "10.0.0.100",
      "MX": [{ "priority": 10, "host": "mx.company.com" }],
      "TXT": ["v=spf1 include:company.com ~all"]
    }
  },
  "dispatch": [
    { "listener": "web", "handler": "static", "root": "/var/www/company" },
    { "listener": "dns", "handler": "dns" }
  ]
}
```

---

## Build

```bash
cmake . && make
```

**Dependencies:**
- **OpenSSL** — required for TLS (`https://`, `ssl://` listeners)
- **zlib** — optional, enables gzip compression (auto-detected by CMake)

Both are auto-detected. If zlib isn't found, npserver builds without compression. Override compile-time limits with `-D` flags:

```bash
cmake . -DCMAKE_C_FLAGS="-DMAX_BODY_SIZE=134217728 -DNUM_CLIENTS=50000 -DGZIP_MAX_SIZE=10485760"
```

| Define          | Default   | What it controls                        |
|-----------------|-----------|-----------------------------------------|
| `NUM_CLIENTS`   | 10,000    | Max concurrent connections              |
| `MAX_BODY_SIZE` | 64 MiB    | Max HTTP request body                   |
| `MAX_HEADERS`   | 128       | Max HTTP headers per request            |
| `MAX_URL_SIZE`  | 4,096     | Max URI length                          |
| `GZIP_MAX_SIZE` | 5 MiB     | Max file size for in-memory compression |
| `GZIP_MIN_SIZE` | 256       | Min file size to bother compressing     |

---

## Known Limitations

npserver is a sharp tool, not a padded one. Know the edges:

### No Proxy Retry / Failover
If a chosen upstream fails to connect, the request gets a `502 Bad Gateway`. There is no automatic retry to the next backend in the pool. A failed health check marks a target as unhealthy, but active health checking is not yet implemented — the config fields exist as placeholders.

### Connection Timeouts
Header, body, and idle timeouts are enforced on all incoming connections (configurable via the `"timeouts"` config section). Per-IP connection limits provide basic DoS defense. Proxy upstream connect timeouts are also enforced. Forward relay idle timeouts are parsed but not yet enforced.

### No Rate Limiting
There is no per-IP or per-path rate limiting. Every request is served. Pair with an external firewall (`iptables`, `nftables`, `pf`) if you need throttling.

### No HTTP/2 or HTTP/3
The HTTP parser is HTTP/1.1 only. No ALPN negotiation, no binary framing, no stream multiplexing. This is by design — the goal is simplicity, not feature parity with Envoy.

### No HTTPS Upstream Proxying
The reverse proxy only connects to `http://` backends. TLS-to-backend (`https://` upstream) is not **yet** supported. Use this server for TLS termination and talk plaintext to your backends.

### No ACME / Auto-TLS
No built-in Let's Encrypt. Bring your own certs. Works well with `certbot` or `acme.sh` in a cron job.

### Single-Threaded
One thread, one event loop, one core. Throughput scales vertically with CPU clock speed, not core count. For most workloads this is fine — the bottleneck is usually I/O, not CPU.

### No Hot Reload
Changing `config.json` requires a restart. No `SIGHUP` reload, no zero-downtime config swap.

### No SNI / Per-Listener TLS
One global cert/key pair for all TLS listeners. No per-domain certificate selection via SNI.

### No DNSSEC / DNS-over-HTTPS
The DNS server is authoritative-only, plain UDP, no EDNS(0). Good enough for `.local` service discovery. Not a replacement for a public-facing nameserver.

---

## Project Structure

```
server.c          Main entry point, config wiring, event loop setup
request.h         HTTP handlers: static files, reverse proxy, load balancer, forwarding
http.h            Incremental HTTP/1.1 parser and response builder
socket_server.h   Event loop (epoll/kqueue), TLS, connection management
dns.h             Authoritative DNS responder
config.h          JSON config loader
dict.h            JSON parser (hand-rolled, no dependencies)
stringbuf.h       Safe dynamic string buffer
stb_ds.h          Hash map / dynamic array (stb single-header lib)
```

---

## Why?

Because I wanted a small, high-performance, C-only mantlepiece.

One process. One config. One binary you can `scp` to a server and run. No YAML, no plugins, no runtime, no 200MB Docker image. Just a program that listens on sockets and moves bytes where they need to go.

---

*npserver — one proxy to serve them all.*

#ifndef SOCKET_SERVER_H
#define SOCKET_SERVER_H

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#include <sys/uio.h>
#endif

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"
#include "stringbuf.h"

#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#include "http.h"
#include "logger.h"

#define NUM_CLIENTS 10000
#define MAX_EVENTS 32
#define MAX_MSG_SIZE 4096
#define EV_READ 1
#define EV_WRITE 2
#define MAX_FDS 2
#define MAX_SOCKETS 10
#define CHUNK_SIZE 65536

struct event {
    int fd;
    int events;
};

enum conn_type {
    CONN_UNKNOWN,
    CONN_TCPV4,
    CONN_TCPV6,
    CONN_SSLV4,
    CONN_SSLV6,
    CONN_UDPV4,
    CONN_UDPV6,
    CONN_LOCAL,
    CONN_FORWARD_TCP,   // forwarded TCP relay (not HTTP-parsed)
    CONN_FORWARD_UDP    // forwarded UDP relay
};

struct client_info {
    enum conn_type type;
    char hostname[1025];
    char addr[1025];
    int port;
};

struct client_data {
    int fd;
    void (*on_data)();
    bool is_listen;
    size_t bytes_read;
    size_t bytes_written;
    struct sockaddr_storage peer_addr;
    socklen_t peer_len;
    bool connected;
    struct client_info info;
#ifdef HAVE_OPENSSL
    SSL *ssl;
#endif
    bool is_http;
    struct http_parser parser;
    bool handshake_done;
    char *listen_uri;
    struct socket_info *si;
	char *matched_prefix;
	// file sending
	bool sending_body;
    int send_file_fd;
    off_t send_offset;
    size_t send_remaining;
    bool use_sendfile;
    char *send_buffer;
    size_t send_buf_len;
    size_t send_buf_pos;
    int loopfd;
    // deferred access log
    char *log_action;
    int log_status;
    // proxy state
    bool proxy_active;      // connection is owned by proxy (don't reset parser)
    bool proxy_streaming;   // body data should be forwarded to upstream
    // forward state
    bool forward_active;    // connection is a raw forward relay (bypass HTTP)
    int  forward_peer_fd;   // the other side of the forwarded pair
    // connection timing (for timeout sweeps)
    time_t accept_time;         // when connection was accepted
    time_t last_activity;       // last time data was received
} clients[NUM_CLIENTS];

struct fd_hash_entry {
    int key;
    int value;
};

struct fd_hash_entry *fd_to_index = NULL;

struct socket_info {
    int fds[MAX_FDS];
    int num_fds;
    bool is_datagram;
    bool is_tls;
    bool is_http;
    char *uri;

    void (*socket_handler)();
    struct forward_target *fwd_target; // non-NULL if this listener is a forwarding port
};

struct server_sockets {
    struct socket_info sockets[MAX_SOCKETS];
    int num_sockets;
};

struct http_parser;

typedef void (*socket_handler_t)(int fd, const char *data, size_t len, struct client_info *info);
typedef void (*http_handler_t)(struct http_parser *http, struct http_request *req, struct client_data *info);
static void default_http_handler(struct http_parser *http, struct http_request *req, struct client_data *info);

/* ── Port forwarding / protocol translation ───────────────────────────── */

enum forward_proto {
    FWD_TCP,
    FWD_UDP,
    FWD_UNIX
};

/* How to frame data when crossing protocol boundaries (TCP↔UDP) */
enum forward_mode {
    FWD_MODE_AUTO,       // infer from listener + target protocols
    FWD_MODE_STREAM,     // treat as continuous byte stream (TCP semantics)
    FWD_MODE_DATAGRAM    // preserve message boundaries (UDP semantics)
};

/* Parsed forwarding target from config "forward_to": "tcp://host:port" */
struct forward_target {
    enum forward_proto proto;
    enum forward_mode  mode;
    char   host[256];
    int    port;
    char   path[108];         // for unix:// sockets
    int    timeout;           // idle timeout seconds (0 = no timeout)
    int    max_connections;   // concurrent connection limit (0 = unlimited)
    size_t buffer_size;       // relay buffer size (0 = default CHUNK_SIZE)
};

/* Per-connection forwarding relay state (bidirectional pipe) */
typedef struct {
    int  downstream_fd;       // client-facing fd
    int  upstream_fd;         // target-facing fd
    bool connected;           // upstream connect() completed

    enum forward_proto downstream_proto;
    enum forward_proto upstream_proto;
    enum forward_mode  mode;

    struct sockaddr_storage upstream_addr;  // for UDP sendto()
    socklen_t upstream_addr_len;
    struct sockaddr_storage downstream_addr; // for UDP reply
    socklen_t downstream_addr_len;

    /* Relay buffers (downstream→upstream and upstream→downstream) */
    char  *up_buf;            // data flowing client → target
    size_t up_len;
    size_t up_sent;
    char  *down_buf;          // data flowing target → client
    size_t down_len;
    size_t down_sent;

    size_t buffer_size;       // configured buffer cap
    int    timeout;           // idle timeout (seconds)
    time_t last_activity;     // for idle timeout tracking

    size_t bytes_relayed_up;  // total bytes client → target
    size_t bytes_relayed_down;// total bytes target → client
} forward_state_t;

/* Hash map: fd → forward_state_t (keyed by downstream_fd) */
typedef struct { int key; forward_state_t value; } ForwardEntry;
/* Hash map: upstream_fd → downstream_fd (reverse lookup) */
typedef struct { int key; int value; } ForwardUpstreamEntry;

/* ── Connection timeout / anti-abuse defaults ─────────────────────────── */
typedef struct {
    int header_timeout;          /* seconds to receive complete headers (Slowloris) */
    int body_timeout;            /* seconds between body data chunks */
    int idle_timeout;            /* keep-alive idle timeout */
    int max_connections_per_ip;  /* 0 = unlimited */
} ConnectionTimeouts;

/* Defaults chosen for safety without breaking normal use.
 * Overridden by config.json "timeouts" section; hot-reloadable. */
static ConnectionTimeouts g_conn_timeouts = {
    .header_timeout         = 10,   /* 10s to send headers (Slowloris defense) */
    .body_timeout           = 30,   /* 30s between body reads */
    .idle_timeout           = 60,   /* 60s keep-alive idle */
    .max_connections_per_ip = 50,   /* per-IP connection cap */
};

/* Per-IP connection counter (IP string → count) */
typedef struct { char *key; int value; } IpConnEntry;
static IpConnEntry *ip_conn_counts = NULL;

struct event_handlers {
    void (*on_accept)(int loopfd, int local_s);
    void (*on_read)(int loopfd, int fd);
    void (*on_write)(int loopfd, int fd);
    void (*on_connect)(int loopfd, int fd, struct client_info *info);
    void (*on_disconnect)(int loopfd, int fd);
    void (*on_error)(int loopfd, int fd, int err);
    void (*on_access_log)(struct client_info *info, const char *action, int status, size_t bytes_read, size_t bytes_written);
    void (*on_error_log)(const char *msg);
    http_handler_t on_http_request;
    void (*on_upstream_event)(int loopfd, int fd, int events);
    void (*on_body_data)(int loopfd, int fd, const char *data, size_t len);
    void (*on_client_writable)(int loopfd, int fd);
    // forward relay callbacks
    void (*on_forward_accept)(int loopfd, int client_fd, struct forward_target *target);
    void (*on_forward_event)(int loopfd, int fd, int events);
    void (*on_forward_data)(int loopfd, int fd, const char *data, size_t len);
    void (*on_forward_disconnect)(int fd);
    // periodic timer callback (called ~once per second)
    void (*on_timer)(int loopfd);
};

static inline void my_on_error(int loopfd, int fd, int err, struct event_handlers *handlers);
static inline int event_mod(int loopfd, int fd, int events);

static inline void cleanup_send_state(struct client_data *cd) {
    if (cd->send_file_fd >= 0) {
        close(cd->send_file_fd);
        cd->send_file_fd = -1;
    }
    if (cd->send_buffer) {
        free(cd->send_buffer);
        cd->send_buffer = NULL;
    }
    cd->send_offset = 0;
    cd->send_remaining = 0;
    cd->send_buf_len = 0;
    cd->send_buf_pos = 0;
    cd->sending_body = false;
    free(cd->log_action);
    cd->log_action = NULL;
    cd->log_status = 0;
}

#ifdef HAVE_OPENSSL
static SSL_CTX *g_ssl_ctx = NULL;

static inline void socket_server_init_tls(const char *cert_pem, const char *key_pem) {
    if (!cert_pem || !key_pem) return;

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

    const SSL_METHOD *method = TLS_server_method();
    g_ssl_ctx = SSL_CTX_new(method);
    if (!g_ssl_ctx) {
        LOG_ERROR(SERVER, "SSL_CTX_new failed");
        ERR_print_errors_fp(stderr);
        return;
    }

    if (SSL_CTX_use_certificate_file(g_ssl_ctx, cert_pem, SSL_FILETYPE_PEM) <= 0) {
        LOG_ERROR(SERVER, "failed to load cert: %s", cert_pem);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
        return;
    }

    if (SSL_CTX_use_PrivateKey_file(g_ssl_ctx, key_pem, SSL_FILETYPE_PEM) <= 0) {
        LOG_ERROR(SERVER, "failed to load key: %s", key_pem);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
        return;
    }

    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(g_ssl_ctx, TLS1_3_VERSION);

    SSL_CTX_set_tlsext_servername_callback(g_ssl_ctx, NULL);
    SSL_CTX_set_tlsext_servername_arg(g_ssl_ctx, NULL);
}
#endif

static inline void socket_server_init_hash(void) {
    hmdefault(fd_to_index, -1);
    sh_new_strdup(ip_conn_counts);
    shdefault(ip_conn_counts, 0);
}

static inline int get_conn(int fd) {
    int idx = hmget(fd_to_index, fd);
    return idx;
}

static inline int conn_add(int loopfd, int fd, bool is_listen) {
    if (fd < 1) return -1;
    int i;
    for (i = 0; i < NUM_CLIENTS; i++) {
        if (clients[i].fd == 0) {
            break;
        }
    }
    if (i == NUM_CLIENTS) return -1;
    clients[i].fd = fd;
    clients[i].on_data = NULL;
    clients[i].is_listen = is_listen;
    clients[i].bytes_read = 0;
    clients[i].bytes_written = 0;
    memset(&clients[i].peer_addr, 0, sizeof(clients[i].peer_addr));
    clients[i].peer_len = 0;
    clients[i].connected = false;
    clients[i].info = (struct client_info){0};
#ifdef HAVE_OPENSSL
    clients[i].ssl = NULL;
#endif
	// File sending state
	clients[i].sending_body = false;
    clients[i].send_file_fd = -1;
    clients[i].send_offset = 0;
    clients[i].send_remaining = 0;
    clients[i].use_sendfile = false;
    clients[i].send_buffer = NULL;
    clients[i].send_buf_len = 0;
    clients[i].send_buf_pos = 0;
    clients[i].loopfd = loopfd;
    clients[i].accept_time = time(NULL);
    clients[i].last_activity = clients[i].accept_time;

    clients[i].is_http = false;
    memset(&clients[i].parser, 0, sizeof(clients[i].parser));
    clients[i].handshake_done = true;
    clients[i].listen_uri = NULL;
    clients[i].si = NULL;
    hmput(fd_to_index, fd, i);
    return 0;
}

static inline int conn_del(int fd) {
    if (fd < 1) return -1;
    int i = get_conn(fd);
    if (i == -1) return -1;
#ifdef HAVE_OPENSSL
    if (clients[i].ssl) {
        SSL_shutdown(clients[i].ssl);
        SSL_free(clients[i].ssl);
        clients[i].ssl = NULL;
    }
#endif
    if (clients[i].is_http) {
        http_parser_destroy(&clients[i].parser);
    }
	cleanup_send_state(&clients[i]);
    free(clients[i].listen_uri);
    /* Decrement per-IP connection counter */
    {
        char ip_key[INET6_ADDRSTRLEN + 1] = {0};
        if (clients[i].peer_addr.ss_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)&clients[i].peer_addr)->sin_addr, ip_key, sizeof(ip_key));
        } else if (clients[i].peer_addr.ss_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&clients[i].peer_addr)->sin6_addr, ip_key, sizeof(ip_key));
        }
        if (ip_key[0]) {
            int cur = shget(ip_conn_counts, ip_key);
            if (cur > 1) {
                shput(ip_conn_counts, ip_key, cur - 1);
            } else if (cur == 1) {
                shdel(ip_conn_counts, ip_key);
            }
        }
    }
    clients[i].fd = 0;
    clients[i].on_data = NULL;
    clients[i].is_listen = false;
    hmdel(fd_to_index, fd);
    return close(fd);
}

static inline int socket_write(int fd, const void *buf, size_t len);

static inline void default_socket_handler(int fd, const char *data, size_t len, struct client_info *info);

/*static inline void default_socket_handler(int fd, const char *data, size_t len, struct client_info *info) {
    if (info->type == CONN_UDPV4 || info->type == CONN_UDPV6) {
        socket_write(fd, data, len);
    } else {
        const char *msg = "Hello world!\n";
        socket_write(fd, msg, strlen(msg));
    }
}
*/

static inline int socket_write(int fd, const void *buf, size_t len) {
    int idx = get_conn(fd);
    if (idx == -1 || len == 0) {
		LOG_DEBUG(SERVER, "socket_write: invalid idx=%d or len=%lu", idx, len);
		return 0;
	}
    ssize_t sent;
#ifdef HAVE_OPENSSL
    if (!clients[idx].is_listen && clients[idx].ssl) {
        sent = SSL_write(clients[idx].ssl, buf, len);
		LOG_DEBUG(SERVER, "SSL_write client idx=%d buf=%p len=%lu", idx, buf, len);
        if (sent <= 0) {
            int sslerr = SSL_get_error(clients[idx].ssl, sent);
            if (sslerr == SSL_ERROR_WANT_WRITE || (sslerr == SSL_ERROR_SYSCALL && errno == EPIPE)) {
                sent = 0;
            } else {
                return -1;
            }
        }
    } else
#endif
    if (clients[idx].is_listen) {
        sent = sendto(fd, buf, len, 0, (struct sockaddr *)&clients[idx].peer_addr, clients[idx].peer_len);
    } else {
        sent = send(fd, buf, len, 0);
    }
    if (sent > 0) {
        clients[idx].bytes_written += sent;
        return sent;
    }
    if (sent == -1 && errno == EPIPE) {
        return 0;
    }
    return -1;
}

#ifdef linux
static inline int event_create(void) {
	return epoll_create1(EPOLL_CLOEXEC);
}
static inline int event_add(int loopfd, int fd, int events, void *udata) {
	struct epoll_event ev;
	ev.events = ((events & EV_READ) ? EPOLLIN : 0) | ((events & EV_WRITE) ? EPOLLOUT : 0);
	ev.data.fd = fd;
	return epoll_ctl(loopfd, EPOLL_CTL_ADD, fd, &ev);
}
static inline int event_del(int loopfd, int fd, int events, void *udata) {
	return epoll_ctl(loopfd, EPOLL_CTL_DEL, fd, NULL);
}
/* MOD if already registered, ADD if not (avoids ENOENT from EPOLL_CTL_MOD) */
static inline int event_set(int loopfd, int fd, int events) {
	struct epoll_event ev;
	ev.events = ((events & EV_READ) ? EPOLLIN : 0) | ((events & EV_WRITE) ? EPOLLOUT : 0);
	ev.data.fd = fd;
	int r = epoll_ctl(loopfd, EPOLL_CTL_MOD, fd, &ev);
	if (r < 0 && errno == ENOENT)
		r = epoll_ctl(loopfd, EPOLL_CTL_ADD, fd, &ev);
	return r;
}
static inline int event_wait(int loopfd, struct event *evlist, int max, int timeout_ms) {
	struct epoll_event native_events[max];
	int n = epoll_wait(loopfd, native_events, max, timeout_ms);
	for (int j = 0; j < n; j++) {
		evlist[j].fd = native_events[j].data.fd;
		evlist[j].events = 0;
		if (native_events[j].events & EPOLLIN) {
			evlist[j].events |= EV_READ;
		}
		if (native_events[j].events & EPOLLOUT) {
			evlist[j].events |= EV_WRITE;
		}
	}
	return n;
}
#else
static inline int event_create(void) {
    return kqueue();
}

static inline int event_add(int loopfd, int fd, int events, void *udata) {
    struct kevent ev[2];
    int nev = 0;
	LOG_TRACE(SERVER, "event_add fd=%d events=%d", loopfd, fd);
    if (events & EV_READ) {
        EV_SET(&ev[nev++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    }
    if (events & EV_WRITE) {
        EV_SET(&ev[nev++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
    }
    return kevent(loopfd, ev, nev, NULL, 0, NULL);
}

static inline int event_del(int loopfd, int fd, int events, void *udata) {
    struct kevent ev[2];
    int nev = 0;
	LOG_TRACE(SERVER, "event_del fd=%d events=%d", loopfd, fd);
    if (events & EV_READ) {
        EV_SET(&ev[nev++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    }
    if (events & EV_WRITE) {
        EV_SET(&ev[nev++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    }
    return kevent(loopfd, ev, nev, NULL, 0, NULL);
}
/* kqueue: EV_ADD is idempotent (acts as add-or-modify), so event_set == event_add */
static inline int event_set(int loopfd, int fd, int events) {
    struct kevent ev[2];
    int nev = 0;
    if (events & EV_READ) {
        EV_SET(&ev[nev++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    } else {
        EV_SET(&ev[nev++], fd, EVFILT_READ, EV_ADD | EV_DISABLE, 0, 0, NULL);
    }
    if (events & EV_WRITE) {
        EV_SET(&ev[nev++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
    } else {
        EV_SET(&ev[nev++], fd, EVFILT_WRITE, EV_ADD | EV_DISABLE, 0, 0, NULL);
    }
    return kevent(loopfd, ev, nev, NULL, 0, NULL);
}

static inline int event_wait(int loopfd, struct event *evlist, int max, int timeout_ms) {
    struct kevent native_events[max];
    struct timespec *ts = NULL;
    struct timespec t;
    if (timeout_ms >= 0) {
        t.tv_sec = timeout_ms / 1000;
        t.tv_nsec = (timeout_ms % 1000) * 1000000LL;
        ts = &t;
    }

    int n = kevent(loopfd, NULL, 0, native_events, max, ts);
	for (int j = 0; j < n; j++) {
        evlist[j].fd = native_events[j].ident;
        evlist[j].events = 0;
        if (native_events[j].filter == EVFILT_READ) {
            evlist[j].events |= EV_READ;
        }
        if (native_events[j].filter == EVFILT_WRITE) {
            evlist[j].events |= EV_WRITE;
        }
    }

    return n;
}
#endif

static inline void parse_client_info(struct sockaddr_storage *addr, socklen_t addrlen, struct client_info *info, bool is_datagram, bool is_tls) {
    memset(info, 0, sizeof(*info));
    sa_family_t family = addr->ss_family;
    if (family == AF_UNIX) {
        info->type = CONN_LOCAL;
        struct sockaddr_un *un = (struct sockaddr_un *) addr;
        strncpy(info->addr, un->sun_path, sizeof(info->addr) - 1);
        info->addr[sizeof(info->addr) - 1] = '\0';
        if (info->addr[0] == '\0') {
            strcpy(info->addr, "unix");
        }
        strcpy(info->hostname, "unix");
        info->port = 0;
    } else if (family == AF_INET) {
        if (is_tls) {
            info->type = CONN_SSLV4;
        } else {
            info->type = is_datagram ? CONN_UDPV4 : CONN_TCPV4;
        }
        char numeric_host[1025], portstr[32];
        int ret = getnameinfo((struct sockaddr *) addr, addrlen,
                              numeric_host, sizeof(numeric_host),
                              portstr, sizeof(portstr),
                              NI_NUMERICHOST | NI_NUMERICSERV);
        if (ret == 0) {
            strncpy(info->addr, numeric_host, sizeof(info->addr) - 1);
            info->port = atoi(portstr);
        } else {
            info->port = 0;
        }
        ret = getnameinfo((struct sockaddr *) addr, addrlen,
                          info->hostname, sizeof(info->hostname),
                          NULL, 0, 0);
        if (ret != 0) {
            strncpy(info->hostname, info->addr, sizeof(info->hostname) - 1);
        }
    } else if (family == AF_INET6) {
        if (is_tls) {
            info->type = CONN_SSLV6;
        } else {
            info->type = is_datagram ? CONN_UDPV6 : CONN_TCPV6;
        }
        char numeric_host[1025], portstr[32];
        int ret = getnameinfo((struct sockaddr *) addr, addrlen,
                              numeric_host, sizeof(numeric_host),
                              portstr, sizeof(portstr),
                              NI_NUMERICHOST | NI_NUMERICSERV);
        if (ret == 0) {
            strncpy(info->addr, numeric_host, sizeof(info->addr) - 1);
            info->port = atoi(portstr);
        } else {
            info->port = 0;
        }
        ret = getnameinfo((struct sockaddr *) addr, addrlen,
                          info->hostname, sizeof(info->hostname),
                          NULL, 0, 0);
        if (ret != 0) {
            strncpy(info->hostname, info->addr, sizeof(info->hostname) - 1);
        }
    } else {
        info->type = CONN_UNKNOWN;
    }
}

static inline void log_sys_error(struct event_handlers *handlers, const char *msg, int err) {
    LOG_ERROR(SERVER, "%s: %s", msg, strerror(err));
}

static inline struct socket_info *get_socket_info(struct server_sockets *ss, int fd) {
    for (int k = 0; k < ss->num_sockets; k++) {
        for (int j = 0; j < ss->sockets[k].num_fds; j++) {
            if (ss->sockets[k].fds[j] == fd) {
                return &ss->sockets[k];
            }
        }
    }
    return NULL;
}



static inline void my_on_accept(int loopfd, int local_s, struct server_sockets *ss, struct event_handlers *handlers) {
    struct sockaddr_storage peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    int c = accept(local_s, (struct sockaddr *)&peer_addr, &peer_len);
    if (c < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        log_sys_error(handlers, "accept", errno);
        return;
    }
    if (fcntl(c, F_SETFL, O_NONBLOCK) < 0) {
        log_sys_error(handlers, "fcntl accept", errno);
        close(c);
        return;
    }
    { int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }
    if (conn_add(loopfd, c, false) < 0) {
        LOG_WARN(CONN, "too many clients, rejecting fd=%d", c);
        close(c);
        return;
    }
    int idx = get_conn(c);
    if (idx == -1) {
        close(c);
        return;
    }

    /* Per-IP connection limit (basic DoS defense) */
    {
        struct sockaddr_storage *pa = &peer_addr;
        char ip_key[INET6_ADDRSTRLEN + 1] = {0};
        if (pa->ss_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)pa)->sin_addr, ip_key, sizeof(ip_key));
        } else if (pa->ss_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)pa)->sin6_addr, ip_key, sizeof(ip_key));
        }
        if (ip_key[0] && g_conn_timeouts.max_connections_per_ip > 0) {
            int cur = shget(ip_conn_counts, ip_key);
            if (cur >= g_conn_timeouts.max_connections_per_ip) {
                LOG_WARN(CONN, "rejected fd=%d from %s (limit %d)",
                        c, ip_key, g_conn_timeouts.max_connections_per_ip);
                conn_del(c);
                return;
            }
            shput(ip_conn_counts, ip_key, cur + 1);
        }
        /* Store IP for decrement on disconnect */
        memcpy(&clients[idx].peer_addr, pa, sizeof(*pa));
        clients[idx].peer_len = peer_len;
    }

    struct socket_info *si = get_socket_info(ss, local_s);
    if (si) {
        clients[idx].is_http = si->is_http;
        if (clients[idx].is_http) {
            http_parser_init(&clients[idx].parser, c);
        }
        clients[idx].listen_uri = (si->uri==NULL) ? NULL : strdup(si->uri);
        clients[idx].si = si;
    }
#ifdef HAVE_OPENSSL
    if (si && si->is_tls && g_ssl_ctx) {
        clients[idx].ssl = SSL_new(g_ssl_ctx);
        if (!clients[idx].ssl) {
            ERR_print_errors_fp(stderr);
            conn_del(c);
            return;
        }
        if (SSL_set_fd(clients[idx].ssl, c) <= 0) {
            ERR_print_errors_fp(stderr);
            conn_del(c);
            return;
        }
        SSL_set_accept_state(clients[idx].ssl);
        clients[idx].handshake_done = false;
        int ret = SSL_accept(clients[idx].ssl);
        if (ret == 1) {
            clients[idx].handshake_done = true;
        } else if (ret <= 0) {
            int sslerr = SSL_get_error(clients[idx].ssl, ret);
            if (sslerr != SSL_ERROR_WANT_READ && sslerr != SSL_ERROR_WANT_WRITE) {
                LOG_ERROR(CONN, "SSL_accept failed: %d", sslerr);
                ERR_print_errors_fp(stderr);
                conn_del(c);
                return;
            }
        }
    }
#endif
    parse_client_info(&peer_addr, peer_len, &clients[idx].info, false, si ? si->is_tls : false);
    clients[idx].connected = true;
    if (event_add(loopfd, c, EV_READ, NULL) < 0) {
        log_sys_error(handlers, "event_add accept", errno);
        conn_del(c);
        return;
    }

    /* Forward-mode listener: set up raw relay instead of HTTP/socket handler */
    if (si && si->fwd_target && handlers->on_forward_accept) {
        clients[idx].is_http = false; /* bypass HTTP parsing */
        handlers->on_forward_accept(loopfd, c, si->fwd_target);
        return; /* skip normal on_connect — forward_on_accept handles logging */
    }

    if (handlers->on_connect) handlers->on_connect(loopfd, c, &clients[idx].info);
}

static inline void my_on_read(int loopfd, int fd, struct event_handlers *handlers) {
    int idx = get_conn(fd);
    if (idx == -1) return;
    struct client_data *cd = &clients[idx];

#ifdef HAVE_OPENSSL
    if (cd->ssl && !cd->handshake_done) {
        int ret = SSL_accept(cd->ssl);
        if (ret == 1) {
            cd->handshake_done = true;
        } else if (ret <= 0) {
            int sslerr = SSL_get_error(cd->ssl, ret);
            if (sslerr == SSL_ERROR_WANT_READ || sslerr == SSL_ERROR_WANT_WRITE) return;
            LOG_ERROR(CONN, "SSL_accept failed in read: %d", sslerr);
            ERR_print_errors_fp(stderr);
            conn_del(fd);
            return;
        }
    }
#endif
    ssize_t n;

    /* Forward relay fast path: raw byte relay, no HTTP parsing */
    if (cd->forward_active) {
        char fbuf[CHUNK_SIZE];
        n = recv(fd, fbuf, sizeof(fbuf), 0);
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                if (handlers->on_forward_disconnect) handlers->on_forward_disconnect(fd);
                conn_del(fd);
            }
            return;
        }
        cd->bytes_read += n;
        cd->last_activity = time(NULL);
        /* Dispatch to forward relay via callback */
        if (handlers->on_forward_data) {
            handlers->on_forward_data(loopfd, fd, fbuf, (size_t)n);
        }
        return;
    }

    if (cd->is_http) {

        /* Proxy streaming fast path: read into large buffer and forward to upstream */
        if (cd->proxy_streaming) {
            char pbuf[CHUNK_SIZE];
            n = 0;
#ifdef HAVE_OPENSSL
            if (cd->ssl) {
                n = SSL_read(cd->ssl, pbuf, sizeof(pbuf));
                if (n <= 0) {
                    int sslerr = SSL_get_error(cd->ssl, n);
                    if (sslerr == SSL_ERROR_WANT_READ || sslerr == SSL_ERROR_WANT_WRITE) return;
                    conn_del(fd);
                    return;
                }
            } else
#endif
            {
                n = recv(fd, pbuf, sizeof(pbuf), 0);
                if (n <= 0) {
                    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) conn_del(fd);
                    return;
                }
            }
            cd->bytes_read += n;
            cd->last_activity = time(NULL);
            if (handlers->on_body_data) {
                handlers->on_body_data(loopfd, fd, pbuf, n);
            }
            return;
        }

        char buf[MAX_MSG_SIZE];
        n = 0;
#ifdef HAVE_OPENSSL
        if (cd->ssl) {
            n = SSL_read(cd->ssl, buf, sizeof(buf));
            if (n <= 0) {
                int sslerr = SSL_get_error(cd->ssl, n);
                if (sslerr == SSL_ERROR_WANT_READ || sslerr == SSL_ERROR_WANT_WRITE) return;
                if (sslerr == SSL_ERROR_ZERO_RETURN || (sslerr == SSL_ERROR_SYSCALL && errno == 0)) {
                    conn_del(fd);
                    return;
                }
                LOG_ERROR(CONN, "SSL_read failed: %d", sslerr);
                ERR_print_errors_fp(stderr);
                my_on_error(loopfd, fd, sslerr, handlers);
                conn_del(fd);
                return;
            }
        } else
#endif
        {
            n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    conn_del(fd);
                }
                return;
            }
        }
        cd->bytes_read += n;
        cd->last_activity = time(NULL);

        int res = http_parser_feed(&cd->parser, buf, n);
        if (res < 0) {
            if (handlers->on_error_log) handlers->on_error_log("HTTP parse error");
            conn_del(fd);
        } else if (res == 2) {
            /* Headers complete, body streaming begins */
            if (handlers->on_http_request) {
                handlers->on_http_request(&cd->parser, &cd->parser.req, cd);
            } else {
                default_http_handler(&cd->parser, &cd->parser.req, cd);
            }
            /* After handler setup, forward any leftover body data in parser buffer */
            if (cd->proxy_streaming && handlers->on_body_data) {
                const char *leftover;
                size_t leftover_len = http_parser_drain_body(&cd->parser, &leftover);
                if (leftover_len > 0) {
                    handlers->on_body_data(loopfd, fd, leftover, leftover_len);
                }
            }
        } else if (res > 0) {
            if (handlers->on_http_request) {
                handlers->on_http_request(&cd->parser, &cd->parser.req, cd);
            } else {
                default_http_handler(&cd->parser, &cd->parser.req, cd);
            }
            //http_parser_reset(&cd->parser);
            //conn_del(fd);
        }
    } else {
        char buf[MAX_MSG_SIZE];
        if (cd->is_listen) {
            struct sockaddr_storage peer_addr;
            socklen_t peer_len = sizeof(peer_addr);
            n = recvfrom(fd, buf, MAX_MSG_SIZE, 0, (struct sockaddr *)&peer_addr, &peer_len);
            if (n <= 0) return;
            cd->bytes_read += n;
            cd->last_activity = time(NULL);
            /* Always update peer address — UDP is connectionless
             * datagram may come from a different client. */
            memcpy(&cd->peer_addr, &peer_addr, sizeof(peer_addr));
            cd->peer_len = peer_len;
            if (!cd->connected) {
                parse_client_info(&peer_addr, peer_len, &cd->info, true, false);
                cd->connected = true;
                if (handlers->on_connect) handlers->on_connect(loopfd, fd, &cd->info);
            }
            if (cd->si && cd->si->socket_handler) {
                cd->si->socket_handler(fd, buf, n, &cd->info);
            } else {
                default_socket_handler(fd, buf, n, &cd->info);
            }
        } else {
#ifdef HAVE_OPENSSL
            if (cd->ssl) {
                n = SSL_read(cd->ssl, buf, MAX_MSG_SIZE);
                if (n <= 0) {
                    int sslerr = SSL_get_error(cd->ssl, n);
                    if (sslerr == SSL_ERROR_WANT_READ || sslerr == SSL_ERROR_WANT_WRITE) return;
                    if (sslerr == SSL_ERROR_ZERO_RETURN || (sslerr == SSL_ERROR_SYSCALL && errno == 0)) {
                        conn_del(fd);
                        return;
                    }
                    LOG_ERROR(CONN, "SSL_read failed: %d", sslerr);
                    ERR_print_errors_fp(stderr);
                    my_on_error(loopfd, fd, sslerr, handlers);
                    conn_del(fd);
                    return;
                }
            } else
#endif
            {
                n = recv(fd, buf, MAX_MSG_SIZE, 0);
                if (n <= 0) {
                    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                        conn_del(fd);
                    }
                    return;
                }
            }
            cd->bytes_read += n;
            cd->last_activity = time(NULL);
            if (cd->si->socket_handler) {
                cd->si->socket_handler(fd, buf, n, &cd->info);
            } else {
                default_socket_handler(fd, buf, n, &cd->info);
            }
        }
    }
}

static inline void my_on_write(int loopfd, int fd, struct event_handlers *handlers) {
    int idx = get_conn(fd);
    if (idx == -1) return;
    struct client_data *cd = &clients[idx];
    /* Forward active: nothing to drain on client side (upstream writes handled via forward_on_upstream_event) */
    if (cd->forward_active) {
        return;
    }
    /* Proxy active: drain downstream buffer (upstream->client) */
    if (cd->proxy_active && handlers && handlers->on_client_writable) {
        handlers->on_client_writable(loopfd, fd);
        return;
    }
	if(handlers && handlers->on_write) {
        handlers->on_write(loopfd, fd);
	}
}

static inline void my_on_disconnect(int loopfd, int fd, struct event_handlers *handlers) {
    int idx = get_conn(fd);
    if (idx == -1) return;
    /* Clean up forward relay state if this was a forwarded connection */
    if (clients[idx].forward_active && handlers->on_forward_disconnect) {
        handlers->on_forward_disconnect(fd);
    }
    if (handlers->on_access_log && clients[idx].bytes_read > 0 && !clients[idx].is_http) {
        char action[64];
        snprintf(action, sizeof(action), "TCP connection");
        if (clients[idx].forward_active) snprintf(action, sizeof(action), "FORWARD relay");
        else if (clients[idx].is_listen) snprintf(action, sizeof(action), "UDP datagram");
        handlers->on_access_log(&clients[idx].info, action, 200, clients[idx].bytes_read, clients[idx].bytes_written);
    }
    if (handlers->on_disconnect) handlers->on_disconnect(loopfd, fd);
    conn_del(fd);
}

static inline void my_on_error(int loopfd, int fd, int err, struct event_handlers *handlers) {
    if (handlers && handlers->on_error)
		handlers->on_error(loopfd, fd, err);
#ifdef DEBUG
	else
		LOG_DEBUG(CONN, "error: fd=%d err=%s", fd, strerror(err));
#endif //DEBUG

#ifdef HAVE_OPENSSL
    ERR_print_errors_fp(stderr);
#endif
}

static inline int event_mod(int loopfd, int fd, int events) {
	//printf("event_mod%d %d %s %s\n", loopfd, fd, (events & EV_READ) ? "EV_READ":"", (events & EV_WRITE) ? "EV_WRITE":"");
#ifdef __linux__
    struct epoll_event ev = {0};
    ev.events = (events & EV_READ ? EPOLLIN : 0) | (events & EV_WRITE ? EPOLLOUT : 0);
    ev.data.fd = fd;
    return epoll_ctl(loopfd, EPOLL_CTL_MOD, fd, &ev);
#else
    struct kevent ev[2];
    int nev = 0;
    if (events & EV_READ) {
        EV_SET(&ev[nev++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    } else {
        EV_SET(&ev[nev++], fd, EVFILT_READ, EV_ADD | EV_DISABLE, 0, 0, NULL);
    }
    if (events & EV_WRITE) {
        EV_SET(&ev[nev++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
    } else {
        EV_SET(&ev[nev++], fd, EVFILT_WRITE, EV_ADD | EV_DISABLE, 0, 0, NULL);
    }
    return kevent(loopfd, ev, nev, NULL, 0, NULL);
#endif
}

static inline void run_event_loop(int loopfd, struct server_sockets *ss, struct event_handlers *handlers) {
    struct event evlist[MAX_EVENTS];
    bool is_datagram = false;
    while (1) {
        int num = event_wait(loopfd, evlist, MAX_EVENTS, 1000);
        if (num < 0) {
            if (errno == EINTR) continue;
            log_sys_error(handlers, "event_wait", errno);
            break;
        }
        /* Periodic timer — runs even when num==0 (timeout with no events) */
        if (handlers && handlers->on_timer)
            handlers->on_timer(loopfd);
        for (int i = 0; i < num; ++i) {
            int fd = evlist[i].fd;
            bool is_listener_fd = false;
            for (int k = 0; k < ss->num_sockets; k++) {
                for (int j = 0; j < ss->sockets[k].num_fds; j++) {
                    if (ss->sockets[k].fds[j] == fd) {
                        is_listener_fd = true;
                        is_datagram = ss->sockets[k].is_datagram;
                        goto found_listener;
                    }
                }
            }
        found_listener:
            if (is_listener_fd) {
                if (is_datagram) {
                    my_on_read(loopfd, fd, handlers);
                } else {
                    my_on_accept(loopfd, fd, ss, handlers);
                }
            } else {
                int idx = get_conn(fd);
                if (idx == -1) {
                    /* Check for forward upstream fd first, then proxy upstream */
                    if (handlers && handlers->on_forward_event) {
                        handlers->on_forward_event(loopfd, fd, evlist[i].events);
                        /* forward_on_upstream_event returns silently if fd not in map */
                    }
                    if (handlers && handlers->on_upstream_event) {
                        handlers->on_upstream_event(loopfd, fd, evlist[i].events);
                    }
                    continue;
                }
                if (evlist[i].events & EV_READ) {
                    my_on_read(loopfd, fd, handlers);
                }
                if (evlist[i].events & EV_WRITE) {
                    my_on_write(loopfd, fd, handlers);
                }
            }
        }
    }
}

static inline void run_event_loop_old(int loopfd, struct server_sockets *ss, struct event_handlers *handlers) {
    struct event evlist[MAX_EVENTS];
	bool is_datagram = false;

    while (1) {
        int num = event_wait(loopfd, evlist, MAX_EVENTS, -1);
        if (num < 0) {
            log_sys_error(handlers, "event_wait", errno);
            break;
        }
        for (int i = 0; i < num; ++i) {
            int fd = evlist[i].fd;
            bool is_listener_fd = false;
            for (int k = 0; k < ss->num_sockets; k++) {
                for (int j = 0; j < ss->sockets[k].num_fds; j++) {
                    if (ss->sockets[k].fds[j] == fd) {
                        is_listener_fd = true;
                        is_datagram = ss->sockets[k].is_datagram;
                        goto found_listener;
                    }
                }
            }
        found_listener:
            if (is_listener_fd) {
                if (is_datagram) {
                    my_on_read(loopfd, fd, handlers);
                } else {
                    my_on_accept(loopfd, fd, ss, handlers);
                }
            } else {
                my_on_read(loopfd, fd, handlers);
            }
        }
    }
}

static inline struct socket_info create_socket_and_listen(const char *uri, int backlog) {
    struct socket_info si = {0};
    if (!uri) return si;

    char scheme[16] = {0};
    char host[256] = {0};
    char portstr[6] = {0};
    char path[108] = {0};
    bool is_local = false;
    bool is_datagram_local = false;
    bool is_ssl_local = false;
    bool is_http_local = false;
    bool explicit_host = false;

    StringBuf sb;
    stringbuf_init_str(&sb, uri, strlen(uri), 0);

    /* Try the modern scan first (handles IPv6 literals cleanly) */
    int n = stringbuf_sscanf(&sb, "%15[^:]://[%255[^]]]:%5[0-9]", scheme, host, portstr);
    if (n == 3) {
        explicit_host = true;   /* we saw the [ ] form */
    } else {
        /* Try without brackets (plain host or empty host) */
        n = stringbuf_sscanf(&sb, "%15[^:]://%255[^:]:%5[0-9]", scheme, host, portstr);
        if (n != 3) {
            /* empty host form: http://:8080 */
            n = stringbuf_sscanf(&sb, "%15[^:]://:%5[0-9]", scheme, portstr);
            host[0] = '\0';
            if (n != 2) {
                stringbuf_free(&sb);
                /* fall through to legacy unix/local handling below */
                goto legacy;
            }
        }
    }
    stringbuf_free(&sb);

    /* classify scheme */
    if (strcmp(scheme, "http") == 0) {
        is_datagram_local = false;
        is_ssl_local = false;
        is_http_local = true;
    } else if (strcmp(scheme, "https") == 0) {
        is_datagram_local = false;
        is_ssl_local = true;
        is_http_local = true;
    } else if (strcmp(scheme, "tcp") == 0) {
        is_datagram_local = false;
        is_ssl_local = false;
        is_http_local = false;
    } else if (strcmp(scheme, "ssl") == 0) {
        is_datagram_local = false;
        is_ssl_local = true;
        is_http_local = false;
    } else if (strcmp(scheme, "udp") == 0) {
        is_datagram_local = true;
        is_ssl_local = false;
        is_http_local = false;
    } else if (strcmp(scheme, "local") == 0 || strcmp(scheme, "unix") == 0) {
        is_local = true;
        is_datagram_local = false;
        is_ssl_local = false;
        is_http_local = false;
    } else if (strcmp(scheme, "unixgram") == 0) {
        is_local = true;
        is_datagram_local = true;
        is_ssl_local = false;
        is_http_local = false;
    } else {
        return si;
    }

legacy:
    /* minimal legacy path for unix sockets (already classified above) */
    if (is_local) {
        const char *after = strstr(uri, "://");
        if (after) {
            after += 3;
            strncpy(path, after, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }
    }

    int socktype = is_datagram_local ? SOCK_DGRAM : SOCK_STREAM;
    si.is_datagram = is_datagram_local;
    si.is_tls = is_ssl_local;
    si.is_http = is_http_local;
    si.num_fds = 0;

    if (is_local) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
        addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
        /* Ensure parent directory exists (create if needed) */
        {
            char dir[256];
            strncpy(dir, path, sizeof(dir));
            char *slash = strrchr(dir, '/');
            if (slash && slash != dir) {
                *slash = '\0';
                mkdir(dir, 0755); /* ignore error if exists */
            }
        }
        unlink(path);
        int local_s = socket(AF_UNIX, socktype, 0);
        if (local_s < 0) {
            log_sys_error(NULL, "socket unix", errno);
            LOG_ERROR(SERVER, "failed to create unix socket %s: %s", uri, strerror(errno));
            return si;
        }
        if (bind(local_s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
            log_sys_error(NULL, "bind unix", errno);
            LOG_ERROR(SERVER, "bind unix failed for %s: %s", uri, strerror(errno));
            close(local_s);
            return si;
        }
        if (!is_datagram_local) {
            if (listen(local_s, backlog) < 0) {
                log_sys_error(NULL, "listen unix", errno);
                LOG_ERROR(SERVER, "listen unix failed for %s: %s", uri, strerror(errno));
                close(local_s);
                return si;
            }
        }
        if (fcntl(local_s, F_SETFL, O_NONBLOCK) < 0) {
            log_sys_error(NULL, "fcntl listen", errno);
            close(local_s);
            return si;
        }
        si.fds[si.num_fds++] = local_s;
    } else {
        char *host_ptr = NULL;
		if (strlen(host) > 0) {
            host_ptr = host;
            // Strip brackets if present (for IPv6 literals like [::])
            if (host[0] == '[' && host[strlen(host)-1] == ']') {
                host[strlen(host)-1] = '\0';  // Temporarily null-terminate
                host_ptr = host + 1;         // Point inside brackets
            }
        }

        struct addrinfo hints = {0};
        hints.ai_flags = AI_PASSIVE;
        hints.ai_family = PF_UNSPEC;
        hints.ai_socktype = socktype;
        struct addrinfo *res = NULL;
        int e = getaddrinfo(host_ptr, portstr, &hints, &res);
        if (e != 0) {
            LOG_ERROR(SERVER, "getaddrinfo: %s", gai_strerror(e));
            return si;
        }
        int ipv4_fd = -1;
        int ipv6_fd = -1;
        LOG_INFO(SERVER, "listen: host='%s' port='%s' explicit_host=%d", host, portstr, explicit_host);
        for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
            LOG_DEBUG(SERVER, "addrinfo family=%d (AF_INET=%d AF_INET6=%d)", p->ai_family, AF_INET, AF_INET6);
            if (p->ai_family == AF_INET && ipv4_fd < 0) {
                int s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (s < 0) {
                    log_sys_error(NULL, "socket ip", errno);
                    continue;
                }
                int opt = 1;
                setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                if (bind(s, p->ai_addr, p->ai_addrlen) < 0) {
                    log_sys_error(NULL, "bind ip", errno);
                    if (!explicit_host) {
                        /* transient dual-stack collision; try next addrinfo */
                        close(s);
                        continue;
                    }
                    /* explicit host requested - real failure */
                    LOG_ERROR(SERVER, "bind failed for %s: %s", uri, strerror(errno));
                    close(s);
                    continue;
                }
                if (!is_datagram_local) {
                    if (listen(s, backlog) < 0) {
                        log_sys_error(NULL, "listen ip", errno);
                        close(s);
                        continue;
                    }
                }
                if (fcntl(s, F_SETFL, O_NONBLOCK) < 0) {
                    log_sys_error(NULL, "fcntl listen", errno);
                    close(s);
                    continue;
                }
                ipv4_fd = s;
            } else if (p->ai_family == AF_INET6 && ipv6_fd < 0) {
                int s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (s < 0) {
                    log_sys_error(NULL, "socket ip", errno);
                    continue;
                }
                int opt = 1;
                setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                if (bind(s, p->ai_addr, p->ai_addrlen) < 0) {
                    log_sys_error(NULL, "bind ip", errno);
                    if (!explicit_host) {
                        /* transient dual-stack collision; try next addrinfo */
                        close(s);
                        continue;
                    }
                    /* explicit host requested - real failure */
                    LOG_ERROR(SERVER, "bind failed for %s: %s", uri, strerror(errno));
                    close(s);
                    continue;
                }
                if (!is_datagram_local) {
                    if (listen(s, backlog) < 0) {
                        log_sys_error(NULL, "listen ip", errno);
                        close(s);
                        continue;
                    }
                }
                if (fcntl(s, F_SETFL, O_NONBLOCK) < 0) {
                    log_sys_error(NULL, "fcntl listen", errno);
                    close(s);
                    continue;
                }
                ipv6_fd = s;
            }
            /* For empty-host (dual-stack intent) stop after first success */
            if (!explicit_host && strlen(host) == 0 && (ipv4_fd >= 0 || ipv6_fd >= 0)) {
                break;
            }
        }
        freeaddrinfo(res);
        if (ipv4_fd >= 0) {
            si.fds[si.num_fds++] = ipv4_fd;
        }
        if (ipv6_fd >= 0) {
            si.fds[si.num_fds++] = ipv6_fd;
        }
        if (si.num_fds == 0) {
            return si;
        }
    }
    return si;
}

static inline void default_on_connect(int loopfd, int fd, struct client_info *info) {
    const char *type_str = "unknown";
    switch (info->type) {
        case CONN_TCPV4: type_str = "TCPv4"; break;
        case CONN_TCPV6: type_str = "TCPv6"; break;
        case CONN_SSLV4: type_str = "SSLv4"; break;
        case CONN_SSLV6: type_str = "SSLv6"; break;
        case CONN_UDPV4: type_str = "UDPv4"; break;
        case CONN_UDPV6: type_str = "UDPv6"; break;
        case CONN_LOCAL: type_str = "local"; break;
        default: type_str = "unknown"; break;
    }
    //printf("New %s connection from %s (%s:%d)\n", type_str, info->hostname, info->addr, info->port);
}

static inline void default_on_disconnect(int loopfd, int fd) {
	//printf("client #%d disconnected.\n", fd);
}

static inline void default_on_error(int loopfd, int fd, int err) {
    LOG_ERROR(CONN, "error on fd=%d err=%d", fd, err);
#ifdef HAVE_OPENSSL
    ERR_print_errors_fp(stderr);
#endif
}

static inline void socket_server_init_event_handlers(struct event_handlers *handlers) {
    handlers->on_connect = default_on_connect;
    handlers->on_disconnect = default_on_disconnect;
    handlers->on_error = default_on_error;
    handlers->on_access_log = NULL;
    handlers->on_error_log = NULL;
    handlers->on_http_request = NULL;
    handlers->on_upstream_event = NULL;
    handlers->on_body_data = NULL;
    handlers->on_client_writable = NULL;
    handlers->on_timer = NULL;
}

static inline int socket_server_add_sockets(struct server_sockets *ss, const char **uris, int num_uris, int default_backlog) {
    LOG_INFO(SERVER, "adding %d URIs (backlog=%d)", num_uris, default_backlog);
    ss->num_sockets = 0;
    for (int i = 0; i < num_uris; i++) {
        int backlog = default_backlog;
        const char *uri = uris[i];
        LOG_DEBUG(SERVER, "create_socket_and_listen(\"%s\")", uri);
        struct socket_info si = create_socket_and_listen(uri, backlog);
        if (si.num_fds == 0) {
            LOG_ERROR(SERVER, "failed to create socket for URI: %s", uri);
            return -1;
        }
        si.uri = strdup(uri); /* preserve original URI for dispatch matching */
        if (ss->num_sockets >= MAX_SOCKETS) {
            LOG_ERROR(SERVER, "too many sockets");
            return -1;
        }
        ss->sockets[ss->num_sockets++] = si;
    }
    if (ss->num_sockets == 0) {
        LOG_ERROR(SERVER, "no sockets specified");
        return -1;
    }
    return 0;
}

static inline int socket_server_setup_loop(int loopfd, struct server_sockets *ss) {
    for (int k = 0; k < ss->num_sockets; k++) {
        for (int j = 0; j < ss->sockets[k].num_fds; j++) {
            int ls = ss->sockets[k].fds[j];
            if (ls < 0) continue;
            if (event_add(loopfd, ls, EV_READ, NULL) < 0) {
                log_sys_error(NULL, "event_add listen", errno);
                return -1;
            }
            if (ss->sockets[k].is_datagram) {
                if (conn_add(loopfd, ls, true) < 0) {
                    LOG_ERROR(SERVER, "failed to add datagram socket");
                    return -1;
                }
                /* Attach socket_info so the datagram read path can find the handler */
                int di = get_conn(ls);
                if (di >= 0) {
                    clients[di].si = &ss->sockets[k];
                    clients[di].listen_uri = ss->sockets[k].uri ? strdup(ss->sockets[k].uri) : NULL;
                }
            }
        }
    }
    return 0;
}

#endif  // SOCKET_SERVER_H

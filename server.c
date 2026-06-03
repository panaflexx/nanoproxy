#define STRINGBUF_IMPLEMENTATION
#include "stringbuf.h"
#include <signal.h>
#include "socket_server.h"
#include "request.h"
#include <sys/resource.h>
#include <unistd.h>
#include "config.h"
#include "dns.h"

struct base_entry *base_handlers = NULL;
struct path_entry *global_http_handlers = NULL;
struct socket_handler_entry *socket_handler_map = NULL;
LocationEntry *locations = NULL;
int g_loopfd = -1;

/* Forward targets indexed by listener name */
typedef struct { char *key; struct forward_target value; } ForwardTargetEntry;
static ForwardTargetEntry *forward_target_map = NULL; /* listener_name -> forward_target */

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN); // Ignoring SIGPIPE
    np_log_init(); /* auto-detect color support */
    char *cert_file = NULL;
    char *key_file = NULL;
    int i = 1;
    int default_backlog = 1024; // Increased for better performance
    const char *uris[MAX_SOCKETS];
    int num_uris = 0;

    /* Optional config loading via -config <file> or default config.json */
    DictValue *config_root = NULL;
    ServerConfig sc = {0};
    const char *config_path = NULL;
    for (int j = 1; j < argc; j++) {
        if (strcmp(argv[j], "-config") == 0 && j+1 < argc) {
            config_path = argv[++j];
            break;
        }
    }
    if (!config_path) config_path = "config.json";
    if (access(config_path, R_OK) == 0) {
        if (load_config(config_path, &sc, &config_root)) {
            /* Apply config to CLI vars (CLI still wins for explicit overrides) */
            if (sc.tls_cert && !cert_file) cert_file = (char*)sc.tls_cert;
            if (sc.tls_key && !key_file) key_file = (char*)sc.tls_key;
            if (sc.default_backlog) default_backlog = sc.default_backlog;

            /* Apply logging config */
            if (sc.log_configured) {
                g_log_json = sc.log_json;
                for (int s = 0; s < LOG_SUB__COUNT; s++) {
                    if (sc.log_levels[s] > 0)
                        g_log_levels[s] = sc.log_levels[s] - 1; /* undo +1 from parser */
                }
                if (sc.access_log) {
                    if (np_access_log_open(sc.access_log) < 0) {
                        LOG_ERROR(CONFIG, "cannot open access log: %s", sc.access_log);
                    }
                }
            }

            /* Apply connection timeout overrides from config */
            if (sc.header_timeout > 0)         g_conn_timeouts.header_timeout = sc.header_timeout;
            if (sc.body_timeout > 0)           g_conn_timeouts.body_timeout = sc.body_timeout;
            if (sc.idle_timeout > 0)           g_conn_timeouts.idle_timeout = sc.idle_timeout;
            if (sc.max_connections_per_ip > 0)  g_conn_timeouts.max_connections_per_ip = sc.max_connections_per_ip;
            LOG_INFO(CONFIG, "timeouts: header=%ds body=%ds idle=%ds max_conn_per_ip=%d",
                    g_conn_timeouts.header_timeout, g_conn_timeouts.body_timeout,
                    g_conn_timeouts.idle_timeout, g_conn_timeouts.max_connections_per_ip);

            /* Register upstream pools for load-balanced proxying */
            for (int u = 0; u < sc.num_upstreams; u++) {
                UpstreamGroup *ug = &sc.upstreams[u];
                upstream_pool_register(ug->name, ug->targets, ug->num_targets,
                                       ug->balance, ug->health_path, ug->health_interval,
                                       ug->retry_timeout, ug->proxy_timeout);
                LOG_INFO(CONFIG, "registered upstream pool '%s' (%d targets, balance=%s)",
                        ug->name, ug->num_targets, ug->balance ? ug->balance : "round-robin");
            }

            /* Wire up directory serving from dispatch entries that specify "root" */
            for (int d = 0; d < sc.num_dispatch; d++) {
                DispatchEntry *de = &sc.dispatch[d];
                if (!de->raw || de->raw->type != DICT_OBJECT) continue;
                DictValue *rootv = dict_object_get(de->raw, "root");
                DictValue *proxyv = dict_object_get(de->raw, "proxy_pass");

                const char *raw_prefix = "/";
                DictValue *pathv = dict_object_get(de->raw, "path");
                if (pathv && pathv->type == DICT_STRING && pathv->string_value)
                    raw_prefix = pathv->string_value;

                /* Normalize common Caddy/nginx-style patterns: /api/** or /api/* -> /api/ */
                char prefix_buf[256];
                const char *prefix = raw_prefix;
                size_t rlen = strlen(raw_prefix);
                if (rlen >= 2 && rlen < sizeof(prefix_buf)-1) {
                    if (rlen >= 3 && strcmp(raw_prefix + rlen - 3, "/**") == 0) {
                        /* /api/** -> /api/ */
                        size_t plen = rlen - 2; /* drop the final '**' */
                        memcpy(prefix_buf, raw_prefix, plen);
                        prefix_buf[plen] = '\0';
                        if (plen == 0 || prefix_buf[plen-1] != '/') strcat(prefix_buf, "/");
                        prefix = prefix_buf;
                    } else if (strcmp(raw_prefix + rlen - 2, "/*") == 0) {
                        /* /api/* -> /api/ */
                        size_t plen = rlen - 1; /* drop the final '*' */
                        memcpy(prefix_buf, raw_prefix, plen);
                        prefix_buf[plen] = '\0';
                        if (plen == 0 || prefix_buf[plen-1] != '/') strcat(prefix_buf, "/");
                        prefix = prefix_buf;
                    }
                }

                /* DNS handler: wire dns_socket_handler to the listener's URI */
                if (strcmp(de->handler, "dns") == 0) {
                    if (sc.dns.num_entries == 0) {
                        LOG_WARN(DNS, "dns handler on '%s' but no \"dns\" section in config (ignored)", de->listener);
                    } else {
                        /* Safety: require an explicit bind address to avoid hijacking system DNS */
                        const char *dns_uri = NULL;
                        bool dns_explicit = false;
                        for (int li = 0; li < sc.num_listen; li++) {
                            if (sc.listens[li].name && strcmp(sc.listens[li].name, de->listener) == 0) {
                                dns_uri = sc.listens[li].uri;
                                break;
                            }
                        }
                        if (dns_uri) {
                            /* Check if URI has an explicit host (not udp://:port) */
                            const char *scheme_end = strstr(dns_uri, "://");
                            if (scheme_end) {
                                const char *after = scheme_end + 3;
                                /* Empty host: "://:<port>" or "://[::]:port" (all-interfaces) */
                                dns_explicit = (after[0] != ':' && after[0] != '\0');
                                /* Reject [::] and 0.0.0.0 as wildcards too */
                                if (dns_explicit && (strncmp(after, "[::]", 4) == 0 || strncmp(after, "0.0.0.0", 7) == 0))
                                    dns_explicit = false;
                            }
                        }
                        if (!dns_explicit) {
                            LOG_ERROR(DNS, "dns handler on '%s' (%s) binds to all interfaces — refusing to start. Bind to a specific IP, e.g. udp://127.0.0.1:5353", de->listener, dns_uri ? dns_uri : "(unknown)");
                            return EXIT_FAILURE;
                        }
                        g_dns_config = &sc.dns;
                        /* Register as a socket handler keyed by the listen URI */
                        struct socket_handler_entry she = {.key = strdup(dns_uri), .value = (socket_handler_t)dns_socket_handler};
                        shputs(socket_handler_map, she);
                        LOG_INFO(DNS, "handler on %s (%d entries)", dns_uri, sc.dns.num_entries);
                    }
                    continue;
                }

                if (de->forward_to) {
                    /* Raw port forwarding: parse target and store for accept-time dispatch */
                    struct forward_target ft = {0};
                    if (parse_forward_target(de->forward_to, &ft)) {
                        if (de->forward_mode && strcmp(de->forward_mode, "datagram") == 0)
                            ft.mode = FWD_MODE_DATAGRAM;
                        else if (de->forward_mode && strcmp(de->forward_mode, "stream") == 0)
                            ft.mode = FWD_MODE_STREAM;
                        if (de->forward_timeout > 0) ft.timeout = de->forward_timeout;
                        if (de->forward_max_conn > 0) ft.max_connections = de->forward_max_conn;
                        if (de->forward_buf_size > 0) ft.buffer_size = (size_t)de->forward_buf_size;
                        shput(forward_target_map, de->listener, ft);
                        LOG_INFO(FORWARD, "listener=%s -> %s", de->listener, de->forward_to);
                    } else {
                        LOG_ERROR(FORWARD, "bad forward_to URI: %s", de->forward_to);
                    }
                } else if (proxyv && proxyv->type == DICT_STRING && proxyv->string_value) {
                    /* Reverse proxy: store target URL in Location real_path */
                    LOG_INFO(PROXY, "path=%s -> proxy_pass=%s", prefix, proxyv->string_value);
                    char *heap_prefix = strdup(prefix);
                    addLocation(heap_prefix, proxyv->string_value, 1001, 1001);
                    addHandler(heap_prefix, proxy_handler);
                } else if (rootv && rootv->type == DICT_STRING && rootv->string_value) {
                    /* Static file serving */
                    const char *index = NULL;
                    DictValue *idxv = dict_object_get(de->raw, "index");
                    if (idxv && idxv->type == DICT_STRING && idxv->string_value)
                        index = idxv->string_value;

                    addLocation(prefix, rootv->string_value, 1001, 1001);
                    addHandler(prefix, http_static_dir);
                }
            }
            if (sc.num_dispatch == 0) {
                /* Fallback when no dispatch section is present */
                addLocation("/", "html", 1001, 1001);
                addHandler("/", http_static_dir);
            }
        }
    }

    while (i < argc) {
        if (strcmp(argv[i], "-config") == 0) {
            i += 2; /* already handled above, skip flag + value */
            continue;
        }
        if (strcmp(argv[i], "-cert") == 0) {
            i++;
            if (i < argc) cert_file = argv[i];
            i++;
            continue;
        }
        if (strcmp(argv[i], "-key") == 0) {
            i++;
            if (i < argc) key_file = argv[i];
            i++;
            continue;
        }
        if (strcmp(argv[i], "-c") == 0) {
            i++;
            if (i < argc) {
                default_backlog = atoi(argv[i]);
            }
            i++;
            continue;
        }
        /* -to target: forward the preceding listener URI to target.
         * Usage:  ./server tcp://:8080 -to otherhost:80
         * If no scheme on target, infer tcp:// (or udp:// if listener is udp). */
        if (strcmp(argv[i], "-to") == 0 || strcmp(argv[i], "-L") == 0) {
            i++;
            if (i >= argc) {
                LOG_ERROR(SERVER, "%s requires a target argument", argv[i-1]);
                return EXIT_FAILURE;
            }
            if (num_uris == 0) {
                LOG_ERROR(SERVER, "%s must follow a listen URI", argv[i-1]);
                return EXIT_FAILURE;
            }
            const char *listen_uri = uris[num_uris - 1];
            const char *raw_target = argv[i];
            /* Build a full target URI if no scheme was given */
            char target_buf[512];
            if (strstr(raw_target, "://")) {
                /* Already has scheme */
                snprintf(target_buf, sizeof(target_buf), "%s", raw_target);
            } else {
                /* Infer scheme from the listener */
                const char *scheme = "tcp";
                if (strncmp(listen_uri, "udp://", 6) == 0) scheme = "udp";
                snprintf(target_buf, sizeof(target_buf), "%s://%s", scheme, raw_target);
            }
            struct forward_target ft = {0};
            if (parse_forward_target(target_buf, &ft)) {
                /* Key by listen URI so the socket-matching loop can find it */
                shput(forward_target_map, listen_uri, ft);
                LOG_INFO(FORWARD, "%s -> %s", listen_uri, target_buf);
            } else {
                LOG_ERROR(FORWARD, "bad -L target: %s", target_buf);
                return EXIT_FAILURE;
            }
            i++;
            continue;
        }
        if (num_uris >= MAX_SOCKETS) {
            LOG_ERROR(SERVER, "too many URIs");
            return EXIT_FAILURE;
        }
        if (strlen(argv[i]) > MAX_URL_SIZE) {
            LOG_WARN(SERVER, "URI too long: %s", argv[i]);
            i++;
            continue;
        }
        uris[num_uris++] = argv[i];
        i++;
    }

    /* If no CLI URIs, fall back to config */
    if (num_uris == 0 && sc.num_listen > 0) {
        for (int k = 0; k < sc.num_listen && num_uris < MAX_SOCKETS; k++) {
            const char *u = sc.listens[k].uri;
            if (u && strlen(u) < MAX_URL_SIZE) {
                uris[num_uris++] = u;
            }
        }
    }

    if (cert_file && !key_file) {
        LOG_ERROR(SERVER, "-key required with -cert");
        return EXIT_FAILURE;
    }
    if (!cert_file && key_file) {
        LOG_ERROR(SERVER, "-cert required with -key");
        return EXIT_FAILURE;
    }

    struct server_sockets ss = {0};
    if (socket_server_add_sockets(&ss, uris, num_uris, default_backlog) < 0) {
        return EXIT_FAILURE;
    }

    for (int k = 0; k < ss.num_sockets; k++) {
        ptrdiff_t idx = shgeti(socket_handler_map, ss.sockets[k].uri);
        if (idx >= 0) {
            ss.sockets[k].socket_handler = socket_handler_map[idx].value;
        } else {
            ss.sockets[k].socket_handler = default_socket_handler;
        }
        ss.sockets[k].fwd_target = NULL;
    }

    /* Attach forward targets to their listener sockets.
     * Look up by listener name (config file) then by URI (CLI -L). */
    for (int k = 0; k < ss.num_sockets; k++) {
        if (!ss.sockets[k].uri) continue;
        ptrdiff_t fidx = -1;

        /* Try by listener name first (config file dispatch) */
        for (int li = 0; li < sc.num_listen; li++) {
            if (sc.listens[li].uri && sc.listens[li].name &&
                strcmp(sc.listens[li].uri, ss.sockets[k].uri) == 0) {
                fidx = shgeti(forward_target_map, sc.listens[li].name);
                break;
            }
        }
        /* Fall back to lookup by URI directly (CLI -L) */
        if (fidx < 0) {
            fidx = shgeti(forward_target_map, ss.sockets[k].uri);
        }
        if (fidx >= 0) {
            ss.sockets[k].fwd_target = &forward_target_map[fidx].value;
            LOG_INFO(FORWARD, "%s -> %s:%d",
                    ss.sockets[k].uri,
                    forward_target_map[fidx].value.host,
                    forward_target_map[fidx].value.port);
        }
    }

    bool needs_tls = false;
    for (int k = 0; k < ss.num_sockets; k++) {
        if (ss.sockets[k].is_tls) {
            needs_tls = true;
            break;
        }
    }
    if (needs_tls && !(cert_file && key_file)) {
        LOG_ERROR(SERVER, "-cert and -key required for ssl:// URIs");
        return EXIT_FAILURE;
    }

#ifdef HAVE_OPENSSL
    if (cert_file && key_file) {
        socket_server_init_tls(cert_file, key_file);
    }
#endif

    socket_server_init_hash();

	// Increase open file descriptor limit to handle more connections
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max = 10000;  // Set to a high value (adjust as needed)
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            LOG_WARN(SERVER, "failed to set rlimit: %s", strerror(errno));
        }
    } else {
        LOG_WARN(SERVER, "failed to get rlimit: %s", strerror(errno));
    }

    int loopfd = event_create();
    proxy_set_loopfd(loopfd);
    if (loopfd < 0) {
        log_sys_error(NULL, "event_create", errno);
        for (int k = 0; k < ss.num_sockets; k++) {
            for (int j = 0; j < ss.sockets[k].num_fds; j++) {
                close(ss.sockets[k].fds[j]);
            }
        }
        return EXIT_FAILURE;
    }

    if (socket_server_setup_loop(loopfd, &ss) < 0) {
        close(loopfd);
        for (int k = 0; k < ss.num_sockets; k++) {
            for (int j = 0; j < ss.sockets[k].num_fds; j++) {
                close(ss.sockets[k].fds[j]);
            }
        }
        return EXIT_FAILURE;
    }

    struct event_handlers handlers;
    socket_server_init_event_handlers(&handlers);
    handlers.on_access_log = default_access_log;
    handlers.on_error_log = default_error_log;
    handlers.on_http_request = http_dispatcher;
    handlers.on_write = resume_send;
    handlers.on_upstream_event = proxy_on_upstream_event;
    handlers.on_body_data = proxy_on_body_data;
    handlers.on_client_writable = proxy_on_client_writable;
    handlers.on_forward_accept = forward_on_accept;
    handlers.on_forward_event = forward_on_upstream_event;
    handlers.on_forward_data = forward_on_downstream_data;
    handlers.on_forward_disconnect = forward_cleanup;
    handlers.on_timer = server_timeout_sweep;

    /* Default demo handlers when nothing was configured via dispatch */
    if (sc.num_dispatch == 0) {
        addLocation("/", "html", 1001, 1001);
        addHandler("/", http_static_dir);
        addHandler("/hello", hello_http_handler);
    }

    /* Always-on startup banner — one line, shows what's live */
    {
        char listen_summary[1024] = {0};
        size_t pos = 0;
        for (int k = 0; k < ss.num_sockets && pos < sizeof(listen_summary) - 1; k++) {
            if (k > 0) pos += snprintf(listen_summary + pos, sizeof(listen_summary) - pos, ", ");
            pos += snprintf(listen_summary + pos, sizeof(listen_summary) - pos, "%s",
                            ss.sockets[k].uri ? ss.sockets[k].uri : "?");
        }
        if (g_log_json) {
            struct timespec _ts; clock_gettime(CLOCK_REALTIME, &_ts);
            struct tm _tm; localtime_r(&_ts.tv_sec, &_tm);
            char _tb[32]; strftime(_tb, sizeof(_tb), "%Y-%m-%dT%H:%M:%S", &_tm);
            fprintf(stderr, "{\"ts\":\"%s.%03ldZ\",\"level\":\"INFO\",\"sub\":\"server\","
                    "\"msg\":\"npserver ready \u2014 %s\"}\n",
                    _tb, _ts.tv_nsec / 1000000, listen_summary);
        } else if (g_log_color) {
            fprintf(stderr, "\n  \033[1;36m⚡ npserver\033[0m \033[2mready\033[0m — %s\n\n", listen_summary);
        } else {
            fprintf(stderr, "\n  npserver ready — %s\n\n", listen_summary);
        }
    }

    run_event_loop(loopfd, &ss, &handlers);

    hmfree(fd_to_index);
    return EXIT_SUCCESS;
}

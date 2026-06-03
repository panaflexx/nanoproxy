#ifndef CONFIG_H
#define CONFIG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "dict.h"
#include "logger.h"

#ifndef MAX_CONFIG_LISTEN
#define MAX_CONFIG_LISTEN 32
#endif
#ifndef MAX_CONFIG_DISPATCH
#define MAX_CONFIG_DISPATCH 32
#endif
#ifndef MAX_CONFIG_UPSTREAMS
#define MAX_CONFIG_UPSTREAMS 16
#endif
#ifndef MAX_UPSTREAM_TARGETS
#define MAX_UPSTREAM_TARGETS 16
#endif
#ifndef MAX_DNS_ENTRIES
#define MAX_DNS_ENTRIES 256
#endif
#ifndef MAX_DNS_RECORDS
#define MAX_DNS_RECORDS 16          /* per name */
#endif

/* ── DNS record types and name→records map ───────────────────────────── */

enum dns_rtype {
    DNS_A      = 1,
    DNS_NS     = 2,
    DNS_CNAME  = 5,
    DNS_SOA    = 6,
    DNS_PTR    = 12,
    DNS_MX     = 15,
    DNS_TXT    = 16,
    DNS_AAAA   = 28,
    DNS_SRV    = 33
};

typedef struct {
    enum dns_rtype type;
    uint32_t ttl;
    union {
        struct in_addr   a;                     /* A */
        struct in6_addr  aaaa;                  /* AAAA */
        char             cname[256];            /* CNAME */
        char             ns[256];               /* NS */
        char             ptr[256];              /* PTR */
        char             txt[512];              /* TXT */
        struct { uint16_t priority; char host[256]; }       mx;   /* MX */
        struct { uint16_t priority; uint16_t weight;
                 uint16_t port;     char target[256]; }     srv;  /* SRV */
    };
} DnsRecord;

typedef struct {
    char       name[256];                       /* domain name (lower-cased) */
    DnsRecord  records[MAX_DNS_RECORDS];
    int        num_records;
} DnsEntry;

typedef struct {
    DnsEntry     entries[MAX_DNS_ENTRIES];
    int          num_entries;
    uint32_t     default_ttl;                   /* default TTL for all records */
} DnsConfig;

/* Named listener entry (supports both "uri" and {name, uri} forms) */
typedef struct {
    const char *name;   /* optional, for later attach/dispatch by name */
    const char *uri;
} ListenEntry;

/* Dispatch rule: attach a handler name to a listener by listener name.
 * Extra keys in the dispatch object are available via the raw DictValue.
 */
typedef struct {
    const char *listener;  /* listener name */
    const char *handler;   /* handler name: "static", "proxy", "forward", "dns" */
    DictValue *raw;        /* full dispatch object for extra options */
    /* Parsed forward fields (populated when handler == "forward") */
    const char *forward_to;     /* raw URI: "tcp://host:port", "udp://...", "unix://..." */
    const char *forward_mode;   /* "stream", "datagram", or NULL for auto */
    int forward_timeout;        /* idle timeout seconds */
    int forward_max_conn;       /* max concurrent forwarded connections */
    int forward_buf_size;       /* relay buffer size */
    /* Parsed proxy fields (populated when handler == "proxy") */
    const char *proxy_pass;     /* single target or "upstream://name" */
    const char *balance;        /* "round-robin", "random", "ip-hash" */
    bool strip_prefix;          /* remove matched path prefix before forwarding */
    int proxy_timeout;          /* connection + idle timeout */
} DispatchEntry;

/* Named upstream group for load-balanced proxying */
typedef struct {
    const char *name;
    const char *targets[MAX_UPSTREAM_TARGETS];
    int num_targets;
    const char *balance;        /* "round-robin", "random", "ip-hash" */
    const char *health_path;    /* e.g. "/healthz" */
    int health_interval;        /* seconds between checks */
    int retry_timeout;          /* seconds before retrying a dead target (default 30) */
    int proxy_timeout;          /* seconds to wait for upstream connect (default 5) */
} UpstreamGroup;

/* Simplified Caddy-like config structure */
typedef struct {
    ListenEntry listens[MAX_CONFIG_LISTEN];
    int num_listen;
    DispatchEntry dispatch[MAX_CONFIG_DISPATCH];
    int num_dispatch;
    UpstreamGroup upstreams[MAX_CONFIG_UPSTREAMS];
    int num_upstreams;
    DnsConfig dns;
    const char *tls_cert;
    const char *tls_key;
    int default_backlog;
    /* Connection timeout / anti-abuse settings (0 = use built-in defaults) */
    int header_timeout;
    int body_timeout;
    int idle_timeout;
    int max_connections_per_ip;
    /* Logging: per-subsystem levels + format (0 = use built-in defaults) */
    bool log_json;
    bool log_configured;  /* true if "logging" section was present */
    int  log_levels[LOG_SUB__COUNT];  /* indexed by enum np_log_subsystem */
    const char *access_log;  /* "stdout" (default), "stderr", "off", or file path */
} ServerConfig;

/* ── DNS config validation helpers ────────────────────────────────────── */

/* Validate a DNS hostname/domain name (RFC 1123 / RFC 952).
 * Labels: 1–63 chars each, [a-z0-9-] (hyphen not at start/end), _ allowed for SRV.
 * Total length ≤ 253 characters. Returns 1 if valid, 0 if not. */
static inline int dns_valid_hostname(const char *name) {
    if (!name || !name[0]) return 0;
    size_t total = strlen(name);
    if (total > 253) return 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t label_len = dot ? (size_t)(dot - p) : strlen(p);
        if (label_len == 0 || label_len > 63) return 0;
        if (p[0] == '-') return 0;                         /* no leading hyphen */
        if (p[label_len - 1] == '-') return 0;             /* no trailing hyphen */
        for (size_t i = 0; i < label_len; i++) {
            char c = p[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_'))
                return 0;
        }
        p += label_len;
        if (*p == '.') p++;
    }
    return 1;
}

/* Validate an IPv4 address string. Returns 1 if valid. */
static inline int dns_valid_ipv4(const char *s) {
    struct in_addr tmp;
    return (s && inet_pton(AF_INET, s, &tmp) == 1) ? 1 : 0;
}

/* Validate an IPv6 address string. Returns 1 if valid. */
static inline int dns_valid_ipv6(const char *s) {
    struct in6_addr tmp;
    return (s && inet_pton(AF_INET6, s, &tmp) == 1) ? 1 : 0;
}

/* Parse a JSON config file into ServerConfig.
 * Returns 1 on success, 0 on failure (error printed to stderr).
 * The returned config pointers are valid until dict_destroy(root) is called.
 */
static int load_config(const char *path, ServerConfig *cfg, DictValue **root_out) {
    if (!path || !cfg) return 0;
    memset(cfg, 0, sizeof(*cfg));
    cfg->default_backlog = 1024;

    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_ERROR(CONFIG, "cannot open %s", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 1<<20) {
        fclose(f);
        LOG_ERROR(CONFIG, "invalid file size: %s", path);
        return 0;
    }
    char *buf = (char*)malloc(fsize + 1);
    if (!buf) { fclose(f); return 0; }
    size_t n = fread(buf, 1, fsize, f);
    fclose(f);
    buf[n] = '\0';

    char err[2048] = {0};
    DictValue *root = dict_deserialize_json(buf, n+1, n, err, sizeof(err));
    free(buf);
    if (!root) {
        LOG_ERROR(CONFIG, "parse error: %s", err);
        return 0;
    }

    if (root->type != DICT_OBJECT) {
        LOG_ERROR(CONFIG, "root must be JSON object");
        dict_destroy(root);
        return 0;
    }

    /* listen: array of strings or {name, uri} objects */
    DictValue *listen = dict_object_get(root, "listen");
    if (listen && listen->type == DICT_ARRAY) {
        for (size_t i = 0; i < listen->array_value.length && cfg->num_listen < MAX_CONFIG_LISTEN; i++) {
            DictValue *v = listen->array_value.items[i];
            if (!v) continue;
            if (v->type == DICT_STRING && v->string_value) {
                cfg->listens[cfg->num_listen].name = NULL;
                cfg->listens[cfg->num_listen].uri = v->string_value;
                cfg->num_listen++;
            } else if (v->type == DICT_OBJECT) {
                DictValue *namev = dict_object_get(v, "name");
                DictValue *uriv  = dict_object_get(v, "uri");
                const char *uri = (uriv && uriv->type == DICT_STRING) ? uriv->string_value : NULL;
                if (uri) {
                    cfg->listens[cfg->num_listen].name = (namev && namev->type == DICT_STRING) ? namev->string_value : NULL;
                    cfg->listens[cfg->num_listen].uri = uri;
                    cfg->num_listen++;
                }
            }
        }
    }

    /* tls: { cert, key } */
    DictValue *tls = dict_object_get(root, "tls");
    if (tls && tls->type == DICT_OBJECT) {
        DictValue *c = dict_object_get(tls, "cert");
        DictValue *k = dict_object_get(tls, "key");
        if (c && c->type == DICT_STRING) cfg->tls_cert = c->string_value;
        if (k && k->type == DICT_STRING) cfg->tls_key = k->string_value;
    }

    /* backlog (optional) */
    DictValue *bl = dict_object_get(root, "backlog");
    if (bl && bl->type == DICT_INT64) {
        cfg->default_backlog = (int)bl->int64_value;
    } else if (bl && bl->type == DICT_NUMBER) {
        cfg->default_backlog = (int)bl->number_value;
    }

        /* logging: per-subsystem log levels and output format */
        DictValue *logging = dict_object_get(root, "logging");
        if (logging && logging->type == DICT_OBJECT) {
            cfg->log_configured = true;
            DictValue *fmtv = dict_object_get(logging, "format");
            if (fmtv && fmtv->type == DICT_STRING && strcasecmp(fmtv->string_value, "json") == 0)
                cfg->log_json = true;
            DictValue *alv = dict_object_get(logging, "access_log");
            if (alv && alv->type == DICT_STRING)
                cfg->access_log = alv->string_value;
            /* Parse per-subsystem levels: "config": "debug", "proxy": "warn", etc. */
            for (size_t li = 0; li < logging->object_value.count; li++) {
                const char *key = logging->object_value.pairs[li].key;
                DictValue *val = logging->object_value.pairs[li].value;
                if (!key || !val) continue;
                if (strcmp(key, "format") == 0) continue; /* already handled */
                if (val->type != DICT_STRING) continue;
                int sub = np_parse_log_subsystem(key);
                int lvl = np_parse_log_level(val->string_value);
                if (sub >= 0 && lvl >= 0) {
                    cfg->log_levels[sub] = lvl + 1; /* +1 so 0 means "not set" */
                }
            }
            /* Apply immediately so remaining config-parse logs use the right format */
            g_log_json = cfg->log_json;
            for (int _s = 0; _s < LOG_SUB__COUNT; _s++) {
                if (cfg->log_levels[_s] > 0)
                    g_log_levels[_s] = cfg->log_levels[_s] - 1;
            }
        }

    /* timeouts: connection anti-abuse settings */
    DictValue *timeouts = dict_object_get(root, "timeouts");
    if (timeouts && timeouts->type == DICT_OBJECT) {
        DictValue *ht = dict_object_get(timeouts, "header");
        if (ht && ht->type == DICT_INT64) cfg->header_timeout = (int)ht->int64_value;
        else if (ht && ht->type == DICT_NUMBER) cfg->header_timeout = (int)ht->number_value;
        DictValue *bt = dict_object_get(timeouts, "body");
        if (bt && bt->type == DICT_INT64) cfg->body_timeout = (int)bt->int64_value;
        else if (bt && bt->type == DICT_NUMBER) cfg->body_timeout = (int)bt->number_value;
        DictValue *it = dict_object_get(timeouts, "idle");
        if (it && it->type == DICT_INT64) cfg->idle_timeout = (int)it->int64_value;
        else if (it && it->type == DICT_NUMBER) cfg->idle_timeout = (int)it->number_value;
        DictValue *mc = dict_object_get(timeouts, "max_connections_per_ip");
        if (mc && mc->type == DICT_INT64) cfg->max_connections_per_ip = (int)mc->int64_value;
        else if (mc && mc->type == DICT_NUMBER) cfg->max_connections_per_ip = (int)mc->number_value;
        LOG_INFO(CONFIG, "timeouts: header=%d body=%d idle=%d max_conn_per_ip=%d",
                cfg->header_timeout, cfg->body_timeout,
                cfg->idle_timeout, cfg->max_connections_per_ip);
    }

    /* upstreams: { "name": { targets: [...], balance: "..." } } */
    DictValue *ups = dict_object_get(root, "upstreams");
    if (ups && ups->type == DICT_OBJECT) {
        for (size_t i = 0; i < ups->object_value.count && cfg->num_upstreams < MAX_CONFIG_UPSTREAMS; i++) {
            const char *uname = ups->object_value.pairs[i].key;
            DictValue *entry = ups->object_value.pairs[i].value;
            if (!entry || entry->type != DICT_OBJECT || !uname) continue;
            UpstreamGroup *ug = &cfg->upstreams[cfg->num_upstreams];
            memset(ug, 0, sizeof(*ug));
            ug->name = uname;
            DictValue *tgts = dict_object_get(entry, "targets");
            if (tgts && tgts->type == DICT_ARRAY) {
                for (size_t t = 0; t < tgts->array_value.length && ug->num_targets < MAX_UPSTREAM_TARGETS; t++) {
                    DictValue *tv = tgts->array_value.items[t];
                    if (tv && tv->type == DICT_STRING && tv->string_value)
                        ug->targets[ug->num_targets++] = tv->string_value;
                }
            }
            DictValue *balv = dict_object_get(entry, "balance");
            if (balv && balv->type == DICT_STRING) ug->balance = balv->string_value;
            DictValue *hcv = dict_object_get(entry, "health_check");
            if (hcv && hcv->type == DICT_OBJECT) {
                DictValue *hp = dict_object_get(hcv, "path");
                if (hp && hp->type == DICT_STRING) ug->health_path = hp->string_value;
                DictValue *hi = dict_object_get(hcv, "interval");
                if (hi && hi->type == DICT_INT64) ug->health_interval = (int)hi->int64_value;
                else if (hi && hi->type == DICT_NUMBER) ug->health_interval = (int)hi->number_value;
            }
            DictValue *rtv = dict_object_get(entry, "retry_timeout_seconds");
            if (rtv && rtv->type == DICT_INT64) ug->retry_timeout = (int)rtv->int64_value;
            else if (rtv && rtv->type == DICT_NUMBER) ug->retry_timeout = (int)rtv->number_value;
            DictValue *ptv = dict_object_get(entry, "proxy_timeout_seconds");
            if (ptv && ptv->type == DICT_INT64) ug->proxy_timeout = (int)ptv->int64_value;
            else if (ptv && ptv->type == DICT_NUMBER) ug->proxy_timeout = (int)ptv->number_value;
            cfg->num_upstreams++;
            LOG_INFO(CONFIG, "upstream '%s' with %d targets (balance=%s)",
                    ug->name, ug->num_targets, ug->balance ? ug->balance : "round-robin");
        }
    }

    /* dns: { "name": { "A": "...", "AAAA": "...", ... } }
     * Each value can be a string, an array of strings, or an object
     * (for MX/SRV with structured fields). */
    cfg->dns.default_ttl = 300;
    DictValue *dns_ttl_val = dict_object_get(root, "dns_ttl");
    if (dns_ttl_val && dns_ttl_val->type == DICT_INT64)
        cfg->dns.default_ttl = (uint32_t)dns_ttl_val->int64_value;
    else if (dns_ttl_val && dns_ttl_val->type == DICT_NUMBER)
        cfg->dns.default_ttl = (uint32_t)dns_ttl_val->number_value;

    DictValue *dns_section = dict_object_get(root, "dns");
    if (dns_section && dns_section->type == DICT_OBJECT) {
        uint32_t dttl = cfg->dns.default_ttl;
        for (size_t zi = 0; zi < dns_section->object_value.count && cfg->dns.num_entries < MAX_DNS_ENTRIES; zi++) {
            const char *raw_name = dns_section->object_value.pairs[zi].key;
            DictValue  *rr_obj   = dns_section->object_value.pairs[zi].value;
            if (!raw_name || !rr_obj || rr_obj->type != DICT_OBJECT) continue;

            /* Validate the domain name key */
            if (!dns_valid_hostname(raw_name)) {
                LOG_WARN(CONFIG, "dns key '%s': invalid hostname (skipped)", raw_name);
                continue;
            }

            DnsEntry *ze = &cfg->dns.entries[cfg->dns.num_entries];
            memset(ze, 0, sizeof(*ze));
            /* Lower-case the name for case-insensitive lookup */
            size_t nlen = strlen(raw_name);
            if (nlen >= sizeof(ze->name)) nlen = sizeof(ze->name) - 1;
            for (size_t c = 0; c < nlen; c++)
                ze->name[c] = (raw_name[c] >= 'A' && raw_name[c] <= 'Z')
                             ? raw_name[c] + 32 : raw_name[c];
            ze->name[nlen] = '\0';

            /* Per-entry TTL override */
            uint32_t ttl = dttl;
            DictValue *ttlv = dict_object_get(rr_obj, "TTL");
            if (!ttlv) ttlv = dict_object_get(rr_obj, "ttl");
            if (ttlv && ttlv->type == DICT_INT64) ttl = (uint32_t)ttlv->int64_value;
            else if (ttlv && ttlv->type == DICT_NUMBER) ttl = (uint32_t)ttlv->number_value;

            /* Helper macros for record parsing + validation */
            #define DNS_ADD_REC(TYPE) \
                if (ze->num_records < MAX_DNS_RECORDS) { \
                    DnsRecord *r = &ze->records[ze->num_records]; \
                    memset(r, 0, sizeof(*r)); \
                    r->type = (TYPE); r->ttl = ttl;
            #define DNS_END_REC ze->num_records++; }
            #define DNS_SKIP_REC } /* close DNS_ADD_REC without incrementing */
            #define DNS_WARN(rtype, val, reason) \
                LOG_WARN(DNS, "'%s' %s '%s': %s (skipped)", \
                        raw_name, (rtype), (val), (reason))

            /* ── A records ── */
            DictValue *av = dict_object_get(rr_obj, "A");
            if (av && av->type == DICT_STRING && av->string_value) {
                if (!dns_valid_ipv4(av->string_value)) { DNS_WARN("A", av->string_value, "invalid IPv4 address"); }
                else { DNS_ADD_REC(DNS_A) inet_pton(AF_INET, av->string_value, &r->a); DNS_END_REC }
            } else if (av && av->type == DICT_ARRAY) {
                for (size_t j = 0; j < av->array_value.length; j++) {
                    DictValue *item = av->array_value.items[j];
                    if (item && item->type == DICT_STRING && item->string_value) {
                        if (!dns_valid_ipv4(item->string_value)) { DNS_WARN("A", item->string_value, "invalid IPv4 address"); }
                        else { DNS_ADD_REC(DNS_A) inet_pton(AF_INET, item->string_value, &r->a); DNS_END_REC }
                    }
                }
            }

            /* ── AAAA records ── */
            DictValue *aaav = dict_object_get(rr_obj, "AAAA");
            if (aaav && aaav->type == DICT_STRING && aaav->string_value) {
                if (!dns_valid_ipv6(aaav->string_value)) { DNS_WARN("AAAA", aaav->string_value, "invalid IPv6 address"); }
                else { DNS_ADD_REC(DNS_AAAA) inet_pton(AF_INET6, aaav->string_value, &r->aaaa); DNS_END_REC }
            } else if (aaav && aaav->type == DICT_ARRAY) {
                for (size_t j = 0; j < aaav->array_value.length; j++) {
                    DictValue *item = aaav->array_value.items[j];
                    if (item && item->type == DICT_STRING && item->string_value) {
                        if (!dns_valid_ipv6(item->string_value)) { DNS_WARN("AAAA", item->string_value, "invalid IPv6 address"); }
                        else { DNS_ADD_REC(DNS_AAAA) inet_pton(AF_INET6, item->string_value, &r->aaaa); DNS_END_REC }
                    }
                }
            }

            /* ── CNAME ── */
            DictValue *cnv = dict_object_get(rr_obj, "CNAME");
            if (cnv && cnv->type == DICT_STRING && cnv->string_value) {
                if (!dns_valid_hostname(cnv->string_value)) { DNS_WARN("CNAME", cnv->string_value, "invalid hostname"); }
                else { DNS_ADD_REC(DNS_CNAME) strncpy(r->cname, cnv->string_value, sizeof(r->cname) - 1); DNS_END_REC }
            }

            /* ── NS ── */
            DictValue *nsv = dict_object_get(rr_obj, "NS");
            if (nsv && nsv->type == DICT_STRING && nsv->string_value) {
                if (!dns_valid_hostname(nsv->string_value)) { DNS_WARN("NS", nsv->string_value, "invalid hostname"); }
                else { DNS_ADD_REC(DNS_NS) strncpy(r->ns, nsv->string_value, sizeof(r->ns) - 1); DNS_END_REC }
            } else if (nsv && nsv->type == DICT_ARRAY) {
                for (size_t j = 0; j < nsv->array_value.length; j++) {
                    DictValue *item = nsv->array_value.items[j];
                    if (item && item->type == DICT_STRING && item->string_value) {
                        if (!dns_valid_hostname(item->string_value)) { DNS_WARN("NS", item->string_value, "invalid hostname"); }
                        else { DNS_ADD_REC(DNS_NS) strncpy(r->ns, item->string_value, sizeof(r->ns) - 1); DNS_END_REC }
                    }
                }
            }

            /* ── PTR ── */
            DictValue *ptrv = dict_object_get(rr_obj, "PTR");
            if (ptrv && ptrv->type == DICT_STRING && ptrv->string_value) {
                if (!dns_valid_hostname(ptrv->string_value)) { DNS_WARN("PTR", ptrv->string_value, "invalid hostname"); }
                else { DNS_ADD_REC(DNS_PTR) strncpy(r->ptr, ptrv->string_value, sizeof(r->ptr) - 1); DNS_END_REC }
            }

            /* ── TXT (string or array of strings) ── */
            DictValue *txtv = dict_object_get(rr_obj, "TXT");
            if (txtv && txtv->type == DICT_STRING && txtv->string_value) {
                if (strlen(txtv->string_value) == 0) { DNS_WARN("TXT", "", "empty string"); }
                else {
                    if (strlen(txtv->string_value) > 255)
                        LOG_WARN(DNS, "'%s' TXT length %zu > 255, will be split in response", ze->name, strlen(txtv->string_value));
                    DNS_ADD_REC(DNS_TXT) strncpy(r->txt, txtv->string_value, sizeof(r->txt) - 1); DNS_END_REC
                }
            } else if (txtv && txtv->type == DICT_ARRAY) {
                for (size_t j = 0; j < txtv->array_value.length; j++) {
                    DictValue *item = txtv->array_value.items[j];
                    if (item && item->type == DICT_STRING && item->string_value) {
                        if (strlen(item->string_value) == 0) { DNS_WARN("TXT", "", "empty string"); }
                        else {
                            if (strlen(item->string_value) > 255)
                                LOG_WARN(DNS, "'%s' TXT length %zu > 255, will be split in response", ze->name, strlen(item->string_value));
                            DNS_ADD_REC(DNS_TXT) strncpy(r->txt, item->string_value, sizeof(r->txt) - 1); DNS_END_REC
                        }
                    }
                }
            }

            /* ── MX (object or array of objects: {priority, host}) ── */
            DictValue *mxv = dict_object_get(rr_obj, "MX");
            if (mxv && mxv->type == DICT_OBJECT) {
                DictValue *mp = dict_object_get(mxv, "priority");
                DictValue *mh = dict_object_get(mxv, "host");
                if (mh && mh->type == DICT_STRING) {
                    if (!dns_valid_hostname(mh->string_value)) { DNS_WARN("MX", mh->string_value, "invalid hostname"); }
                    else {
                        DNS_ADD_REC(DNS_MX)
                        r->mx.priority = (mp && mp->type == DICT_INT64) ? (uint16_t)mp->int64_value
                                       : (mp && mp->type == DICT_NUMBER) ? (uint16_t)mp->number_value : 10;
                        strncpy(r->mx.host, mh->string_value, sizeof(r->mx.host) - 1);
                        DNS_END_REC
                    }
                }
            } else if (mxv && mxv->type == DICT_ARRAY) {
                for (size_t j = 0; j < mxv->array_value.length; j++) {
                    DictValue *item = mxv->array_value.items[j];
                    if (!item || item->type != DICT_OBJECT) continue;
                    DictValue *mp = dict_object_get(item, "priority");
                    DictValue *mh = dict_object_get(item, "host");
                    if (mh && mh->type == DICT_STRING) {
                        if (!dns_valid_hostname(mh->string_value)) { DNS_WARN("MX", mh->string_value, "invalid hostname"); }
                        else {
                            DNS_ADD_REC(DNS_MX)
                            r->mx.priority = (mp && mp->type == DICT_INT64) ? (uint16_t)mp->int64_value
                                           : (mp && mp->type == DICT_NUMBER) ? (uint16_t)mp->number_value : 10;
                            strncpy(r->mx.host, mh->string_value, sizeof(r->mx.host) - 1);
                            DNS_END_REC
                        }
                    }
                }
            }

            /* ── SRV (object or array: {priority, weight, port, target}) ── */
            DictValue *srvv = dict_object_get(rr_obj, "SRV");
            if (srvv && srvv->type == DICT_OBJECT) {
                DictValue *sp = dict_object_get(srvv, "priority");
                DictValue *sw = dict_object_get(srvv, "weight");
                DictValue *spt = dict_object_get(srvv, "port");
                DictValue *st = dict_object_get(srvv, "target");
                if (st && st->type == DICT_STRING && spt) {
                    if (!dns_valid_hostname(st->string_value)) { DNS_WARN("SRV target", st->string_value, "invalid hostname"); }
                    else {
                        DNS_ADD_REC(DNS_SRV)
                        r->srv.priority = (sp && sp->type == DICT_INT64) ? (uint16_t)sp->int64_value : 0;
                        r->srv.weight   = (sw && sw->type == DICT_INT64) ? (uint16_t)sw->int64_value : 100;
                        r->srv.port     = (spt->type == DICT_INT64) ? (uint16_t)spt->int64_value
                                        : (spt->type == DICT_NUMBER) ? (uint16_t)spt->number_value : 0;
                        strncpy(r->srv.target, st->string_value, sizeof(r->srv.target) - 1);
                        DNS_END_REC
                    }
                }
            } else if (srvv && srvv->type == DICT_ARRAY) {
                for (size_t j = 0; j < srvv->array_value.length; j++) {
                    DictValue *item = srvv->array_value.items[j];
                    if (!item || item->type != DICT_OBJECT) continue;
                    DictValue *sp = dict_object_get(item, "priority");
                    DictValue *sw = dict_object_get(item, "weight");
                    DictValue *spt = dict_object_get(item, "port");
                    DictValue *st = dict_object_get(item, "target");
                    if (st && st->type == DICT_STRING && spt) {
                        if (!dns_valid_hostname(st->string_value)) { DNS_WARN("SRV target", st->string_value, "invalid hostname"); }
                        else {
                            DNS_ADD_REC(DNS_SRV)
                            r->srv.priority = (sp && sp->type == DICT_INT64) ? (uint16_t)sp->int64_value : 0;
                            r->srv.weight   = (sw && sw->type == DICT_INT64) ? (uint16_t)sw->int64_value : 100;
                            r->srv.port     = (spt->type == DICT_INT64) ? (uint16_t)spt->int64_value
                                            : (spt->type == DICT_NUMBER) ? (uint16_t)spt->number_value : 0;
                            strncpy(r->srv.target, st->string_value, sizeof(r->srv.target) - 1);
                            DNS_END_REC
                        }
                    }
                }
            }

            #undef DNS_ADD_REC
            #undef DNS_END_REC
            #undef DNS_SKIP_REC
            #undef DNS_WARN

            if (ze->num_records > 0) {
                cfg->dns.num_entries++;
                LOG_INFO(DNS, "%s (%d records)", ze->name, ze->num_records);
            }
        }
        if (cfg->dns.num_entries > 0)
            LOG_INFO(DNS, "loaded %d entries (default TTL=%u)",
                    cfg->dns.num_entries, cfg->dns.default_ttl);
    }

    /* dispatch: array of { listener, handler, ... } */
    DictValue *disp = dict_object_get(root, "dispatch");
    if (disp && disp->type == DICT_ARRAY) {
        for (size_t i = 0; i < disp->array_value.length && cfg->num_dispatch < MAX_CONFIG_DISPATCH; i++) {
            DictValue *v = disp->array_value.items[i];
            if (!v || v->type != DICT_OBJECT) continue;
            DictValue *lv = dict_object_get(v, "listener");
            DictValue *hv = dict_object_get(v, "handler");
            const char *l = (lv && lv->type == DICT_STRING) ? lv->string_value : NULL;
            const char *h = (hv && hv->type == DICT_STRING) ? hv->string_value : NULL;
            if (l && h) {
                DispatchEntry *de = &cfg->dispatch[cfg->num_dispatch];
                memset(de, 0, sizeof(*de));
                de->listener = l;
                de->handler = h;
                de->raw = v;

                /* Parse forward-specific fields */
                if (strcmp(h, "forward") == 0) {
                    DictValue *ft = dict_object_get(v, "forward_to");
                    if (ft && ft->type == DICT_STRING) de->forward_to = ft->string_value;
                    DictValue *fm = dict_object_get(v, "mode");
                    if (fm && fm->type == DICT_STRING) de->forward_mode = fm->string_value;
                    DictValue *fto = dict_object_get(v, "timeout");
                    if (fto && fto->type == DICT_INT64) de->forward_timeout = (int)fto->int64_value;
                    else if (fto && fto->type == DICT_NUMBER) de->forward_timeout = (int)fto->number_value;
                    DictValue *fmc = dict_object_get(v, "max_connections");
                    if (fmc && fmc->type == DICT_INT64) de->forward_max_conn = (int)fmc->int64_value;
                    else if (fmc && fmc->type == DICT_NUMBER) de->forward_max_conn = (int)fmc->number_value;
                    DictValue *fbs = dict_object_get(v, "buffer_size");
                    if (fbs && fbs->type == DICT_INT64) de->forward_buf_size = (int)fbs->int64_value;
                    else if (fbs && fbs->type == DICT_NUMBER) de->forward_buf_size = (int)fbs->number_value;
                    LOG_INFO(CONFIG, "forward: %s -> %s (mode=%s timeout=%d)",
                            l, de->forward_to ? de->forward_to : "(null)",
                            de->forward_mode ? de->forward_mode : "auto",
                            de->forward_timeout);
                }

                /* Parse proxy-specific fields */
                if (strcmp(h, "proxy") == 0) {
                    DictValue *pp = dict_object_get(v, "proxy_pass");
                    if (pp && pp->type == DICT_STRING) de->proxy_pass = pp->string_value;
                    DictValue *bal = dict_object_get(v, "balance");
                    if (bal && bal->type == DICT_STRING) de->balance = bal->string_value;
                    DictValue *sp = dict_object_get(v, "strip_prefix");
                    if (sp && sp->type == DICT_BOOL && sp->bool_value) de->strip_prefix = true;
                    DictValue *pto = dict_object_get(v, "timeout");
                    if (pto && pto->type == DICT_INT64) de->proxy_timeout = (int)pto->int64_value;
                    else if (pto && pto->type == DICT_NUMBER) de->proxy_timeout = (int)pto->number_value;
                }

                cfg->num_dispatch++;
            }
        }
    }

    if (root_out) *root_out = root;
    else dict_destroy(root);

    /* Debug: show what we parsed */
    LOG_INFO(CONFIG, "loaded %d listen URIs, %d dispatch rules", cfg->num_listen, cfg->num_dispatch);
    for (int i = 0; i < cfg->num_listen; i++) {
        LOG_DEBUG(CONFIG, "listen[%d] name=%s uri=%s", i,
                cfg->listens[i].name ? cfg->listens[i].name : "(null)",
                cfg->listens[i].uri);
    }
    return 1;
}

#endif /* CONFIG_H */
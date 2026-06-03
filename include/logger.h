/*
 * logger.h — single-header structured logger for npserver
 *
 * Pretty colored output (default) or JSON, with per-subsystem log levels.
 * Levels and format are hot-reloadable — just update g_log_levels[] / g_log_json.
 *
 * Usage:
 *   #include "logger.h"
 *   LOG_INFO(CONFIG, "loaded %d listen URIs", n);
 *   LOG_ERROR(PROXY, "upstream %s:%d failed", host, port);
 *   LOG_DEBUG(HTTP, "fd=%d method=%s uri=%s", fd, method, uri);
 */
#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <unistd.h>

/* ── Log levels ──────────────────────────────────────────────────────── */

enum np_log_level {
    NP_TRACE = 0,
    NP_DEBUG = 1,
    NP_INFO  = 2,
    NP_WARN  = 3,
    NP_ERROR = 4,
    NP_FATAL = 5,
    NP_OFF   = 6    /* disables all logging for a subsystem */
};

/* ── Subsystem IDs ───────────────────────────────────────────────────── */

enum np_log_subsystem {
    LOG_SUB_CONFIG  = 0,
    LOG_SUB_SERVER  = 1,
    LOG_SUB_CONN    = 2,
    LOG_SUB_HTTP    = 3,
    LOG_SUB_PROXY   = 4,
    LOG_SUB_FORWARD = 5,
    LOG_SUB_TIMEOUT = 6,
    LOG_SUB_DNS     = 7,
    LOG_SUB_ACCESS  = 8,
    LOG_SUB__COUNT
};

/* ── Global state (hot-reloadable) ───────────────────────────────────── */

static int  g_log_levels[LOG_SUB__COUNT] = {
    [LOG_SUB_CONFIG]  = NP_WARN,
    [LOG_SUB_SERVER]  = NP_WARN,
    [LOG_SUB_CONN]    = NP_WARN,
    [LOG_SUB_HTTP]    = NP_WARN,
    [LOG_SUB_PROXY]   = NP_WARN,
    [LOG_SUB_FORWARD] = NP_WARN,
    [LOG_SUB_TIMEOUT] = NP_WARN,
    [LOG_SUB_DNS]     = NP_WARN,
    [LOG_SUB_ACCESS]  = NP_WARN,
};
static bool g_log_json  = false;
static bool g_log_color = true;   /* auto-detected, can be overridden */

/*
 * Access log destination.
 *   stdout  (default) — classic behaviour, keeps access logs separate from stderr diagnostics
 *   stderr  — merge with diagnostic output
 *   <path>  — write to file (append mode)
 *   NULL    — disabled
 * Managed via np_access_log_open() / np_access_log_close().
 */
static FILE *g_access_log_fp = NULL;     /* set during init */
static bool  g_access_log_is_file = false; /* true when we opened a path (must fclose) */
static bool  g_access_log_init_done = false;

/* ── ANSI color codes ────────────────────────────────────────────────── */

#define NP_CLR_RESET   "\033[0m"
#define NP_CLR_DIM     "\033[2m"
#define NP_CLR_BOLD    "\033[1m"
#define NP_CLR_RED     "\033[31m"
#define NP_CLR_GREEN   "\033[32m"
#define NP_CLR_YELLOW  "\033[33m"
#define NP_CLR_CYAN    "\033[36m"
#define NP_CLR_MAGENTA "\033[35m"
#define NP_CLR_BRED    "\033[1;31m"
#define NP_CLR_BGREEN  "\033[1;32m"
#define NP_CLR_BYELLOW "\033[1;33m"
#define NP_CLR_BCYAN   "\033[1;36m"
#define NP_CLR_WHITE   "\033[37m"

/* ── Internal helpers ────────────────────────────────────────────────── */

static inline const char *np_level_name(int level) {
    switch (level) {
    case NP_TRACE: return "TRACE";
    case NP_DEBUG: return "DEBUG";
    case NP_INFO:  return "INFO";
    case NP_WARN:  return "WARN";
    case NP_ERROR: return "ERROR";
    case NP_FATAL: return "FATAL";
    default:       return "?????";
    }
}

static inline const char *np_level_color(int level) {
    switch (level) {
    case NP_TRACE: return NP_CLR_DIM;
    case NP_DEBUG: return NP_CLR_CYAN;
    case NP_INFO:  return NP_CLR_GREEN;
    case NP_WARN:  return NP_CLR_YELLOW;
    case NP_ERROR: return NP_CLR_BRED;
    case NP_FATAL: return NP_CLR_BRED;
    default:       return "";
    }
}

static inline const char *np_sub_name(int sub) {
    static const char *names[] = {
        "config", "server", "conn", "http", "proxy",
        "forward", "timeout", "dns", "access"
    };
    if (sub >= 0 && sub < LOG_SUB__COUNT) return names[sub];
    return "???";
}

/* Map a level string ("trace","debug","info","warn","error","fatal","off")
 * to the enum value.  Returns -1 on unrecognized input. */
static inline int np_parse_log_level(const char *s) {
    if (!s) return -1;
    if (strcasecmp(s, "trace") == 0) return NP_TRACE;
    if (strcasecmp(s, "debug") == 0) return NP_DEBUG;
    if (strcasecmp(s, "info")  == 0) return NP_INFO;
    if (strcasecmp(s, "warn")  == 0) return NP_WARN;
    if (strcasecmp(s, "error") == 0) return NP_ERROR;
    if (strcasecmp(s, "fatal") == 0) return NP_FATAL;
    if (strcasecmp(s, "off")   == 0) return NP_OFF;
    return -1;
}

/* Map a subsystem name string to the enum value. Returns -1 if unknown. */
static inline int np_parse_log_subsystem(const char *s) {
    if (!s) return -1;
    if (strcasecmp(s, "config")  == 0) return LOG_SUB_CONFIG;
    if (strcasecmp(s, "server")  == 0) return LOG_SUB_SERVER;
    if (strcasecmp(s, "conn")    == 0) return LOG_SUB_CONN;
    if (strcasecmp(s, "http")    == 0) return LOG_SUB_HTTP;
    if (strcasecmp(s, "proxy")   == 0) return LOG_SUB_PROXY;
    if (strcasecmp(s, "forward") == 0) return LOG_SUB_FORWARD;
    if (strcasecmp(s, "timeout") == 0) return LOG_SUB_TIMEOUT;
    if (strcasecmp(s, "dns")     == 0) return LOG_SUB_DNS;
    if (strcasecmp(s, "access")  == 0) return LOG_SUB_ACCESS;
    return -1;
}

/* Auto-detect whether stderr is a terminal (for color support) */
static inline void np_log_init(void) {
    g_log_color = isatty(STDERR_FILENO);
    /* Default access log: stdout (separate from stderr diagnostics) */
    if (!g_access_log_init_done) {
        g_access_log_fp = stdout;
        g_access_log_init_done = true;
    }
}

/*
 * Open access log destination.
 *   dest = NULL or "stdout" → stdout  (default)
 *   dest = "stderr"          → stderr
 *   dest = "off" / "none"    → disabled
 *   dest = anything else     → file path (append)
 * Returns 0 on success, -1 on error (file open failure).
 */
static inline int np_access_log_open(const char *dest) {
    /* Close previous file if we opened one */
    if (g_access_log_is_file && g_access_log_fp) {
        fclose(g_access_log_fp);
    }
    g_access_log_fp = NULL;
    g_access_log_is_file = false;
    g_access_log_init_done = true;

    if (!dest || strcasecmp(dest, "stdout") == 0) {
        g_access_log_fp = stdout;
    } else if (strcasecmp(dest, "stderr") == 0) {
        g_access_log_fp = stderr;
    } else if (strcasecmp(dest, "off") == 0 || strcasecmp(dest, "none") == 0) {
        g_access_log_fp = NULL;  /* disabled */
    } else {
        FILE *f = fopen(dest, "a");
        if (!f) return -1;
        setvbuf(f, NULL, _IOLBF, 0);  /* line-buffered for tail -f */
        g_access_log_fp = f;
        g_access_log_is_file = true;
    }
    return 0;
}

static inline void np_access_log_close(void) {
    if (g_access_log_is_file && g_access_log_fp) {
        fclose(g_access_log_fp);
    }
    g_access_log_fp = NULL;
    g_access_log_is_file = false;
}

/* ── Core log function ───────────────────────────────────────────────── */

__attribute__((format(printf, 3, 4)))
static inline void np_log(int level, int subsystem, const char *fmt, ...) {
    /* Timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    if (g_log_json) {
        /* JSON output: one line per log entry */
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", &tm);

        /* Format the message */
        char msg[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);

        /* Escape quotes/backslashes in msg for valid JSON */
        char escaped[4096];
        size_t ei = 0;
        for (size_t i = 0; msg[i] && ei < sizeof(escaped) - 2; i++) {
            if (msg[i] == '"' || msg[i] == '\\') {
                escaped[ei++] = '\\';
            } else if (msg[i] == '\n') {
                escaped[ei++] = '\\';
                escaped[ei] = 'n';
                ei++;
                continue;
            } else if (msg[i] == '\r') {
                continue;
            }
            escaped[ei++] = msg[i];
        }
        escaped[ei] = '\0';

        fprintf(stderr, "{\"ts\":\"%s.%03ldZ\",\"level\":\"%s\",\"sub\":\"%s\",\"msg\":\"%s\"}\n",
                timebuf, ts.tv_nsec / 1000000,
                np_level_name(level), np_sub_name(subsystem), escaped);
    } else {
        /* Pretty output */
        char timebuf[16];
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm);

        if (g_log_color) {
            fprintf(stderr, "%s%s.%03ld%s %s%-5s%s %s[%-7s]%s ",
                    NP_CLR_DIM, timebuf, ts.tv_nsec / 1000000, NP_CLR_RESET,
                    np_level_color(level), np_level_name(level), NP_CLR_RESET,
                    NP_CLR_BOLD, np_sub_name(subsystem), NP_CLR_RESET);
        } else {
            fprintf(stderr, "%s.%03ld %-5s [%-7s] ",
                    timebuf, ts.tv_nsec / 1000000,
                    np_level_name(level), np_sub_name(subsystem));
        }

        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);

        fputc('\n', stderr);
    }
}

/* ── Public macros ───────────────────────────────────────────────────── */

/*
 * Usage:  LOG_INFO(PROXY, "connected to %s:%d", host, port);
 *
 * The subsystem name (CONFIG, SERVER, CONN, HTTP, PROXY, FORWARD, TIMEOUT,
 * DNS, ACCESS) is pasted with LOG_SUB_ to form the enum constant.
 * The check is compiled away when the level is below the threshold.
 */

#define NP_LOG(level, subsys, fmt, ...) \
    do { \
        if ((level) >= g_log_levels[LOG_SUB_##subsys]) \
            np_log((level), LOG_SUB_##subsys, fmt, ##__VA_ARGS__); \
    } while (0)

#define LOG_TRACE(subsys, fmt, ...) NP_LOG(NP_TRACE, subsys, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(subsys, fmt, ...) NP_LOG(NP_DEBUG, subsys, fmt, ##__VA_ARGS__)
#define LOG_INFO(subsys, fmt, ...)  NP_LOG(NP_INFO,  subsys, fmt, ##__VA_ARGS__)
#define LOG_WARN(subsys, fmt, ...)  NP_LOG(NP_WARN,  subsys, fmt, ##__VA_ARGS__)
#define LOG_ERROR(subsys, fmt, ...) NP_LOG(NP_ERROR, subsys, fmt, ##__VA_ARGS__)
#define LOG_FATAL(subsys, fmt, ...) NP_LOG(NP_FATAL, subsys, fmt, ##__VA_ARGS__)

/* ── Access log (CLF format, separate destination) ──────────────────── */

/*
 * Access log entries are written in Common Log Format to g_access_log_fp.
 * When JSON logging is on, they are emitted as structured JSON instead.
 * Set access_log to "off" in config (or g_access_log_fp = NULL) to disable.
 */
__attribute__((format(printf, 1, 2)))
static inline void np_access_log(const char *fmt, ...) {
    if (!g_access_log_fp) return;

    if (g_log_json) {
        /* Format the CLF message, then wrap it in JSON */
        char msg[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);

        /* Strip trailing newline for cleaner JSON */
        size_t mlen = strlen(msg);
        while (mlen > 0 && (msg[mlen-1] == '\n' || msg[mlen-1] == '\r')) msg[--mlen] = '\0';

        /* Escape for JSON */
        char escaped[4096];
        size_t ei = 0;
        for (size_t i = 0; msg[i] && ei < sizeof(escaped) - 2; i++) {
            if (msg[i] == '"' || msg[i] == '\\') escaped[ei++] = '\\';
            else if (msg[i] == '\n') { escaped[ei++] = '\\'; escaped[ei++] = 'n'; continue; }
            else if (msg[i] == '\r') continue;
            escaped[ei++] = msg[i];
        }
        escaped[ei] = '\0';

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", &tm);
        fprintf(g_access_log_fp,
                "{\"ts\":\"%s.%03ldZ\",\"level\":\"INFO\",\"sub\":\"access\",\"msg\":\"%s\"}\n",
                timebuf, ts.tv_nsec / 1000000, escaped);
    } else {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_access_log_fp, fmt, ap);
        va_end(ap);
    }
}

#endif /* LOGGER_H */

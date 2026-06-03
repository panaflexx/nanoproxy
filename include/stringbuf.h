#ifndef STRINGBUF_H
#define STRINGBUF_H

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>

/* Default max size if user passes 0 or huge value */
#ifndef STRINGBUF_DEFAULT_CAP
#define STRINGBUF_DEFAULT_CAP (1024 * 64) /* 64 KB default */
#endif
#ifndef STRINGBUF_MAX_CAP
#define STRINGBUF_MAX_CAP (64 * 1024 * 1024) /* 64 MiB – allow large uploads; parser still buffers the whole request body in RAM */
#endif

typedef struct {
    char* data;        /* Heap-allocated buffer, always null-terminated */
    ssize_t size;      /* Used bytes (excluding terminating '\0') */
    ssize_t capacity;  /* Total allocated bytes (including space for '\0') */
    bool owned;
} StringBuf;

/* Forward declarations for header-only usage */
bool stringbuf_init_str(StringBuf* sb, const char* src, size_t srclen, size_t capacity_hint);
bool stringbuf_init(StringBuf* sb, size_t capacity_hint);
bool stringbuf_init_buf(StringBuf* sb, char* src, size_t srclen);
void stringbuf_free(StringBuf* sb);
bool stringbuf_reserve(StringBuf* sb, ssize_t need);
bool stringbuf_append(StringBuf* sb, const char* src, ssize_t len);
bool stringbuf_append_str(StringBuf* sb, const char* str);
bool stringbuf_append_char(StringBuf* sb, char c);
bool stringbuf_appendf(StringBuf* sb, const char* fmt, ...);
int stringbuf_vsscanf(const StringBuf* sb, const char* fmt, va_list ap);
int stringbuf_sscanf(const StringBuf* sb, const char* fmt, ...);
void stringbuf_clear(StringBuf* sb);
void stringbuf_consume(StringBuf* sb, size_t n);
char* stringbuf_strndup(const StringBuf* sb, size_t start, size_t n);
ssize_t stringbuf_find(const StringBuf* sb, size_t start, const char* needle, size_t needle_len);
ssize_t stringbuf_memchr(const StringBuf* sb, char c, size_t start);
size_t stringbuf_trim_left(const StringBuf* sb, size_t start);
size_t stringbuf_trim_right(const StringBuf* sb, size_t end);

/* Accessors */
static inline char* stringbuf_data(StringBuf* sb) { return sb ? sb->data : NULL; }
static inline const char* stringbuf_cstr(const StringBuf* sb) { return sb && sb->data ? sb->data : ""; }
static inline size_t stringbuf_size(const StringBuf* sb) { return sb ? sb->size : 0; }
static inline size_t stringbuf_capacity(const StringBuf* sb) { return sb ? sb->capacity : 0; }
static inline bool stringbuf_empty(const StringBuf* sb) { return !sb || sb->size == 0; }

#ifdef STRINGBUF_IMPLEMENTATION
/* Implementation section - only compiled when STRINGBUF_IMPLEMENTATION is defined */

bool stringbuf_init_str(StringBuf* sb, const char* src, size_t srclen, size_t capacity_hint)
{
    if (!sb) return false;
    size_t cap = capacity_hint;
    if (cap == 0 || cap > 1024*1024*16) /* sanity cap at 16 MiB */
        cap = STRINGBUF_DEFAULT_CAP;
    char* mem = (char*)malloc(cap);
    if (!mem) return false;
    sb->data = mem;
    sb->capacity = cap;
    sb->size = 0;
    sb->data[0] = '\0';
    sb->owned = true;
    if (src && srclen) {
        if (srclen > cap - 1) srclen = cap - 1;
        memcpy(sb->data, src, srclen);
        sb->size = srclen;
        sb->data[sb->size] = '\0';
    }
    return true;
}

bool stringbuf_init(StringBuf* sb, size_t capacity_hint)
{
    return stringbuf_init_str(sb, NULL, 0, capacity_hint);
}

bool stringbuf_init_buf(StringBuf* sb, char* src, size_t srclen)
{
    if (!sb) return false;
    sb->data = src;
    sb->capacity = srclen;
    sb->size = 0;
    sb->data[0] = '\0';
    sb->owned = false;
    return true;
}

void stringbuf_free(StringBuf* sb)
{
    if (sb && sb->data && sb->owned) {
        free(sb->data);
    }
    if (sb) {
        sb->data = NULL;
        sb->size = sb->capacity = 0;
    }
}

bool stringbuf_reserve(StringBuf* sb, ssize_t need)
{
    if (!sb) return false;
    if (need < 0 || need > SSIZE_MAX - 1) return false;
    need++; /* +1 for '\0' */
    if (need <= sb->capacity) return true;
    if (!sb->owned) return false;
    if (sb->capacity > SSIZE_MAX / 2) return false;
    ssize_t newcap = sb->capacity * 2;
    if (newcap < need) newcap = need;
    if (newcap < 128) newcap = 128;
    if (newcap > STRINGBUF_MAX_CAP) return false;
    char* newmem = (char*)realloc(sb->data, newcap);
    if (!newmem) return false;
    sb->data = newmem;
    sb->capacity = newcap;
    return true;
}

bool stringbuf_append(StringBuf* sb, const char* src, ssize_t len)
{
    if (!sb || !src || len <= 0) return false;
    if (sb->size > SSIZE_MAX - len) return false;
    if (!stringbuf_reserve(sb, sb->size + len)) return false;
    if (sb->size + len >= sb->capacity) {
        fprintf(stderr, "StringBuf ERROR: TARGET OVER CAPACITY\n");
        return false;
    }
    memcpy(sb->data + sb->size, src, len);
    sb->size += len;
    sb->data[sb->size] = '\0';
    return true;
}

bool stringbuf_append_str(StringBuf* sb, const char* str)
{
    return str ? stringbuf_append(sb, str, strlen(str)) : false;
}

bool stringbuf_append_char(StringBuf* sb, char c)
{
    if (!sb) return false;
    if (!stringbuf_reserve(sb, sb->size + 1)) return false;
    sb->data[sb->size] = c;
    sb->data[++sb->size] = '\0';
    return true;
}

bool stringbuf_appendf(StringBuf* sb, const char* fmt, ...)
{
    if (!sb || !fmt) return false;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (needed < 0) { va_end(ap); return false; }
    if (sb->size > SSIZE_MAX - needed) { va_end(ap); return false; }
    if (!stringbuf_reserve(sb, sb->size + (size_t)needed)) {
        va_end(ap);
        return false;
    }
    vsnprintf(sb->data + sb->size, sb->capacity - sb->size, fmt, ap);
    sb->size += (size_t)needed;
    va_end(ap);
    return true;
}

int stringbuf_vsscanf(const StringBuf* sb, const char* fmt, va_list ap)
{
    if (!sb || !sb->data || !fmt) return EOF;
    return vsscanf(sb->data, fmt, ap);
}

int stringbuf_sscanf(const StringBuf* sb, const char* fmt, ...)
{
    if (!sb || !sb->data || !fmt) return EOF;
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(sb->data, fmt, ap);
    va_end(ap);
    return n;
}

void stringbuf_clear(StringBuf* sb)
{
    if (sb) {
        sb->size = 0;
        if (sb->data) sb->data[0] = '\0';
    }
}

void stringbuf_consume(StringBuf* sb, size_t n)
{
    if (!sb || !sb->data) return;
    if (n > (size_t)sb->size) n = sb->size;
    memmove(sb->data, sb->data + n, sb->size - n);
    sb->size -= n;
    sb->data[sb->size] = '\0';
}

char* stringbuf_strndup(const StringBuf* sb, size_t start, size_t n)
{
    if (!sb || !sb->data || start > (size_t)sb->size) return NULL;
    if (n > (size_t)sb->size - start) n = sb->size - start;
    char* p = (char*)malloc(n + 1);
    if (p) {
        memcpy(p, sb->data + start, n);
        p[n] = '\0';
    }
    return p;
}

ssize_t stringbuf_find(const StringBuf* sb, size_t start, const char* needle, size_t needle_len)
{
    if (!sb || !sb->data || start > (size_t)sb->size || needle_len == 0 || needle_len > (size_t)sb->size - start) return -1;
    const char* hay = sb->data + start;
    size_t hay_len = sb->size - start;
    if (needle_len == 1) {
        return stringbuf_memchr(sb, needle[0], start);
    }
    for (size_t i = 0; i <= hay_len - needle_len; ++i) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0) return start + i;
    }
    return -1;
}

ssize_t stringbuf_memchr(const StringBuf* sb, char c, size_t start)
{
    if (!sb || !sb->data || start > (size_t)sb->size) return -1;
    const char* p = (const char*)memchr(sb->data + start, c, sb->size - start);
    return p ? p - sb->data : -1;
}

size_t stringbuf_trim_left(const StringBuf* sb, size_t start)
{
    if (!sb || !sb->data) return start;
    while (start < (size_t)sb->size && isspace((unsigned char)sb->data[start])) ++start;
    return start;
}

size_t stringbuf_trim_right(const StringBuf* sb, size_t end)
{
    if (!sb || !sb->data) return end;
    while (end > 0 && isspace((unsigned char)sb->data[end - 1])) --end;
    return end;
}

#endif /* STRINGBUF_IMPLEMENTATION */

#endif /* STRINGBUF_H */

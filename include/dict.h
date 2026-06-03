/* Copyright 2025 Roger Davenport */
/* MIT Licensed */

#ifndef DICT_H
#define DICT_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <ctype.h>
#include <inttypes.h> // for PRId64

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_KEY_LEN   (64 * 1024) // 8 KB max key length (bytes)
#define MAX_VALUE_LEN 20000000  // 20 million chars max string value length (characters)

typedef enum {
    DICT_NULL,
    DICT_BOOL,
    DICT_NUMBER,
	DICT_INT64,
    DICT_STRING,
    DICT_ARRAY,
    DICT_OBJECT
} DictType;

typedef struct DictValue DictValue;

typedef struct {
    size_t length;
    DictValue **items;
} DictArray;

typedef struct {
    char *key;
    DictValue *value;
} DictKeyValuePair;

typedef struct {
    size_t count;
    size_t capacity;
    DictKeyValuePair *pairs;
} DictObject;

struct DictValue {
    DictType type;
    union {
        int bool_value;
        double number_value;
		int64_t int64_value;
        char *string_value;
        DictArray array_value;
        DictObject object_value;
    };
};

// --- Internal utility ---

// Duplicate string utility
static char *dict_strdup(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *dst = (char *)malloc(len + 1);
    if (dst) memcpy(dst, src, len + 1);
    return dst;
}

// Free DictValue (recursive)
static void dict_value_free(DictValue *val);

// Initialize empty DictValue of object type
static void dict_object_init(DictObject *obj) {
	if (!obj) return;
    obj->count = 0;
    obj->capacity = 4;
    obj->pairs = (DictKeyValuePair *)malloc(sizeof(DictKeyValuePair) * obj->capacity);
	if (!obj->pairs) {
        obj->capacity = 0;
    }
}

// Clear object pairs and free keys and values
static void dict_object_clear(DictObject *obj) {
    if (!obj) return;
    for (size_t i = 0; i < obj->count; i++) {
        free(obj->pairs[i].key);
        dict_value_free(obj->pairs[i].value);
        free(obj->pairs[i].value);
    }
    free(obj->pairs);
    obj->pairs = NULL;
    obj->count = 0;
    obj->capacity = 0;
}

// Resize pairs array if needed
static int dict_object_ensure_capacity(DictObject *obj, size_t new_capacity) {
    if (new_capacity <= obj->capacity)
        return 1; // already enough

    size_t new_cap;
    if (obj->capacity > (SIZE_MAX / 2)) {
        new_cap = new_capacity;
    } else {
        new_cap = obj->capacity * 2;
        if (new_cap < new_capacity)
            new_cap = new_capacity;
    }

    if (new_cap > SIZE_MAX / sizeof(DictKeyValuePair)) {
        // Too large, prevent overflow
        return 0;
    }

    DictKeyValuePair *new_pairs = (DictKeyValuePair *)realloc(obj->pairs, new_cap * sizeof(DictKeyValuePair));
    if (!new_pairs)
        return 0; // failure

    obj->pairs = new_pairs;
    obj->capacity = new_cap;
    return 1;
}

// --- Public API ---

// Create a new empty DICT object value (caller owns)
static DictValue *dict_create_object() {
    DictValue *val = (DictValue *)malloc(sizeof(DictValue));
    if (!val) return NULL;
    val->type = DICT_OBJECT;
    dict_object_init(&val->object_value);
    if (!val->object_value.pairs && val->object_value.capacity == 0) {
        free(val);
        return NULL;
    }
    return val;
}

// Create a new DICT value of null type
static DictValue *dict_create_null() {
    DictValue *val = (DictValue *)malloc(sizeof(DictValue));
    if (!val) return NULL;
    val->type = DICT_NULL;
    return val;
}

// Create a new DICT bool value
static DictValue *dict_create_bool(int b) {
    DictValue *val = (DictValue *)malloc(sizeof(DictValue));
    if (!val) return NULL;
    val->type = DICT_BOOL;
    val->bool_value = b ? 1 : 0;
    return val;
}

// Create a new DICT number value
static DictValue *dict_create_number(double n) {
    DictValue *val = (DictValue *)malloc(sizeof(DictValue));
    if (!val) return NULL;
    val->type = DICT_NUMBER;
    val->number_value = n;
    return val;
}

static DictValue *dict_create_int64(int64_t n) {
    DictValue *val = (DictValue *)malloc(sizeof(DictValue));
    if (!val) return NULL;
    val->type = DICT_INT64;
    val->int64_value = n;
    return val;
}

// Create a new DICT string value (copies)
static DictValue *dict_create_string(const char *s) {
	if (!s) return NULL;
    size_t len = strnlen(s, MAX_VALUE_LEN);
    if (len == 0 || len >= MAX_VALUE_LEN) {
        return NULL;
    }

    DictValue *val = (DictValue *)malloc(sizeof(DictValue));
    if (!val) return NULL;
    val->type = DICT_STRING;
    val->string_value = dict_strdup(s);
    if (!val->string_value) {
        free(val);
        return NULL;
    }
    return val;
}

// Create a new DICT array value (empty)
static DictValue *dict_create_array() {
    DictValue *val = (DictValue *)malloc(sizeof(DictValue));
    if (!val) return NULL;
    val->type = DICT_ARRAY;
    val->array_value.length = 0;
    val->array_value.items = NULL;
    return val;
}

// Ensure capacity for DictArray, returns 1 on success, 0 on failure
static int dict_array_ensure_capacity(DictArray *array, size_t new_capacity) {
    if (new_capacity <= array->length)
        return 1; // already enough capacity

    size_t new_cap = array->length == 0 ? 4 : array->length * 2;
    if (new_cap < new_capacity) new_cap = new_capacity;

    if (new_cap > SIZE_MAX / sizeof(DictValue *)) {
        // Prevent overflow
        return 0;
    }

    DictValue **new_items = (DictValue **)realloc(array->items, new_cap * sizeof(DictValue *));
    if (!new_items) return 0;

    array->items = new_items;
    return 1;
}

// Append a DictValue to a DictValue array (caller owns new_val)
// Returns 1 on success, 0 on failure (e.g. realloc fail or wrong type)
static int dict_array_append(DictValue *array_val, DictValue *new_val) {
    if (!array_val || array_val->type != DICT_ARRAY || !new_val) return 0;

    DictArray *array = &array_val->array_value;

    if (!dict_array_ensure_capacity(array, array->length + 1))
        return 0; // realloc failed

    array->items[array->length] = new_val;
    array->length++;

    return 1;
}

// Free a DictValue recursively
static void dict_value_free(DictValue *val) {
    if (!val) return;
    switch (val->type) {
        case DICT_STRING:
            free(val->string_value);
            break;
        case DICT_ARRAY:
            for (size_t i = 0; i < val->array_value.length; i++) {
                dict_value_free(val->array_value.items[i]);
                free(val->array_value.items[i]);
            }
            free(val->array_value.items);
            break;
        case DICT_OBJECT:
            dict_object_clear(&val->object_value);
            break;
        default:
            break; // nothing to free
    }
}

// Find index of key in object, returns size_t max if not found
static size_t dict_object_find_key(const DictObject *obj, const char *key) {
    if (!obj || !key) return (size_t)-1;
    for (size_t i = 0; i < obj->count; i++) {
        if (strcmp(obj->pairs[i].key, key) == 0) return i;
    }
    return (size_t)-1;
}

// Set or insert a key-value pair into an object.
// If key exists, replaces old value with new value (caller owns new_val).
// If key doesn't exist, inserts new key-value pair.
// Does NOT copy the key or value; ownership transferred.
// Returns 1 on success, 0 on failure.
static int dict_object_set(DictValue *obj_val, const char *key, DictValue *new_val) {
    if (!obj_val || obj_val->type != DICT_OBJECT || !key || !new_val) return 0;
	size_t key_len = strnlen(key, MAX_KEY_LEN);
    if (key_len == 0 || key_len >= MAX_KEY_LEN) return 0;

    DictObject *obj = &obj_val->object_value;
    size_t idx = dict_object_find_key(obj, key);
    if (idx != (size_t)-1) {
        // Key exists, free old value and replace
        dict_value_free(obj->pairs[idx].value);
        free(obj->pairs[idx].value);
        obj->pairs[idx].value = new_val;
        return 1;
    }
    // Key does not exist, insert new
    if (!dict_object_ensure_capacity(obj, obj->count + 1))
        return 0; // allocation failure

    char *key_copy = dict_strdup(key);
    if (!key_copy)
        return 0;

    obj->pairs[obj->count].key = key_copy;
    obj->pairs[obj->count].value = new_val;
    obj->count++;
    return 1;
}

// Get value pointer by key, or NULL if not found
static DictValue *dict_object_get(const DictValue *obj_val, const char *key) {
    if (!obj_val || obj_val->type != DICT_OBJECT || !key) return NULL;
    const DictObject *obj = &obj_val->object_value;
    size_t idx = dict_object_find_key(obj, key);
    if (idx == (size_t)-1) return NULL;
    return obj->pairs[idx].value;
}

// Remove key-value pair by key from object.
// Frees key and value.
// Returns 1 if removed, 0 if not found.
static int dict_object_remove(DictValue *obj_val, const char *key) {
    if (!obj_val || obj_val->type != DICT_OBJECT || !key) return 0;
    DictObject *obj = &obj_val->object_value;

    size_t idx = dict_object_find_key(obj, key);
    if (idx == (size_t)-1) return 0;

    free(obj->pairs[idx].key);
    dict_value_free(obj->pairs[idx].value);
    free(obj->pairs[idx].value);

    // Move last pair to idx to fill gap (order not kept)
    if (idx != obj->count - 1) {
        obj->pairs[idx] = obj->pairs[obj->count - 1];
    }
    obj->count--;
    return 1;
}

// Comparison helper for qsort for sorting DictKeyValuePair by key
static int dict_key_compare(const void *a, const void *b) {
    const DictKeyValuePair *pa = (const DictKeyValuePair *)a;
    const DictKeyValuePair *pb = (const DictKeyValuePair *)b;
    return strcmp(pa->key, pb->key);
}

// Sort keys lexicographically (stable sort)
// Returns void
static void dict_object_sort_keys(DictValue *obj_val) {
    if (!obj_val || obj_val->type != DICT_OBJECT) return;
    DictObject *obj = &obj_val->object_value;
    // qsort is not stable but good enough here
    qsort(obj->pairs, obj->count, sizeof(DictKeyValuePair), dict_key_compare);
}

// Internal helper: Append a string to buffer, updating cursor pointer and remaining size.
// Returns 1 on success, 0 if insufficient space.
static int dict_append_to_buffer(char **cursor, size_t *remaining, const char *src) {
    while (*src) {
        if (*remaining <= 1) return 0; // not enough space (need at least one char + null)
        **cursor = *src;
        (*cursor)++;
        (*remaining)--;
        src++;
    }
    return 1;
}

// Internal helper: Append a single char to buffer, updating cursor and remaining.
// Returns 1 on success, 0 if insufficient space.
static int dict_append_char(char **cursor, size_t *remaining, char c) {
    if (*remaining <= 1) return 0;
    **cursor = c;
    (*cursor)++;
    (*remaining)--;
    return 1;
}

// Internal helper: Escape and append a string value as JSON string with quotes.
static int dict_append_escaped_string(char **cursor, size_t *remaining, const char *str) {
    if (!dict_append_char(cursor, remaining, '"')) return 0;

    while (*str) {
        unsigned char c = (unsigned char)*str;
        if (c == '"' || c == '\\') {
            if (!dict_append_char(cursor, remaining, '\\')) return 0;
            if (!dict_append_char(cursor, remaining, c)) return 0;
        } else if (c == '\b') {
            if (!dict_append_to_buffer(cursor, remaining, "\\b")) return 0;
        } else if (c == '\f') {
            if (!dict_append_to_buffer(cursor, remaining, "\\f")) return 0;
        } else if (c == '\n') {
            if (!dict_append_to_buffer(cursor, remaining, "\\n")) return 0;
        } else if (c == '\r') {
            if (!dict_append_to_buffer(cursor, remaining, "\\r")) return 0;
        } else if (c == '\t') {
            if (!dict_append_to_buffer(cursor, remaining, "\\t")) return 0;
        } else if (c < 0x20) {
            // control character escape (\u00XX)
            char esc[7];
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            if (!dict_append_to_buffer(cursor, remaining, esc)) return 0;
        } else {
            if (!dict_append_char(cursor, remaining, c)) return 0;
        }
        str++;
    }

    if (!dict_append_char(cursor, remaining, '"')) return 0;

    return 1;
}

// Internal recursive serialization function with pretty print option.
// indentation_level = current indent depth, pretty = 1 to enable pretty printing
// Returns 1 on success, 0 if buffer too small.
static int dict_serialize_value_pretty(const DictValue *val, char **cursor, size_t *remaining, int indentation_level, int pretty) {
    if (!val) return dict_append_to_buffer(cursor, remaining, "null");

    const char *indent_str = "  "; // 2 spaces per level
    #define APPEND_NEWLINE_AND_INDENT() \
        do { \
            if (pretty) { \
                if (!dict_append_char(cursor, remaining, '\n')) return 0; \
                for (int _i = 0; _i < indentation_level; _i++) { \
                    if (!dict_append_to_buffer(cursor, remaining, indent_str)) return 0; \
                } \
            } \
        } while(0)

    switch (val->type) {
        case DICT_NULL:
            return dict_append_to_buffer(cursor, remaining, "null");
        case DICT_BOOL:
            return dict_append_to_buffer(cursor, remaining, val->bool_value ? "true" : "false");
        case DICT_NUMBER: {
            char numbuf[64];
            int n = snprintf(numbuf, sizeof(numbuf), "%.17g", val->number_value);
            if (n < 0 || (size_t)n >= sizeof(numbuf)) return 0;
            return dict_append_to_buffer(cursor, remaining, numbuf);
        }
		case DICT_INT64: {
            // Serialize int64 as JSON number if fits 53 bits, else as string
            // double precision floats are exact integers up to 2^53
            int64_t n = val->int64_value;
            // Check if fits in double integer range
            if (n <= 9007199254740991LL && n >= -9007199254740991LL) {
                // Convert to double and print
                char numbuf[64];
                int length = snprintf(numbuf, sizeof(numbuf), "%" PRId64, n);
                if (length < 0 || (size_t)length >= sizeof(numbuf)) return 0;
                return dict_append_to_buffer(cursor, remaining, numbuf);
            } else {
                // Large int64 out of JSON safe range: serialize as string
                char strbuf[32];
                int length = snprintf(strbuf, sizeof(strbuf), "%" PRId64, n);
                if (length < 0 || (size_t)length >= sizeof(strbuf)) return 0;
                return dict_append_escaped_string(cursor, remaining, strbuf);
            }
        }
        case DICT_STRING:
            if (!val->string_value) return dict_append_to_buffer(cursor, remaining, "null");
            return dict_append_escaped_string(cursor, remaining, val->string_value);
        case DICT_ARRAY: {
            if (!dict_append_char(cursor, remaining, '[')) return 0;
            if (val->array_value.length > 0 && pretty) {
                APPEND_NEWLINE_AND_INDENT();
            }
            for (size_t i = 0; i < val->array_value.length; i++) {
                if (i > 0) {
                    if (!dict_append_char(cursor, remaining, ',')) return 0;
                    if (pretty) APPEND_NEWLINE_AND_INDENT();
                }
                if (!dict_serialize_value_pretty(val->array_value.items[i], cursor, remaining, indentation_level + 1, pretty)) return 0;
            }
            if (val->array_value.length > 0 && pretty) {
                // newline and indent for closing bracket
                if (!dict_append_char(cursor, remaining, '\n')) return 0;
                for (int i = 0; i < indentation_level; i++) {
                    if (!dict_append_to_buffer(cursor, remaining, indent_str)) return 0;
                }
            }
            return dict_append_char(cursor, remaining, ']');
        }
        case DICT_OBJECT: {
            if (!dict_append_char(cursor, remaining, '{')) return 0;
            if (val->object_value.count > 0 && pretty) {
                APPEND_NEWLINE_AND_INDENT();
            }
            for (size_t i = 0; i < val->object_value.count; i++) {
                if (i > 0) {
                    if (!dict_append_char(cursor, remaining, ',')) return 0;
                    if (pretty) APPEND_NEWLINE_AND_INDENT();
                }
                if (!dict_append_escaped_string(cursor, remaining, val->object_value.pairs[i].key)) return 0;
                if (!dict_append_char(cursor, remaining, ':')) return 0;
                if (pretty && !dict_append_char(cursor, remaining, ' ')) return 0;
                if (!dict_serialize_value_pretty(val->object_value.pairs[i].value, cursor, remaining, indentation_level + 1, pretty)) return 0;
            }
            if (val->object_value.count > 0 && pretty) {
                // newline and indent for closing brace
                if (!dict_append_char(cursor, remaining, '\n')) return 0;
                for (int i = 0; i < indentation_level; i++) {
                    if (!dict_append_to_buffer(cursor, remaining, indent_str)) return 0;
                }
            }
            return dict_append_char(cursor, remaining, '}');
        }
        default:
            return dict_append_to_buffer(cursor, remaining, "null");
    }
    #undef APPEND_NEWLINE_AND_INDENT
}

// Public function to serialize DictValue as JSON into user buffer, with pretty print option.
// returns buffer pointer on success, NULL if buffer too small or invalid input.
// The output is null-terminated C string.
static char *dict_serialize_json(const DictValue *val, char *buffer, size_t buf_len, int pretty) {
    if (!buffer || buf_len == 0) return NULL;

    char *cursor = buffer;
    size_t remaining = buf_len;

    if (!dict_serialize_value_pretty(val, &cursor, &remaining, 0, pretty)) {
        if (buf_len > 0) buffer[0] = '\0';
        return NULL;
    }

    if (remaining == 0) {
        if (buf_len > 0) buffer[0] = '\0';
        return NULL;
    }
    *cursor = '\0';
    return buffer;
}

// Search dict by path string separated by '/', e.g. "key1/key2/key3".
// Returns pointer to DictValue found or NULL if not found or invalid input.
// Does not allocate or copy, just traverses existing objects.
// Accepts NULL or empty path returns NULL.
static DictValue *dict_find_path(const DictValue *root, const char *path) {
    if (!root || root->type != DICT_OBJECT || !path || *path == '\0') return NULL;

    const DictValue *current = root;
    const char *segment_start = path;
    while (segment_start) {
        // Find next delimiter
        const char *slash = strchr(segment_start, '/');
        size_t len = slash ? (size_t)(slash - segment_start) : strlen(segment_start);

        if (len == 0) return NULL; // empty segment invalid

        // Extract key segment string (not allocating, just comparing)
        // We'll compare via strncmp with the key length.
        const DictObject *obj = &current->object_value;
        DictValue *next_val = NULL;
        for (size_t i = 0; i < obj->count; i++) {
            if (strncmp(obj->pairs[i].key, segment_start, len) == 0 && obj->pairs[i].key[len] == '\0') {
                next_val = obj->pairs[i].value;
                break;
            }
        }
        if (!next_val) return NULL; // key not found

        if (!slash) {
            // Last segment - return this value found
            return (DictValue *)next_val; // cast to non-const for API consistency
        }

        // Intermediate segment must be an object
        if (next_val->type != DICT_OBJECT) return NULL;

        current = next_val;
        segment_start = slash + 1;
    }

    return NULL; // Should never get here logically
}

// Destroy entire DictValue recursively and free the pointer
static void dict_destroy(DictValue *val) {
    if (!val) return;
    dict_value_free(val);
    free(val);
}

// --- JSON deserialization ---

// Utility: Structure to hold parser state
typedef struct {
    const char *buffer;
    size_t buffer_len;
    size_t pos;
    char *error_str;
    size_t error_str_len;
} DictJsonParser;

// Forward declarations
static DictValue *dict_parse_value(DictJsonParser *p);

static void dict_parser_error(DictJsonParser *p, const char *msg) {
    if (!p->error_str || p->error_str_len == 0) return;

    const char *b = p->buffer;
    size_t blen = p->buffer_len;
    size_t epos = p->pos < blen ? p->pos : (blen > 0 ? blen - 1 : 0);

    /* Compute error line (1-based) and column, track recent line-start offsets */
    size_t line = 1, col = 1;
    size_t starts[5];          /* ring of the last 5 line-start offsets */
    int ns = 0;                /* number stored */
    starts[0] = 0; ns = 1;
    for (size_t i = 0; i < epos; i++) {
        if (b[i] == '\n') {
            line++; col = 1;
            if (ns < 5) { starts[ns++] = i + 1; }
            else { for (int k = 0; k < 4; k++) starts[k] = starts[k+1]; starts[4] = i + 1; }
        } else { col++; }
    }

    /* Context window: up to 2 lines before, error line, up to 2 lines after */
    int ctx_before = ns - 1;  if (ctx_before > 2) ctx_before = 2;
    size_t first_start = starts[ns - 1 - ctx_before];
    size_t first_line  = line - (size_t)ctx_before;

    /* Find end of context: skip past error line + 2 more lines */
    size_t ctx_end = epos;
    while (ctx_end < blen && b[ctx_end] != '\n') ctx_end++;   /* rest of error line */
    int after = 2;
    while (ctx_end < blen && after > 0) {
        ctx_end++;
        if (ctx_end >= blen || b[ctx_end] == '\n') after--;
    }
    if (ctx_end < blen) ctx_end++;

    /* Compute gutter width from the highest line number shown */
    size_t last_line = line + 2;
    int gw = 1; { size_t n = last_line; while (n >= 10) { gw++; n /= 10; } }

    /* Format into error buffer */
    char *out = p->error_str;
    size_t rem = p->error_str_len;
    int w;

    w = snprintf(out, rem, "line %zu, col %zu:\n\n", line, col);
    if (w > 0 && (size_t)w < rem) { out += w; rem -= w; }

    /* Render each line in the context window */
    size_t cur_line = first_line;
    size_t i = first_start;
    while (i < ctx_end && i < blen && rem > 1) {
        /* Find end of this line */
        size_t le = i;
        while (le < blen && b[le] != '\n') le++;
        size_t line_len = le - i;
        /* Strip trailing \r */
        size_t show_len = line_len;
        if (show_len > 0 && b[i + show_len - 1] == '\r') show_len--;
        if (show_len > 120) show_len = 120;

        w = snprintf(out, rem, " %*zu | %.*s\n", gw, cur_line, (int)show_len, b + i);
        if (w > 0 && (size_t)w < rem) { out += w; rem -= w; }

        /* Caret on the error line */
        if (cur_line == line && rem > 1) {
            size_t caret_col = col - 1;
            if (caret_col > 120) caret_col = 120;
            w = snprintf(out, rem, " %*s | %*s^\n", gw, "", (int)caret_col, "");
            if (w > 0 && (size_t)w < rem) { out += w; rem -= w; }
        }

        cur_line++;
        i = le < blen ? le + 1 : blen;
    }

    snprintf(out, rem, "\n%s", msg);
}

// Return true if we can consume next char, else false
static int dict_parser_peek(DictJsonParser *p, char *out_char) {
    if (p->pos >= p->buffer_len) return 0;
    *out_char = p->buffer[p->pos];
    return 1;
}

static int dict_parser_consume(DictJsonParser *p, char expect) {
    if (p->pos >= p->buffer_len || p->buffer[p->pos] != expect) return 0;
    p->pos++;
    return 1;
}

static void dict_parser_skip_whitespace(DictJsonParser *p) {
    while (p->pos < p->buffer_len) {
        char c = p->buffer[p->pos];
        if (isspace((unsigned char)c)) {
            p->pos++;
            continue;
        }
        /* C++-style line comment */
        if (c == '/' && p->pos + 1 < p->buffer_len && p->buffer[p->pos + 1] == '/') {
            p->pos += 2;
            while (p->pos < p->buffer_len && p->buffer[p->pos] != '\n' && p->buffer[p->pos] != '\r') {
                p->pos++;
            }
            continue;
        }
        /* C-style block comment */
        if (c == '/' && p->pos + 1 < p->buffer_len && p->buffer[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->pos + 1 < p->buffer_len &&
                   !(p->buffer[p->pos] == '*' && p->buffer[p->pos + 1] == '/')) {
                p->pos++;
            }
            if (p->pos + 1 < p->buffer_len) p->pos += 2; /* skip */
            continue;
        }
        break;
    }
}

// Parse literal keywords: true, false, null
static int dict_parse_literal(DictJsonParser *p, const char *literal) {
    size_t len = strlen(literal);
    if (p->pos + len > p->buffer_len) return 0;
    if (strncmp(p->buffer + p->pos, literal, len) == 0) {
        p->pos += len;
        return 1;
    }
    return 0;
}

// Parse JSON string with escapes, returns newly allocated C string or NULL on error
// Advances p->pos beyond the string's closing quote if successful.
static char *dict_parse_json_string(DictJsonParser *p) {
    if (!dict_parser_consume(p, '"')) {
        dict_parser_error(p, "Expected opening quote for string");
        return NULL;
    }
    // Parse characters until closing quote or error
    //size_t start_pos = p->pos;
    size_t out_capacity = 64;
    size_t out_length = 0;
    char *out = (char *)malloc(out_capacity);
    if (!out) {
        dict_parser_error(p, "Out of memory");
        return NULL;
    }

    while (p->pos < p->buffer_len) {
        char c = p->buffer[p->pos++];

        if (c == '"') {
            // End of string
            if (out_length >= out_capacity) {
                char *tmp = (char *)realloc(out, out_length + 1);
                if (!tmp) {
                    free(out);
                    dict_parser_error(p, "Out of memory");
                    return NULL;
                }
                out = tmp;
            }
            out[out_length] = '\0';
            return out;
        } else if (c == '\\') {
            // Escape sequence
            if (p->pos >= p->buffer_len) {
                free(out);
                dict_parser_error(p, "Unexpected end of input in escape sequence");
                return NULL;
            }
            char esc = p->buffer[p->pos++];
            char decoded = 0;
            switch (esc) {
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                case 'u': {
                    // Unicode escape; parse 4 hex digits
                    if (p->pos + 4 > p->buffer_len) {
                        free(out);
                        dict_parser_error(p, "Incomplete unicode escape");
                        return NULL;
                    }
                    unsigned int codepoint = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = p->buffer[p->pos++];
                        unsigned int val = 0;
                        if (h >= '0' && h <= '9') val = h - '0';
                        else if (h >= 'a' && h <= 'f') val = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') val = h - 'A' + 10;
                        else {
                            free(out);
                            dict_parser_error(p, "Invalid hex digit in unicode escape");
                            return NULL;
                        }
                        codepoint = (codepoint << 4) | val;
                    }
                    // For simplicity, only support codepoints <= 0x7F (ASCII)
                    if (codepoint <= 0x7F) {
                        decoded = (char)codepoint;
                    } else {
                        // Replace non-ASCII with '?'
                        decoded = '?';
                    }
                    break;
                }
                default:
                    free(out);
                    dict_parser_error(p, "Invalid escape sequence");
                    return NULL;
            }
            if (out_length + 1 >= out_capacity) {
                size_t new_capacity = out_capacity * 2;
                char *tmp = (char *)realloc(out, new_capacity);
                if (!tmp) {
                    free(out);
                    dict_parser_error(p, "Out of memory");
                    return NULL;
                }
                out = tmp;
                out_capacity = new_capacity;
            }
            out[out_length++] = decoded;
        } else {
            // Normal char
            if ((unsigned char)c < 0x20) {
                free(out);
                dict_parser_error(p, "Unescaped control character in string");
                return NULL;
            }
            if (out_length + 1 >= out_capacity) {
                size_t new_capacity = out_capacity * 2;
                char *tmp = (char *)realloc(out, new_capacity);
                if (!tmp) {
                    free(out);
                    dict_parser_error(p, "Out of memory");
                    return NULL;
                }
                out = tmp;
                out_capacity = new_capacity;
            }
            out[out_length++] = c;
        }
    }
    free(out);
    dict_parser_error(p, "Unterminated string");
    return NULL;
}

// Parse a JSON number (integer or floating).
// Returns 1 on success and sets *out_num, else 0 on failure.
// Advances p->pos.
static int dict_parse_number(DictJsonParser *p, double *out_num) {
    size_t start_pos = p->pos;
    if (start_pos >= p->buffer_len) return 0;

    // JSON number pattern: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    // We'll scan chars that could belong to a JSON number.

    size_t pos = p->pos;
    if (p->buffer[pos] == '-') pos++;

    if (pos >= p->buffer_len) return 0;

    if (p->buffer[pos] == '0') {
        pos++;
    } else if (isdigit((unsigned char)p->buffer[pos])) {
        while (pos < p->buffer_len && isdigit((unsigned char)p->buffer[pos])) pos++;
    } else {
        return 0;
    }

    if (pos < p->buffer_len && p->buffer[pos] == '.') {
        pos++;
        if (pos >= p->buffer_len || !isdigit((unsigned char)p->buffer[pos])) return 0;
        while (pos < p->buffer_len && isdigit((unsigned char)p->buffer[pos])) pos++;
    }

    if (pos < p->buffer_len && (p->buffer[pos] == 'e' || p->buffer[pos] == 'E')) {
        pos++;
        if (pos < p->buffer_len && (p->buffer[pos] == '+' || p->buffer[pos] == '-')) pos++;
        if (pos >= p->buffer_len || !isdigit((unsigned char)p->buffer[pos])) return 0;
        while (pos < p->buffer_len && isdigit((unsigned char)p->buffer[pos])) pos++;
    }

    size_t num_len = pos - p->pos;
    if (num_len == 0) return 0;

    // Copy the number substring into a null-terminated buffer
    char numbuf[64];
    if (num_len >= sizeof(numbuf)) {
        dict_parser_error(p, "Number too long");
        return 0;
    }
    memcpy(numbuf, p->buffer + p->pos, num_len);
    numbuf[num_len] = '\0';

    char *endptr = NULL;
    double val = strtod(numbuf, &endptr);
    if (endptr != numbuf + num_len) {
        dict_parser_error(p, "Invalid number format");
        return 0;
    }

    *out_num = val;
    p->pos = pos;
    return 1;
}

// Forward declarations for array and object
static DictValue *dict_parse_array(DictJsonParser *p);
static DictValue *dict_parse_object(DictJsonParser *p);

// Parse any JSON value
static DictValue *dict_parse_value(DictJsonParser *p) {
    dict_parser_skip_whitespace(p);
    char c = 0;
    if (!dict_parser_peek(p, &c)) {
        dict_parser_error(p, "Unexpected end of input");
        return NULL;
    }

    if (c == '"') {
        char *str = dict_parse_json_string(p);
        if (!str) return NULL;
        DictValue *val = dict_create_string(str);
        free(str);
        return val;
    } else if (c == '{') {
        return dict_parse_object(p);
    } else if (c == '[') {
        return dict_parse_array(p);
    } else if ((c == '-') || (c >= '0' && c <= '9')) {
        double num;
        if (!dict_parse_number(p, &num)) return NULL;
        return dict_create_number(num);
    } else if (dict_parse_literal(p, "true")) {
        return dict_create_bool(1);
    } else if (dict_parse_literal(p, "false")) {
        return dict_create_bool(0);
    } else if (dict_parse_literal(p, "null")) {
        return dict_create_null();
    } else {
        dict_parser_error(p, "Invalid value");
        return NULL;
    }
}

// Parse JSON array: [ value, value, ... ]
static DictValue *dict_parse_array(DictJsonParser *p) {
    if (!dict_parser_consume(p, '[')) {
        dict_parser_error(p, "Expected '[' for array");
        return NULL;
    }
    dict_parser_skip_whitespace(p);

    DictValue *array = dict_create_array();
    if (!array) {
        dict_parser_error(p, "Out of memory creating array");
        return NULL;
    }

    // Temporary storage for items — reallocate as needed
    size_t capacity = 4;
    size_t length = 0;
    DictValue **items = (DictValue **)malloc(capacity * sizeof(DictValue *));
    if (!items) {
        dict_destroy(array);
        dict_parser_error(p, "Out of memory allocating array items");
        return NULL;
    }

    dict_parser_skip_whitespace(p);
    char c=0;
    if (dict_parser_peek(p, &c) && c == ']') {
        p->pos++; // consume ']'
        array->array_value.items = NULL;
        array->array_value.length = 0;
        free(items);
        return array;
    }

    while (1) {
        DictValue *item = dict_parse_value(p);
        if (!item) {
            // Cleanup all allocated items
            for (size_t i = 0; i < length; i++) {
                dict_destroy(items[i]);
            }
            free(items);
            dict_destroy(array);
            return NULL;
        }
        if (length == capacity) {
            size_t new_capacity = capacity * 2;
            DictValue **tmp = (DictValue **)realloc(items, new_capacity * sizeof(DictValue *));
            if (!tmp) {
                for (size_t i = 0; i < length; i++) {
                    dict_destroy(items[i]);
                }
                dict_destroy(item);
                free(items);
                dict_destroy(array);
                dict_parser_error(p, "Out of memory resizing array");
                return NULL;
            }
            items = tmp;
            capacity = new_capacity;
        }
        items[length++] = item;

        dict_parser_skip_whitespace(p);

        if (!dict_parser_peek(p, &c)) {
            dict_parser_error(p, "Unexpected end of input in array");
            for (size_t i = 0; i < length; i++) {
                dict_destroy(items[i]);
            }
            free(items);
            dict_destroy(array);
            return NULL;
        }
        if (c == ',') {
            p->pos++;
            dict_parser_skip_whitespace(p);
            continue;
        } else if (c == ']') {
            p->pos++;
            break;
        } else {
            dict_parser_error(p, "Expected ',' or ']' in array");
            for (size_t i = 0; i < length; i++) {
                dict_destroy(items[i]);
            }
            free(items);
            dict_destroy(array);
            return NULL;
        }
    }

    // Set array items and length
    array->array_value.items = items;
    array->array_value.length = length;
    return array;
}

// Parse JSON object: { "key": value, ... }
static DictValue *dict_parse_object(DictJsonParser *p) {
    if (!dict_parser_consume(p, '{')) {
        dict_parser_error(p, "Expected '{' for object");
        return NULL;
    }
    dict_parser_skip_whitespace(p);

    DictValue *obj = dict_create_object();
    if (!obj) {
        dict_parser_error(p, "Out of memory creating object");
        return NULL;
    }

    dict_parser_skip_whitespace(p);
    char c=0;
    if (dict_parser_peek(p, &c) && c == '}') {
        p->pos++; // consume '}'
        return obj;
    }

    while (1) {
        dict_parser_skip_whitespace(p);

        if (!dict_parser_peek(p, &c) || c != '"') {
            dict_parser_error(p, "Expected string quote \" in object");
            dict_destroy(obj);
            return NULL;
        }

        char *key = dict_parse_json_string(p);
        if (!key) {
            dict_destroy(obj);
            return NULL;
        }

        dict_parser_skip_whitespace(p);

        if (!dict_parser_consume(p, ':')) {
            free(key);
            dict_destroy(obj);
            dict_parser_error(p, "Expected ':' after key in object");
            return NULL;
        }

        dict_parser_skip_whitespace(p);

        DictValue *value = dict_parse_value(p);
        if (!value) {
            free(key);
            dict_destroy(obj);
            return NULL;
        }

        if (!dict_object_set(obj, key, value)) {
            free(key);
            dict_destroy(value);
            dict_destroy(obj);
            dict_parser_error(p, "Failed to insert key-value pair in object");
            return NULL;
        }
        free(key);

        dict_parser_skip_whitespace(p);

        if (!dict_parser_peek(p, &c)) {
            dict_destroy(obj);
            dict_parser_error(p, "Unexpected end of input in object");
            return NULL;
        }
        if (c == ',') {
            p->pos++;
            continue;
        }
        if (c == '}') {
            p->pos++;
            break;
        }
        dict_destroy(obj);
        dict_parser_error(p, "Expected ',' or '}' in object");
        return NULL;
    }
    return obj;
}

// Public API function: Parse JSON string to DictValue object.
// buffer: input JSON buffer (not necessarily null-terminated)
// buffer_len: total buffer capacity in bytes
// content_len: length of JSON content within buffer
// error_str: optional output buffer for error message (can be NULL)
// error_str_len: size of error buffer (ignored if error_str is NULL)
// Returns allocated DictValue (caller must free with dict_destroy) or NULL on error.
static DictValue *dict_deserialize_json(const char *buffer, size_t buffer_len, size_t content_len, char *error_str, size_t error_str_len) {
    if (!buffer || content_len > buffer_len || content_len == 0) {
        if (error_str && error_str_len > 0)
            snprintf(error_str, error_str_len, "Invalid input buffer or content length");
        return NULL;
    }

    DictJsonParser parser = {
        .buffer = buffer,
        .buffer_len = content_len,
        .pos = 0,
        .error_str = error_str,
        .error_str_len = error_str_len
    };

    dict_parser_skip_whitespace(&parser);
    DictValue *result = dict_parse_value(&parser);
    if (!result) {
        // error_str already set in parser
        return NULL;
    }

    dict_parser_skip_whitespace(&parser);
    if (parser.pos != content_len) {
        dict_destroy(result);
        if (error_str && error_str_len > 0) {
            snprintf(error_str, error_str_len, "Extra trailing data after JSON value at pos %zu", parser.pos);
        }
        return NULL;
    }

    return result;
}

// --- BSON serialization and deserialization with int64 support added ---

/* BSON constants and helpers */
// BSON type tags for your dict, plus int64:
#define BSON_TYPE_DOUBLE   0x01
#define BSON_TYPE_STRING   0x02
#define BSON_TYPE_DOCUMENT 0x03
#define BSON_TYPE_ARRAY    0x04
#define BSON_TYPE_BOOL     0x08
#define BSON_TYPE_NULL     0x0A
#define BSON_TYPE_INT64    0x12  // new BSON int64 type

// Helper functions:
static void bson_write_int32_le(uint8_t *buf, int32_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}
static int32_t bson_read_int32_le(const uint8_t *buf) {
    return (int32_t)(buf[0]) |
           ((int32_t)(buf[1]) << 8) |
           ((int32_t)(buf[2]) << 16) |
           ((int32_t)(buf[3]) << 24);
}

static void bson_write_int64_le(uint8_t *buf, int64_t val) {
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)((val >> (8 * i)) & 0xFF);
    }
}
static int64_t bson_read_int64_le(const uint8_t *buf) {
    int64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= ((int64_t)buf[i]) << (8 * i);
    }
    return val;
}

// Forward declaration
static int bson_serialize_value(const DictValue *val, uint8_t *buf, size_t buf_len, size_t *written);
static DictValue *bson_deserialize_document(const uint8_t *buf, size_t buf_len, size_t *read_bytes, int is_root);

// Serialize document or array (keys "0", "1", ...)
static int bson_serialize_document(const DictValue *val, uint8_t *buf, size_t buf_len, size_t *written) {
    if (!val || !buf || !written) return 0;
    if (val->type != DICT_OBJECT && val->type != DICT_ARRAY) return 0;

    size_t pos = 4; // reserve length space
    size_t count = (val->type == DICT_OBJECT) ? val->object_value.count : val->array_value.length;

    for (size_t i = 0; i < count; i++) {
        uint8_t type_byte = 0;
        const char *key = NULL;
        const DictValue *item_val = NULL;

        if (val->type == DICT_OBJECT) {
            key = val->object_value.pairs[i].key;
            item_val = val->object_value.pairs[i].value;
        } else {
            static char index_key[32];
            snprintf(index_key, sizeof(index_key), "%zu", i);
            key = index_key;
            item_val = val->array_value.items[i];
        }

        if (!item_val || !key) return 0;

        switch (item_val->type) {
            case DICT_NULL:   type_byte = BSON_TYPE_NULL; break;
            case DICT_BOOL:   type_byte = BSON_TYPE_BOOL; break;
            case DICT_NUMBER: type_byte = BSON_TYPE_DOUBLE; break;
            case DICT_INT64:  type_byte = BSON_TYPE_INT64; break;  // new int64 type tag
            case DICT_STRING: type_byte = BSON_TYPE_STRING; break;
            case DICT_ARRAY:  type_byte = BSON_TYPE_ARRAY; break;
            case DICT_OBJECT: type_byte = BSON_TYPE_DOCUMENT; break;
            default: return 0; // unsupported
        }

        if (pos + 1 >= buf_len) return 0;
        buf[pos++] = type_byte;

        size_t key_len = strlen(key);
        if (pos + key_len + 1 >= buf_len) return 0;
        memcpy(buf + pos, key, key_len);
        pos += key_len;
        buf[pos++] = 0;

        size_t val_written = 0;
        if (!bson_serialize_value(item_val, buf + pos, buf_len - pos, &val_written)) return 0;

        pos += val_written;
    }

    if (pos >= buf_len) return 0;
    buf[pos++] = 0; // terminator

    bson_write_int32_le(buf, (int32_t)pos);
    *written = pos;
    return 1;
}

// Serialize single value without type or key
static int bson_serialize_value(const DictValue *val, uint8_t *buf, size_t buf_len, size_t *written) {
    if (!val || !buf || !written) return 0;

    switch (val->type) {
        case DICT_NULL:
            *written = 0;
            return 1;

        case DICT_BOOL:
            if (buf_len < 1) return 0;
            buf[0] = val->bool_value ? 1 : 0;
            *written = 1;
            return 1;

        case DICT_NUMBER:
            if (buf_len < 8) return 0;
            {
                union {
                    double d;
                    uint8_t b[8];
                } u;
                u.d = val->number_value;
                for (int i = 0; i < 8; i++) {
                    buf[i] = u.b[i];
                }
                *written = 8;
            }
            return 1;

        case DICT_INT64:
            if (buf_len < 8) return 0;
            bson_write_int64_le(buf, val->int64_value);
            *written = 8;
            return 1;

        case DICT_STRING:
            {
                if (!val->string_value) return 0;
                size_t str_len = strlen(val->string_value);
                size_t total_len = 4 + str_len + 1;
                if (buf_len < total_len) return 0;
                bson_write_int32_le(buf, (int32_t)(str_len + 1));
                memcpy(buf + 4, val->string_value, str_len);
                buf[4 + str_len] = 0;
                *written = total_len;
                return 1;
            }
        case DICT_ARRAY:
        case DICT_OBJECT:
            return bson_serialize_document(val, buf, buf_len, written);

        default:
            return 0;
    }
}


// Optimized deserialize document that builds DICT_OBJECT or DICT_ARRAY directly,
// avoiding intermediate object-to-array conversion.

static DictValue *bson_deserialize_document_internal(const uint8_t *buf, size_t buf_len, size_t *read_bytes, int is_array) {
    if (!buf || buf_len < 5 || !read_bytes) return NULL;

    int32_t doc_len = bson_read_int32_le(buf);
    if (doc_len < 5 || (size_t)doc_len > buf_len) return NULL;

    size_t pos = 4; // past length

    DictValue *result = is_array ? dict_create_array() : dict_create_object();
    if (!result) return NULL;

    while (pos < (size_t)doc_len - 1) {
        uint8_t type_byte = buf[pos++];
        if (type_byte == 0) break;

        // Read CString key:
        size_t key_start = pos;
        while (pos < (size_t)doc_len && buf[pos] != 0) pos++;
        if (pos == (size_t)doc_len) goto fail;
        //size_t key_len = pos - key_start;
        const char *key_ptr = (const char *)(buf + key_start);
        pos++;

        size_t val_read = 0;
        DictValue *val = NULL;

        // Parsing value based on type_byte:
        switch(type_byte) {
            case BSON_TYPE_DOUBLE:
                if (pos + 8 > (size_t)doc_len) goto fail;
                {
                    union { double d; uint8_t b[8]; } u;
                    for (int i = 0; i < 8; i++) u.b[i] = buf[pos + i];
                    val = dict_create_number(u.d);
                    val_read = 8;
                } break;
            case BSON_TYPE_INT64:
                if (pos + 8 > (size_t)doc_len) goto fail;
                {
                    int64_t n = bson_read_int64_le(buf + pos);
                    val = dict_create_int64(n);
                    val_read = 8;
                } break;
            case BSON_TYPE_STRING:
                if (pos + 4 > (size_t)doc_len) goto fail;
                {
                    int32_t str_len = bson_read_int32_le(buf + pos);
                    if (str_len < 1 || pos + 4 + (size_t)str_len > (size_t)doc_len) goto fail;
                    // Copy string; consider keeping pointer and length to avoid allocation if your API supports it.
                    char *str = (char *)malloc(str_len);
                    if (!str) goto fail;
                    memcpy(str, buf + pos + 4, str_len);
                    str[str_len-1] = 0;
                    val = dict_create_string(str);
                    free(str);
                    val_read = 4 + (size_t)str_len;
                } break;
            case BSON_TYPE_BOOL:
                if (pos + 1 > (size_t)doc_len) goto fail;
                val = dict_create_bool(buf[pos] != 0);
                val_read = 1;
                break;
            case BSON_TYPE_NULL:
                val = dict_create_null();
                val_read = 0;
                break;
            case BSON_TYPE_DOCUMENT:
                val = bson_deserialize_document_internal(buf + pos, (size_t)doc_len - pos, &val_read, 0);
                if (!val) goto fail;
                break;
            case BSON_TYPE_ARRAY:
                val = bson_deserialize_document_internal(buf + pos, (size_t)doc_len - pos, &val_read, 1);
                if (!val) goto fail;
                break;
            default:
                goto fail;
        }

        if (!val) goto fail;

        if (is_array) {
            // key_ptr is "0", "1", etc. but we just append in order expecting correct order
            if (!dict_array_append(result, val)) { dict_destroy(val); goto fail;}
        } else {
            // For objects, key must be copied - make a strdup of key_ptr before storing
            char *key_copy = dict_strdup(key_ptr);
            if (!key_copy) { dict_destroy(val); goto fail; }
            if (!dict_object_set(result, key_copy, val)) {
                free(key_copy);
                dict_destroy(val);
                goto fail;
            }
            free(key_copy); // dict_object_set copies key, so free ours
        }

        pos += val_read;
    }
	pos++; // eat final byte

    if (pos != (size_t)doc_len) goto fail;

    *read_bytes = pos;
    return result;

fail:
    dict_destroy(result);
    return NULL;
}

// Public API: serialize to BSON
static size_t dict_serialize_bson(const DictValue *val, uint8_t *buf, size_t buf_len) {
    size_t written = 0;
    if (!bson_serialize_document(val, buf, buf_len, &written)) {
        return 0;
    }
    return written;
}

// Public API: deserialize BSON
static DictValue *dict_deserialize_bson(const uint8_t *buf, size_t buf_len) {
    size_t read_bytes = 0;
    DictValue *val = bson_deserialize_document_internal(buf, buf_len, &read_bytes, 0);
    if (!val) return NULL;
    if (read_bytes != buf_len) {
        dict_destroy(val);
        return NULL;
    }
    return val;
}


#ifdef __cplusplus
}
#endif

#endif // DICT_H


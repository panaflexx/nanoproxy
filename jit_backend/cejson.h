/* cejson.h – Ultra-fast zero-copy streaming JSON parser + serializer (FINAL, MAX-SPEED EDITION) */
/* (C) 2025 Roger Davenport */
/* LGPL 2.1 license */
#ifndef CEJSON_H
#define CEJSON_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdio.h>
#define STRINGBUF_IMPLEMENTATION
#include "stringbuf.h"

/* Debug levels */
typedef enum {
    DBG_DEBUG = 0,
    DBG_INFO,
    DBG_WARN,
    DBG_ERROR
} DbgLevel;

/* Default: only warnings + errors in release builds */
#ifndef DEBUGLEVEL
#define DEBUGLEVEL DBG_DEBUG
#endif

#ifdef DEBUG
    #define dprintf(level, ...)                     \
        do {                                        \
            if (unlikely((level) >= DEBUGLEVEL)) { \
                fprintf(stderr, __VA_ARGS__);      \
            }                                       \
        } while (0)
#else
    #define dprintf(level, ...) do {} while (0)
#endif

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#ifndef MAX
#define MAX(a,b) ((a)<(b)?(b):(a))
#endif

typedef enum {
    JSON_NULL = 0,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NUMBER_INT,
    JSON_NUMBER_FLOAT,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct {
    uint32_t type : 4;
    uint32_t hash  : 28;
    uint32_t offset;   // absolute offset in the final concatenated buffer
    uint32_t len;
    uint32_t children;
	char*    strval;  // builder string
} JsonNode;

typedef enum {
    PS_NORMAL,
    PS_AFTER_VALUE,
    PS_EXPECT_COLON,
    PS_IN_STRING,
    PS_IN_NUMBER,
    PS_IN_LITERAL
} ParseState;

static const char * const ParseStateStr[] = {
    "PS_NORMAL",
    "PS_AFTER_VALUE",
    "PS_EXPECT_COLON",
    "PS_IN_STRING",
    "PS_IN_NUMBER",
    "PS_IN_LITERAL"
};

typedef enum {
    LIT_NONE = 0,
    LIT_TRUE,
    LIT_FALSE,
    LIT_NULL
} LiteralType;

typedef struct {
    const char* buffer;
    uint64_t    buf_len;
    uint64_t    consumed;
	uint32_t	line;

    JsonNode*   nodes;
    uint64_t    nodes_cap;
    uint64_t    nodes_len;

    uint32_t*   stack;
    uint64_t    stack_cap;
    uint64_t    stack_len;

    uint8_t*    expecting_key;

    int         error;
    uint64_t    error_pos;

    ParseState  state;
    uint64_t    pending_offset;
    uint32_t    pending_len;
    uint32_t    pending_hash;
    bool        in_escape;
    bool        in_uni_escape;
    uint8_t     uni_digits;
    bool        is_key_string;
    bool        num_has_dot;
    bool        num_has_exp;
    bool        num_has_digit;
    bool        num_has_digit_after_dot;
    bool        num_has_digit_after_exp;
    bool        num_ends_with_dot;
    bool        num_ends_with_e;
    bool        num_ends_with_esgn;
    bool        num_is_negative;
    LiteralType pending_literal;
    uint32_t    literal_matched;   // renamed – now counts matched characters (1-based on start)
	bool		pending_value;

    /* Opt-in, default false (json_init zeroes the whole struct, so
     * existing callers are unaffected): skip computing json_create_string's
     * per-character hash. That hash exists solely to make
     * json_get_object_value()'s key lookup fast; a pure builder-then-
     * serialize caller that never looks a key back up by name is paying
     * for a hash it will never read. Set p->skip_string_hash = true right
     * after json_init() if you're only building + dumping, never calling
     * json_get_object_value() / json_object_get() on this parser's tree. */
    bool        skip_string_hash;
} JsonParser;

#define JSON_ERR_NONE       0
#define JSON_ERR_UNEXPECTED 1
#define JSON_ERR_INCOMPLETE 2
#define JSON_ERR_CAPACITY   3

static const char * const JsonErrorStr[] = {
    "JSON_ERR_NONE",
    "JSON_ERR_UNEXPECTED",
    "JSON_ERR_INCOMPLETE",
    "JSON_ERR_CAPACITY"
};

static void json_dump_node(JsonParser* p, const JsonNode* node, FILE* out, int indent, bool pretty);

static inline void json_init(JsonParser* p,
                             JsonNode* nodes, uint64_t nodes_cap,
                             uint32_t* stack, uint64_t stack_cap,
                             uint8_t* expecting_key)
{
	memset(p, 0, sizeof(JsonParser));
    p->buffer = NULL;
    p->buf_len = p->consumed = p->line = 0;
    p->nodes = nodes; p->nodes_cap = nodes_cap; p->nodes_len = 0;
    p->stack = stack; p->stack_cap = stack_cap; p->stack_len = 0;
    p->expecting_key = expecting_key;
    p->error = JSON_ERR_NONE;
    p->error_pos = 0;
    p->state = PS_NORMAL;
	p->pending_len = 0;
    p->pending_literal = LIT_NONE;
    p->literal_matched = 0;
    p->in_escape = p->in_uni_escape = false;
    p->uni_digits = 0;
    p->num_has_dot = p->num_has_exp = p->num_has_digit = p->num_has_digit_after_dot = p->num_has_digit_after_exp = false;
	p->num_ends_with_dot = p->num_ends_with_e = p->num_ends_with_esgn = p->num_is_negative = false;
	//memset(nodes, 0, sizeof(JsonNode) * nodes_cap);
}

static inline void skip_ws(const char* data, uint64_t len, uint64_t* pos, uint32_t* line)
{
    while (*pos < len) {
        char c = data[*pos];
		if (c == '\n' || c == '\r') (*line)++;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        (*pos)++;
    }
}

static inline void boop() { printf("CAPACITY BOOP\n"); }
static inline void poop(JsonParser *p)
{
    printf("UNEXPECTED POOP, state=%s line=%u pos=%llu\n", ParseStateStr[p->state], p->line + 1, p->error_pos);

    // Safely calculate snippet start and length
    uint64_t start = (p->error_pos > 20) ? p->error_pos - 20 : 0;
    uint64_t snippet_len = (p->buf_len - start > 40) ? 40 : p->buf_len - start;

    // Print the snippet
    printf("%.*s\n", (int)snippet_len, p->buffer + start);

    // Print caret ^ at the error position (relative to start)
    for (uint64_t i = 0; i < p->error_pos - start; ++i) putchar(' ');
    printf("^\n");
}

/* Ultra-tight, fully streaming-safe json_feed – now correctly handles \uXXXX and literals split across chunks */
static inline bool json_feed(JsonParser* p, const char* data, uint64_t len)
{
    if (unlikely(p->error)) return false;

    p->buffer = data;
    p->buf_len = len;

    uint64_t pos = 0;

    while (pos < len) {
		if(p->state == PS_NORMAL || p->state == PS_AFTER_VALUE)
			skip_ws(data, len, &pos, &p->line);

        if (unlikely(pos >= len)) break;

        char c = data[pos];
		dprintf(DBG_DEBUG, "%s [%c] pending_len = %u\n", ParseStateStr[p->state], c, p->pending_len);

        /* ---------- expect colon after key ---------- */
        if (p->state == PS_EXPECT_COLON) {
            if (unlikely(c != ':')) {
                p->error = JSON_ERR_UNEXPECTED;
                p->error_pos = p->consumed + pos;
				poop(p);
                return false;
            }
            p->expecting_key[p->stack_len - 1] = 0;
            p->state = PS_NORMAL;
            pos++;
            continue;
        }

        /* ---------- literal handling (true/false/null) ---------- */
        if (p->state == PS_IN_LITERAL) {
            const char* expected;
            uint32_t total;
            JsonType target;

            switch (p->pending_literal) {
                case LIT_TRUE:  expected = "true";  total = 4; target = JSON_TRUE;   break;
                case LIT_FALSE: expected = "false"; total = 5; target = JSON_FALSE;  break;
                case LIT_NULL:  expected = "null";  total = 4; target = JSON_NULL;   break;
                default: __builtin_unreachable();
            }

            if (unlikely(c != expected[p->literal_matched])) {
                p->error = JSON_ERR_UNEXPECTED;
                p->error_pos = p->consumed + pos;
				poop(p);
                return false;
            }

            p->literal_matched++;
            pos++;

            if (p->literal_matched == total) {
                JsonNode node = { .type = target, .offset = p->pending_offset, .len = total };
                uint64_t idx = p->nodes_len++;
                if (unlikely(idx >= p->nodes_cap)) { p->error = JSON_ERR_CAPACITY; boop(); return false; }
                p->nodes[idx] = node;

                if (p->stack_len && p->nodes[p->stack[p->stack_len - 1]].type == JSON_OBJECT &&
                    idx > 0 && p->nodes[idx - 1].type == JSON_STRING) {
                    p->nodes[idx].hash = p->nodes[idx - 1].hash;
                }
                if (p->stack_len) p->nodes[p->stack[p->stack_len - 1]].children++;

                p->state = PS_AFTER_VALUE;
                p->pending_literal = LIT_NONE;
                p->literal_matched = 0;
            }
            continue;
        }

        if (p->state == PS_IN_STRING) {
            if (p->in_uni_escape) {
                unsigned char uc = (unsigned char)c;
                if (unlikely(!((uc >= '0' && uc <= '9') ||
                               (uc >= 'A' && uc <= 'F') ||
                               (uc >= 'a' && uc <= 'f')))) {
                    p->error = JSON_ERR_UNEXPECTED;
                    p->error_pos = p->consumed + pos;
					poop(p);
                    return false;
                }
                p->uni_digits++;
                if (p->uni_digits == 4) p->in_uni_escape = false;

                p->pending_len++;
                pos++;
                continue;
            }

            if (p->in_escape) {
                p->in_escape = false;
                switch (c) {
                    case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
                        break;
                    case 'u':
                        p->in_uni_escape = true;
                        p->uni_digits = 0;
                        break;
                    default:
                        p->error = JSON_ERR_UNEXPECTED;
                        p->error_pos = p->consumed + pos;
						poop(p);
                        return false;
                }

                p->pending_len++;
                pos++;
                continue;
            }

            if (c == '\\') {
                p->in_escape = true;
                p->pending_len++;
                pos++;
                continue;
            }

            if (c == '"') {
                JsonNode n = { .type = JSON_STRING, .offset = p->pending_offset, .len = p->pending_len,
                               .hash = p->is_key_string ? p->pending_hash : 0 };
#ifdef DEBUG
				json_dump_node(p, &n, stdout, 4, true); fputs("\n", stdout);
#endif

                uint64_t idx = p->nodes_len++;
                if (unlikely(idx >= p->nodes_cap)) { p->error = JSON_ERR_CAPACITY; boop(); return false; }
                p->nodes[idx] = n;

                if (p->stack_len && !p->is_key_string) p->nodes[p->stack[p->stack_len - 1]].children++;

                pos++;
                p->state = p->is_key_string ? PS_EXPECT_COLON : PS_AFTER_VALUE;
				if(p->state == PS_EXPECT_COLON) p->pending_value = true;
                p->in_escape = p->in_uni_escape = false;
                p->uni_digits = 0;
                continue;
            }

            /* normal character */
            p->pending_len++;
            if (p->is_key_string) p->pending_hash = p->pending_hash * 33 ^ (unsigned char)c;
            pos++;
            continue;
        } 

        /* ---------- number handling ---------- */
        if (p->state == PS_IN_NUMBER) {
            /* ... (unchanged except tiny cleanups – original was already excellent) */
            if (c >= '0' && c <= '9') {
                p->num_has_digit = true;
                if (p->num_has_dot) p->num_has_digit_after_dot = true;
                if (p->num_has_exp) p->num_has_digit_after_exp = true;
                p->num_ends_with_dot = p->num_ends_with_e = p->num_ends_with_esgn = false;
                goto num_char_consumed;
            }
            if (c == '.' && !p->num_has_dot && !p->num_has_exp) { p->num_has_dot = true; p->num_ends_with_dot = true; goto num_char_consumed; }
            if ((c == 'e' || c == 'E') && !p->num_has_exp && p->num_has_digit) { p->num_has_exp = true; p->num_ends_with_e = true; goto num_char_consumed; }
            if ((c == '+' || c == '-') && p->num_ends_with_e) { p->num_ends_with_esgn = true; p->num_ends_with_e = false; goto num_char_consumed; }

            /* end of number – validate */
            if (unlikely(!p->num_has_digit || (p->num_is_negative && p->pending_len == 1) ||
                         (p->num_has_dot && !p->num_has_digit_after_dot) ||
                         (p->num_has_exp && !p->num_has_digit_after_exp) ||
                         p->num_ends_with_dot || p->num_ends_with_e || p->num_ends_with_esgn)) {
                p->error = JSON_ERR_UNEXPECTED;
                p->error_pos = p->consumed + pos;
				poop(p);
                return false;
            }

            JsonNode node = {
                .type = (p->num_has_dot || p->num_has_exp) ? JSON_NUMBER_FLOAT : JSON_NUMBER_INT,
                .offset = p->pending_offset,
                .len = p->pending_len
            };
            uint64_t idx = p->nodes_len++;
            if (unlikely(idx >= p->nodes_cap)) { p->error = JSON_ERR_CAPACITY; boop(); return false; }
            p->nodes[idx] = node;

            if (p->stack_len && p->nodes[p->stack[p->stack_len - 1]].type == JSON_OBJECT &&
                idx > 0 && p->nodes[idx - 1].type == JSON_STRING) {
                p->nodes[idx].hash = p->nodes[idx - 1].hash;
            }
            if (p->stack_len) p->nodes[p->stack[p->stack_len - 1]].children++;

            p->state = PS_AFTER_VALUE;
            continue;

        num_char_consumed:
            p->pending_len++;
            pos++;
            continue;
        }

        /* ---------- normal state / after-value / container close ---------- */
        if (p->state == PS_NORMAL || p->state == PS_AFTER_VALUE) {
            /* container close – works in both NORMAL and AFTER_VALUE */
			if (p->stack_len) {
				uint32_t top_type = p->nodes[p->stack[p->stack_len - 1]].type;
				if ((c == '}' && top_type == JSON_OBJECT) || (c == ']' && top_type == JSON_ARRAY)) {
					if(p->pending_value) {
						p->error = JSON_ERR_UNEXPECTED;
						p->error_pos = p->consumed + pos;
						poop(p);
						return false;  // missing value after key!
					}
					uint64_t open_idx = p->stack[--p->stack_len];
					p->nodes[open_idx].len = (uint32_t)(p->consumed + pos - p->nodes[open_idx].offset + 1);

					uint64_t content_nodes = p->nodes_len - (open_idx + 1);
					p->nodes[open_idx].hash = (uint32_t)content_nodes;

					p->state = PS_AFTER_VALUE;
					pos++;
					continue;
				}
			}

            /* AFTER_VALUE only allows comma or close – everything else goes to NORMAL path */
            if (p->state == PS_AFTER_VALUE) {
                if (c == ',') {
                    p->state = PS_NORMAL;
                    pos++;
                    if (p->stack_len && p->nodes[p->stack[p->stack_len - 1]].type == JSON_OBJECT) {
                        p->expecting_key[p->stack_len - 1] = 1;
                    }
                    continue;
                }
                p->error = JSON_ERR_UNEXPECTED;
                p->error_pos = p->consumed + pos;
				poop(p);
                return false;
            }

            bool expecting_key = p->stack_len && p->expecting_key[p->stack_len - 1];

            if (expecting_key) {
                if (unlikely(c != '"')) { p->error = JSON_ERR_UNEXPECTED; p->error_pos = p->consumed + pos; poop(p); return false; }
                p->state = PS_IN_STRING;
                p->is_key_string = true;
                p->pending_hash = 0;
                p->pending_offset = p->consumed + pos + 1;
                p->pending_len = 0;
                p->in_escape = false;
                pos++;
                continue;
            }

			p->pending_value = false;
            if (c == '"') { p->state = PS_IN_STRING; p->is_key_string = false; p->pending_offset = p->consumed + pos + 1; p->pending_len = 0; p->in_escape = false; pos++; continue; }
            if (c == '{') {
				JsonNode n = { .type = JSON_OBJECT, .offset = p->consumed + pos };
				uint64_t idx = p->nodes_len++;
				if (unlikely(idx >= p->nodes_cap)) {
					p->error = JSON_ERR_CAPACITY;
					boop();
					return false; 
				}
				p->nodes[idx] = n;
				p->expecting_key[p->stack_len] = 1;
				if (unlikely(p->stack_len >= p->stack_cap)) {
					p->error = JSON_ERR_CAPACITY;
					boop();
					return false;
				}
				p->stack[p->stack_len++] = idx;
				if (p->stack_len > 1) p->nodes[p->stack[p->stack_len - 2]].children++;
				pos++;
				continue;
			}
            if (c == '[') { JsonNode n = { .type = JSON_ARRAY, .offset = p->consumed + pos }; uint64_t idx = p->nodes_len++; if (unlikely(idx >= p->nodes_cap)) { p->error = JSON_ERR_CAPACITY; boop(); return false; } p->nodes[idx] = n; p->expecting_key[p->stack_len] = 0; if (unlikely(p->stack_len >= p->stack_cap)) { p->error = JSON_ERR_CAPACITY; boop(); return false; } p->stack[p->stack_len++] = idx; if (p->stack_len > 1) p->nodes[p->stack[p->stack_len - 2]].children++; pos++; continue; }
            if (c == '-' || (c >= '0' && c <= '9')) { p->state = PS_IN_NUMBER; p->pending_offset = p->consumed + pos; p->pending_len = 1; p->num_has_digit = (c >= '0' && c <= '9'); p->num_is_negative = (c == '-'); p->num_has_dot = p->num_has_exp = false; pos++; continue; }
            if (c == 't') { p->pending_literal = LIT_TRUE;  p->literal_matched = 1; p->pending_offset = p->consumed + pos; p->state = PS_IN_LITERAL; pos++; continue; }
            if (c == 'f') { p->pending_literal = LIT_FALSE; p->literal_matched = 1; p->pending_offset = p->consumed + pos; p->state = PS_IN_LITERAL; pos++; continue; }
            if (c == 'n') { p->pending_literal = LIT_NULL;  p->literal_matched = 1; p->pending_offset = p->consumed + pos; p->state = PS_IN_LITERAL; pos++; continue; }

            p->error = JSON_ERR_UNEXPECTED;
            p->error_pos = p->consumed + pos;
			poop(p);
            return false;
        }
    }

    p->consumed += pos;
    return true;
}

static inline bool json_finish(JsonParser* p)
{
    if (unlikely(p->error)) return false;
    if (unlikely(p->stack_len != 0)) { p->error = JSON_ERR_INCOMPLETE; return false; }

    if (p->state == PS_IN_NUMBER) {
        if (unlikely(!p->num_has_digit || (p->num_is_negative && p->pending_len == 1) ||
                     (p->num_has_dot && !p->num_has_digit_after_dot) ||
                     (p->num_has_exp && !p->num_has_digit_after_exp) ||
                     p->num_ends_with_dot || p->num_ends_with_e || p->num_ends_with_esgn)) {
            p->error = JSON_ERR_UNEXPECTED;
            return false;
        }
        JsonNode node = { .type = (p->num_has_dot || p->num_has_exp) ? JSON_NUMBER_FLOAT : JSON_NUMBER_INT,
                          .offset = p->pending_offset, .len = p->pending_len };
        uint64_t idx = p->nodes_len++;
        if (unlikely(idx >= p->nodes_cap)) { p->error = JSON_ERR_CAPACITY; boop(); return false; }
        p->nodes[idx] = node;
    }
    else if (unlikely(p->state == PS_IN_STRING || p->state == PS_IN_LITERAL)) {
        p->error = JSON_ERR_INCOMPLETE;
        return false;
    }

    return p->nodes_len > 0;
}

/* Number of flat p->nodes[] slots `node`'s own subtree occupies, not
 * counting any slot before it: 1 for a leaf (string/number/bool/null),
 * or 1 (its own header slot) + node->hash (its already-accumulated
 * descendant span) for an object/array. json_next_sibling() needs this
 * to skip a whole subtree in one jump instead of walking it. */
static inline uint32_t json_node_span(const JsonNode* node)
{
    if (node->type == JSON_OBJECT || node->type == JSON_ARRAY) return 1 + node->hash;
    return 1;
}

static inline void json_free_tree(JsonParser* p, JsonNode* root)
{
    if (!root) return;
    /* `children` is the logical element/key-value-pair count, not the
     * flat p->nodes[] span a container's subtree actually occupies (an
     * object with 1 key-value pair has children==1 but spans 3 slots:
     * itself, the key, the value). Using `children` here under-walks any
     * subtree containing a nested object/array, leaking those
     * descendants' strval buffers. */
    uint64_t start = root - p->nodes;
    uint64_t end = start + json_node_span(root);

    for (uint64_t i = start; i < end && i < p->nodes_len; ++i) {
        if (p->nodes[i].strval)
            free(p->nodes[i].strval);
    }
}

static inline char* json_str(JsonParser* p, const JsonNode* n,
                             char* tmpstr, size_t strLen)
{
    if (!n || n->type != JSON_STRING) {
        if (strLen > 0) tmpstr[0] = '\0';
        return tmpstr;
    }

    const char* src = n->strval ? n->strval : p->buffer + n->offset;
    size_t len = n->len;

    if (len < strLen) {
        memcpy(tmpstr, src, len);
        tmpstr[len] = '\0';
        return tmpstr;
    } else {
        memcpy(tmpstr, src, strLen - 1);
        tmpstr[strLen - 1] = '\0';
        return tmpstr;
    }
}

static inline char* json_str_old(JsonParser* p, const JsonNode* n,
                             char* tmpstr, size_t strLen)
{
    if (!n || n->type != JSON_STRING) {
        if (strLen > 0) tmpstr[0] = '\0';
        return tmpstr;
    }

    if (n->len < strLen) {
        memcpy(tmpstr, p->buffer + n->offset, n->len);
        tmpstr[n->len] = '\0';
        return tmpstr;
    } else {
        memcpy(tmpstr, p->buffer + n->offset, strLen - 1);
        tmpstr[strLen - 1] = '\0';
        return tmpstr;
    }
}

static inline JsonNode* json_root(JsonParser* p) { return p->nodes_len ? &p->nodes[0] : NULL; }

static inline bool json_as_i64(JsonParser* p, const JsonNode* n, int64_t* out)
{
    const char* s = p->buffer + n->offset;
    char* end;
    *out = strtoll(s, &end, 10);
    return (size_t)(end - s) == n->len;
}

static inline bool json_as_f64(JsonParser* p, const JsonNode* n, double* out)
{
    const char* s = p->buffer + n->offset;
    char* end;
    *out = strtod(s, &end);
    return (size_t)(end - s) == n->len;
}

static inline bool json_as_bool(JsonParser* p, const JsonNode* n)
{
    (void)p;
    return n->type == JSON_TRUE;
}

static inline JsonNode* json_first_child(JsonParser* p, const JsonNode* parent)
{
    if (!parent || (parent->type != JSON_OBJECT && parent->type != JSON_ARRAY) || parent->children == 0) {
        return NULL;
    }
    uint64_t parent_idx = parent - p->nodes;
    return &p->nodes[parent_idx + 1];
}

static inline JsonNode* json_next_sibling(JsonParser* p, const JsonNode* node)
{
    if (!node) return NULL;
    uint64_t idx = node - p->nodes;
    uint64_t next_idx = idx + 1;
    if (node->type == JSON_OBJECT || node->type == JSON_ARRAY) {
        next_idx += node->hash;                // <-- changed from node->children
    }
    if (next_idx >= p->nodes_len) return NULL;
    return &p->nodes[next_idx];
}


static inline JsonNode* json_next_sibling_old(JsonParser* p, const JsonNode* node)
{
    if (!node) return NULL;
    uint64_t idx = node - p->nodes;
    uint64_t next_idx = idx + 1;
    if (node->type == JSON_OBJECT || node->type == JSON_ARRAY) {
        next_idx += node->children;
    }
    if (next_idx >= p->nodes_len) return NULL;
    return &p->nodes[next_idx];
}

static inline JsonNode* json_get_array_element(JsonParser* p, const JsonNode* arr, uint32_t index)
{
    if (!arr || arr->type != JSON_ARRAY || index >= arr->children) return NULL;
    JsonNode* child = json_first_child(p, arr);
    for (uint32_t i = 0; i < index; ++i) {
        child = json_next_sibling(p, child);
    }
    return child;
}

static inline uint32_t json_compute_hash(const char* key)
{
    uint32_t hash = 0;
    while (*key) {
        hash = hash * 33 ^ (uint8_t)*key++;
    }
    return hash;
}

static inline JsonNode* json_get_object_value(JsonParser* p, const JsonNode* obj, const char* key)
{
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    uint32_t target_hash = json_compute_hash(key);
    size_t key_len = strlen(key);
    JsonNode* child = json_first_child(p, obj);
    while (child) {
        if (child->type == JSON_STRING && child->hash == target_hash && child->len == key_len &&
            memcmp(p->buffer + child->offset, key, key_len) == 0) {
            return json_next_sibling(p, child);
        }
        child = json_next_sibling(p, child);
    }
    return NULL;
}

#define JSON_FOREACH_CHILD(p, parent, child) \
    for (JsonNode* child = json_first_child(p, parent); child != NULL; child = json_next_sibling(p, child))

#define JSON_FOREACH_OBJECT_PAIR(p, obj, key_node, value_node) \
    for (JsonNode* key_node = json_first_child(p, obj), *value_node = json_next_sibling(p, key_node); \
         key_node != NULL && value_node != NULL; \
         key_node = json_next_sibling(p, value_node), value_node = json_next_sibling(p, key_node))

/* ====================== SERIALIZER  ====================== */

static inline void json_dump_escape(FILE* out, const char* s, size_t len)
{
    fputc('"', out);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\b': fputs("\\b",  out); break;
            case '\f': fputs("\\f",  out); break;
            case '\n': fputs("\\n",  out); break;
            case '\r': fputs("\\r",  out); break;
            case '\t': fputs("\\t",  out); break;
            default:
                if (c < 0x20) {
                    fprintf(out, "\\u%04x", c);
                } else {
                    fputc(c, out);
                }
                break;
        }
    }
    fputc('"', out);
}

/* Fast path: a cheap pre-scan (simple byte comparisons an optimizing
 * compiler pipelines well) checks whether ANY byte in the string needs
 * escaping at all. Most real-world strings (names, post text, URLs)
 * need none -- for those, this writes the opening quote, the whole
 * content, and the closing quote with exactly ONE stringbuf_reserve
 * call and two direct memcpy-style writes, instead of the general path's
 * three separate stringbuf_append_char/stringbuf_append/
 * stringbuf_append_char calls (three reserve-checks instead of one).
 * Falls back to the batched-run escaping loop only for the strings that
 * actually contain something to escape. */
static inline void json_dump_escape_buf(StringBuf* sb, const char* s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char) s[i];
        if (c == '"' || c == '\\' || c < 0x20) break;
    }
    if (i == len) {
        if (!stringbuf_reserve(sb, sb->size + (ssize_t)len + 2)) return;
        char *p = sb->data + sb->size;
        *p = '"';
        if (len) memcpy(p + 1, s, len);
        p[len + 1] = '"';
        sb->size += (ssize_t)len + 2;
        sb->data[sb->size] = '\0';
        return;
    }

    stringbuf_append_char(sb, '"');
    size_t run_start = 0;
    for (size_t j = 0; j < len; ++j) {
        unsigned char c = (unsigned char) s[j];
        const char *esc;
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if (c < 0x20) {
                    if (j > run_start) stringbuf_append(sb, s + run_start, j - run_start);
                    stringbuf_appendf(sb, "\\u%04x", c);
                    run_start = j + 1;
                }
                continue; /* normal byte -- stays part of the current run */
        }
        if (j > run_start) stringbuf_append(sb, s + run_start, j - run_start);
        stringbuf_append_str(sb, esc);
        run_start = j + 1;
    }
    if (len > run_start) stringbuf_append(sb, s + run_start, len - run_start);
    stringbuf_append_char(sb, '"');
}

static void json_dump_node(JsonParser* p, const JsonNode* node,
                           FILE* out, int indent, bool pretty)
{
    if (!node) { fputs("null", out); return; }

    const char* src = node->strval ? node->strval : p->buffer + node->offset;

    switch (node->type) {
        case JSON_NULL:   fputs("null", out); break;
        case JSON_TRUE:   fputs("true", out); break;
        case JSON_FALSE:  fputs("false", out); break;

        case JSON_NUMBER_INT:
        case JSON_NUMBER_FLOAT:
            fwrite(src, 1, node->len, out);
            break;

        case JSON_STRING:
            json_dump_escape(out, src, node->len);
            break;

        case JSON_ARRAY: {
            if (node->children == 0) { fputs("[]", out); return; }

            fputc('[', out);
            if (pretty) fputc('\n', out);

            JsonNode* child = json_first_child(p, node);
            for (uint32_t i = 0; i < node->children; ++i) {
                if (pretty) for (int k = 0; k < indent + 2; ++k) fputc(' ', out);
                json_dump_node(p, child, out, indent + 2, pretty);
                child = json_next_sibling(p, child);
                if (i + 1 < node->children) {
                    fputc(',', out);
                    if (pretty) fputc('\n', out);
                }
            }
            if (pretty) {
                fputc('\n', out);
                for (int k = 0; k < indent; ++k) fputc(' ', out);
            }
            fputc(']', out);
            break;
        }

       case JSON_OBJECT: {
			if (node->children == 0) { fputs("{}", out); break; }

			fputc('{', out);
			if (pretty) fputc('\n', out);

			JsonNode* key_node = json_first_child(p, node);
			for (uint32_t i = 0; i < node->children; ++i) {
				JsonNode* value_node = json_next_sibling(p, key_node);

				if (pretty) for (int k = 0; k < indent + 2; ++k) fputc(' ', out);

				if(key_node->strval)
					json_dump_escape(out, key_node->strval, key_node->len);
				else
					json_dump_escape(out, p->buffer + key_node->offset, key_node->len);
				if (pretty) fputs(": ", out); else fputc(':', out);
				json_dump_node(p, value_node, out, indent + 2, pretty);

				if (i + 1 < node->children) {
					fputc(',', out);
					if (pretty) fputc('\n', out);
				}

				key_node = json_next_sibling(p, value_node);   // now skips nested objects correctly
			}
			if (pretty) {
				fputc('\n', out);
				for (int k = 0; k < indent; ++k) fputc(' ', out);
			}
			fputc('}', out);
			break;
		} 
    }
}

/* Writes `n` spaces in ceil(n/64) stringbuf_append calls instead of n
 * separate stringbuf_append_char calls -- only reached in pretty-print
 * mode (this project's own hot path always calls with pretty=false), but
 * cheap and correct to fix while touching this file. */
static inline void json_dump_indent_buf(StringBuf* sb, int n)
{
    static const char spaces[64] =
        "                                                                ";
    while (n > 0) {
        int chunk = n > 64 ? 64 : n;
        stringbuf_append(sb, spaces, chunk);
        n -= chunk;
    }
}

static ssize_t json_dump_node_buf(JsonParser* p, const JsonNode* node,
                           StringBuf *sb, int indent, bool pretty)
{
    if (!node) { stringbuf_append_str(sb, "null"); return sb->size; }

    const char* src = node->strval ? node->strval : p->buffer + node->offset;

	if(node->offset > p->buf_len) {
		printf("SOURCE IS PAST LEN\n");
		return -1;
	}
    switch (node->type) {
        case JSON_NULL:   stringbuf_append_str(sb, "null"); break;
        case JSON_TRUE:   stringbuf_append_str(sb, "true"); break;
        case JSON_FALSE:  stringbuf_append_str(sb, "false"); break;

        case JSON_NUMBER_INT:
        case JSON_NUMBER_FLOAT:
			stringbuf_append(sb, src, node->len);
            break;

        case JSON_STRING:
            /* Must escape, not raw-append between quotes: an unescaped
             * '"' or '\' or control byte inside the string would break
             * the JSON here (found while wiring this in -- the previous
             * raw stringbuf_append with escaping commented out silently
             * produced invalid JSON for any string containing one). */
            json_dump_escape_buf(sb, src, node->len);
            break;

        case JSON_ARRAY: {
            if (node->children == 0) { stringbuf_append_str(sb, "[]"); return sb->size; }

            /* "[\n" / "," + "\n" written as one literal each in pretty
             * mode -- one reserve-check instead of two. Compact mode
             * (this project's own hot path) was already a single
             * isolated byte either way, so unaffected either way. */
            stringbuf_append(sb, pretty ? "[\n" : "[", pretty ? 2 : 1);

            JsonNode* child = json_first_child(p, node);
            for (uint32_t i = 0; i < node->children; ++i) {
                if (pretty) json_dump_indent_buf(sb, indent + 2);
                json_dump_node_buf(p, child, sb, indent + 2, pretty);
                child = json_next_sibling(p, child);
                if (i + 1 < node->children) {
                    stringbuf_append(sb, pretty ? ",\n" : ",", pretty ? 2 : 1);
                }
            }
            if (pretty) {
				stringbuf_append_char(sb, '\n');
                json_dump_indent_buf(sb, indent);
            }
			stringbuf_append_char(sb, ']');
            break;
        }

       case JSON_OBJECT: {
			if (node->children == 0) { stringbuf_append_str(sb, "{}"); break; }

            stringbuf_append(sb, pretty ? "{\n" : "{", pretty ? 2 : 1);

			JsonNode* key_node = json_first_child(p, node);
			for (uint32_t i = 0; i < node->children; ++i) {
				JsonNode* value_node = json_next_sibling(p, key_node);

				if (pretty) json_dump_indent_buf(sb, indent + 2);

				/* Same escaping requirement as the JSON_STRING value case
				 * above -- a key is just another string on the wire. */
				if(key_node->strval)
					json_dump_escape_buf(sb, key_node->strval, key_node->len);
				else
					json_dump_escape_buf(sb, p->buffer + key_node->offset, key_node->len);

				if (pretty) stringbuf_append_str(sb, ": "); else stringbuf_append_char(sb, ':');
				json_dump_node_buf(p, value_node, sb, indent + 2, pretty);

				if (i + 1 < node->children) {
                    stringbuf_append(sb, pretty ? ",\n" : ",", pretty ? 2 : 1);
				}

				key_node = json_next_sibling(p, value_node);   // now skips nested objects correctly
			}
			if (pretty) {
				stringbuf_append_char(sb, '\n');
				json_dump_indent_buf(sb, indent);
			}
			stringbuf_append_char(sb, '}');
			break;
		}
    }
	return sb->size;
}

static inline void json_dump_debug(JsonParser* p, const JsonNode* node,
                           FILE* out, int indent, bool pretty)
{
    if (!node) { fputs("null", out); return; }

    switch (node->type) {
        case JSON_NULL:   fputs("null", out); break;
        case JSON_TRUE:   fputs("true", out); break;
        case JSON_FALSE:  fputs("false", out); break;

        case JSON_NUMBER_INT:
			fputs("JSON_NUMBER_INT", out); break;
        case JSON_NUMBER_FLOAT:
			fputs("JSON_NUMBER_FLOAT", out); break;

        case JSON_STRING:
			fputs("JSON_STRING", out); break;

        case JSON_ARRAY: {
            if (node->children == 0) { fputs("[]", out); return; }

            fputc('[', out);
            if (pretty) fputc('\n', out);

            JsonNode* child = json_first_child(p, node);
            for (uint32_t i = 0; i < node->children; ++i) {
                if (pretty) for (int k = 0; k < indent + 2; ++k) fputc(' ', out);
                json_dump_debug(p, child, out, indent + 2, pretty);
                child = json_next_sibling(p, child);
                if (i + 1 < node->children) {
                    fputc(',', out);
                    if (pretty) fputc('\n', out);
                }
            }
            if (pretty) {
                fputc('\n', out);
                for (int k = 0; k < indent; ++k) fputc(' ', out);
            }
            fputc(']', out);
            break;
        }

        case JSON_OBJECT: {
            if (node->children == 0) { fputs("{}", out); return; }

            fputc('{', out);
            if (pretty) fputc('\n', out);

            JsonNode* key_node = json_first_child(p, node);
            for (uint32_t i = 0; i < node->children; ++i) {
                JsonNode* value_node = json_next_sibling(p, key_node);

                if (pretty) for (int k = 0; k < indent + 2; ++k) fputc(' ', out);

                json_dump_escape(out, p->buffer + key_node->offset, key_node->len);
                if (pretty) fputs(" : ", out); else fputc(':', out);
                json_dump_debug(p, value_node, out, indent + 2, pretty);

                if (i + 1 < node->children) {
                    fputc(',', out);
                    if (pretty) fputc('\n', out);
                }

                /* advance to the next key (skip the value we just printed) */
                key_node = json_next_sibling(p, value_node);
            }
            if (pretty) {
                fputc('\n', out);
                for (int k = 0; k < indent; ++k) fputc(' ', out);
            }
            fputc('}', out);
            break;
        }
    }
}

/* Public API – dump the whole parsed document to a FILE stream */
static inline void json_dump(JsonParser* p, FILE* out, bool pretty)
{
    if (!p || p->nodes_len == 0) {
        fputs("null", out);
        return;
    }
    json_dump_node(p, &p->nodes[0], out, 0, pretty);
    if (pretty) fputc('\n', out);
#ifdef DEBUG
	fputs("---- DEBUG ----\n", out);
    json_dump_debug(p, &p->nodes[0], out, 0, pretty);
    if (pretty) fputc('\n', out);
#endif
}

static inline void json_print(JsonParser* p, bool pretty)
{ json_dump(p, stdout, pretty); }
/* Public API – dump the whole parsed document to a buffer */
static inline ssize_t json_serialize(JsonParser* p, bool pretty, StringBuf *sb)
{ return json_dump_node_buf(p, &p->nodes[0], sb, 0, pretty); }
static inline void json_print_pretty(JsonParser* p)  { json_print(p, true); }
static inline void json_print_compact(JsonParser* p) { json_print(p, false); }

/* === Builder API === */

static inline JsonNode* json_create_null(JsonParser* p)
{
    uint64_t idx = p->nodes_len++;
    if (unlikely(idx >= p->nodes_cap)) return NULL;
    p->nodes[idx] = (JsonNode){ .type = JSON_NULL, .strval = NULL };
    return &p->nodes[idx];
}

static inline JsonNode* json_create_bool(JsonParser* p, bool value)
{
    uint64_t idx = p->nodes_len++;
    if (unlikely(idx >= p->nodes_cap)) return NULL;
    p->nodes[idx] = (JsonNode){ .type = value ? JSON_TRUE : JSON_FALSE, .strval = NULL };
    return &p->nodes[idx];
}

static inline JsonNode* json_create_int(JsonParser* p, int64_t value)
{
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%" PRId64, value);
    char* dup = malloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, buf, len + 1);

    uint64_t idx = p->nodes_len++;
    if (unlikely(idx >= p->nodes_cap)) { free(dup); return NULL; }
    p->nodes[idx] = (JsonNode){ .type = JSON_NUMBER_INT, .len = len, .strval = dup };
    return &p->nodes[idx];
}

static inline JsonNode* json_create_float(JsonParser* p, double value)
{
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%.17g", value);
    char* dup = malloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, buf, len + 1);

    uint64_t idx = p->nodes_len++;
    if (unlikely(idx >= p->nodes_cap)) { free(dup); return NULL; }
    p->nodes[idx] = (JsonNode){ .type = JSON_NUMBER_FLOAT, .len = len, .strval = dup };
    return &p->nodes[idx];
}

static inline JsonNode* json_create_string(JsonParser* p, const char* str)
{
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, str, len + 1);

    uint64_t idx = p->nodes_len++;
    if (unlikely(idx >= p->nodes_cap)) { free(dup); return NULL; }
    uint32_t h = 0;
    if (!p->skip_string_hash) {
        for (size_t i = 0; i < len; ++i) h = h * 33 ^ (uint8_t)str[i];
    }

    p->nodes[idx] = (JsonNode){ .type = JSON_STRING, .len = (uint32_t)len, .hash = h, .strval = dup };
    return &p->nodes[idx];
}

/* Neither of these pushes onto p->stack anymore (they used to,
 * unconditionally and with no bounds check). Verified nothing in the
 * builder API path reads it: json_object_set/json_array_append never
 * touch p->stack, and json_first_child/json_next_sibling/
 * json_dump_node_buf/json_node_span navigate by direct node-index
 * arithmetic, not via the stack. p->stack/stack_len/expecting_key exist
 * for json_feed()'s streaming parse (properly paired push-on-open,
 * pop-on-close there) and json_finish()'s "everything got closed" check
 * -- neither of which a pure builder caller uses. The old unconditional
 * push was a real, confirmed bug for builder use: since nothing in
 * builder mode ever pops, stack_len only ever grows for the life of the
 * JsonParser, so any structure with more total container-creation calls
 * than stack_cap silently wrote past p->stack[]/p->expecting_key[]'s
 * allocated bounds -- reproduced as heap corruption (`free(): invalid
 * size`, SIGABRT) crashing a real server via a single oversized request. */
static inline JsonNode* json_create_array(JsonParser* p)
{
    uint64_t idx = p->nodes_len++;
    if (unlikely(idx >= p->nodes_cap)) return NULL;
    p->nodes[idx] = (JsonNode){ .type = JSON_ARRAY, .children = 0 };
    return &p->nodes[idx];
}

static inline JsonNode* json_create_object(JsonParser* p)
{
    uint64_t idx = p->nodes_len++;
    if (unlikely(idx >= p->nodes_cap)) return NULL;
    p->nodes[idx] = (JsonNode){ .type = JSON_OBJECT, .children = 0 };
    return &p->nodes[idx];
}

static inline bool json_array_append(JsonParser* p, JsonNode* array, JsonNode* element)
{
    if (!array || array->type != JSON_ARRAY) return false;
    // In builder mode, children count is maintained manually
    array->children++;
    array->hash += json_node_span(element);
    return true;
}

static inline bool json_object_set(JsonParser* p, JsonNode* obj, JsonNode* key_node, JsonNode* value_node)
{
    if (!obj || obj->type != JSON_OBJECT || !key_node || key_node->type != JSON_STRING) return false;
    obj->children++;
    /* key_node is always a JSON_STRING leaf (span 1); value_node may itself
     * be a container, so its span must be computed before obj's own hash
     * absorbs it -- do NOT overwrite value_node->hash here (a prior version
     * of this function did, "inherit key hash for fast lookup", but nothing
     * ever reads a value node's hash for lookup -- json_get_object_value()
     * only ever compares a *key* node's hash -- and overwriting it here
     * destroyed a container value's own accumulated span). */
    obj->hash += json_node_span(key_node) + json_node_span(value_node);
    return true;
}

#endif /* CEJSON_H */

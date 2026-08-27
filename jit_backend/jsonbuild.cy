/* jit_backend/jsonbuild.cy — cejson-backed implementation of jsonbuild.h's
 * opaque JSON builder API. cejson.h is vendored in this directory (a
 * plain copy of ~/src/GUI/cejson/cejson.h -- header-only, so no build
 * dependency on that repo). See jsonbuild.h's header comment for why this
 * is its own TU (must be compiled with -fno-ownership; see build.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include "cejson.h"
#include "jsonbuild.h"

/* Was a much bigger stopgap value (4096): cejson's json_create_object()/
 * json_create_array() used to push onto p->stack with no bounds check and
 * no way to ever pop it in builder mode, so stack usage grew with the
 * total number of containers ever created (not nesting depth) -- confirmed
 * as a real `free(): invalid size` / SIGABRT crash of the whole server via
 * `GET /api/posts?count=50`. Fixed at the root in cejson.h itself:
 * json_create_object()/json_create_array() no longer touch p->stack at
 * all (verified nothing in the builder API path reads it -- see cejson.h's
 * comment on those two functions), so this is back to a small, genuinely
 * sufficient value; json_init() still requires a stack/expecting_key
 * pointer + capacity, it's just never written to by builder calls now. */
#define JB_STACK_CAP 32

struct JBuilder {
    JsonParser p;
    JsonNode *nodes;
    uint32_t stack[JB_STACK_CAP];
    uint8_t expecting_key[JB_STACK_CAP];
};

JBuilder *jb_create(int max_nodes) {
    if (max_nodes < 1) max_nodes = 1;
    JBuilder *jb = (JBuilder *) malloc(sizeof(JBuilder));
    if (!jb) return 0;
    jb->nodes = (JsonNode *) malloc(sizeof(JsonNode) * (size_t) max_nodes);
    if (!jb->nodes) { free(jb); return 0; }
    json_init(&jb->p, jb->nodes, (uint64_t) max_nodes, jb->stack, JB_STACK_CAP, jb->expecting_key);
    /* We only ever build-then-serialize, never look a key back up by name
     * (json_get_object_value), so the per-character hash json_create_string
     * would otherwise compute on every string is pure waste here. */
    jb->p.skip_string_hash = true;
    return jb;
}

void jb_destroy(JBuilder *jb) {
    if (!jb) return;
    /* Each leaf value (json_create_int/float/string) individually mallocs
     * its own text buffer into .strval; container nodes leave .strval
     * NULL. No bulk "destroy a builder-created tree" exists in cejson
     * (json_free_tree is written for PARSED trees), so free every node's
     * strval directly -- cheap, since jb->p.nodes_len is exactly the
     * number of nodes actually created (not the allocated capacity). */
    for (uint64_t i = 0; i < jb->p.nodes_len; i++) {
        if (jb->nodes[i].strval) free(jb->nodes[i].strval);
    }
    free(jb->nodes);
    free(jb);
}

JBNode *jb_object(JBuilder *jb) { return (JBNode *) json_create_object(&jb->p); }
JBNode *jb_array(JBuilder *jb)  { return (JBNode *) json_create_array(&jb->p); }
JBNode *jb_string(JBuilder *jb, const char *s) { return (JBNode *) json_create_string(&jb->p, s ? s : ""); }
JBNode *jb_int(JBuilder *jb, long v) { return (JBNode *) json_create_int(&jb->p, (int64_t) v); }
JBNode *jb_bool(JBuilder *jb, int v) { return (JBNode *) json_create_bool(&jb->p, v ? true : false); }
JBNode *jb_null(JBuilder *jb) { return (JBNode *) json_create_null(&jb->p); }

JBNode *jb_key(JBuilder *jb, const char *key) {
    return (JBNode *) json_create_string(&jb->p, key);
}

void jb_attach(JBuilder *jb, JBNode *obj, JBNode *key, JBNode *value) {
    json_object_set(&jb->p, (JsonNode *) obj, (JsonNode *) key, (JsonNode *) value);
}

void jb_append(JBuilder *jb, JBNode *arr, JBNode *value) {
    json_array_append(&jb->p, (JsonNode *) arr, (JsonNode *) value);
}

char *jb_finish(JBuilder *jb, JBNode *root) {
    /* json_dump_node_buf writes directly into a growable StringBuf (plain
     * memcpy-based appends, batched runs for string escaping) instead of
     * through a FILE* -- the original open_memstream + json_dump_node
     * version went through stdio's fputc/fputs/fwrite, which each take an
     * internal lock by default even on a single-threaded-only stream;
     * profiled as the dominant cost once the dict-based leak/query fixes
     * were in (measured 4-12% slower than the dict-based version it
     * replaced, worse at higher concurrency -- lock-taking overhead, not
     * contention, since each stream here is only ever touched by the one
     * thread that created it). */
    StringBuf sb;
    if (!stringbuf_init(&sb, 256)) return 0;
    json_dump_node_buf(&jb->p, (JsonNode *) root, &sb, 0, false);
    return sb.data; /* caller hands this to String.attach(); don't free it here */
}

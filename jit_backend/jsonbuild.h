/* jit_backend/jsonbuild.h — opaque JSON-response-builder API backed by
 * jit_backend/cejson.h (vendored from ~/src/GUI/cejson -- a flat-array,
 * bump-allocated JSON builder/serializer), replacing classyc's
 * dict/List<dict> for the hottest response path (/api/posts's
 * post_to_json, called once per post).
 *
 * Why a separate TU instead of api.cy just #include-ing cejson.h directly:
 * cejson.h's builder functions (json_create_int/float/string) trip
 * classyc's ownership checker with false-positive "double-free risk"
 * errors on their malloc-then-maybe-free-on-capacity-failure pattern (the
 * same category of false positive documented on classyc's own dict.h).
 * The only fix is `-fno-ownership`, and that flag is whole-compile-unit,
 * not scoped to one file's declarations -- turning it on for api.cy would
 * also disable ownership checking for every handler's own dict/List<dict>
 * cleanup, exactly the class of bug this project spent real effort finding
 * and fixing. So jsonbuild.cy is its own tiny TU, built with
 * `-fno-ownership` in isolation, and exposes only OPAQUE pointer types
 * here -- api.cy never sees cejson.h's actual struct layout (and doesn't
 * `#include` it), so api.cy's own compile keeps full ownership checking.
 *
 * ── The one rule that matters: key before value, always ──────────────
 * cejson's flat node array requires a key's node to be created
 * IMMEDIATELY before its value's node -- json_dump_node()'s OBJECT case
 * walks key,value,key,value,... by raw array adjacency, not by pointer.
 * Two ways to get this right, and one very easy way to get it wrong:
 *
 *   RIGHT (scalar value -- use the JB_FIELD macro, see api.cy):
 *     JB_FIELD(jb, obj, "id", jb_int(jb, 42));
 *     // expands to separate statements: jb_key() runs, THEN jb_int(jb,42)
 *     // is evaluated, THEN jb_attach() -- order is by construction, not
 *     // convention.
 *
 *   RIGHT (container value -- key first, THEN build the whole subtree):
 *     JBNode *tags_key = jb_key(jb, "tags");
 *     JBNode *tags = jb_array(jb);
 *     jb_append(jb, tags, jb_string(jb, "x"));
 *     jb_append(jb, tags, jb_string(jb, "y"));
 *     jb_attach(jb, obj, tags_key, tags);
 *
 *   WRONG -- do not do this:
 *     JBNode *tags = jb_array(jb);
 *     jb_append(jb, tags, jb_string(jb, "x"));
 *     JB_FIELD(jb, obj, "tags", tags);   // BUG: tags' entire subtree was
 *     // already built before this key gets created, so the key ends up
 *     // positioned AFTER the whole subtree instead of before it --
 *     // breaks adjacency for this pair AND desyncs every field that
 *     // follows it in the same object. Found exactly this way: "media"
 *     // vanished from a real response and the next key printed as "".
 *     // A plain function can't fix this either -- jb_field(jb, obj,
 *     // "tags", tags) has the identical bug, since `tags` is fully
 *     // evaluated as an argument before the function body (which creates
 *     // the key) ever runs. There is no single-call-with-a-pre-built-
 *     // container-value form that works; the key must always be created
 *     // before that container's first node.
 *
 * `jb_finish` must be called with the ROOT node of the tree you want
 * serialized -- for a single object/array response that's whatever
 * `jb_object`/`jb_array` call happened first on this builder (cejson's
 * flat-array model always treats index 0 as the document root); for
 * appending one post at a time into a shared outer array (h_get_posts's
 * shape), call jb_finish once at the end against the OUTER array, not
 * each post's own object.
 */
#ifndef JSONBUILD_H
#define JSONBUILD_H

#include <string.h>

typedef struct JBuilder JBuilder;
typedef struct JBNode JBNode;

JBuilder *jb_create(int max_nodes);
void      jb_destroy(JBuilder *jb);

JBNode *jb_object(JBuilder *jb);
JBNode *jb_array(JBuilder *jb);
JBNode *jb_string(JBuilder *jb, const char *s);
JBNode *jb_int(JBuilder *jb, long v);
JBNode *jb_bool(JBuilder *jb, int v);
JBNode *jb_null(JBuilder *jb);

/* Object field attachment is two calls, not one, and the order between
 * them is load-bearing: cejson's flat node array requires a key's node to
 * be created immediately before its value's node (json_dump_node's OBJECT
 * case walks key,value,key,value,... by raw adjacency, not by pointer).
 * A single jb_set(jb, obj, "key", jb_int(jb, v))-style call is UNSAFE:
 * jb_int(jb, v) is evaluated as an argument before the call happens, so
 * the value's node would already exist before jb_set ever created the key
 * node, silently swapping every key/value pair in the output (found the
 * hard way -- see jit_backend/api.cy's post_to_json for the correct
 * pattern: jb_key() then the value constructor as two separate statements,
 * then jb_attach() to record the pair once both nodes already exist). */
JBNode *jb_key(JBuilder *jb, const char *key);
void jb_attach(JBuilder *jb, JBNode *obj, JBNode *key, JBNode *value);

/* Array elements have no key, so there's no ordering hazard here: build
 * `value` completely (whatever that takes), then append it. */
void jb_append(JBuilder *jb, JBNode *arr, JBNode *value);

/* Serializes `root`'s subtree to a freshly-malloc'd, NUL-terminated
 * buffer. Returns a plain char* (not a String) deliberately: converting
 * to String has to happen in ownership-checked code (i.e. in api.cy,
 * right where you use this) for the result to survive -- doing the
 * char*-to-String conversion inside jsonbuild.cy's -fno-ownership code
 * produced a corrupted/truncated String by the time it reached a normal
 * ownership-checked caller (found the hard way: a perfectly correct
 * 183-byte JSON buffer confirmed present inside jb_finish became 5
 * garbage bytes by the time api.cy read it back).
 *
 * Convert with String.attach(), not a plain assignment. Two wrong ways
 * were each tried and found broken the hard way before landing on this:
 *   - `String body = raw; free(raw);` -- a plain assignment doesn't
 *     deep-copy raw's bytes, it just holds the same pointer, so the
 *     explicit free() is a use-after-free the instant anything downstream
 *     (Response::wire()) reads body.
 *   - `String body = raw;` with NO free -- avoids the crash, but now
 *     nothing ever frees raw's buffer, leaking it (measured: ~1.8KB per
 *     request on /api/posts).
 * String.attach(ptr) (c2m_str_attach) is the real fix: it transfers
 * ownership of an externally-malloc'd buffer into String's own
 * scope-exit cleanup, so the buffer gets freed automatically exactly
 * once, at the right time. Correct pattern:
 *
 *   char *raw = jb_finish(jb, root);
 *   String body = String.attach(raw);   // body now owns raw -- don't free it
 *   jb_destroy(jb);                      // safe: only frees jb's own node
 *                                         // array/strvals, never this buffer
 */
char *jb_finish(JBuilder *jb, JBNode *root);

#endif

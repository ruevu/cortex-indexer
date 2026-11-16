
/*
 * registry.c — Function/Method/Class registry for call resolution.
 *
 * Indexes all callable symbols by qualified name and simple name.
 * Resolution uses a prioritized strategy chain:
 *   1. Import map lookup
 *   2. Same-module match
 *   3. Unique name (single candidate project-wide)
 *   4. Suffix match with import distance scoring
 */
#include "foundation/constants.h"

enum { REG_INIT_CAP = 16, REG_MIN_CANDIDATES = 3, REG_RESOLVED = 1, REG_SUFFIX_ALLOC = 2 };
#define REG_FULL_CONF 1.0
#define REG_HALF_PENALTY 0.5

#define DEFAULT_CONFIDENCE 0.5
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "foundation/hash_table.h"
#include "foundation/dyn_array.h"
#include "foundation/platform.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Confidence score → human-readable band label. */
#define CONF_BAND_HIGH 0.7
#define CONF_BAND_MEDIUM 0.45
#define CONF_BAND_SPECULATIVE 0.25

const char *ctx_confidence_band(double score) {
    if (score >= CONF_BAND_HIGH) {
        return "high";
    }
    if (score >= CONF_BAND_MEDIUM) {
        return "medium";
    }
    if (score >= CONF_BAND_SPECULATIVE) {
        return "speculative";
    }
    return "";
}

/* ── Resolution confidence scores ────────────────────────────────── */
/* Strategy 1: import_map — direct import → high confidence */
#define CONF_IMPORT_MAP 0.95
#define CONF_IMPORT_MAP_SUFFIX 0.85
/* Strategy 2: same_module — same file/package → high confidence */
#define CONF_SAME_MODULE 0.90
/* Strategy 3: unique_name — only one candidate project-wide */
#define CONF_UNIQUE_NAME 0.75
/* Strategy 4: suffix_match — multiple candidates, filtered */
#define CONF_SUFFIX_MATCH 0.55
/* Fuzzy fallback: lower confidence */
#define CONF_FUZZY_SINGLE 0.40
#define CONF_FUZZY_MULTI 0.30
/* Candidate count penalty cap */
#define CANDIDATE_PENALTY_CAP 3.0

/* ── Internal types ──────────────────────────────────────────────── */

/* Array of QN strings for byName index */
typedef CTX_DYN_ARRAY(char *) qn_array_t;

struct ctx_registry {
    /* exact: qualifiedName → label string (heap-owned copies) */
    CtxHashTable *exact;

    /* byName: simpleName → qn_array_t* (heap-owned) */
    CtxHashTable *by_name;
};

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Extract last dot-separated segment from a QN. Returns pointer into s. */
static const char *simple_name(const char *qn) {
    const char *last = strrchr(qn, '.');
    return last ? last + SKIP_ONE : qn;
}

/* Extract everything before the last dot. Returns heap-allocated string. */

/* Count common dot-separated prefix segments. */
static int common_prefix_len(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }
    int count = 0;
    while (*a && *b) {
        /* Find next segment in each */
        const char *adot = strchr(a, '.');
        const char *bdot = strchr(b, '.');
        size_t alen = adot ? (size_t)(adot - a) : strlen(a);
        size_t blen = bdot ? (size_t)(bdot - b) : strlen(b);
        if (alen != blen || memcmp(a, b, alen) != 0) {
            break;
        }
        count++;
        a += alen + (adot ? SKIP_ONE : 0);
        b += blen + (bdot ? SKIP_ONE : 0);
        if (!adot || !bdot) {
            break;
        }
    }
    return count;
}

/* Pick candidate with longest common prefix with caller module. */
static const char *best_by_import_distance(const char **candidates, int count,
                                           const char *module_qn) {
    const char *best = NULL;
    int best_len = CTX_NOT_FOUND;
    for (int i = 0; i < count; i++) {
        int plen = common_prefix_len(candidates[i], module_qn);
        if (plen > best_len) {
            best_len = plen;
            best = candidates[i];
        }
    }
    return best;
}

/* Check if candidate's module prefix appears in import map values.
 * Uses stack buffer to avoid malloc/free per call in hot resolution loop. */
/* The scope an import value stands for. An index module (`ui.index`, a
 * barrel) stands for its directory, the package; the definitions in the index
 * file are qualified by the directory too (`ui.Button` for a Button declared in
 * ui/index.ts). Any other value is its own scope. Returns val or buf. */
static const char *import_scope(const char *val, char *buf, size_t size) {
    static const char index_seg[] = ".index";
    size_t seg_len = sizeof(index_seg) - SKIP_ONE;
    size_t len = strlen(val);
    if (len <= seg_len || strcmp(val + len - seg_len, index_seg) != 0 || len - seg_len >= size) {
        return val;
    }
    memcpy(buf, val, len - seg_len);
    buf[len - seg_len] = '\0';
    return buf;
}

/* How a candidate is reachable through the caller's imports: through an
 * imported module itself, only through the package a barrel import stands
 * for, or not at all. A guess keeps to the strongest reach it finds, so a
 * module the caller names outranks the rest of a package it also imports
 * whole. */
enum { REACH_NONE = 0, REACH_PACKAGE = 1, REACH_MODULE = 2 };

static int import_reach(const char *candidate_qn, const char **import_vals, int import_count) {
    char cand_mod[CTX_SZ_512];
    const char *last = strrchr(candidate_qn, '.');
    if (last) {
        size_t len = (size_t)(last - candidate_qn);
        if (len >= sizeof(cand_mod)) {
            len = sizeof(cand_mod) - SKIP_ONE;
        }
        memcpy(cand_mod, candidate_qn, len);
        cand_mod[len] = '\0';
    } else {
        snprintf(cand_mod, sizeof(cand_mod), "%s", candidate_qn);
    }
    int reach = REACH_NONE;
    for (int i = 0; i < import_count; i++) {
        const char *val = import_vals[i];
        if (strstr(cand_mod, val) || strstr(val, cand_mod)) {
            return REACH_MODULE;
        }
        char scope_buf[CTX_SZ_512];
        const char *scope = import_scope(val, scope_buf, sizeof(scope_buf));
        if (scope != val && (strstr(cand_mod, scope) || strstr(scope, cand_mod))) {
            reach = REACH_PACKAGE;
        }
    }
    return reach;
}

static bool is_import_reachable(const char *candidate_qn, const char **import_vals,
                                int import_count) {
    return import_reach(candidate_qn, import_vals, import_count) != REACH_NONE;
}

/* Scale confidence inversely with candidate count. */
static double candidate_count_penalty(double base, int count) {
    if (count <= REG_MIN_CANDIDATES) {
        return base;
    }
    return base * fmin(REG_FULL_CONF, CANDIDATE_PENALTY_CAP / (double)count);
}

static ctx_resolution_t empty_result(void) {
    ctx_resolution_t r = {0};
    return r;
}

/* ── Lifecycle ──────────────────────────────────────────────────── */

ctx_registry_t *ctx_registry_new(void) {
    ctx_registry_t *r = calloc(CTX_ALLOC_ONE, sizeof(ctx_registry_t));
    if (!r) {
        return NULL;
    }
    r->exact = ctx_ht_create(CTX_SZ_1K);
    r->by_name = ctx_ht_create(CTX_SZ_512);
    return r;
}

static void free_label(const char *key, void *value, void *ud) {
    (void)ud;
    free((void *)key);
    free(value);
}

static void free_qn_array(const char *key, void *value, void *ud) {
    (void)ud;
    qn_array_t *arr = value;
    if (arr) {
        for (int i = 0; i < arr->count; i++) {
            free(arr->items[i]);
        }
        ctx_da_free(arr);
        free(arr);
    }
    free((void *)key);
}

void ctx_registry_free(ctx_registry_t *r) {
    if (!r) {
        return;
    }
    ctx_ht_foreach(r->exact, free_label, NULL);
    ctx_ht_free(r->exact);
    ctx_ht_foreach(r->by_name, free_qn_array, NULL);
    ctx_ht_free(r->by_name);
    free(r);
}

/* ── Registration ────────────────────────────────────────────────── */

void ctx_registry_add(ctx_registry_t *r, const char *name, const char *qualified_name,
                      const char *label) {
    (void)name;
    if (!r || !qualified_name || !label) {
        return;
    }

    /* Check for duplicate */
    if (ctx_ht_get(r->exact, qualified_name)) {
        return;
    }

    /* Store in exact map: QN → label */
    ctx_ht_set(r->exact, strdup(qualified_name), strdup(label));

    /* Index by simple name.
     * No array dedup needed: exact-map check above guarantees uniqueness. */
    const char *simple = simple_name(qualified_name);
    qn_array_t *arr = ctx_ht_get(r->by_name, simple);
    if (!arr) {
        arr = calloc(CTX_ALLOC_ONE, sizeof(qn_array_t));
        ctx_ht_set(r->by_name, strdup(simple), arr);
    }
    ctx_da_push(arr, strdup(qualified_name));
}

/* ── Lookup ──────────────────────────────────────────────────────── */

bool ctx_registry_exists(const ctx_registry_t *r, const char *qn) {
    if (!r || !qn) {
        return false;
    }
    return ctx_ht_get(r->exact, qn) != NULL;
}

const char *ctx_registry_label_of(const ctx_registry_t *r, const char *qn) {
    if (!r || !qn) {
        return NULL;
    }
    return ctx_ht_get(r->exact, qn);
}

int ctx_registry_find_by_name(const ctx_registry_t *r, const char *name, const char ***out,
                              int *count) {
    if (!r || !out || !count) {
        return CTX_NOT_FOUND;
    }
    qn_array_t *arr = ctx_ht_get(r->by_name, name);
    if (arr && arr->count > 0) {
        *out = (const char **)arr->items;
        *count = arr->count;
    } else {
        *out = NULL;
        *count = 0;
    }
    return 0;
}

int ctx_registry_size(const ctx_registry_t *r) {
    return r ? (int)ctx_ht_count(r->exact) : 0;
}

/* ── Resolution ──────────────────────────────────────────────────── */

static int qn_depth(const char *qn) {
    int depth = 0;
    for (; *qn; qn++) {
        depth += *qn == '.';
    }
    return depth;
}

/* The registered definition under `scope` (a QN prefix, matched up to a
 * segment boundary) whose QN ends with the whole segments `tail`
 * ("format", or "Panel.open"). Found through the by-name index, keyed by
 * tail's last segment, so it costs one bucket, not a registry scan.
 *
 * This is how an import reaches a barrel: `import { Button } from './ui'`
 * names the module ui/index.ts, which defines nothing it re-exports, so the
 * definition is looked for anywhere inside the package instead.
 *
 * bare: a bare call reaches a Function or a Class, never a Method.
 * Several matches: the shallowest QN (nearest the package's root), then the
 * lexicographically smallest, so the answer never depends on hash order.
 * *matches is set to how many qualified. */
static const char *lookup_in_scope(const ctx_registry_t *r, const char *scope, const char *tail,
                                   bool bare, int *matches) {
    *matches = 0;
    const char *last_dot = strrchr(tail, '.');
    qn_array_t *arr = ctx_ht_get(r->by_name, last_dot ? last_dot + SKIP_ONE : tail);
    if (!arr) {
        return NULL;
    }
    size_t scope_len = strlen(scope);
    size_t tail_len = strlen(tail);
    const char *best = NULL;
    int best_depth = 0;
    for (int i = 0; i < arr->count; i++) {
        const char *qn = arr->items[i];
        size_t len = strlen(qn);
        if (len < scope_len + SKIP_ONE + tail_len || strncmp(qn, scope, scope_len) != 0 ||
            qn[scope_len] != '.' || strcmp(qn + len - tail_len, tail) != 0 ||
            qn[len - tail_len - SKIP_ONE] != '.') {
            continue;
        }
        if (bare) {
            const char *label = ctx_ht_get(r->exact, qn);
            if (!label || (strcmp(label, "Function") != 0 && strcmp(label, "Class") != 0)) {
                continue;
            }
        }
        /* The tail's first segment must be a module-level name, the only kind a
         * package exports. When what precedes the tail is itself a definition,
         * the match is a member of something else (`open` found as
         * Drawer.open while the call was `Panel.open()`). */
        char owner[CTX_SZ_512];
        size_t owner_len = len - tail_len - SKIP_ONE;
        if (owner_len < sizeof(owner)) {
            memcpy(owner, qn, owner_len);
            owner[owner_len] = '\0';
            if (ctx_ht_get(r->exact, owner)) {
                continue;
            }
        }
        (*matches)++;
        int depth = qn_depth(qn);
        if (!best || depth < best_depth || (depth == best_depth && strcmp(qn, best) < 0)) {
            best = qn;
            best_depth = depth;
        }
    }
    return best ? ctx_ht_get_key(r->exact, best) : NULL;
}

/* Strategy 1: Import map lookup (exact → suffix fallback) */
static ctx_resolution_t resolve_import_map(const ctx_registry_t *r, const char *prefix,
                                           const char *suffix, const char **keys, const char **vals,
                                           int map_count) {
    if (!keys || !vals || map_count <= 0) {
        return empty_result();
    }

    /* Find prefix in import map keys */
    const char *resolved = NULL;
    for (int i = 0; i < map_count; i++) {
        if (strcmp(keys[i], prefix) == 0) {
            resolved = vals[i];
            break;
        }
    }
    if (!resolved) {
        return empty_result();
    }

    /* The map value is either the imported symbol itself (Python's
     * `from m import f` maps f to m.f) or the module it came from (an ES
     * `import { f } from './m'` maps f to the module m). Try the symbol reading
     * first, then the module reading: the imported name inside the module.
     *
     * The bare `resolved` hit is for suffix-less callees only. For `f.get()`
     * the callee is a member of f, never f itself. The module reading is
     * tried against the module's scope too: a name declared in a barrel's own
     * index file is qualified by the directory. */
    char scope[CTX_SZ_512];
    const char *scope_qn = import_scope(resolved, scope, sizeof(scope));
    const char *bases[] = {resolved, scope_qn};
    int base_count = scope_qn == resolved ? 1 : 2;
    char candidate[CTX_SZ_512];
    const char *stored_key = suffix && suffix[0] ? NULL : ctx_ht_get_key(r->exact, resolved);
    for (int b = 0; b < base_count && !stored_key; b++) {
        if (suffix && suffix[0]) {
            snprintf(candidate, sizeof(candidate), "%s.%s", bases[b], suffix);
            stored_key = ctx_ht_get_key(r->exact, candidate);
            if (!stored_key) {
                snprintf(candidate, sizeof(candidate), "%s.%s.%s", bases[b], prefix, suffix);
                stored_key = ctx_ht_get_key(r->exact, candidate);
            }
        } else {
            snprintf(candidate, sizeof(candidate), "%s.%s", bases[b], prefix);
            stored_key = ctx_ht_get_key(r->exact, candidate);
        }
    }
    if (stored_key) {
        /* ctx_ht_get_key returns the registry's persistent heap-owned key. */
        return (ctx_resolution_t){stored_key, "import_map", CONF_IMPORT_MAP, REG_RESOLVED};
    }

    /* import_map_suffix: no exact hit, so look inside the imported module's
     * scope. A barrel (ui/index.ts, module `ui.index`) defines nothing it
     * re-exports; its scope is its directory, the package. A member call
     * tries the imported name as its owner first (`Panel.open` → Panel's
     * open), then the member alone (`ns.format` on a namespace import). */
    int matches = 0;
    const char *found = NULL;
    if (suffix && suffix[0]) {
        snprintf(candidate, sizeof(candidate), "%s.%s", prefix, suffix);
        found = lookup_in_scope(r, scope_qn, candidate, false, &matches);
        if (!found) {
            found = lookup_in_scope(r, scope_qn, suffix, false, &matches);
        }
    } else {
        found = lookup_in_scope(r, scope_qn, prefix, true, &matches);
    }
    if (found) {
        /* One match is what the import names. Several make the pick a
         * tie-break (shallowest, then lexicographic), as sure as a suffix
         * match and no surer. */
        double confidence = matches == REG_RESOLVED
                                ? CONF_IMPORT_MAP_SUFFIX
                                : candidate_count_penalty(CONF_SUFFIX_MATCH, matches);
        return (ctx_resolution_t){found, "import_map_suffix", confidence, matches};
    }
    return empty_result();
}

/* Strategy 2: Same-module match */
static ctx_resolution_t resolve_same_module(const ctx_registry_t *r, const char *callee_name,
                                            const char *suffix, const char *module_qn) {
    char candidate[CTX_SZ_512];
    snprintf(candidate, sizeof(candidate), "%s.%s", module_qn, callee_name);
    const char *stored_key = ctx_ht_get_key(r->exact, candidate);
    if (stored_key) {
        return (ctx_resolution_t){stored_key, "same_module", CONF_SAME_MODULE, REG_RESOLVED};
    }
    if (suffix && suffix[0]) {
        snprintf(candidate, sizeof(candidate), "%s.%s", module_qn, suffix);
        stored_key = ctx_ht_get_key(r->exact, candidate);
        if (stored_key) {
            return (ctx_resolution_t){stored_key, "same_module", CONF_SAME_MODULE, REG_RESOLVED};
        }
    }
    return empty_result();
}

/* Strategy 4: multiple candidates with import filtering. */
/* The candidates at the strongest import reach any of them has (none when
 * nothing is reachable), up to max_out. Returns how many were kept. */
static int filter_import_reachable(const char **candidates, int count, const char **import_vals,
                                   int import_count, const char **out, int max_out) {
    int n = 0;
    int top = REACH_NONE;
    for (int i = 0; i < count; i++) {
        int reach = import_reach(candidates[i], import_vals, import_count);
        if (reach == REACH_NONE || reach < top) {
            continue;
        }
        if (reach > top) {
            top = reach;
            n = 0;
        }
        if (n < max_out) {
            out[n++] = candidates[i];
        }
    }
    return n;
}

static ctx_resolution_t resolve_multi_with_imports(const qn_array_t *arr, const char *module_qn,
                                                   const char **import_vals, int import_count) {
    const char *filtered[CTX_SZ_256];
    int fcount = filter_import_reachable((const char **)arr->items, arr->count, import_vals,
                                         import_count, filtered, CTX_SZ_256);
    if (fcount == SKIP_ONE) {
        double conf = candidate_count_penalty(CONF_SUFFIX_MATCH, arr->count);
        return (ctx_resolution_t){filtered[0], "suffix_match", conf, arr->count};
    }
    if (fcount > SKIP_ONE) {
        const char *best = best_by_import_distance(filtered, fcount, module_qn);
        if (best) {
            double conf = candidate_count_penalty(CONF_SUFFIX_MATCH, fcount);
            return (ctx_resolution_t){best, "suffix_match", conf, fcount};
        }
    }
    /* No import-reachable — use all candidates with penalty */
    const char *best = best_by_import_distance((const char **)arr->items, arr->count, module_qn);
    if (best) {
        double conf = candidate_count_penalty(CONF_SUFFIX_MATCH * REG_HALF_PENALTY, arr->count);
        return (ctx_resolution_t){best, "suffix_match", conf, arr->count};
    }
    return empty_result();
}

/* Strategy 3+4: Name lookup + suffix match */
static ctx_resolution_t resolve_name_lookup(const ctx_registry_t *r, const char *callee_name,
                                            const char *module_qn, const char **import_vals,
                                            int import_count) {
    const char *lookup = simple_name(callee_name);
    qn_array_t *arr = ctx_ht_get(r->by_name, lookup);
    if (!arr || arr->count == 0) {
        return empty_result();
    }

    /* Strategy 3: unique name */
    if (arr->count == SKIP_ONE) {
        double conf = CONF_UNIQUE_NAME;
        if (import_vals && import_count > 0 &&
            !is_import_reachable(arr->items[0], import_vals, import_count)) {
            conf *= DEFAULT_CONFIDENCE;
        }
        return (ctx_resolution_t){arr->items[0], "unique_name", conf, REG_RESOLVED};
    }

    /* Strategy 4: multiple candidates */
    if (import_vals && import_count > 0) {
        return resolve_multi_with_imports(arr, module_qn, import_vals, import_count);
    }
    const char *best = best_by_import_distance((const char **)arr->items, arr->count, module_qn);
    if (best) {
        double conf = candidate_count_penalty(CONF_SUFFIX_MATCH, arr->count);
        return (ctx_resolution_t){best, "suffix_match", conf, arr->count};
    }
    return empty_result();
}

ctx_resolution_t ctx_registry_resolve(const ctx_registry_t *r, const char *callee_name,
                                      const char *module_qn, const char **import_map_keys,
                                      const char **import_map_vals, int import_map_count) {
    if (!r || !callee_name) {
        return empty_result();
    }

    /* Split callee: "pkg.Func" → prefix="pkg", suffix="Func" */
    char prefix[CTX_SZ_256] = {0};
    const char *suffix = NULL;
    const char *dot = strchr(callee_name, '.');
    if (dot) {
        size_t plen = dot - callee_name;
        if (plen >= sizeof(prefix)) {
            plen = sizeof(prefix) - SKIP_ONE;
        }
        memcpy(prefix, callee_name, plen);
        prefix[plen] = '\0';
        suffix = dot + SKIP_ONE;
    } else {
        snprintf(prefix, sizeof(prefix), "%s", callee_name);
    }

    /* Strategy 1: import map */
    ctx_resolution_t res =
        resolve_import_map(r, prefix, suffix, import_map_keys, import_map_vals, import_map_count);
    if (res.qualified_name && res.qualified_name[0]) {
        return res;
    }

    /* Strategy 2: same module */
    res = resolve_same_module(r, callee_name, suffix, module_qn);
    if (res.qualified_name && res.qualified_name[0]) {
        return res;
    }

    /* Strategy 3+4: name lookup */
    return resolve_name_lookup(r, callee_name, module_qn, import_map_vals, import_map_count);
}

/* ── Member calls (T-dt3b) ───────────────────────────────────────── */

/* The languages the member-call rule covers: measured on TypeScript monorepos.
 * Other languages call module functions with member syntax without importing
 * the module first (Elixir's `MyApp.Utils.f()`), so they need their own rule. */
static bool lang_has_member_call_rule(CtxLanguage lang) {
    return lang == CTX_LANG_JAVASCRIPT || lang == CTX_LANG_TYPESCRIPT || lang == CTX_LANG_TSX ||
           lang == CTX_LANG_VUE || lang == CTX_LANG_SVELTE;
}

/* Strategies that bind a callee by its last segment alone. */
static bool binds_by_name_alone(const char *strategy) {
    return strategy && (strcmp(strategy, "unique_name") == 0 ||
                        strcmp(strategy, "suffix_match") == 0 || strcmp(strategy, "fuzzy") == 0);
}

static bool root_is_imported(const char *root, size_t root_len, const CtxImportArray *imports) {
    if (!imports) {
        return false;
    }
    for (int i = 0; i < imports->count; i++) {
        const char *name = imports->items[i].local_name;
        if (name && strlen(name) == root_len && strncmp(name, root, root_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool root_is(const char *root, size_t root_len, const char *word) {
    return strlen(word) == root_len && strncmp(root, word, root_len) == 0;
}

/* The callee is raw source text, so a chain a formatter broke before the dot
 * (`fmt\n    .format()`) carries that layout inside the receiver. */
static size_t trim_trailing_layout(const char *s, size_t len) {
    while (len > 0 && (s[len - SKIP_ONE] == ' ' || s[len - SKIP_ONE] == '\t' ||
                       s[len - SKIP_ONE] == '\n' || s[len - SKIP_ONE] == '\r')) {
        len--;
    }
    return len;
}

/* ── Builtin method names on methods (T-byzx item 5) ─────────────── */

/* ECMAScript methods an untyped receiver reaches far more often than any repo
 * method of the same name: Array, String, Promise, Map/Set and object
 * iteration, plus React's `ref.current`. Platform and library methods
 * (`db.close()`, `canvas.fill()`) are deliberately absent: a name list cannot
 * cover them, so they wait for receiver types. */
static const char *const BUILTIN_METHOD_NAMES[] = {
    "replace",  "split",       "join",        "slice",      "splice",   "push",
    "pop",      "shift",       "unshift",     "map",        "filter",   "reduce",
    "forEach",  "find",        "findIndex",   "some",       "every",    "includes",
    "indexOf",  "concat",      "flat",        "flatMap",    "sort",     "reverse",
    "trim",     "toLowerCase", "toUpperCase", "startsWith", "endsWith", "padStart",
    "then",     "catch",       "finally",     "resolve",    "reject",   "all",
    "keys",     "values",      "entries",     "toString",   "toFixed",  "current",
    "get",      "set",         "has",         "add",        "delete",   "clear",
};

/* Global namespaces: a call on one reaches the platform, whatever its name.
 * Lower-case browser and Node globals (`document`, `process`) are left out
 * because a parameter of that name commonly shadows them. */
static const char *const GLOBAL_NAMESPACES[] = {
    "Object", "Array", "JSON",    "Math",   "Promise", "Number",
    "String", "Date",  "Reflect", "Symbol", "Intl",    "console",
};

enum { IDENT_MAX_WORDS = 16, IDENT_WORD_SZ = 64, IDENT_MIN_WORD = 3, IDENT_MIN_PREFIX = 4 };

typedef struct {
    char text[IDENT_WORD_SZ];
    size_t len;
} ident_word_t;

static bool in_list(const char *s, size_t len, const char *const *list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strlen(list[i]) == len && strncmp(list[i], s, len) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '$';
}

/* Does the word at `s[i]` end here? `fooBar` breaks before `B`; an acronym
 * breaks before its last capital when a lower-case letter follows
 * (`HTTPClient` → `HTTP`, `Client`). */
static bool camel_break(const char *s, size_t len, size_t i) {
    unsigned char prev = (unsigned char)s[i - SKIP_ONE];
    unsigned char cur = (unsigned char)s[i];
    if (!isupper(cur)) {
        return false;
    }
    if (islower(prev) || isdigit(prev)) {
        return true;
    }
    return isupper(prev) && i + SKIP_ONE < len && islower((unsigned char)s[i + SKIP_ONE]);
}

/* Split an identifier into lower-cased camel-case words. One trailing `s` is
 * dropped so `todos` and `Todo` agree, and words shorter than three letters
 * (`r`, `by`) are skipped as carrying no evidence. */
static int ident_words(const char *s, size_t len, ident_word_t *out, int max) {
    int n = 0;
    size_t i = 0;
    while (i < len && n < max) {
        while (i < len && !isalnum((unsigned char)s[i])) {
            i++;
        }
        if (i >= len) {
            break;
        }
        size_t start = i++;
        while (i < len && isalnum((unsigned char)s[i]) && !camel_break(s, len, i)) {
            i++;
        }
        size_t wlen = i - start;
        if (wlen >= IDENT_WORD_SZ) {
            wlen = IDENT_WORD_SZ - SKIP_ONE;
        }
        for (size_t k = 0; k < wlen; k++) {
            out[n].text[k] = (char)tolower((unsigned char)s[start + k]);
        }
        if (wlen > SKIP_ONE && out[n].text[wlen - SKIP_ONE] == 's') {
            wlen--;
        }
        out[n].text[wlen] = '\0';
        out[n].len = wlen;
        if (wlen >= IDENT_MIN_WORD) {
            n++;
        }
    }
    return n;
}

/* The class segment of a method QN: `proj.mod.TodoRepository.get` → `TodoRepository`. */
static bool owner_class(const char *target_qn, char *buf, size_t size) {
    const char *last = strrchr(target_qn, '.');
    if (!last || last == target_qn) {
        return false;
    }
    const char *start = last - SKIP_ONE;
    while (start > target_qn && *start != '.') {
        start--;
    }
    if (*start == '.') {
        start++;
    }
    size_t len = (size_t)(last - start);
    if (len == 0 || len >= size) {
        return false;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    return true;
}

/* Does the receiver text carry evidence for the target's class?
 *   - the class name is one of its identifiers: `new TodoRepository().get`,
 *     `inject(HistoryStack).push`;
 *   - or its trailing identifier shares a word with the class name, whole or
 *     as an abbreviation of four letters or more: `historyStack` for
 *     `HistoryStack`; `this.todos`, `todosRepo` and `repo` for `TodoRepository`;
 *     `cache` for `JSONCache`. */
static bool receiver_names_class(const char *recv, size_t len, const char *owner) {
    size_t owner_len = strlen(owner);
    size_t i = 0;
    while (i < len) {
        if (!is_ident_char(recv[i])) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < len && is_ident_char(recv[i])) {
            i++;
        }
        if (i - start == owner_len && strncmp(recv + start, owner, owner_len) == 0) {
            return true;
        }
    }
    if (len == 0 || !is_ident_char(recv[len - SKIP_ONE])) {
        return false; /* a call result or an index: no name of its own */
    }
    size_t start = len;
    while (start > 0 && is_ident_char(recv[start - SKIP_ONE])) {
        start--;
    }
    ident_word_t rw[IDENT_MAX_WORDS];
    ident_word_t cw[IDENT_MAX_WORDS];
    int rn = ident_words(recv + start, len - start, rw, IDENT_MAX_WORDS);
    int cn = ident_words(owner, owner_len, cw, IDENT_MAX_WORDS);
    for (int a = 0; a < rn; a++) {
        for (int b = 0; b < cn; b++) {
            if (rw[a].len == cw[b].len && strcmp(rw[a].text, cw[b].text) == 0) {
                return true;
            }
            if (rw[a].len >= IDENT_MIN_PREFIX && rw[a].len < cw[b].len &&
                strncmp(cw[b].text, rw[a].text, rw[a].len) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* A name-guessed member call reaching a method (not a Function). Refused when
 * the receiver is a global namespace or the method name is a builtin, unless
 * the receiver is `this`/`super` or names the method's class. */
static bool admits_method_guess(const char *callee_name, const char *target_qn) {
    const char *last_dot = strrchr(callee_name, '.');
    size_t recv_len = trim_trailing_layout(callee_name, (size_t)(last_dot - callee_name));
    while (recv_len > 0 &&
           (callee_name[recv_len - SKIP_ONE] == '?' || callee_name[recv_len - SKIP_ONE] == '!')) {
        recv_len = trim_trailing_layout(callee_name, recv_len - SKIP_ONE); /* `a?.b`, `a!.b` */
    }
    if (root_is(callee_name, recv_len, "this") || root_is(callee_name, recv_len, "super")) {
        return true;
    }
    char owner[CTX_SZ_256];
    if (owner_class(target_qn, owner, sizeof(owner)) &&
        receiver_names_class(callee_name, recv_len, owner)) {
        return true;
    }
    if (in_list(callee_name, recv_len, GLOBAL_NAMESPACES,
                sizeof(GLOBAL_NAMESPACES) / sizeof(GLOBAL_NAMESPACES[0]))) {
        return false;
    }
    const char *method = last_dot + SKIP_ONE;
    return !in_list(method, trim_trailing_layout(method, strlen(method)), BUILTIN_METHOD_NAMES,
                    sizeof(BUILTIN_METHOD_NAMES) / sizeof(BUILTIN_METHOD_NAMES[0]));
}

bool ctx_registry_admits_member_call(const ctx_registry_t *r, CtxLanguage lang,
                                     const char *callee_name, const ctx_resolution_t *res,
                                     const CtxImportArray *imports) {
    if (!lang_has_member_call_rule(lang) || !callee_name || !res || !res->qualified_name) {
        return true;
    }
    const char *dot = strchr(callee_name, '.');
    if (!dot) {
        return true; /* a plain call */
    }
    if (!binds_by_name_alone(res->strategy)) {
        /* import_map / same_module are out of scope here. same_module's
         * last-segment fallback is a guess too; upstream guards it separately
         * (PR #1128, plan step 3 of T-dt3b). */
        return true;
    }
    const char *label = ctx_registry_label_of(r, res->qualified_name);
    if (!label) {
        return true;
    }
    if (strcmp(label, "Function") != 0) {
        /* Methods and classes are what member calls reach, but a guess by a
         * builtin name, or on a global namespace, reaches the builtin. */
        return admits_method_guess(callee_name, res->qualified_name);
    }
    if (strchr(dot + SKIP_ONE, '.') != NULL) {
        /* `fmt.wrap(x).format()`, `ns.inner.f()`, `this.rows.map()`: the
         * receiver of the last segment is a property or a call result, never
         * the root itself, whatever the root is. */
        return false;
    }
    size_t root_len = trim_trailing_layout(callee_name, (size_t)(dot - callee_name));
    if (root_len > 0 && callee_name[root_len - SKIP_ONE] == '?') {
        root_len = trim_trailing_layout(callee_name, root_len - SKIP_ONE); /* `a?.b()` */
    }
    if (root_is(callee_name, root_len, "this") || root_is(callee_name, root_len, "super")) {
        return true; /* `this.f()`: kept as before, as upstream keeps it */
    }
    return root_is_imported(callee_name, root_len, imports);
}

/* ── Fuzzy Resolve ──────────────────────────────────────────────── */

/* Filter candidates by import reachability. Returns count of reachable. */


ctx_fuzzy_result_t ctx_registry_fuzzy_resolve(const ctx_registry_t *r, const char *callee_name,
                                              const char *module_qn, const char **import_map_keys,
                                              const char **import_map_vals, int import_map_count) {
    (void)import_map_keys;
    ctx_fuzzy_result_t no_match = {{0}, false};
    if (!r || !callee_name) {
        return no_match;
    }

    /* Extract simple name (last segment after dots) */
    const char *lookup = simple_name(callee_name);
    qn_array_t *arr = ctx_ht_get(r->by_name, lookup);
    if (!arr || arr->count == 0) {
        return no_match;
    }

    bool have_imports = (import_map_vals && import_map_count > 0);

    /* Single candidate */
    if (arr->count == SKIP_ONE) {
        double conf = CONF_FUZZY_SINGLE;
        if (have_imports &&
            !is_import_reachable(arr->items[0], import_map_vals, import_map_count)) {
            conf *= DEFAULT_CONFIDENCE;
        }
        return (ctx_fuzzy_result_t){{arr->items[0], "fuzzy", conf, REG_RESOLVED}, true};
    }

    /* Multiple candidates: filter by import reachability */
    const char *filtered[CTX_SZ_256];
    int fcount = arr->count;
    const char **fptr = (const char **)arr->items;

    if (have_imports) {
        fcount = filter_import_reachable((const char **)arr->items, arr->count, import_map_vals,
                                         import_map_count, filtered, CTX_SZ_256);
        fptr = filtered;
    }

    if (fcount == 0) {
        /* No import-reachable — use originals with penalty */
        const char *best =
            best_by_import_distance((const char **)arr->items, arr->count, module_qn);
        if (!best) {
            return no_match;
        }
        return (ctx_fuzzy_result_t){
            {best, "fuzzy",
             candidate_count_penalty(CONF_FUZZY_MULTI * REG_HALF_PENALTY, arr->count), arr->count},
            true};
    }
    if (fcount == SKIP_ONE) {
        return (ctx_fuzzy_result_t){
            {fptr[0], "fuzzy", candidate_count_penalty(CONF_FUZZY_SINGLE, arr->count), arr->count},
            true};
    }
    const char *best = best_by_import_distance(fptr, fcount, module_qn);
    if (!best) {
        return no_match;
    }
    return (ctx_fuzzy_result_t){
        {best, "fuzzy", candidate_count_penalty(CONF_FUZZY_MULTI, fcount), fcount}, true};
}

/* ── FindEndingWith ─────────────────────────────────────────────── */

struct few_ctx {
    const char *target; /* ".suffix" */
    size_t target_len;
    const char **results;
    int count;
    int cap;
};

static void few_scan(const char *key, void *value, void *ud) {
    (void)value;
    struct few_ctx *ctx = ud;
    size_t klen = strlen(key);
    if (klen >= ctx->target_len && strcmp(key + klen - ctx->target_len, ctx->target) == 0) {
        if (ctx->count >= ctx->cap) {
            ctx->cap = ctx->cap ? ctx->cap * PAIR_LEN : REG_INIT_CAP;
            ctx->results = safe_realloc(ctx->results, (size_t)ctx->cap * sizeof(char *));
        }
        ctx->results[ctx->count++] = key;
    }
}

int ctx_registry_find_ending_with(const ctx_registry_t *r, const char *suffix, const char ***out) {
    if (!r || !suffix || !out) {
        if (out) {
            *out = NULL;
        }
        return 0;
    }

    /* Build ".suffix" target */
    size_t slen = strlen(suffix);
    char *target = malloc(slen + REG_SUFFIX_ALLOC);
    target[0] = '.';
    memcpy(target + SKIP_ONE, suffix, slen + SKIP_ONE);

    struct few_ctx ctx = {target, slen + SKIP_ONE, NULL, 0, 0};
    ctx_ht_foreach(r->exact, few_scan, &ctx);

    free(target);
    *out = ctx.results;
    return ctx.count;
}

/* Public wrapper for is_import_reachable (testing). */
bool ctx_registry_is_import_reachable(const char *candidate_qn, const char **import_vals,
                                      int import_count) {
    return is_import_reachable(candidate_qn, import_vals, import_count);
}

/*
 * path_alias.c — tsconfig / jsconfig path aliases. See path_alias.h for the
 * rules followed; this file is the loader (repo walk + `extends` chains) and
 * the candidate lookup.
 *
 * Every config in the repo gets a scope, including one that declares no
 * alias: a project whose tsconfig.json sets no `paths` gets none, and must not
 * fall through to an ancestor's. Configs are parsed once per scope; a shared
 * base is reparsed per project that extends it, which on a 112-project Nx
 * workspace is a few milliseconds.
 */
#include "pipeline/path_alias.h"
#include "pipeline/pipeline_internal.h"

#include "discover/discover.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/platform.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson/yyjson.h>

enum {
    PA_MAX_CONFIGS = 1024,         /* config files kept from one repo walk */
    PA_MAX_WALK_DEPTH = 32,        /* directory recursion */
    PA_MAX_EXTENDS_DEPTH = 16,     /* `extends` hops */
    PA_MAX_EXTENDS_LOADS = 64,     /* config files read for one project, bases included */
    PA_MAX_TARGETS = CTX_PATH_ALIAS_MAX_TARGETS,
    PA_MAX_FILE_BYTES = 1 << 20,   /* a tsconfig larger than this is skipped */
    PA_PATH_MAX = 1024,
};

typedef struct {
    char *prefix; /* the pattern before its '*', or all of it */
    char *suffix; /* the pattern after its '*'; "" without one */
    bool wildcard;
    char *targets[PA_MAX_TARGETS]; /* as written, or repo-relative once anchored */
    int target_count;
} pa_pattern_t;

typedef struct {
    pa_pattern_t *items;
    int count;
} pa_patterns_t;

/* One config's effective options. */
typedef struct {
    pa_patterns_t paths;
    char *paths_dir; /* repo-relative directory of the file that declared paths */
    char *base_url;  /* repo-relative, resolved; NULL when unset */
} pa_options_t;

typedef struct {
    char *dir; /* repo-relative directory of the tsconfig/jsconfig ("" = root) */
    pa_patterns_t paths;
    char *base_url;
} pa_scope_t;

struct ctx_path_aliases {
    pa_scope_t *scopes; /* deepest directory first */
    int count;
};

/* ── Paths ───────────────────────────────────────────────────────── */

/* dir + "/" + rel, with "." and ".." segments collapsed, repo-relative. A
 * trailing '/' on rel survives (a target written "libs/x/" keeps its
 * shape). NULL when the path climbs above the repo root or does not fit. */
static char *pa_join(const char *dir, const char *rel) {
    char buf[PA_PATH_MAX];
    size_t len = 0;
    buf[0] = '\0';
    const char *parts[2] = {dir ? dir : "", rel ? rel : ""};
    for (int part = 0; part < 2; part++) {
        const char *p = parts[part];
        while (*p) {
            while (*p == '/') {
                p++;
            }
            const char *seg = p;
            while (*p && *p != '/') {
                p++;
            }
            size_t seg_len = (size_t)(p - seg);
            if (seg_len == 0 || (seg_len == 1 && seg[0] == '.')) {
                continue;
            }
            if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
                if (len == 0) {
                    return NULL;
                }
                char *last = strrchr(buf, '/');
                len = last ? (size_t)(last - buf) : 0;
                buf[len] = '\0';
                continue;
            }
            if (len + (len ? 1 : 0) + seg_len + 1 >= sizeof(buf)) {
                return NULL;
            }
            if (len) {
                buf[len++] = '/';
            }
            memcpy(buf + len, seg, seg_len);
            len += seg_len;
            buf[len] = '\0';
        }
    }
    size_t rel_len = rel ? strlen(rel) : 0;
    if (rel_len > 0 && rel[rel_len - 1] == '/' && len > 0 && len + 1 < sizeof(buf)) {
        buf[len++] = '/';
        buf[len] = '\0';
    }
    return strdup(buf);
}

/* The directory part of a repo-relative file path ("" at the root). */
static char *pa_dirname(const char *rel) {
    const char *slash = strrchr(rel, '/');
    return slash ? ctx_strndup(rel, (size_t)(slash - rel)) : strdup("");
}

static bool pa_exists(const char *repo, const char *rel) {
    char abs[PA_PATH_MAX];
    int n = snprintf(abs, sizeof(abs), "%s/%s", repo, rel);
    return n > 0 && (size_t)n < sizeof(abs) && ctx_file_exists(abs) && !ctx_is_dir(abs);
}

/* ── Patterns ────────────────────────────────────────────────────── */

static void pa_patterns_free(pa_patterns_t *ps) {
    for (int i = 0; i < ps->count; i++) {
        free(ps->items[i].prefix);
        free(ps->items[i].suffix);
        for (int t = 0; t < ps->items[i].target_count; t++) {
            free(ps->items[i].targets[t]);
        }
    }
    free(ps->items);
    ps->items = NULL;
    ps->count = 0;
}

/* compilerOptions.paths, targets as written. */
static pa_patterns_t pa_patterns_parse(yyjson_val *paths) {
    pa_patterns_t ps = {0};
    size_t size = yyjson_obj_size(paths);
    if (size == 0) {
        return ps;
    }
    ps.items = calloc(size, sizeof(pa_pattern_t));
    if (!ps.items) {
        return ps;
    }
    yyjson_val *key;
    yyjson_obj_iter iter = yyjson_obj_iter_with(paths);
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        const char *pattern = yyjson_get_str(key);
        yyjson_val *targets = yyjson_obj_iter_get_val(key);
        if (!pattern || !yyjson_is_arr(targets)) {
            continue;
        }
        pa_pattern_t *p = &ps.items[ps.count];
        const char *star = strchr(pattern, '*');
        p->wildcard = star != NULL;
        p->prefix = star ? ctx_strndup(pattern, (size_t)(star - pattern)) : strdup(pattern);
        p->suffix = strdup(star ? star + 1 : "");
        yyjson_val *target;
        yyjson_arr_iter titer = yyjson_arr_iter_with(targets);
        while ((target = yyjson_arr_iter_next(&titer)) != NULL &&
               p->target_count < PA_MAX_TARGETS) {
            const char *t = yyjson_get_str(target);
            if (t && t[0]) {
                p->targets[p->target_count++] = strdup(t);
            }
        }
        ps.count++;
    }
    return ps;
}

/* Targets as written → repo-relative under anchor. A target that climbs out
 * of the repo is dropped. */
static void pa_patterns_anchor(pa_patterns_t *ps, const char *anchor) {
    for (int i = 0; i < ps->count; i++) {
        pa_pattern_t *p = &ps->items[i];
        int kept = 0;
        for (int t = 0; t < p->target_count; t++) {
            char *joined = pa_join(anchor, p->targets[t]);
            free(p->targets[t]);
            if (joined) {
                p->targets[kept++] = joined;
            }
        }
        p->target_count = kept;
    }
}

/* ── Configs and `extends` ───────────────────────────────────────── */

static void pa_options_free(pa_options_t *o) {
    pa_patterns_free(&o->paths);
    free(o->paths_dir);
    free(o->base_url);
    *o = (pa_options_t){0};
}

/* A base's options under the ones collected so far: what the base sets wins
 * over earlier bases. `paths` moves as a whole, with the directory it was
 * declared in. */
static void pa_options_take(pa_options_t *into, pa_options_t *from) {
    if (from->paths_dir) {
        pa_patterns_free(&into->paths);
        free(into->paths_dir);
        into->paths = from->paths;
        into->paths_dir = from->paths_dir;
        from->paths = (pa_patterns_t){0};
        from->paths_dir = NULL;
    }
    if (from->base_url) {
        free(into->base_url);
        into->base_url = from->base_url;
        from->base_url = NULL;
    }
}

static yyjson_doc *pa_read_json(const char *repo, const char *rel) {
    char abs[PA_PATH_MAX];
    int n = snprintf(abs, sizeof(abs), "%s/%s", repo, rel);
    if (n <= 0 || (size_t)n >= sizeof(abs)) {
        return NULL;
    }
    FILE *f = fopen(abs, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > PA_MAX_FILE_BYTES) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    /* tsconfig is JSONC: comments and trailing commas are legal, and a file
     * saved with a UTF-8 byte order mark still loads in TypeScript. */
    yyjson_doc *doc = yyjson_read(buf, got,
                                  YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS |
                                      YYJSON_READ_ALLOW_BOM);
    free(buf);
    return doc;
}

/* The repo-relative file an `extends` entry names, or NULL: relative to the
 * extending file's directory, `.json` appended when left off. A package
 * name is not followed (see path_alias.h). */
static char *pa_extends_target(const char *repo, const char *dir, const char *spec) {
    if (spec[0] != '.') {
        return NULL;
    }
    char *rel = pa_join(dir, spec);
    if (!rel || pa_exists(repo, rel)) {
        return rel;
    }
    size_t len = strlen(rel);
    if (len < 5 || strcmp(rel + len - 5, ".json") != 0) {
        char with_ext[PA_PATH_MAX];
        snprintf(with_ext, sizeof(with_ext), "%s.json", rel);
        free(rel);
        return pa_exists(repo, with_ext) ? strdup(with_ext) : NULL;
    }
    free(rel);
    return NULL;
}

/* The effective options of the config at rel: its bases first, in order,
 * then its own. depth bounds the chain and *budget the files read for it, so
 * a cycle ends; depth alone would not end one that branches, a config whose
 * `extends` array names itself k times costing k^depth reads. */
static pa_options_t pa_options_load(const char *repo, const char *rel, int depth, int *budget) {
    pa_options_t opts = {0};
    if (depth > PA_MAX_EXTENDS_DEPTH || *budget <= 0) {
        return opts;
    }
    (*budget)--;
    yyjson_doc *doc = pa_read_json(repo, rel);
    if (!doc) {
        return opts;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    char *dir = pa_dirname(rel);

    yyjson_val *ext = yyjson_obj_get(root, "extends");
    yyjson_val *one = NULL;
    yyjson_arr_iter it = {0};
    bool is_arr = yyjson_is_arr(ext);
    if (is_arr) {
        it = yyjson_arr_iter_with(ext);
    } else if (yyjson_is_str(ext)) {
        one = ext;
    }
    for (yyjson_val *e = is_arr ? yyjson_arr_iter_next(&it) : one; e;
         e = is_arr ? yyjson_arr_iter_next(&it) : NULL) {
        const char *spec = yyjson_get_str(e);
        char *base_rel = spec ? pa_extends_target(repo, dir, spec) : NULL;
        if (base_rel) {
            pa_options_t base = pa_options_load(repo, base_rel, depth + 1, budget);
            pa_options_take(&opts, &base);
            pa_options_free(&base);
            free(base_rel);
        }
    }

    yyjson_val *co = yyjson_obj_get(root, "compilerOptions");
    yyjson_val *paths = yyjson_obj_get(co, "paths");
    if (yyjson_is_obj(paths)) {
        pa_options_t own = {.paths = pa_patterns_parse(paths), .paths_dir = strdup(dir)};
        pa_options_take(&opts, &own);
        pa_options_free(&own);
    }
    const char *base_url = yyjson_get_str(yyjson_obj_get(co, "baseUrl"));
    if (base_url) {
        char *resolved = pa_join(dir, base_url);
        if (resolved) {
            free(opts.base_url);
            opts.base_url = resolved;
        }
    }
    free(dir);
    yyjson_doc_free(doc);
    return opts;
}

/* ── Repo walk ───────────────────────────────────────────────────── */

typedef struct {
    const char *repo;
    ctx_gitignore_t *gitignore;
    char **configs; /* repo-relative paths */
    int count;
} pa_walk_t;

static void pa_walk(pa_walk_t *w, const char *rel_dir, int depth) {
    if (w->count >= PA_MAX_CONFIGS || depth > PA_MAX_WALK_DEPTH) {
        return;
    }
    char abs[PA_PATH_MAX];
    snprintf(abs, sizeof(abs), "%s%s%s", w->repo, rel_dir[0] ? "/" : "", rel_dir);
    static const char *const names[] = {"tsconfig.json", "jsconfig.json"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char *rel = pa_join(rel_dir, names[i]);
        if (rel && pa_exists(w->repo, rel)) {
            w->configs[w->count++] = rel;
            break;
        }
        free(rel);
    }
    ctx_dir_t *d = ctx_opendir(abs);
    if (!d) {
        return;
    }
    ctx_dirent_t *ent;
    while ((ent = ctx_readdir(d)) != NULL && w->count < PA_MAX_CONFIGS) {
        if (!ent->is_dir || ctx_should_skip_dir(ent->name, CTX_MODE_FULL)) {
            continue;
        }
        char *child = pa_join(rel_dir, ent->name);
        if (child && !(w->gitignore && ctx_gitignore_matches(w->gitignore, child, true))) {
            pa_walk(w, child, depth + 1);
        }
        free(child);
    }
    ctx_closedir(d);
}

static int pa_scope_deeper_first(const void *a, const void *b) {
    size_t la = strlen(((const pa_scope_t *)a)->dir);
    size_t lb = strlen(((const pa_scope_t *)b)->dir);
    return la < lb ? 1 : (la > lb ? -1 : 0);
}

/* ── Public API ──────────────────────────────────────────────────── */

void ctx_path_aliases_free(ctx_path_aliases_t *aliases) {
    if (!aliases) {
        return;
    }
    for (int i = 0; i < aliases->count; i++) {
        free(aliases->scopes[i].dir);
        pa_patterns_free(&aliases->scopes[i].paths);
        free(aliases->scopes[i].base_url);
    }
    free(aliases->scopes);
    free(aliases);
}

ctx_path_aliases_t *ctx_path_aliases_load(const char *repo_path) {
    if (!repo_path || !repo_path[0]) {
        return NULL;
    }
    pa_walk_t w = {.repo = repo_path, .configs = calloc(PA_MAX_CONFIGS, sizeof(char *))};
    if (!w.configs) {
        return NULL;
    }
    char gitignore_path[PA_PATH_MAX];
    snprintf(gitignore_path, sizeof(gitignore_path), "%s/.gitignore", repo_path);
    w.gitignore = ctx_gitignore_load(gitignore_path);
    pa_walk(&w, "", 0);
    ctx_gitignore_free(w.gitignore);
    if (w.count >= PA_MAX_CONFIGS) {
        ctx_log_warn("path_alias.configs.cap_hit", "repo", repo_path);
    }

    ctx_path_aliases_t *aliases = calloc(1, sizeof(*aliases));
    if (aliases && w.count > 0) {
        aliases->scopes = calloc((size_t)w.count, sizeof(pa_scope_t));
    }
    bool any = false;
    for (int i = 0; i < w.count; i++) {
        if (aliases && aliases->scopes) {
            int budget = PA_MAX_EXTENDS_LOADS;
            pa_options_t opts = pa_options_load(repo_path, w.configs[i], 0, &budget);
            /* paths resolve against baseUrl when there is one, else against
             * the file that declared them. */
            pa_patterns_anchor(&opts.paths, opts.base_url ? opts.base_url : opts.paths_dir);
            pa_scope_t *s = &aliases->scopes[aliases->count++];
            s->dir = pa_dirname(w.configs[i]);
            s->paths = opts.paths;
            s->base_url = opts.base_url;
            any = any || s->paths.count > 0 || s->base_url;
            opts.paths = (pa_patterns_t){0};
            opts.base_url = NULL;
            pa_options_free(&opts);
        }
        free(w.configs[i]);
    }
    free(w.configs);
    if (!any) {
        ctx_path_aliases_free(aliases);
        return NULL;
    }
    qsort(aliases->scopes, (size_t)aliases->count, sizeof(pa_scope_t), pa_scope_deeper_first);
    char count_buf[CTX_SZ_32];
    snprintf(count_buf, sizeof(count_buf), "%d", aliases->count);
    ctx_log_info("path_alias.loaded", "configs", count_buf);
    return aliases;
}

/* tsconfig governs the JS family only; a Python file under one must not have
 * its dotted module names read as baseUrl paths. */
bool ctx_path_aliases_is_script(const char *rel) {
    if (!rel) {
        return false;
    }
    static const char *const exts[] = {".ts",  ".tsx", ".mts", ".cts",  ".js",     ".jsx",
                                       ".mjs", ".cjs", ".vue", ".svelte", ".astro"};
    const char *dot = strrchr(rel, '.');
    if (!dot || strchr(dot, '/')) {
        return false;
    }
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (strcmp(dot, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* The nearest config at or above the file's directory. */
static const pa_scope_t *pa_scope_for(const ctx_path_aliases_t *aliases, const char *rel) {
    for (int i = 0; i < aliases->count; i++) {
        const char *dir = aliases->scopes[i].dir;
        size_t len = strlen(dir);
        if (len == 0 || (strncmp(rel, dir, len) == 0 && rel[len] == '/')) {
            return &aliases->scopes[i];
        }
    }
    return NULL;
}

/* The pattern TypeScript picks: an exact one, else the wildcard whose prefix
 * is longest. *star_len is the length of the text the '*' stands for. */
static const pa_pattern_t *pa_best_pattern(const pa_patterns_t *ps, const char *spec,
                                           size_t *star_start, size_t *star_len) {
    size_t spec_len = strlen(spec);
    const pa_pattern_t *best = NULL;
    for (int i = 0; i < ps->count; i++) {
        const pa_pattern_t *p = &ps->items[i];
        if (!p->wildcard) {
            if (strcmp(spec, p->prefix) == 0) {
                *star_start = spec_len;
                *star_len = 0;
                return p;
            }
            continue;
        }
        size_t plen = strlen(p->prefix);
        size_t slen = strlen(p->suffix);
        if (spec_len < plen + slen || strncmp(spec, p->prefix, plen) != 0 ||
            strcmp(spec + spec_len - slen, p->suffix) != 0) {
            continue;
        }
        if (!best || plen > strlen(best->prefix)) {
            best = p;
            *star_start = plen;
            *star_len = spec_len - plen - slen;
        }
    }
    return best;
}

int ctx_path_aliases_candidates(const ctx_path_aliases_t *aliases, const char *source_rel,
                                const char *specifier, char **out, int max) {
    if (!aliases || !source_rel || !specifier || !specifier[0] || specifier[0] == '.' ||
        specifier[0] == '/' || max <= 0 || !ctx_path_aliases_is_script(source_rel)) {
        return 0;
    }
    const pa_scope_t *scope = pa_scope_for(aliases, source_rel);
    if (!scope) {
        return 0;
    }
    int n = 0;
    size_t star_start = 0;
    size_t star_len = 0;
    const pa_pattern_t *p = pa_best_pattern(&scope->paths, specifier, &star_start, &star_len);
    for (int t = 0; p && t < p->target_count && n < max; t++) {
        const char *target = p->targets[t];
        const char *star = strchr(target, '*');
        char buf[PA_PATH_MAX];
        int len = star ? snprintf(buf, sizeof(buf), "%.*s%.*s%s", (int)(star - target), target,
                                  (int)star_len, specifier + star_start, star + 1)
                       : snprintf(buf, sizeof(buf), "%s", target);
        if (len > 0 && (size_t)len < sizeof(buf)) {
            out[n++] = strdup(buf);
        }
    }
    if (scope->base_url && n < max) {
        char *joined = pa_join(scope->base_url, specifier);
        if (joined) {
            out[n++] = joined;
        }
    }
    return n;
}

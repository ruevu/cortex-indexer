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

#include <ctype.h>
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
    PA_MAX_PACKAGES = 4096,        /* workspace package names kept from one repo walk */
    PA_MAX_BASE_ENTRIES = 4096,    /* entries listed under one baseUrl */
    PA_MAX_ALIAS_KEYS = 4096,      /* `paths` keys kept from configs no scope reads */
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

/* A sorted set of strings. */
typedef struct {
    char **items;
    int count;
} pa_names_t;

typedef struct {
    char *dir; /* repo-relative directory of the tsconfig/jsconfig ("" = root) */
    pa_patterns_t paths;
    char *base_url;
    pa_names_t base_entries; /* what baseUrl reaches: directory names, file stems */
    bool base_truncated;     /* base_entries stopped at its cap */
} pa_scope_t;

struct ctx_path_aliases {
    pa_scope_t *scopes; /* deepest directory first */
    int count;
    pa_names_t packages;   /* package.json names, and keys of dependencies in the tree */
    pa_names_t alias_keys; /* `paths` keys of the configs no scope reads */
    bool incomplete;       /* the walk stopped at a cap: claims every package */
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

/* ── Name sets ───────────────────────────────────────────────────── */

static int pa_strcmp_ptr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Sort and drop duplicates; call once, after the last add. */
static void pa_names_seal(pa_names_t *ns) {
    if (ns->count <= 1) {
        return;
    }
    qsort(ns->items, (size_t)ns->count, sizeof(char *), pa_strcmp_ptr);
    int kept = 1;
    for (int i = 1; i < ns->count; i++) {
        if (strcmp(ns->items[i], ns->items[kept - 1]) == 0) {
            free(ns->items[i]);
        } else {
            ns->items[kept++] = ns->items[i];
        }
    }
    ns->count = kept;
}

static bool pa_names_has(const pa_names_t *ns, const char *name) {
    return ns->count > 0 &&
           bsearch(&name, ns->items, (size_t)ns->count, sizeof(char *), pa_strcmp_ptr) != NULL;
}

static void pa_names_free(pa_names_t *ns) {
    for (int i = 0; i < ns->count; i++) {
        free(ns->items[i]);
    }
    free(ns->items);
    *ns = (pa_names_t){0};
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
    pa_names_t packages;   /* capacity PA_MAX_PACKAGES */
    pa_names_t alias_keys; /* capacity PA_MAX_ALIAS_KEYS */
    bool truncated;        /* something the walk would have kept was left out */
} pa_walk_t;

/* A copy of s into ns, which holds at most cap. What does not fit truncates
 * the walk. */
static void pa_walk_keep(pa_walk_t *w, pa_names_t *ns, int cap, const char *s) {
    char *copy = ns->items && ns->count < cap ? strdup(s) : NULL;
    if (!copy) {
        w->truncated = true;
        return;
    }
    ns->items[ns->count++] = copy;
}

/* What a package.json in rel_dir lets the tree import by a package name,
 * kept in w->packages: its `name`, a workspace package's; and each
 * dependency key whose version points into the tree (`file:`, `link:`,
 * `portal:`, `workspace:`), which the code imports by that key whatever the
 * package there calls itself. node_modules is never walked, so a
 * dependency's own manifest adds nothing. */
static void pa_collect_package(pa_walk_t *w, const char *rel_dir) {
    char *rel = pa_join(rel_dir, "package.json");
    yyjson_doc *doc = rel && pa_exists(w->repo, rel) ? pa_read_json(w->repo, rel) : NULL;
    free(rel);
    if (!doc) {
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *name = yyjson_get_str(yyjson_obj_get(root, "name"));
    if (name && name[0]) {
        pa_walk_keep(w, &w->packages, PA_MAX_PACKAGES, name);
    }
    static const char *const fields[] = {"dependencies", "devDependencies", "peerDependencies",
                                         "optionalDependencies"};
    static const char *const local[] = {"file:", "link:", "portal:", "workspace:"};
    for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
        yyjson_val *deps = yyjson_obj_get(root, fields[f]);
        yyjson_val *key;
        yyjson_obj_iter iter = yyjson_obj_iter_with(deps);
        while (yyjson_is_obj(deps) && (key = yyjson_obj_iter_next(&iter)) != NULL) {
            const char *dep = yyjson_get_str(key);
            const char *version = yyjson_get_str(yyjson_obj_iter_get_val(key));
            for (size_t l = 0; dep && dep[0] && version && l < sizeof(local) / sizeof(local[0]);
                 l++) {
                if (strncmp(version, local[l], strlen(local[l])) == 0) {
                    pa_walk_keep(w, &w->packages, PA_MAX_PACKAGES, dep);
                    break;
                }
            }
        }
    }
    yyjson_doc_free(doc);
}

/* Whether name is a tsconfig/jsconfig variant: tsconfig.app.json,
 * jsconfig.base.json, the plain tsconfig.json too. */
static bool pa_is_config_name(const char *name) {
    size_t len = strlen(name);
    return (strncmp(name, "tsconfig.", 9) == 0 || strncmp(name, "jsconfig.", 9) == 0) &&
           len >= strlen("tsconfig.json") && strcmp(name + len - 5, ".json") == 0;
}

/* The `paths` keys of a config no scope reads, kept in w->alias_keys. Vite's
 * template keeps its aliases in tsconfig.app.json, which the project's
 * tsconfig.json only references; the loader scopes configs by directory, so
 * these keys are kept for the whole tree, to claim with. A bare "*" key would
 * claim every package and is left out. */
static void pa_collect_alias_keys(pa_walk_t *w, const char *rel_dir, const char *name) {
    char *rel = pa_join(rel_dir, name);
    yyjson_doc *doc = rel ? pa_read_json(w->repo, rel) : NULL;
    free(rel);
    if (!doc) {
        return;
    }
    yyjson_val *opts = yyjson_obj_get(yyjson_doc_get_root(doc), "compilerOptions");
    yyjson_val *paths = yyjson_obj_get(opts, "paths");
    yyjson_val *key;
    yyjson_obj_iter iter = yyjson_obj_iter_with(paths);
    while (yyjson_is_obj(paths) && (key = yyjson_obj_iter_next(&iter)) != NULL) {
        const char *pattern = yyjson_get_str(key);
        if (pattern && pattern[0] && strcmp(pattern, "*") != 0) {
            pa_walk_keep(w, &w->alias_keys, PA_MAX_ALIAS_KEYS, pattern);
        }
    }
    yyjson_doc_free(doc);
}

static void pa_walk(pa_walk_t *w, const char *rel_dir, int depth) {
    if (w->count >= PA_MAX_CONFIGS || depth > PA_MAX_WALK_DEPTH) {
        w->truncated = true;
        return;
    }
    char abs[PA_PATH_MAX];
    snprintf(abs, sizeof(abs), "%s%s%s", w->repo, rel_dir[0] ? "/" : "", rel_dir);
    static const char *const names[] = {"tsconfig.json", "jsconfig.json"};
    const char *scoped = NULL; /* the config this directory's scope reads */
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char *rel = pa_join(rel_dir, names[i]);
        if (rel && pa_exists(w->repo, rel)) {
            w->configs[w->count++] = rel;
            scoped = names[i];
            break;
        }
        free(rel);
    }
    pa_collect_package(w, rel_dir);
    ctx_dir_t *d = ctx_opendir(abs);
    if (!d) {
        return;
    }
    ctx_dirent_t *ent;
    while ((ent = ctx_readdir(d)) != NULL) {
        if (w->count >= PA_MAX_CONFIGS) {
            w->truncated = true;
            break;
        }
        if (!ent->is_dir) {
            if (pa_is_config_name(ent->name) && !(scoped && strcmp(ent->name, scoped) == 0)) {
                pa_collect_alias_keys(w, rel_dir, ent->name);
            }
            continue;
        }
        if (ctx_should_skip_dir(ent->name, CTX_MODE_FULL)) {
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

/* What a specifier under baseUrl can start with: the names directly under
 * it, a file by its stem as well ("legacy" for legacy.ts). Directories
 * discovery skips (node_modules, dist, ...) and dotted names are left out:
 * a baseUrl of "." must not make every installed package a project path. */
static pa_names_t pa_list_base_entries(const char *repo, const char *base_url,
                                       bool *truncated) {
    pa_names_t ns = {0};
    char abs[PA_PATH_MAX];
    int n = snprintf(abs, sizeof(abs), "%s%s%s", repo, base_url[0] ? "/" : "", base_url);
    ctx_dir_t *d = n > 0 && (size_t)n < sizeof(abs) ? ctx_opendir(abs) : NULL;
    if (!d) {
        return ns;
    }
    ns.items = calloc(PA_MAX_BASE_ENTRIES, sizeof(char *));
    *truncated = ns.items == NULL;
    ctx_dirent_t *ent;
    while (ns.items && (ent = ctx_readdir(d)) != NULL && ns.count < PA_MAX_BASE_ENTRIES - 1) {
        if (ent->name[0] == '.' || (ent->is_dir && ctx_should_skip_dir(ent->name, CTX_MODE_FULL))) {
            continue;
        }
        ns.items[ns.count++] = strdup(ent->name);
        const char *dot = ent->is_dir ? NULL : strrchr(ent->name, '.');
        if (dot && dot != ent->name) {
            ns.items[ns.count++] = ctx_strndup(ent->name, (size_t)(dot - ent->name));
        }
    }
    if (ns.count >= PA_MAX_BASE_ENTRIES - 1) {
        *truncated = true;
        ctx_log_warn("path_alias.base_entries.cap_hit", "base_url", base_url);
    }
    ctx_closedir(d);
    pa_names_seal(&ns);
    return ns;
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
        pa_names_free(&aliases->scopes[i].base_entries);
    }
    free(aliases->scopes);
    pa_names_free(&aliases->packages);
    pa_names_free(&aliases->alias_keys);
    free(aliases);
}

ctx_path_aliases_t *ctx_path_aliases_load(const char *repo_path) {
    if (!repo_path || !repo_path[0]) {
        return NULL;
    }
    pa_walk_t w = {.repo = repo_path,
                   .configs = calloc(PA_MAX_CONFIGS, sizeof(char *)),
                   .packages = {.items = calloc(PA_MAX_PACKAGES, sizeof(char *))},
                   .alias_keys = {.items = calloc(PA_MAX_ALIAS_KEYS, sizeof(char *))}};
    if (!w.configs) {
        pa_names_free(&w.packages);
        pa_names_free(&w.alias_keys);
        return NULL;
    }
    /* What discovery walks, so that a package it indexes is one the tree
     * declares: the root .gitignore applies only where discovery applies it. */
    w.gitignore = ctx_discover_root_gitignore(repo_path);
    pa_walk(&w, "", 0);
    ctx_gitignore_free(w.gitignore);
    if (w.count >= PA_MAX_CONFIGS) {
        ctx_log_warn("path_alias.configs.cap_hit", "repo", repo_path);
    }
    if (w.truncated) {
        ctx_log_warn("path_alias.walk.truncated", "repo", repo_path);
    }
    pa_names_seal(&w.packages);
    pa_names_seal(&w.alias_keys);

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
            if (s->base_url) {
                s->base_entries =
                    pa_list_base_entries(repo_path, s->base_url, &s->base_truncated);
            }
            any = any || s->paths.count > 0 || s->base_url;
            opts.paths = (pa_patterns_t){0};
            opts.base_url = NULL;
            pa_options_free(&opts);
        }
        free(w.configs[i]);
    }
    free(w.configs);
    if (aliases) {
        aliases->packages = w.packages;
        aliases->alias_keys = w.alias_keys;
        aliases->incomplete = w.truncated;
        w.packages = (pa_names_t){0};
        w.alias_keys = (pa_names_t){0};
        any = any || aliases->packages.count > 0 || aliases->alias_keys.count > 0 ||
              aliases->incomplete;
    }
    pa_names_free(&w.packages);
    pa_names_free(&w.alias_keys);
    if (!any) {
        ctx_path_aliases_free(aliases);
        return NULL;
    }
    if (aliases->count > 1) {
        qsort(aliases->scopes, (size_t)aliases->count, sizeof(pa_scope_t),
              pa_scope_deeper_first);
    }
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

/* Whether [s, s+len) is shaped like an npm name or scope: a letter or digit,
 * then letters, digits, '.', '_' and '-'. */
static bool pa_npm_segment(const char *s, size_t len) {
    if (len == 0 || !isalnum((unsigned char)s[0])) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

/* Whether a specifier can name a package at all: a Node, npm, JSR or Bun
 * scheme (`node:path`), or an npm name (`react`, `@scope/pkg`) with any
 * subpath. `~/x`, `@/x`, `#imports`, `$lib/x` and `virtual:x` cannot: they are
 * aliases a framework or bundler resolves, often into the tree, or virtual
 * modules. */
static bool pa_package_shaped(const char *specifier) {
    static const char *const schemes[] = {"node:", "npm:", "jsr:", "bun:"};
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
        if (strncmp(specifier, schemes[i], strlen(schemes[i])) == 0) {
            return true;
        }
    }
    const char *name = specifier;
    if (specifier[0] == '@') {
        const char *slash = strchr(specifier, '/');
        if (!slash || !pa_npm_segment(specifier + 1, (size_t)(slash - specifier - 1))) {
            return false;
        }
        name = slash + 1;
    }
    const char *end = strchr(name, '/');
    return pa_npm_segment(name, end ? (size_t)(end - name) : strlen(name));
}

/* Whether the tree declares the package a specifier names: the whole
 * specifier, else each shorter prefix ending at a '/', so a subpath import
 * ("@acme/kit/src/use") finds its package ("@acme/kit"). */
static bool pa_names_package(const ctx_path_aliases_t *aliases, const char *specifier) {
    if (aliases->packages.count == 0) {
        return false;
    }
    char buf[PA_PATH_MAX];
    int n = snprintf(buf, sizeof(buf), "%s", specifier);
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        return true; /* too long to look up: never judged external */
    }
    for (;;) {
        if (pa_names_has(&aliases->packages, buf)) {
            return true;
        }
        char *slash = strrchr(buf, '/');
        if (!slash) {
            return false;
        }
        *slash = '\0';
    }
}

/* Whether baseUrl reaches something real for the specifier: its first
 * segment is a directory or file directly under baseUrl. */
static bool pa_base_reaches(const pa_scope_t *scope, const char *specifier) {
    if (!scope->base_url) {
        return false;
    }
    if (scope->base_truncated) {
        return true; /* the listing stopped short: whatever it missed */
    }
    const char *slash = strchr(specifier, '/');
    size_t len = slash ? (size_t)(slash - specifier) : strlen(specifier);
    char first[PA_PATH_MAX];
    if (len >= sizeof(first)) {
        return true; /* too long to look up: never judged external */
    }
    if (len == 0 || scope->base_entries.count == 0) {
        return false;
    }
    memcpy(first, specifier, len);
    first[len] = '\0';
    return pa_names_has(&scope->base_entries, first);
}

/* Whether a `paths` key of a config no scope reads matches the specifier,
 * by TypeScript's rule: whole, or around the key's one '*'. */
static bool pa_unscoped_key_matches(const ctx_path_aliases_t *aliases, const char *specifier) {
    size_t spec_len = strlen(specifier);
    for (int i = 0; i < aliases->alias_keys.count; i++) {
        const char *key = aliases->alias_keys.items[i];
        const char *star = strchr(key, '*');
        if (!star) {
            if (strcmp(key, specifier) == 0) {
                return true;
            }
            continue;
        }
        size_t plen = (size_t)(star - key);
        size_t slen = strlen(star + 1);
        if (spec_len >= plen + slen && strncmp(specifier, key, plen) == 0 &&
            strcmp(specifier + spec_len - slen, star + 1) == 0) {
            return true;
        }
    }
    return false;
}

bool ctx_path_aliases_claims(const ctx_path_aliases_t *aliases, const char *source_rel,
                             const char *specifier) {
    if (!specifier || !specifier[0]) {
        return false;
    }
    if (specifier[0] == '.' || specifier[0] == '/') {
        return true;
    }
    if (!source_rel || !ctx_path_aliases_is_script(source_rel)) {
        return false;
    }
    if (!pa_package_shaped(specifier)) {
        return true;
    }
    if (!aliases) {
        return false;
    }
    /* A walk that stopped at a cap missed what it would have claimed. */
    if (aliases->incomplete || pa_names_package(aliases, specifier) ||
        pa_unscoped_key_matches(aliases, specifier)) {
        return true;
    }
    const pa_scope_t *scope = pa_scope_for(aliases, source_rel);
    if (!scope) {
        return false;
    }
    size_t star_start = 0;
    size_t star_len = 0;
    if (pa_best_pattern(&scope->paths, specifier, &star_start, &star_len)) {
        return true;
    }
    return pa_base_reaches(scope, specifier);
}

/* Whether s is two or more segments, none empty. */
static bool pa_two_segments(const char *s) {
    const char *slash = strchr(s, '/');
    if (!slash || slash == s) {
        return false;
    }
    for (const char *p = slash; *p; p++) {
        if (*p == '/' && (p[1] == '/' || p[1] == '\0')) {
            return false;
        }
    }
    return true;
}

/* Whether run occurs in path as whole segments. */
static bool pa_has_segment_run(const char *path, const char *run) {
    size_t n = strlen(run);
    for (const char *p = path; (p = strstr(p, run)) != NULL; p++) {
        if ((p == path || p[-1] == '/') && (p[n] == '\0' || p[n] == '/')) {
            return true;
        }
    }
    return false;
}

bool ctx_path_aliases_specifier_names_file(const char *specifier, const char *file_rel) {
    if (!specifier || !file_rel) {
        return false;
    }
    char path[PA_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s", file_rel);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return false;
    }
    /* A specifier names a module: the file's extension is not part of it. */
    char *slash = strrchr(path, '/');
    char *base = slash ? slash + 1 : path;
    char *dot = strrchr(base, '.');
    if (dot && dot != base) {
        *dot = '\0';
    }
    /* `@utils/money` is spelled with its '@' or without it: an alias key
     * borrows the scope sign, a workspace directory may carry it. */
    const char *forms[] = {specifier, specifier[0] == '@' ? specifier + 1 : NULL};
    for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++) {
        if (forms[i] && pa_two_segments(forms[i]) && pa_has_segment_run(path, forms[i])) {
            return true;
        }
    }
    return false;
}

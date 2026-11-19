/*
 * import_target.c — what an import names: the graph node an import specifier
 * means, seen from the importing file, and the per-file import map built from
 * it. Every import consumer goes through here — IMPORTS edges and the import
 * map of every resolution pass, sequential and parallel — so no two of them
 * can disagree about which module an import means (cortex-indexer T-fdya).
 */
#include "pipeline/path_alias.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"

#include "extract.h"
#include "graph_buffer/graph_buffer.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IT_PATH_MAX = 1024 };

/* The node whose qn is the module qn of path, or NULL. Extension-less, a
 * dot in the last segment belongs to the file's name (`tag.mapper` is
 * tag.mapper.ts); otherwise the last dot starts its extension (`index.ts`).
 * The module qn strips one extension, so an extension-less path is given a
 * placeholder. A Folder shares the module qn of its path; with modules_only
 * it does not count. */
static const ctx_gbuf_node_t *it_node_at(const ctx_pipeline_ctx_t *ctx, const char *path,
                                         bool extensionless, bool modules_only) {
    char buf[IT_PATH_MAX];
    if (extensionless) {
        int n = snprintf(buf, sizeof(buf), "%s.x", path);
        if (n <= 0 || (size_t)n >= sizeof(buf)) {
            return NULL;
        }
        path = buf;
    }
    char *qn = ctx_pipeline_fqn_module(ctx->project_name, path);
    const ctx_gbuf_node_t *node = qn ? ctx_gbuf_find_by_qn(ctx->gbuf, qn) : NULL;
    free(qn);
    if (node && modules_only && node->label && strcmp(node->label, "Folder") == 0) {
        return NULL;
    }
    return node;
}

/* The dot that starts the last path segment's extension, if it has one. */
static const char *it_last_dot(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *seg = slash ? slash + 1 : path;
    const char *dot = strrchr(seg, '.');
    return dot && dot > seg ? dot : NULL;
}

/* The module a path names. For a JS-family importer, TypeScript's order: the
 * file named without its extension (tag.mapper → tag.mapper.ts); the
 * directory's index module (a barrel: `from '../ui'` is ui/index.ts, whose
 * module qn keeps `index`); the file named with its extension (index.ts, or
 * b.js written for b.ts); and only then a Folder, by either reading. For any other
 * importer the last dot starts an extension, or, in a dotted module path
 * (Python's `util.deco.register`), the imported symbol, so the path's own
 * node is the answer, as it always was. */
static const ctx_gbuf_node_t *it_module_node(const ctx_pipeline_ctx_t *ctx, const char *path,
                                             bool ts_rules) {
    if (!ts_rules) {
        return it_node_at(ctx, path, false, false);
    }
    const ctx_gbuf_node_t *node = it_node_at(ctx, path, true, true);
    if (node) {
        return node;
    }
    char index[IT_PATH_MAX];
    size_t len = strlen(path);
    int n = snprintf(index, sizeof(index), "%s%sindex", path,
                     len > 0 && path[len - 1] == '/' ? "" : "/");
    node = n > 0 && (size_t)n < sizeof(index) ? it_node_at(ctx, index, true, true) : NULL;
    if (!node && it_last_dot(path)) {
        node = it_node_at(ctx, path, false, true);
    }
    if (node) {
        return node;
    }
    /* No module: a Folder, by either reading. A file beside the directory of
     * the same name (store.ts and store/) has the Folder's qualified name, and
     * the graph keeps only the Folder; its scope holds what the file
     * re-exports from the directory. */
    node = it_node_at(ctx, path, true, false);
    return node || !it_last_dot(path) ? node : it_node_at(ctx, path, false, false);
}

/* A relative specifier against the importing file, with its last segment
 * whole. ctx_pipeline_resolve_relative_import drops that segment's extension
 * and, with it, any dot in a file's own name: `./tag.mapper` comes back as
 * `tag`. For a JS-style specifier the dropped part is put back;
 * it_module_node then tries it both as a name and as an extension. */
static char *it_relative_path(const char *source_rel, const char *module_path) {
    char *relative = ctx_pipeline_resolve_relative_import(source_rel, module_path);
    bool js_style = module_path[0] == '.' &&
                    (module_path[1] == '/' || (module_path[1] == '.' && module_path[2] == '/'));
    const char *dot = js_style ? it_last_dot(module_path) : NULL;
    if (!relative || !dot) {
        return relative;
    }
    size_t rel_len = strlen(relative);
    size_t dot_len = strlen(dot);
    char *whole = malloc(rel_len + dot_len + SKIP_ONE);
    if (whole) {
        memcpy(whole, relative, rel_len);
        memcpy(whole + rel_len, dot, dot_len + SKIP_ONE);
    }
    free(relative);
    return whole;
}

const ctx_gbuf_node_t *ctx_pipeline_import_target(const ctx_pipeline_ctx_t *ctx,
                                                  const char *source_rel,
                                                  const char *module_path) {
    if (!ctx || !module_path || !module_path[0]) {
        return NULL;
    }
    bool ts_rules = ctx_path_aliases_is_script(source_rel);
    char *relative = NULL;
    if (source_rel) {
        relative = ts_rules ? it_relative_path(source_rel, module_path)
                            : ctx_pipeline_resolve_relative_import(source_rel, module_path);
    }
    if (relative) {
        const ctx_gbuf_node_t *node = it_module_node(ctx, relative, ts_rules);
        free(relative);
        return node;
    }
    char *candidates[CTX_PATH_ALIAS_MAX_CANDIDATES];
    int n = ctx_path_aliases_candidates(ctx->path_aliases, source_rel, module_path, candidates,
                                        CTX_PATH_ALIAS_MAX_CANDIDATES);
    const ctx_gbuf_node_t *node = NULL;
    for (int i = 0; i < n; i++) {
        if (!node) {
            node = it_module_node(ctx, candidates[i], ts_rules);
        }
        free(candidates[i]);
    }
    return node ? node : it_module_node(ctx, module_path, ts_rules);
}

/* Whether some import of local_name in the file is in-tree. */
static bool it_name_in_tree(const CtxFileResult *result, const bool *in_tree, const char *name) {
    for (int i = 0; i < result->imports.count; i++) {
        const char *other = result->imports.items[i].local_name;
        if (in_tree[i] && other && strcmp(other, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Whether names[0..count) already holds name. */
static bool it_listed(const char *const *names, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* The local names a script file imports only from specifiers nothing in the
 * tree claims, into a fresh array of names borrowed from result. */
static void it_collect_externals(const ctx_pipeline_ctx_t *ctx, const char *source_rel,
                                 const CtxFileResult *result, const bool *in_tree,
                                 const char ***out_names, int *out_count) {
    *out_names = NULL;
    *out_count = 0;
    if (!ctx_path_aliases_is_script(source_rel)) {
        return;
    }
    const char **names = calloc((size_t)result->imports.count, sizeof(const char *));
    if (!names) {
        return;
    }
    int count = 0;
    for (int i = 0; i < result->imports.count; i++) {
        const char *name = result->imports.items[i].local_name;
        if (in_tree[i] || !name || !name[0] || it_listed(names, count, name) ||
            it_name_in_tree(result, in_tree, name)) {
            continue;
        }
        names[count++] = name;
    }
    if (count == 0) {
        free((void *)names);
        return;
    }
    *out_names = names;
    *out_count = count;
}

void ctx_pipeline_import_map_with_externals(const ctx_pipeline_ctx_t *ctx, const char *source_rel,
                                            const CtxFileResult *result, const char ***out_keys,
                                            const char ***out_vals, int *out_count,
                                            const char ***out_externals,
                                            int *out_external_count) {
    *out_keys = NULL;
    *out_vals = NULL;
    *out_count = 0;
    if (out_externals) {
        *out_externals = NULL;
        *out_external_count = 0;
    }
    if (!result || result->imports.count <= 0) {
        return;
    }
    const char **keys = calloc((size_t)result->imports.count, sizeof(const char *));
    const char **vals = calloc((size_t)result->imports.count, sizeof(const char *));
    bool *in_tree = calloc((size_t)result->imports.count, sizeof(bool));
    if (!keys || !vals || !in_tree) {
        free((void *)keys);
        free((void *)vals);
        free(in_tree);
        return;
    }
    int count = 0;
    for (int i = 0; i < result->imports.count; i++) {
        const CtxImport *imp = &result->imports.items[i];
        if (!imp->local_name || !imp->local_name[0] || !imp->module_path) {
            in_tree[i] = true; /* nothing to say about it: never an external name */
            continue;
        }
        const ctx_gbuf_node_t *target = ctx_pipeline_import_target(ctx, source_rel,
                                                                   imp->module_path);
        if (!target) {
            in_tree[i] = ctx_path_aliases_claims(ctx->path_aliases, source_rel, imp->module_path);
            continue;
        }
        in_tree[i] = true;
        keys[count] = strdup(imp->local_name);
        vals[count] = target->qualified_name; /* borrowed from ctx->gbuf */
        count++;
    }
    if (out_externals) {
        it_collect_externals(ctx, source_rel, result, in_tree, out_externals, out_external_count);
    }
    free(in_tree);
    *out_keys = keys;
    *out_vals = vals;
    *out_count = count;
}

void ctx_pipeline_import_map_from_result(const ctx_pipeline_ctx_t *ctx, const char *source_rel,
                                         const CtxFileResult *result, const char ***out_keys,
                                         const char ***out_vals, int *out_count) {
    ctx_pipeline_import_map_with_externals(ctx, source_rel, result, out_keys, out_vals, out_count,
                                           NULL, NULL);
}

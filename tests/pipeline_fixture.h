/*
 * pipeline_fixture.h — run a temp-repo fixture through both call resolvers
 * and ask what the graph says about one call.
 *
 * Resolution has two implementations: pass_calls.c (sequential) and
 * pass_parallel.c (used above 50 files). They have drifted apart before, so a
 * resolution test should hold both to the same answer: build the fixture once
 * with pf_run_sequential and once with pf_run_parallel, then assert on each.
 */
#ifndef PIPELINE_FIXTURE_H
#define PIPELINE_FIXTURE_H

#include "../src/foundation/compat.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "extract.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* The File nodes pipeline.c's structure pass creates before extraction. That
 * pass is static there; the parallel resolver reads each file's imports from
 * IMPORTS edges, which hang off these nodes. */
static inline void pf_add_file_nodes(ctx_gbuf_t *gbuf, const char *project,
                                     const ctx_file_info_t *files, int file_count) {
    for (int i = 0; i < file_count; i++) {
        const char *rel = files[i].rel_path;
        const char *slash = strrchr(rel, '/');
        char *file_qn = ctx_pipeline_fqn_file(project, rel);
        ctx_gbuf_upsert_node(gbuf, "File", slash ? slash + 1 : rel, file_qn, rel, 0, 0, "{}");
        free(file_qn);
    }
}

/* The resolution passes in the order run_sequential_pipeline (pipeline.c)
 * runs them, sharing one extraction cache as it does. */
static inline ctx_gbuf_t *pf_run_sequential(const char *project, const char *repo,
                                            ctx_file_info_t *files, int file_count) {
    ctx_gbuf_t *gbuf = ctx_gbuf_new(project, repo);
    pf_add_file_nodes(gbuf, project, files, file_count);
    ctx_registry_t *reg = ctx_registry_new();
    atomic_int cancelled;
    atomic_init(&cancelled, 0);
    CtxFileResult **cache = calloc(file_count, sizeof(CtxFileResult *));
    ctx_pipeline_ctx_t ctx = {
        .project_name = project,
        .repo_path = repo,
        .gbuf = gbuf,
        .registry = reg,
        .cancelled = &cancelled,
        .result_cache = cache,
    };
    ctx_init();
    ctx_pipeline_pass_definitions(&ctx, files, file_count);
    ctx_pipeline_pass_calls(&ctx, files, file_count);
    ctx_pipeline_pass_usages(&ctx, files, file_count);
    ctx_pipeline_pass_semantic(&ctx, files, file_count);
    for (int i = 0; i < file_count; i++) {
        if (cache[i]) {
            ctx_free_result(cache[i]);
        }
    }
    free(cache);
    ctx_registry_free(reg);
    return gbuf;
}

/* Extract, build the registry, resolve: the parallel pipeline on 2 workers.
 * Called directly, so the 50-file threshold doesn't apply. */
static inline ctx_gbuf_t *pf_run_parallel(const char *project, const char *repo,
                                          ctx_file_info_t *files, int file_count) {
    ctx_gbuf_t *gbuf = ctx_gbuf_new(project, repo);
    pf_add_file_nodes(gbuf, project, files, file_count);
    ctx_registry_t *reg = ctx_registry_new();
    atomic_int cancelled;
    atomic_init(&cancelled, 0);
    ctx_pipeline_ctx_t ctx = {
        .project_name = project,
        .repo_path = repo,
        .gbuf = gbuf,
        .registry = reg,
        .cancelled = &cancelled,
    };
    _Atomic int64_t shared_ids;
    atomic_init(&shared_ids, ctx_gbuf_next_id(gbuf));
    CtxFileResult **cache = calloc(file_count, sizeof(CtxFileResult *));
    ctx_init();
    ctx_parallel_extract(&ctx, files, file_count, cache, &shared_ids, 2);
    ctx_gbuf_set_next_id(gbuf, atomic_load(&shared_ids));
    ctx_build_registry_from_cache(&ctx, files, file_count, cache);
    ctx_parallel_resolve(&ctx, files, file_count, cache, &shared_ids, 2);
    ctx_gbuf_set_next_id(gbuf, atomic_load(&shared_ids));
    for (int i = 0; i < file_count; i++) {
        if (cache[i]) {
            ctx_free_result(cache[i]);
        }
    }
    free(cache);
    ctx_registry_free(reg);
    return gbuf;
}

/* The `type` edge (CALLS, USAGE, INHERITS, ...) from the definition named
 * `from` to a target whose qualified name ends with `to_qn_suffix` (e.g.
 * "util.date.formatDate"), or NULL. */
static inline const ctx_gbuf_edge_t *pf_find_edge(const ctx_gbuf_t *gb, const char *from,
                                                  const char *type, const char *to_qn_suffix) {
    const ctx_gbuf_node_t **sources = NULL;
    int source_count = 0;
    if (ctx_gbuf_find_by_name(gb, from, &sources, &source_count) != 0) {
        return NULL;
    }
    size_t suffix_len = strlen(to_qn_suffix);
    for (int s = 0; s < source_count; s++) {
        const ctx_gbuf_edge_t **edges = NULL;
        int edge_count = 0;
        if (ctx_gbuf_find_edges_by_source_type(gb, sources[s]->id, type, &edges, &edge_count) !=
            0) {
            continue;
        }
        for (int e = 0; e < edge_count; e++) {
            const ctx_gbuf_node_t *target = ctx_gbuf_find_by_id(gb, edges[e]->target_id);
            if (!target || !target->qualified_name) {
                continue;
            }
            size_t qn_len = strlen(target->qualified_name);
            if (qn_len >= suffix_len &&
                strcmp(target->qualified_name + qn_len - suffix_len, to_qn_suffix) == 0) {
                return edges[e];
            }
        }
    }
    return NULL;
}

/* The CALLS edge from `from` to a target whose qualified name ends with
 * `to_qn_suffix`, or NULL. */
static inline const ctx_gbuf_edge_t *pf_find_call(const ctx_gbuf_t *gb, const char *from,
                                                  const char *to_qn_suffix) {
    return pf_find_edge(gb, from, "CALLS", to_qn_suffix);
}

/* True when a CALLS edge leads from `from` to a target named `to`. */
static inline bool pf_has_call(const ctx_gbuf_t *gb, const char *from, const char *to) {
    char suffix[256];
    snprintf(suffix, sizeof(suffix), ".%s", to);
    return pf_find_call(gb, from, suffix) != NULL;
}

/* True when the call from `from` to `to_qn_suffix` exists and was resolved by
 * `strategy` (the edge's "strategy" property). */
static inline bool pf_call_resolved_by(const ctx_gbuf_t *gb, const char *from,
                                       const char *to_qn_suffix, const char *strategy) {
    const ctx_gbuf_edge_t *edge = pf_find_call(gb, from, to_qn_suffix);
    if (!edge || !edge->properties_json) {
        return false;
    }
    char needle[128];
    snprintf(needle, sizeof(needle), "\"strategy\":\"%s\"", strategy);
    return strstr(edge->properties_json, needle) != NULL;
}

/* The source definition must exist, or a missing edge would prove nothing. */
static inline bool pf_has_def(const ctx_gbuf_t *gb, const char *name) {
    const ctx_gbuf_node_t **nodes = NULL;
    int count = 0;
    return ctx_gbuf_find_by_name(gb, name, &nodes, &count) == 0 && count > 0;
}

#endif /* PIPELINE_FIXTURE_H */

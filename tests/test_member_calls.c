/*
 * test_member_calls.c — a member call on a value never binds a free function
 * by name (cortex-indexer todo T-dt3b).
 *
 * `items.map(...)` used to resolve by its last segment: with one repo function
 * named `map` anywhere in the project, unique_name bound every array `.map` to
 * it. The same fixture runs through the sequential and the parallel resolver,
 * because each carries its own emit path.
 *
 * Suite: suite_member_calls
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "discover/discover.h"
#include "foundation/platform.h"
#include "extract.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>

static char g_mc_tmpdir[256];

static int write_file(const char *rel, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_mc_tmpdir, rel);
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fputs(content, f);
    fclose(f);
    return 0;
}

static int setup_member_call_repo(void) {
    snprintf(g_mc_tmpdir, sizeof(g_mc_tmpdir), "/tmp/ctx_mc_XXXXXX");
    if (!ctx_mkdtemp(g_mc_tmpdir)) {
        return -1;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/util", g_mc_tmpdir);
    ctx_mkdir(path);

    /* Repo functions that share their names with built-in methods. */
    if (write_file("util/map.ts", "export function map(xs: number[]): number[] {\n"
                                  "  return xs;\n"
                                  "}\n"
                                  "export function refresh(): void {}\n") != 0) {
        return -1;
    }
    /* Reached only through an import path that does not resolve (an alias). */
    if (write_file("util/format.ts", "export function format(s: string): string {\n"
                                     "  return s;\n"
                                     "}\n") != 0) {
        return -1;
    }
    if (write_file("service.ts", "export class Store {\n"
                                 "  save(): void {}\n"
                                 "}\n") != 0) {
        return -1;
    }
    if (write_file("app.ts", "import * as fmt from '@shared/format';\n"
                             "import { Store } from './service';\n"
                             "import { map } from './util/map';\n"
                             "\n"
                             "export function mapsAnArray(items: number[]): void {\n"
                             "  items.map((x) => x);\n"
                             "}\n"
                             "\n"
                             "export function callsBare(items: number[]): void {\n"
                             "  map(items);\n"
                             "}\n"
                             "\n"
                             "export function callsNamespaceImport(): void {\n"
                             "  fmt.format('a');\n"
                             "}\n"
                             "\n"
                             "export function callsMethod(store: Store): void {\n"
                             "  store.save();\n"
                             "}\n"
                             "\n"
                             "export class Grid {\n"
                             "  rows: number[] = [];\n"
                             "  renderGrid(): void {\n"
                             "    this.rows.map((r) => r);\n"
                             "  }\n"
                             "}\n"
                             "\n"
                             "export class Panel {\n"
                             "  showPanel(): void {\n"
                             "    this.refresh();\n"
                             "  }\n"
                             "}\n") != 0) {
        return -1;
    }
    /* Outside JS/TS the rule does not apply (yet): Python keeps today's binding. */
    if (write_file("helpers.py", "def helper():\n"
                                 "    pass\n") != 0) {
        return -1;
    }
    if (write_file("use.py", "def use_helper(obj):\n"
                             "    obj.helper()\n") != 0) {
        return -1;
    }
    return 0;
}

static ctx_gbuf_t *run_sequential(const char *project, ctx_file_info_t *files, int file_count) {
    ctx_gbuf_t *gbuf = ctx_gbuf_new(project, g_mc_tmpdir);
    ctx_registry_t *reg = ctx_registry_new();
    atomic_int cancelled;
    atomic_init(&cancelled, 0);
    ctx_pipeline_ctx_t ctx = {
        .project_name = project,
        .repo_path = g_mc_tmpdir,
        .gbuf = gbuf,
        .registry = reg,
        .cancelled = &cancelled,
    };
    ctx_init();
    ctx_pipeline_pass_definitions(&ctx, files, file_count);
    ctx_pipeline_pass_calls(&ctx, files, file_count);
    ctx_registry_free(reg);
    return gbuf;
}

static ctx_gbuf_t *run_parallel(const char *project, ctx_file_info_t *files, int file_count) {
    ctx_gbuf_t *gbuf = ctx_gbuf_new(project, g_mc_tmpdir);
    ctx_registry_t *reg = ctx_registry_new();
    atomic_int cancelled;
    atomic_init(&cancelled, 0);
    ctx_pipeline_ctx_t ctx = {
        .project_name = project,
        .repo_path = g_mc_tmpdir,
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

static ctx_gbuf_t *g_seq = NULL;
static ctx_gbuf_t *g_par = NULL;
static bool g_mc_ready = false;

static int ensure_member_call_graphs(void) {
    if (g_mc_ready) {
        return 0;
    }
    if (setup_member_call_repo() != 0) {
        return -1;
    }
    ctx_discover_opts_t opts = {.mode = CTX_MODE_FULL};
    ctx_file_info_t *files = NULL;
    int file_count = 0;
    if (ctx_discover(g_mc_tmpdir, &opts, &files, &file_count) != 0) {
        return -1;
    }
    g_seq = run_sequential("mc-test", files, file_count);
    g_par = run_parallel("mc-test", files, file_count);
    ctx_discover_free(files, file_count);
    g_mc_ready = true;
    return 0;
}

/* True when a CALLS edge leads from the definition named `from` to one named `to`. */
static bool has_call(const ctx_gbuf_t *gb, const char *from, const char *to) {
    const ctx_gbuf_node_t **sources = NULL;
    int source_count = 0;
    if (ctx_gbuf_find_by_name(gb, from, &sources, &source_count) != 0) {
        return false;
    }
    for (int s = 0; s < source_count; s++) {
        const ctx_gbuf_edge_t **edges = NULL;
        int edge_count = 0;
        if (ctx_gbuf_find_edges_by_source_type(gb, sources[s]->id, "CALLS", &edges, &edge_count) !=
            0) {
            continue;
        }
        for (int e = 0; e < edge_count; e++) {
            const ctx_gbuf_node_t *target = ctx_gbuf_find_by_id(gb, edges[e]->target_id);
            if (target && target->name && strcmp(target->name, to) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* The source definition must exist, or a missing edge would prove nothing. */
static bool has_def(const ctx_gbuf_t *gb, const char *name) {
    const ctx_gbuf_node_t **nodes = NULL;
    int count = 0;
    return ctx_gbuf_find_by_name(gb, name, &nodes, &count) == 0 && count > 0;
}

#define BOTH_MODES(expr_seq, expr_par)                                                             \
    do {                                                                                           \
        if (ensure_member_call_graphs() != 0) {                                                    \
            SKIP("setup failed");                                                                  \
        }                                                                                          \
        ASSERT_TRUE(expr_seq);                                                                     \
        ASSERT_TRUE(expr_par);                                                                     \
    } while (0)

TEST(member_call_on_value_skips_free_function) {
    BOTH_MODES(has_def(g_seq, "mapsAnArray"), has_def(g_par, "mapsAnArray"));
    BOTH_MODES(!has_call(g_seq, "mapsAnArray", "map"), !has_call(g_par, "mapsAnArray", "map"));
    PASS();
}

TEST(member_call_on_this_field_skips_free_function) {
    BOTH_MODES(has_def(g_seq, "renderGrid"), has_def(g_par, "renderGrid"));
    BOTH_MODES(!has_call(g_seq, "renderGrid", "map"), !has_call(g_par, "renderGrid", "map"));
    PASS();
}

TEST(bare_call_still_binds_free_function) {
    BOTH_MODES(has_call(g_seq, "callsBare", "map"), has_call(g_par, "callsBare", "map"));
    PASS();
}

TEST(member_call_on_imported_name_still_binds) {
    BOTH_MODES(has_call(g_seq, "callsNamespaceImport", "format"),
               has_call(g_par, "callsNamespaceImport", "format"));
    PASS();
}

TEST(member_call_still_binds_method) {
    BOTH_MODES(has_call(g_seq, "callsMethod", "save"), has_call(g_par, "callsMethod", "save"));
    PASS();
}

TEST(member_call_on_bare_this_keeps_binding) {
    BOTH_MODES(has_call(g_seq, "showPanel", "refresh"), has_call(g_par, "showPanel", "refresh"));
    PASS();
}

TEST(member_call_rule_is_js_ts_only) {
    BOTH_MODES(has_call(g_seq, "use_helper", "helper"), has_call(g_par, "use_helper", "helper"));
    PASS();
}

static void member_calls_teardown(void) {
    if (g_seq) {
        ctx_gbuf_free(g_seq);
        g_seq = NULL;
    }
    if (g_par) {
        ctx_gbuf_free(g_par);
        g_par = NULL;
    }
    if (g_mc_tmpdir[0]) {
        th_rmtree(g_mc_tmpdir);
        g_mc_tmpdir[0] = '\0';
    }
    g_mc_ready = false;
}

SUITE(member_calls) {
    RUN_TEST(member_call_on_value_skips_free_function);
    RUN_TEST(member_call_on_this_field_skips_free_function);
    RUN_TEST(bare_call_still_binds_free_function);
    RUN_TEST(member_call_on_imported_name_still_binds);
    RUN_TEST(member_call_still_binds_method);
    RUN_TEST(member_call_on_bare_this_keeps_binding);
    RUN_TEST(member_call_rule_is_js_ts_only);
    member_calls_teardown();
}

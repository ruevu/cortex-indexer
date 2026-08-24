/*
 * test_parallel.c — Tests for the three-phase parallel pipeline.
 *
 * Validates parity between sequential (4-pass) and parallel (3-phase)
 * pipeline modes on a small Go test fixture.
 *
 * Suite: suite_parallel
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "pipeline/worker_pool.h"
#include "graph_buffer/graph_buffer.h"
#include "discover/discover.h"
#include "foundation/platform.h"
#include "foundation/log.h"
#include "extract.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>

/* ── Helper: create temp test repo ───────────────────────────────── */

static char g_par_tmpdir[256];

static int setup_parallel_repo(void) {
    snprintf(g_par_tmpdir, sizeof(g_par_tmpdir), "/tmp/ctx_par_XXXXXX");
    if (!ctx_mkdtemp(g_par_tmpdir))
        return -1;

    char path[512];

    /* main.go */
    snprintf(path, sizeof(path), "%s/main.go", g_par_tmpdir);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "package main\n\nimport \"pkg\"\n\n"
               "func main() {\n\tpkg.Serve()\n}\n");
    fclose(f);

    /* pkg/ */
    snprintf(path, sizeof(path), "%s/pkg", g_par_tmpdir);
    ctx_mkdir(path);

    /* pkg/service.go */
    snprintf(path, sizeof(path), "%s/pkg/service.go", g_par_tmpdir);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "package pkg\n\nimport \"pkg/util\"\n\n"
               "func Serve() {\n\tutil.Help()\n}\n");
    fclose(f);

    /* pkg/util/ */
    snprintf(path, sizeof(path), "%s/pkg/util", g_par_tmpdir);
    ctx_mkdir(path);

    /* pkg/util/helper.go */
    snprintf(path, sizeof(path), "%s/pkg/util/helper.go", g_par_tmpdir);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "package util\n\nfunc Help() {}\n");
    fclose(f);

    return 0;
}

static void rm_rf(const char *path) {
    th_rmtree(path);
}

static void teardown_parallel_repo(void) {
    if (g_par_tmpdir[0])
        rm_rf(g_par_tmpdir);
    g_par_tmpdir[0] = '\0';
}

/* ── Run sequential pipeline on files, returning gbuf ─────────────── */

static ctx_gbuf_t *run_sequential(const char *project, const char *repo_path,
                                  ctx_file_info_t *files, int file_count) {
    ctx_gbuf_t *gbuf = ctx_gbuf_new(project, repo_path);
    ctx_registry_t *reg = ctx_registry_new();
    atomic_int cancelled;
    atomic_init(&cancelled, 0);

    ctx_pipeline_ctx_t ctx = {
        .project_name = project,
        .repo_path = repo_path,
        .gbuf = gbuf,
        .registry = reg,
        .cancelled = &cancelled,
    };

    ctx_init();
    ctx_pipeline_pass_definitions(&ctx, files, file_count);
    ctx_pipeline_pass_calls(&ctx, files, file_count);
    ctx_pipeline_pass_usages(&ctx, files, file_count);
    ctx_pipeline_pass_semantic(&ctx, files, file_count);

    ctx_registry_free(reg);
    return gbuf;
}

/* ── Run parallel pipeline on files, returning gbuf ───────────────── */

static ctx_gbuf_t *run_parallel(const char *project, const char *repo_path, ctx_file_info_t *files,
                                int file_count, int worker_count) {
    ctx_gbuf_t *gbuf = ctx_gbuf_new(project, repo_path);
    ctx_registry_t *reg = ctx_registry_new();
    atomic_int cancelled;
    atomic_init(&cancelled, 0);

    ctx_pipeline_ctx_t ctx = {
        .project_name = project,
        .repo_path = repo_path,
        .gbuf = gbuf,
        .registry = reg,
        .cancelled = &cancelled,
    };

    _Atomic int64_t shared_ids;
    int64_t gbuf_next = ctx_gbuf_next_id(gbuf);
    atomic_init(&shared_ids, gbuf_next);

    CtxFileResult **result_cache = calloc(file_count, sizeof(CtxFileResult *));

    ctx_init();
    ctx_parallel_extract(&ctx, files, file_count, result_cache, &shared_ids, worker_count);
    ctx_gbuf_set_next_id(gbuf, atomic_load(&shared_ids));

    ctx_build_registry_from_cache(&ctx, files, file_count, result_cache);

    ctx_parallel_resolve(&ctx, files, file_count, result_cache, &shared_ids, worker_count);
    ctx_gbuf_set_next_id(gbuf, atomic_load(&shared_ids));

    for (int i = 0; i < file_count; i++)
        if (result_cache[i])
            ctx_free_result(result_cache[i]);
    free(result_cache);

    ctx_registry_free(reg);
    return gbuf;
}

/* ── Parity Tests ─────────────────────────────────────────────────── */

static ctx_gbuf_t *g_seq_gbuf = NULL;
static ctx_gbuf_t *g_par_gbuf = NULL;
static int g_parity_setup_done = 0;

static int ensure_parity_setup(void) {
    if (g_parity_setup_done)
        return 0;

    if (setup_parallel_repo() != 0)
        return -1;

    /* Discover files */
    ctx_discover_opts_t opts = {.mode = CTX_MODE_FULL};
    ctx_file_info_t *files = NULL;
    int file_count = 0;
    if (ctx_discover(g_par_tmpdir, &opts, &files, &file_count) != 0)
        return -1;

    const char *project = "par-test";

    /* Build structure for both (need File/Folder nodes before definitions) */
    /* For parity, we need the structure pass too. Let's just compare
     * definition/call/usage/semantic edge counts. */

    /* Run both modes */
    g_seq_gbuf = run_sequential(project, g_par_tmpdir, files, file_count);
    g_par_gbuf = run_parallel(project, g_par_tmpdir, files, file_count, 2);

    ctx_discover_free(files, file_count);
    g_parity_setup_done = 1;
    return 0;
}

static void parity_teardown(void) {
    if (g_seq_gbuf) {
        ctx_gbuf_free(g_seq_gbuf);
        g_seq_gbuf = NULL;
    }
    if (g_par_gbuf) {
        ctx_gbuf_free(g_par_gbuf);
        g_par_gbuf = NULL;
    }
    teardown_parallel_repo();
    g_parity_setup_done = 0;
}

/* Node count parity */
TEST(parallel_node_count) {
    if (ensure_parity_setup() != 0)
        SKIP("setup failed");
    int seq = ctx_gbuf_node_count(g_seq_gbuf);
    int par = ctx_gbuf_node_count(g_par_gbuf);
    ASSERT_GT(seq, 0);
    ASSERT_EQ(seq, par);
    PASS();
}

/* Edge type parity tests */
static int assert_edge_type_parity(const char *type) {
    if (ensure_parity_setup() != 0)
        return -1;
    int seq = ctx_gbuf_edge_count_by_type(g_seq_gbuf, type);
    int par = ctx_gbuf_edge_count_by_type(g_par_gbuf, type);
    if (seq != par) {
        printf("  FAIL: %s edges: seq=%d par=%d\n", type, seq, par);
        return 1;
    }
    return 0;
}

TEST(parallel_calls_parity) {
    int rc = assert_edge_type_parity("CALLS");
    if (rc == -1)
        SKIP("setup failed");
    ASSERT_EQ(rc, 0);
    PASS();
}

TEST(parallel_defines_parity) {
    int rc = assert_edge_type_parity("DEFINES");
    if (rc == -1)
        SKIP("setup failed");
    ASSERT_EQ(rc, 0);
    PASS();
}

TEST(parallel_defines_method_parity) {
    int rc = assert_edge_type_parity("DEFINES_METHOD");
    if (rc == -1)
        SKIP("setup failed");
    ASSERT_EQ(rc, 0);
    PASS();
}

TEST(parallel_imports_parity) {
    int rc = assert_edge_type_parity("IMPORTS");
    if (rc == -1)
        SKIP("setup failed");
    ASSERT_EQ(rc, 0);
    PASS();
}

TEST(parallel_usage_parity) {
    int rc = assert_edge_type_parity("USAGE");
    if (rc == -1)
        SKIP("setup failed");
    ASSERT_EQ(rc, 0);
    PASS();
}

TEST(parallel_inherits_parity) {
    int rc = assert_edge_type_parity("INHERITS");
    if (rc == -1)
        SKIP("setup failed");
    ASSERT_EQ(rc, 0);
    PASS();
}

TEST(parallel_implements_parity) {
    int rc = assert_edge_type_parity("IMPLEMENTS");
    if (rc == -1)
        SKIP("setup failed");
    ASSERT_EQ(rc, 0);
    PASS();
}

TEST(parallel_total_edges) {
    if (ensure_parity_setup() != 0)
        SKIP("setup failed");
    int seq = ctx_gbuf_edge_count(g_seq_gbuf);
    int par = ctx_gbuf_edge_count(g_par_gbuf);
    ASSERT_GT(seq, 0);
    ASSERT_EQ(seq, par);
    PASS();
}

/* ── Empty file list ──────────────────────────────────────────────── */

TEST(parallel_empty_files) {
    ctx_gbuf_t *gbuf = ctx_gbuf_new("empty-proj", "/tmp");
    ctx_registry_t *reg = ctx_registry_new();
    atomic_int cancelled;
    atomic_init(&cancelled, 0);

    ctx_pipeline_ctx_t ctx = {
        .project_name = "empty-proj",
        .repo_path = "/tmp",
        .gbuf = gbuf,
        .registry = reg,
        .cancelled = &cancelled,
    };

    _Atomic int64_t shared_ids;
    atomic_init(&shared_ids, 1);

    CtxFileResult **cache = NULL;
    int rc = ctx_parallel_extract(&ctx, NULL, 0, cache, &shared_ids, 2);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx_gbuf_node_count(gbuf), 0);

    ctx_registry_free(reg);
    ctx_gbuf_free(gbuf);
    PASS();
}

/* ── Graph buffer merge tests ─────────────────────────────────────── */

TEST(gbuf_shared_ids_unique) {
    _Atomic int64_t shared = 1;
    ctx_gbuf_t *ga = ctx_gbuf_new_shared_ids("proj", "/", &shared);
    ctx_gbuf_t *gb = ctx_gbuf_new_shared_ids("proj", "/", &shared);

    int64_t id1 = ctx_gbuf_upsert_node(ga, "Function", "foo", "proj.foo", "a.go", 1, 5, "{}");
    int64_t id2 = ctx_gbuf_upsert_node(gb, "Function", "bar", "proj.bar", "b.go", 1, 3, "{}");
    ASSERT_GT(id1, 0);
    ASSERT_GT(id2, 0);
    ASSERT_NEQ(id1, id2);

    ctx_gbuf_free(ga);
    ctx_gbuf_free(gb);
    PASS();
}

TEST(gbuf_merge_nodes) {
    _Atomic int64_t shared = 1;
    ctx_gbuf_t *dst = ctx_gbuf_new_shared_ids("proj", "/", &shared);
    ctx_gbuf_t *src = ctx_gbuf_new_shared_ids("proj", "/", &shared);

    ctx_gbuf_upsert_node(dst, "Function", "a", "proj.a", "a.go", 1, 5, "{}");
    ctx_gbuf_upsert_node(dst, "Function", "b", "proj.b", "a.go", 6, 10, "{}");
    ctx_gbuf_upsert_node(src, "Function", "c", "proj.c", "b.go", 1, 5, "{}");
    ctx_gbuf_upsert_node(src, "Function", "d", "proj.d", "b.go", 6, 10, "{}");

    ASSERT_EQ(ctx_gbuf_node_count(dst), 2);
    ctx_gbuf_merge(dst, src);
    ASSERT_EQ(ctx_gbuf_node_count(dst), 4);

    ASSERT_NOT_NULL(ctx_gbuf_find_by_qn(dst, "proj.c"));
    ASSERT_NOT_NULL(ctx_gbuf_find_by_qn(dst, "proj.d"));
    /* dst originals still there */
    ASSERT_NOT_NULL(ctx_gbuf_find_by_qn(dst, "proj.a"));
    ASSERT_NOT_NULL(ctx_gbuf_find_by_qn(dst, "proj.b"));

    ctx_gbuf_free(src);
    ctx_gbuf_free(dst);
    PASS();
}

TEST(gbuf_merge_edges) {
    _Atomic int64_t shared = 1;
    ctx_gbuf_t *dst = ctx_gbuf_new_shared_ids("proj", "/", &shared);
    ctx_gbuf_t *src = ctx_gbuf_new_shared_ids("proj", "/", &shared);

    int64_t a = ctx_gbuf_upsert_node(dst, "Function", "a", "proj.a", "a.go", 1, 5, "{}");
    int64_t b = ctx_gbuf_upsert_node(dst, "Function", "b", "proj.b", "a.go", 6, 10, "{}");
    /* Put an edge in src that references dst nodes (by ID) */
    ctx_gbuf_insert_edge(src, a, b, "CALLS", "{}");

    ctx_gbuf_merge(dst, src);
    ASSERT_GT(ctx_gbuf_edge_count(dst), 0);

    const ctx_gbuf_edge_t **edges = NULL;
    int count = 0;
    ctx_gbuf_find_edges_by_source_type(dst, a, "CALLS", &edges, &count);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(edges[0]->target_id, b);

    ctx_gbuf_free(src);
    ctx_gbuf_free(dst);
    PASS();
}

TEST(gbuf_merge_empty_src) {
    _Atomic int64_t shared = 1;
    ctx_gbuf_t *dst = ctx_gbuf_new_shared_ids("proj", "/", &shared);
    ctx_gbuf_t *src = ctx_gbuf_new_shared_ids("proj", "/", &shared);

    ctx_gbuf_upsert_node(dst, "Function", "a", "proj.a", "a.go", 1, 5, "{}");
    int before = ctx_gbuf_node_count(dst);
    ctx_gbuf_merge(dst, src);
    ASSERT_EQ(ctx_gbuf_node_count(dst), before);

    ctx_gbuf_free(src);
    ctx_gbuf_free(dst);
    PASS();
}

TEST(gbuf_merge_src_free_safe) {
    _Atomic int64_t shared = 1;
    ctx_gbuf_t *dst = ctx_gbuf_new_shared_ids("proj", "/", &shared);
    ctx_gbuf_t *src = ctx_gbuf_new_shared_ids("proj", "/", &shared);

    ctx_gbuf_upsert_node(src, "Function", "x", "proj.x", "x.go", 1, 5, "{}");
    ctx_gbuf_merge(dst, src);
    ctx_gbuf_free(src); /* must not crash */

    /* dst node still accessible */
    ASSERT_NOT_NULL(ctx_gbuf_find_by_qn(dst, "proj.x"));
    ctx_gbuf_free(dst);
    PASS();
}

/* Cross-worker scope-local collision at the merge layer.
 *
 * upsert_def_node's scope-local rule only sees ONE worker's local gbuf, but two
 * DIFFERENT files can produce the same qualified name: a *named* def's FQN drops
 * a trailing `index` segment, so `lib.ts` and `lib/index.ts` both yield
 * `proj.lib.collideMe`. Land them in different workers and the collision is only
 * visible to ctx_gbuf_merge — which used to overwrite unconditionally, so a test
 * closure took over the exported function's file/line span whenever the closure's
 * worker merged last. Both merge orders must end with the real definition owning
 * the node. */
TEST(gbuf_merge_scope_local_never_clobbers) {
    _Atomic int64_t shared = 1;
    ctx_gbuf_t *w_real = ctx_gbuf_new_shared_ids("proj", "/", &shared);
    ctx_gbuf_t *w_local = ctx_gbuf_new_shared_ids("proj", "/", &shared);
    ctx_gbuf_t *dst_real_first = ctx_gbuf_new_shared_ids("proj", "/", &shared);
    ctx_gbuf_t *dst_local_first = ctx_gbuf_new_shared_ids("proj", "/", &shared);

    /* worker A: the real exported definition (lib.ts line 4). */
    ctx_gbuf_upsert_def_node(w_real, "Function", "collideMe", "proj.lib.collideMe", "lib.ts", 4, 6,
                             "{}", false);
    /* worker B: a describe-block closure of the same name (lib/index.ts line 2). */
    int64_t local_id = ctx_gbuf_upsert_def_node(w_local, "Function", "collideMe",
                                                "proj.lib.collideMe", "lib/index.ts", 2, 2, "{}",
                                                true);
    /* An edge sourced at the scope-local node must follow it onto the survivor. */
    ctx_gbuf_insert_edge(w_local, local_id, local_id, "CALLS", "{}");

    ctx_gbuf_merge(dst_real_first, w_real);
    ctx_gbuf_merge(dst_real_first, w_local);

    ctx_gbuf_merge(dst_local_first, w_local);
    ctx_gbuf_merge(dst_local_first, w_real);

    const ctx_gbuf_node_t *rf = ctx_gbuf_find_by_qn(dst_real_first, "proj.lib.collideMe");
    const ctx_gbuf_node_t *lf = ctx_gbuf_find_by_qn(dst_local_first, "proj.lib.collideMe");
    int rf_line = rf ? rf->start_line : -1;
    int lf_line = lf ? lf->start_line : -1;
    char rf_file[64] = {0};
    char lf_file[64] = {0};
    if (rf && rf->file_path)
        snprintf(rf_file, sizeof(rf_file), "%s", rf->file_path);
    if (lf && lf->file_path)
        snprintf(lf_file, sizeof(lf_file), "%s", lf->file_path);
    int rf_nodes = ctx_gbuf_node_count(dst_real_first);
    int lf_nodes = ctx_gbuf_node_count(dst_local_first);
    /* The scope-local node's edge was remapped onto the surviving node. */
    const ctx_gbuf_edge_t **edges = NULL;
    int edge_count = 0;
    if (rf)
        ctx_gbuf_find_edges_by_source_type(dst_real_first, rf->id, "CALLS", &edges, &edge_count);

    ctx_gbuf_free(w_real);
    ctx_gbuf_free(w_local);
    ctx_gbuf_free(dst_real_first);
    ctx_gbuf_free(dst_local_first);

    /* Real merged first — the scope-local merge must not touch it. */
    ASSERT_EQ(rf_line, 4);
    ASSERT_STR_EQ(rf_file, "lib.ts");
    /* Scope-local merged first — the real definition must take the node over. */
    ASSERT_EQ(lf_line, 4);
    ASSERT_STR_EQ(lf_file, "lib.ts");
    /* One QN, one node, either way. */
    ASSERT_EQ(rf_nodes, 1);
    ASSERT_EQ(lf_nodes, 1);
    ASSERT_EQ(edge_count, 1);
    PASS();
}

TEST(gbuf_next_id_accessors) {
    ctx_gbuf_t *gb = ctx_gbuf_new("proj", "/");
    ASSERT_EQ(ctx_gbuf_next_id(gb), 1);

    ctx_gbuf_upsert_node(gb, "Function", "foo", "proj.foo", "f.go", 1, 5, "{}");
    ASSERT_GT(ctx_gbuf_next_id(gb), 1);

    ctx_gbuf_set_next_id(gb, 100);
    int64_t id = ctx_gbuf_upsert_node(gb, "Function", "bar", "proj.bar", "f.go", 6, 10, "{}");
    ASSERT_GTE(id, 100);

    ctx_gbuf_free(gb);
    PASS();
}

/* ── Nuxt route extraction through the parallel path ──────────────── */

/* Exercises insert_def_into_gbuf (pass_parallel.c) — the parallel pipeline's
 * DIRECT Route + HANDLES creation. The sequential path's route emission is
 * covered by ts_nuxt_route_handler in test_extraction.c; this is the only
 * automated check of the parallel-path equivalent.
 *
 * Setup: a temp repo with one Nuxt server route (server/api/orgs/index.get.ts)
 * containing a defineEventHandler. We discover it and run ctx_parallel_extract
 * directly (the same Phase-3A entry the >50-file pipeline uses), which dispatches
 * to extract_worker → insert_def_into_gbuf.
 *
 * Asserts: (1) a route_path-tagged Function node exists, (2) a Route node with
 * qn "__route__GET__/api/orgs" exists, (3) a HANDLES edge runs from the function
 * to that Route node. If insert_def_into_gbuf's route block were removed/broken,
 * the Route node and HANDLES edge would be absent and assertions (2)/(3) fail. */
TEST(parallel_nuxt_route_handles) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/ctx_par_nuxt_XXXXXX");
    if (!ctx_mkdtemp(tmpdir))
        SKIP("mkdtemp failed");

    char path[512];
    snprintf(path, sizeof(path), "%s/server", tmpdir);
    ctx_mkdir(path);
    snprintf(path, sizeof(path), "%s/server/api", tmpdir);
    ctx_mkdir(path);
    snprintf(path, sizeof(path), "%s/server/api/orgs", tmpdir);
    ctx_mkdir(path);

    snprintf(path, sizeof(path), "%s/server/api/orgs/index.get.ts", tmpdir);
    FILE *f = fopen(path, "w");
    if (!f) {
        th_rmtree(tmpdir);
        SKIP("write fixture failed");
    }
    fprintf(f, "export default defineEventHandler(async (event) => {\n"
               "  return $fetch('/api/platform/orgs')\n"
               "})\n");
    fclose(f);

    ctx_discover_opts_t opts = {.mode = CTX_MODE_FULL};
    ctx_file_info_t *files = NULL;
    int file_count = 0;
    if (ctx_discover(tmpdir, &opts, &files, &file_count) != 0 || file_count == 0) {
        th_rmtree(tmpdir);
        SKIP("discover failed");
    }

    const char *project = "par-nuxt-test";
    ctx_gbuf_t *gbuf = ctx_gbuf_new(project, tmpdir);
    ctx_registry_t *reg = ctx_registry_new();
    atomic_int cancelled;
    atomic_init(&cancelled, 0);

    ctx_pipeline_ctx_t ctx = {
        .project_name = project,
        .repo_path = tmpdir,
        .gbuf = gbuf,
        .registry = reg,
        .cancelled = &cancelled,
    };

    _Atomic int64_t shared_ids;
    atomic_init(&shared_ids, ctx_gbuf_next_id(gbuf));

    CtxFileResult **result_cache = calloc((size_t)file_count, sizeof(CtxFileResult *));

    ctx_init();
    int rc = ctx_parallel_extract(&ctx, files, file_count, result_cache, &shared_ids, 2);
    ctx_gbuf_set_next_id(gbuf, atomic_load(&shared_ids));
    ASSERT_EQ(rc, 0);

    /* (1) The route_path-tagged Function node exists. */
    const ctx_gbuf_node_t **fns = NULL;
    int fn_count = 0;
    ctx_gbuf_find_by_label(gbuf, "Function", &fns, &fn_count);
    int64_t handler_id = 0;
    for (int i = 0; i < fn_count; i++) {
        const ctx_gbuf_edge_t **handles = NULL;
        int hcount = 0;
        ctx_gbuf_find_edges_by_source_type(gbuf, fns[i]->id, "HANDLES", &handles, &hcount);
        if (hcount > 0) {
            handler_id = fns[i]->id;
            break;
        }
    }

    /* (2) The Route node with the derived qn exists. */
    const ctx_gbuf_node_t *route = ctx_gbuf_find_by_qn(gbuf, "__route__GET__/api/orgs");

    /* (3) A HANDLES edge from the handler function to that Route node. */
    int handles_total = ctx_gbuf_edge_count_by_type(gbuf, "HANDLES");

    int ok = 1;
    if (route == NULL) {
        printf("  Route node __route__GET__/api/orgs missing\n");
        ok = 0;
    }
    if (handler_id == 0) {
        printf("  no Function node with an outgoing HANDLES edge\n");
        ok = 0;
    }
    if (handles_total < 1) {
        printf("  HANDLES edge count = %d\n", handles_total);
        ok = 0;
    }
    if (ok && route != NULL) {
        const ctx_gbuf_edge_t **handles = NULL;
        int hcount = 0;
        ctx_gbuf_find_edges_by_source_type(gbuf, handler_id, "HANDLES", &handles, &hcount);
        if (hcount < 1 || handles[0]->target_id != route->id) {
            printf("  HANDLES edge does not target the Route node\n");
            ok = 0;
        }
    }

    ctx_registry_free(reg);
    for (int i = 0; i < file_count; i++)
        if (result_cache[i])
            ctx_free_result(result_cache[i]);
    free(result_cache);
    ctx_gbuf_free(gbuf);
    ctx_discover_free(files, file_count);
    th_rmtree(tmpdir);

    ASSERT_TRUE(ok);
    PASS();
}

/* ── Nested definitions (spec 2026-08-10) ─────────────────────────── */

static char g_nest_tmpdir[256];

static int setup_nested_repo(void) {
    snprintf(g_nest_tmpdir, sizeof(g_nest_tmpdir), "/tmp/ctx_nest_XXXXXX");
    if (!ctx_mkdtemp(g_nest_tmpdir))
        return -1;
    char path[512];

    snprintf(path, sizeof(path), "%s/lib.ts", g_nest_tmpdir);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "export function helper() { return 0; }\n"
               "export function useHelper() { return helper(); }\n");
    fclose(f);

    snprintf(path, sizeof(path), "%s/a.ts", g_nest_tmpdir);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "export function alpha() {\n"
               "  const helper = () => 1;\n"
               "  return helper();\n"
               "}\n");
    fclose(f);
    return 0;
}

TEST(nested_defs_enclose_and_skip_registry) {
    if (setup_nested_repo() != 0)
        SKIP("setup failed");

    ctx_discover_opts_t opts = {.mode = CTX_MODE_FULL};
    ctx_file_info_t *files = NULL;
    int file_count = 0;
    if (ctx_discover(g_nest_tmpdir, &opts, &files, &file_count) != 0) {
        rm_rf(g_nest_tmpdir);
        SKIP("discover failed");
    }

    ctx_gbuf_t *gbuf = run_parallel("nest-test", g_nest_tmpdir, files, file_count, 2);
    ctx_discover_free(files, file_count);

    /* The closure is a node, scoped under its enclosing function... */
    const ctx_gbuf_node_t *closure = ctx_gbuf_find_by_qn(gbuf, "nest-test.a.alpha.helper");
    ASSERT_NOT_NULL(closure);
    /* ...and linked to it. */
    ASSERT_EQ(ctx_gbuf_edge_count_by_type(gbuf, "ENCLOSES"), 1);

    /* Nothing resolves TO the closure — it is not in the symbol registry. */
    const ctx_gbuf_edge_t **into_closure = NULL;
    int closure_callers = 0;
    ctx_gbuf_find_edges_by_target_type(gbuf, closure->id, "CALLS", &into_closure,
                                       &closure_callers);
    ASSERT_EQ(closure_callers, 0);

    /* The top-level helper still owns the name. */
    const ctx_gbuf_node_t *top = ctx_gbuf_find_by_qn(gbuf, "nest-test.lib.helper");
    ASSERT_NOT_NULL(top);
    const ctx_gbuf_edge_t **into_top = NULL;
    int top_callers = 0;
    ctx_gbuf_find_edges_by_target_type(gbuf, top->id, "CALLS", &into_top, &top_callers);
    ASSERT_GTE(top_callers, 1);

    ctx_gbuf_free(gbuf);
    rm_rf(g_nest_tmpdir);
    PASS();
}

/* ENCLOSES has no coverage in the shared parity-test group above: that group's
 * fixture (setup_parallel_repo, three Go files) has no nested definitions, so
 * an ENCLOSES parity assertion against it would be a vacuous 0 == 0. Run both
 * pipeline paths over the nested fixture instead, and require the shared
 * non-zero count the brief calls for — not just that they agree. */
TEST(nested_defs_encloses_parity) {
    if (setup_nested_repo() != 0)
        SKIP("setup failed");

    ctx_discover_opts_t opts = {.mode = CTX_MODE_FULL};
    ctx_file_info_t *files = NULL;
    int file_count = 0;
    if (ctx_discover(g_nest_tmpdir, &opts, &files, &file_count) != 0) {
        rm_rf(g_nest_tmpdir);
        SKIP("discover failed");
    }

    ctx_gbuf_t *seq_gbuf = run_sequential("nest-test", g_nest_tmpdir, files, file_count);
    ctx_gbuf_t *par_gbuf = run_parallel("nest-test", g_nest_tmpdir, files, file_count, 2);
    ctx_discover_free(files, file_count);

    int seq_encloses = ctx_gbuf_edge_count_by_type(seq_gbuf, "ENCLOSES");
    int par_encloses = ctx_gbuf_edge_count_by_type(par_gbuf, "ENCLOSES");

    ctx_gbuf_free(seq_gbuf);
    ctx_gbuf_free(par_gbuf);
    rm_rf(g_nest_tmpdir);

    ASSERT_EQ(seq_encloses, 1);
    ASSERT_EQ(par_encloses, 1);
    PASS();
}

/* ── Scope-local definitions: registry + node ownership ───────────
 *
 * These live at the pipeline layer, not in test_extraction.c, because both
 * things under test only exist once definitions have been through a pipeline
 * pass: the symbol registry (built by ctx_registry_add in pass_definitions.c /
 * register_and_link_def) is what turns a name into a cross-file resolution
 * target, and the graph-buffer node — whose start_line the clobber test reads —
 * is created by the same pass. Extraction alone has neither a registry nor a
 * cross-file view. Every case runs BOTH pipeline paths, so the two stay honest
 * about sharing this logic. */

static char g_scope_tmpdir[256];

static int write_fixture(const char *dir, const char *name, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

static int setup_scope_local_repo(void) {
    snprintf(g_scope_tmpdir, sizeof(g_scope_tmpdir), "/tmp/ctx_scope_XXXXXX");
    if (!ctx_mkdtemp(g_scope_tmpdir))
        return -1;

    /* prod.ts — a production call site for a name that exists ONLY as a closure
     * inside spec.ts's top-level (anonymous) describe callback. */
    if (write_fixture(g_scope_tmpdir, "prod.ts",
                      "export function prodFn(thing) { return parseThing(thing); }\n") != 0)
        return -1;

    /* spec.ts — line 1 declares the real exported dupName; line 4 re-declares
     * the same name as a closure inside the anonymous callback, so both land on
     * the QN <proj>.spec.dupName. Real definition first. */
    if (write_fixture(g_scope_tmpdir, "spec.ts",
                      "export function dupName() { return 0; }\n"       /* 1 */
                      "describe(\"suite\", () => {\n"                   /* 2 */
                      "  const parseThing = (x) => { return x + 1; };\n" /* 3 */
                      "  const dupName = () => { return 99; };\n"       /* 4 */
                      "  it(\"works\", () => { parseThing(1); });\n"    /* 5 */
                      "});\n") != 0)                                    /* 6 */
        return -1;

    /* late.ts — the same collision in the OPPOSITE insertion order: the
     * scope-local dupLate (line 2) is extracted before the real one (line 4). */
    if (write_fixture(g_scope_tmpdir, "late.ts",
                      "describe(\"suite\", () => {\n"                   /* 1 */
                      "  const dupLate = () => { return 99; };\n"       /* 2 */
                      "});\n"                                           /* 3 */
                      "export function dupLate() { return 0; }\n") != 0) /* 4 */
        return -1;

    /* lib.ts + lib/index.ts — the SAME collision across two files. A named def
     * drops a trailing `index` path segment from its FQN, so both files produce
     * `scope-test.lib.collideMe`: lib.ts on line 4 (the real export) and
     * lib/index.ts on line 2 (a describe-block closure). On the parallel path
     * the two files can land in different workers, so this collision is only
     * ever seen by ctx_gbuf_merge. */
    if (write_fixture(g_scope_tmpdir, "lib.ts",
                      "export function pad() { return 0; }\n"        /* 1 */
                      "\n"                                           /* 2 */
                      "\n"                                           /* 3 */
                      "export function collideMe() { return 1; }\n") /* 4 */
        != 0)
        return -1;

    char libdir[512];
    snprintf(libdir, sizeof(libdir), "%s/lib", g_scope_tmpdir);
    ctx_mkdir(libdir);
    if (write_fixture(libdir, "index.ts",
                      "describe(\"suite\", () => {\n"                 /* 1 */
                      "  const collideMe = () => { return 99; };\n"   /* 2 */
                      "});\n") != 0)                                  /* 3 */
        return -1;

    /* a.py — class Local declared inside a method body. */
    if (write_fixture(g_scope_tmpdir, "a.py",
                      "class Outer:\n"
                      "    def m(self):\n"
                      "        class Local:\n"
                      "            def p(self):\n"
                      "                return 1\n") != 0)
        return -1;

    /* b.py — an unrelated file naming that function-local class. */
    if (write_fixture(g_scope_tmpdir, "b.py", "def uses():\n"
                                              "    return Local()\n") != 0)
        return -1;

    return 0;
}

/* Edges of the given type running src_qn -> dst_qn. Returns -1 when either node
 * is missing, so an assertion of 0 can never pass vacuously on a typo'd QN. */
static int count_edges_between(ctx_gbuf_t *gbuf, const char *src_qn, const char *dst_qn,
                               const char *type) {
    const ctx_gbuf_node_t *src = ctx_gbuf_find_by_qn(gbuf, src_qn);
    const ctx_gbuf_node_t *dst = ctx_gbuf_find_by_qn(gbuf, dst_qn);
    if (!src || !dst)
        return -1;
    const ctx_gbuf_edge_t **edges = NULL;
    int edge_count = 0;
    ctx_gbuf_find_edges_by_source_type(gbuf, src->id, type, &edges, &edge_count);
    int hits = 0;
    for (int i = 0; i < edge_count; i++)
        if (edges[i]->target_id == dst->id)
            hits++;
    return hits;
}

/* start_line of a node, or -1 when absent. */
static int node_start_line(ctx_gbuf_t *gbuf, const char *qn) {
    const ctx_gbuf_node_t *node = ctx_gbuf_find_by_qn(gbuf, qn);
    return node ? node->start_line : -1;
}

/* 1 when the node exists and its file_path is exactly `want`. */
static int node_file_is(ctx_gbuf_t *gbuf, const char *qn, const char *want) {
    const ctx_gbuf_node_t *node = ctx_gbuf_find_by_qn(gbuf, qn);
    return node && node->file_path && strcmp(node->file_path, want) == 0;
}

/* How many nodes carry this short name. */
static int node_count_by_name(ctx_gbuf_t *gbuf, const char *name) {
    const ctx_gbuf_node_t **nodes = NULL;
    int count = 0;
    ctx_gbuf_find_by_name(gbuf, name, &nodes, &count);
    return count;
}

/* Run both pipeline paths over the scope-local fixture. Returns -1 on setup
 * failure (caller SKIPs); caller frees both gbufs and removes the tmpdir. */
static int run_scope_local_both(ctx_gbuf_t **out_seq, ctx_gbuf_t **out_par) {
    if (setup_scope_local_repo() != 0)
        return -1;
    ctx_discover_opts_t opts = {.mode = CTX_MODE_FULL};
    ctx_file_info_t *files = NULL;
    int file_count = 0;
    if (ctx_discover(g_scope_tmpdir, &opts, &files, &file_count) != 0) {
        rm_rf(g_scope_tmpdir);
        return -1;
    }
    *out_seq = run_sequential("scope-test", g_scope_tmpdir, files, file_count);
    *out_par = run_parallel("scope-test", g_scope_tmpdir, files, file_count, 2);
    ctx_discover_free(files, file_count);
    return 0;
}

static void free_scope_local(ctx_gbuf_t *seq, ctx_gbuf_t *par) {
    ctx_gbuf_free(seq);
    ctx_gbuf_free(par);
    rm_rf(g_scope_tmpdir);
}

/* C1: a helper declared inside a top-level ANONYMOUS callable has no
 * parent_function (it is module-QN'd by design), so the parent_function-only
 * registry guard let it into the project-wide symbol table — and a production
 * function in another file resolved its call straight into a spec file's
 * describe-block closure. */
TEST(scope_local_def_not_cross_file_resolvable) {
    ctx_gbuf_t *seq = NULL;
    ctx_gbuf_t *par = NULL;
    if (run_scope_local_both(&seq, &par) != 0)
        SKIP("setup failed");

    /* The closure is still a node — reachable by name and by source. */
    int seq_has_node = ctx_gbuf_find_by_qn(seq, "scope-test.spec.parseThing") != NULL;
    int par_has_node = ctx_gbuf_find_by_qn(par, "scope-test.spec.parseThing") != NULL;
    /* Its caller is still function-sourced, not file-sourced. */
    int seq_has_caller = ctx_gbuf_find_by_qn(seq, "scope-test.prod.prodFn") != NULL;
    /* ...but nothing outside spec.ts resolves TO it. */
    int seq_edges =
        count_edges_between(seq, "scope-test.prod.prodFn", "scope-test.spec.parseThing", "CALLS");
    int par_edges =
        count_edges_between(par, "scope-test.prod.prodFn", "scope-test.spec.parseThing", "CALLS");

    free_scope_local(seq, par);

    ASSERT_TRUE(seq_has_node);
    ASSERT_TRUE(par_has_node);
    ASSERT_TRUE(seq_has_caller);
    ASSERT_EQ(seq_edges, 0);
    ASSERT_EQ(par_edges, 0);
    PASS();
}

/* I1: ctx_gbuf_upsert_node updates in place on a QN collision, so a scope-local
 * def overwrote the label / file / line span of a real module-level def with the
 * same name — get_code_snippet on the exported API returned the closure body. */
TEST(scope_local_def_does_not_clobber_toplevel_node) {
    ctx_gbuf_t *seq = NULL;
    ctx_gbuf_t *par = NULL;
    if (run_scope_local_both(&seq, &par) != 0)
        SKIP("setup failed");

    /* Real definition inserted FIRST (spec.ts line 1, closure line 4). */
    int seq_dup = node_start_line(seq, "scope-test.spec.dupName");
    int par_dup = node_start_line(par, "scope-test.spec.dupName");
    /* Scope-local inserted FIRST (late.ts line 2, real definition line 4) — the
     * later real def must still be able to take the node over. */
    int seq_late = node_start_line(seq, "scope-test.late.dupLate");
    int par_late = node_start_line(par, "scope-test.late.dupLate");

    free_scope_local(seq, par);

    ASSERT_EQ(seq_dup, 1);
    ASSERT_EQ(par_dup, 1);
    ASSERT_EQ(seq_late, 4);
    ASSERT_EQ(par_late, 4);
    PASS();
}

enum { PAR_COLLIDE_RUNS = 8 };

/* Same clobber, but the two colliding definitions live in DIFFERENT files
 * (lib.ts line 4 vs lib/index.ts line 2, folded to one QN because a named def
 * drops a trailing `index` segment). scope_local_def_does_not_clobber_toplevel_node
 * cannot catch this: all of its collisions are intra-file, so they are always
 * resolved inside a single worker's local gbuf. Here the two files can land in
 * different workers, and the collision surfaces only in ctx_gbuf_merge — which
 * overwrote unconditionally, so the closure won whenever its worker merged last.
 *
 * Worker→file assignment comes from a shared atomic counter, so a single
 * parallel run may or may not split the pair; repeat the run so the split is
 * actually exercised. The sequential path is asserted on the same fixture, so
 * the two can never silently diverge again. */
TEST(scope_local_cross_file_collision_owned_by_real_def) {
    if (setup_scope_local_repo() != 0)
        SKIP("setup failed");
    ctx_discover_opts_t opts = {.mode = CTX_MODE_FULL};
    ctx_file_info_t *files = NULL;
    int file_count = 0;
    if (ctx_discover(g_scope_tmpdir, &opts, &files, &file_count) != 0) {
        rm_rf(g_scope_tmpdir);
        SKIP("discover failed");
    }

    ctx_gbuf_t *seq = run_sequential("scope-test", g_scope_tmpdir, files, file_count);
    int seq_line = node_start_line(seq, "scope-test.lib.collideMe");
    int seq_file_ok = node_file_is(seq, "scope-test.lib.collideMe", "lib.ts");
    /* Guards the fixture: if the two files ever stop folding to one QN there
     * would be two collideMe nodes and this test would prove nothing. */
    int seq_named = node_count_by_name(seq, "collideMe");
    ctx_gbuf_free(seq);

    /* Repeated because worker/file assignment is a shared atomic counter: a
     * single run may not split the colliding pair across the two workers, and
     * detecting the pre-fix regression needs both a split AND the scope-local
     * side merging last. So a green run here is not by itself evidence that
     * the interesting interleaving occurred — it can pass by luck. It can
     * never fail spuriously (every assertion is a == 0 counter). The real,
     * deterministic guard is gbuf_merge_scope_local_never_clobbers, which
     * drives both merge orders directly; this one is the end-to-end companion. */
    int par_bad_line = 0;
    int par_bad_file = 0;
    int par_bad_named = 0;
    for (int i = 0; i < PAR_COLLIDE_RUNS; i++) {
        ctx_gbuf_t *par = run_parallel("scope-test", g_scope_tmpdir, files, file_count, 2);
        if (node_start_line(par, "scope-test.lib.collideMe") != 4)
            par_bad_line++;
        if (!node_file_is(par, "scope-test.lib.collideMe", "lib.ts"))
            par_bad_file++;
        /* Accumulated, not assigned: assigning would only ever report the
         * final run and silently drop a mid-loop fixture breakage. */
        if (node_count_by_name(par, "collideMe") != 1)
            par_bad_named++;
        ctx_gbuf_free(par);
    }

    ctx_discover_free(files, file_count);
    rm_rf(g_scope_tmpdir);

    /* One QN for both files — otherwise there is no collision to test. */
    ASSERT_EQ(seq_named, 1);
    ASSERT_EQ(par_bad_named, 0);
    /* The real, module-level export owns the node on both paths. */
    ASSERT_EQ(seq_line, 4);
    ASSERT_TRUE(seq_file_ok);
    ASSERT_EQ(par_bad_line, 0);
    ASSERT_EQ(par_bad_file, 0);
    PASS();
}

/* I3: a class declared in a method body got a function-scoped QN, but
 * registry-exclusion and ENCLOSES both keyed on parent_function, which only
 * extract_func_def used to set — so the class was both project-wide resolvable
 * and structurally orphaned. */
TEST(function_local_class_not_cross_file_resolvable) {
    ctx_gbuf_t *seq = NULL;
    ctx_gbuf_t *par = NULL;
    if (run_scope_local_both(&seq, &par) != 0)
        SKIP("setup failed");

    int seq_has_node = ctx_gbuf_find_by_qn(seq, "scope-test.a.Outer.m.Local") != NULL;
    int par_has_node = ctx_gbuf_find_by_qn(par, "scope-test.a.Outer.m.Local") != NULL;
    /* Structurally attached to the method that declares it. */
    int seq_encloses = count_edges_between(seq, "scope-test.a.Outer.m",
                                           "scope-test.a.Outer.m.Local", "ENCLOSES");
    int par_encloses = count_edges_between(par, "scope-test.a.Outer.m",
                                           "scope-test.a.Outer.m.Local", "ENCLOSES");
    /* ...and invisible to another file. */
    int seq_leak =
        count_edges_between(seq, "scope-test.b.uses", "scope-test.a.Outer.m.Local", "CALLS");
    int par_leak =
        count_edges_between(par, "scope-test.b.uses", "scope-test.a.Outer.m.Local", "CALLS");

    free_scope_local(seq, par);

    ASSERT_TRUE(seq_has_node);
    ASSERT_TRUE(par_has_node);
    ASSERT_EQ(seq_encloses, 1);
    ASSERT_EQ(par_encloses, 1);
    ASSERT_EQ(seq_leak, 0);
    ASSERT_EQ(par_leak, 0);
    PASS();
}

/* ── Suite Registration ──────────────────────────────────────────── */

SUITE(parallel) {
    /* Graph buffer merge/shared-ID tests */
    RUN_TEST(gbuf_shared_ids_unique);
    RUN_TEST(gbuf_merge_nodes);
    RUN_TEST(gbuf_merge_edges);
    RUN_TEST(gbuf_merge_empty_src);
    RUN_TEST(gbuf_merge_src_free_safe);
    RUN_TEST(gbuf_merge_scope_local_never_clobbers);
    RUN_TEST(gbuf_next_id_accessors);

    /* Parallel pipeline parity tests */
    RUN_TEST(parallel_node_count);
    RUN_TEST(parallel_calls_parity);
    RUN_TEST(parallel_defines_parity);
    RUN_TEST(parallel_defines_method_parity);
    RUN_TEST(parallel_imports_parity);
    RUN_TEST(parallel_usage_parity);
    RUN_TEST(parallel_inherits_parity);
    RUN_TEST(parallel_implements_parity);
    RUN_TEST(parallel_total_edges);
    RUN_TEST(parallel_empty_files);

    /* Parallel-path Route + HANDLES creation (Nuxt routes) */
    RUN_TEST(parallel_nuxt_route_handles);
    RUN_TEST(nested_defs_enclose_and_skip_registry);
    RUN_TEST(nested_defs_encloses_parity);

    /* Scope-local definitions: registry exclusion + node ownership */
    RUN_TEST(scope_local_def_not_cross_file_resolvable);
    RUN_TEST(scope_local_def_does_not_clobber_toplevel_node);
    RUN_TEST(scope_local_cross_file_collision_owned_by_real_def);
    RUN_TEST(function_local_class_not_cross_file_resolvable);

    /* Cleanup shared state */
    parity_teardown();
}

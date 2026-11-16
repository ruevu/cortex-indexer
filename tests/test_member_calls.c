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
#include "pipeline_fixture.h"
#include "discover/discover.h"
#include "foundation/platform.h"

#include <stdlib.h>
#include <string.h>
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
                             /* Formatters break long chains before the dot; the
                              * callee text keeps that whitespace. */
                             "export function callsNamespaceAcrossLines(): void {\n"
                             "  fmt\n"
                             "    .format('a');\n"
                             "}\n"
                             "\n"
                             /* The receiver of `format` here is a call result or a
                              * property, not the import itself. */
                             "export function chainsPastImport(x: string): void {\n"
                             "  fmt.wrap(x).format();\n"
                             "}\n"
                             "\n"
                             "export function dotsPastImport(): void {\n"
                             "  fmt.inner.format();\n"
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
                             "  showPanelAcrossLines(): void {\n"
                             "    this\n"
                             "      .refresh();\n"
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
    g_seq = pf_run_sequential("mc-test", g_mc_tmpdir, files, file_count);
    g_par = pf_run_parallel("mc-test", g_mc_tmpdir, files, file_count);
    ctx_discover_free(files, file_count);
    g_mc_ready = true;
    return 0;
}

#define has_call pf_has_call
#define has_def pf_has_def

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

TEST(line_break_before_the_dot_keeps_the_receiver_root) {
    BOTH_MODES(has_call(g_seq, "callsNamespaceAcrossLines", "format"),
               has_call(g_par, "callsNamespaceAcrossLines", "format"));
    BOTH_MODES(has_call(g_seq, "showPanelAcrossLines", "refresh"),
               has_call(g_par, "showPanelAcrossLines", "refresh"));
    PASS();
}

TEST(member_call_past_imported_root_skips_free_function) {
    BOTH_MODES(has_def(g_seq, "chainsPastImport"), has_def(g_par, "chainsPastImport"));
    BOTH_MODES(!has_call(g_seq, "chainsPastImport", "format"),
               !has_call(g_par, "chainsPastImport", "format"));
    BOTH_MODES(has_def(g_seq, "dotsPastImport"), has_def(g_par, "dotsPastImport"));
    BOTH_MODES(!has_call(g_seq, "dotsPastImport", "format"),
               !has_call(g_par, "dotsPastImport", "format"));
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
    RUN_TEST(line_break_before_the_dot_keeps_the_receiver_root);
    RUN_TEST(member_call_past_imported_root_skips_free_function);
    RUN_TEST(member_call_rule_is_js_ts_only);
    member_calls_teardown();
}

/*
 * test_import_resolution.c — a call to an imported name resolves through the
 * import map, the same way in both resolvers (cortex-indexer todo T-fdya).
 *
 * `import { f } from './m'; f()` used to be decided by name alone. The import
 * map pointed `f` at module `m`, but for a bare callee the resolver looked up
 * the module itself, which is never registered, so the lookup always missed.
 * The sequential resolver also ignored relative specifiers when building its
 * import map. A same-named function nearer to the caller then won.
 *
 * Suite: suite_import_resolution
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include "pipeline_fixture.h"
#include "discover/discover.h"
#include "foundation/platform.h"

#include <stdbool.h>
#include <stdio.h>

static char g_ir_tmpdir[256];

static int ir_write(const char *rel, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_ir_tmpdir, rel);
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = '\0';
        th_mkdir_p(path);
        *slash = '/';
    }
    return th_write_file(path, content);
}

static int setup_import_repo(void) {
    snprintf(g_ir_tmpdir, sizeof(g_ir_tmpdir), "/tmp/ctx_ir_XXXXXX");
    if (!ctx_mkdtemp(g_ir_tmpdir)) {
        return -1;
    }
    const char *const files[][2] = {
        /* The function every caller means. */
        {"util/date.ts", "export function formatDate(d: string): string {\n"
                         "  return d;\n"
                         "}\n"},
        /* A same-named function beside the relative importer: nearest by path,
         * but not what it imports. */
        {"feature/date.ts", "export function formatDate(d: string): string {\n"
                            "  return d + '!';\n"
                            "}\n"},
        /* Python maps an imported name to the symbol itself (util.client.client),
         * so this is where a member call could collapse onto the name. */
        {"util/client.py", "def client():\n"
                           "    pass\n"},
        {"util/uses_client.py", "from .client import client\n"
                                "\n"
                                "\n"
                                "def uses_client():\n"
                                "    client.get()\n"},
        /* Two classes in one module, each with a `create`. */
        {"util/store.ts", "export class Store {\n"
                          "  static create(): Store {\n"
                          "    return new Store();\n"
                          "  }\n"
                          "}\n"
                          "export class Other {\n"
                          "  static create(): Other {\n"
                          "    return new Other();\n"
                          "  }\n"
                          "}\n"},
        /* A root-relative specifier: the module path is the repo path. */
        {"app.ts", "import { formatDate } from 'util/date';\n"
                   "import { Store } from 'util/store';\n"
                   "\n"
                   "export function useRootImport(): string {\n"
                   "  return formatDate('x');\n"
                   "}\n"
                   "\n"
                   "export function makesStore(): void {\n"
                   "  Store.create();\n"
                   "}\n"},
        {"feature/view.ts", "import { formatDate } from '../util/date';\n"
                            "\n"
                            "export function useRelativeImport(): string {\n"
                            "  return formatDate('y');\n"
                            "}\n"
                            "\n"
                            /* Not exported, no const: the JS/TS usage walker
                             * records nothing under either (cortex-indexer
                             * T-dm3s). */
                            "function referencesRelativeImport(): unknown {\n"
                            "  return [formatDate];\n"
                            "}\n"},
        /* Decorators: the imported one, and a same-named one beside the
         * importer. The semantic pass resolves decorators through the import
         * map. Base classes would be the natural case, but they don't resolve
         * at all yet (cortex-indexer T-s4d5). */
        {"util/deco.py", "def register(f):\n"
                         "    return f\n"},
        {"feature/deco.py", "def register(f):\n"
                            "    return f\n"},
        {"feature/handlers.py", "from ..util.deco import register\n"
                                "\n"
                                "\n"
                                "@register\n"
                                "def handle():\n"
                                "    pass\n"},
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (ir_write(files[i][0], files[i][1]) != 0) {
            return -1;
        }
    }
    return 0;
}

static ctx_gbuf_t *g_seq = NULL;
static ctx_gbuf_t *g_par = NULL;
static bool g_ir_ready = false;
static bool g_ir_failed = false;

static int ensure_import_graphs(void) {
    if (g_ir_ready) {
        return 0;
    }
    if (g_ir_failed || setup_import_repo() != 0) {
        g_ir_failed = true;
        return -1;
    }
    ctx_discover_opts_t opts = {.mode = CTX_MODE_FULL};
    ctx_file_info_t *files = NULL;
    int file_count = 0;
    if (ctx_discover(g_ir_tmpdir, &opts, &files, &file_count) != 0) {
        g_ir_failed = true;
        return -1;
    }
    g_seq = pf_run_sequential("ir-test", g_ir_tmpdir, files, file_count);
    g_par = pf_run_parallel("ir-test", g_ir_tmpdir, files, file_count);
    ctx_discover_free(files, file_count);
    g_ir_ready = true;
    return 0;
}

#define BOTH_MODES(expr_seq, expr_par)                                                             \
    do {                                                                                           \
        if (ensure_import_graphs() != 0) {                                                         \
            SKIP("setup failed");                                                                  \
        }                                                                                          \
        ASSERT_TRUE(expr_seq);                                                                     \
        ASSERT_TRUE(expr_par);                                                                     \
    } while (0)

TEST(bare_imported_call_resolves_through_import_map) {
    BOTH_MODES(pf_call_resolved_by(g_seq, "useRootImport", "util.date.formatDate", "import_map"),
               pf_call_resolved_by(g_par, "useRootImport", "util.date.formatDate", "import_map"));
    PASS();
}

/* `Store.create()` on an imported class is that class's member, found exactly,
 * not whichever `create` in the module a suffix scan meets first. */
TEST(member_of_imported_class_resolves_exactly) {
    BOTH_MODES(pf_call_resolved_by(g_seq, "makesStore", "util.store.Store.create", "import_map"),
               pf_call_resolved_by(g_par, "makesStore", "util.store.Store.create", "import_map"));
    BOTH_MODES(!pf_find_call(g_seq, "makesStore", "util.store.Other.create"),
               !pf_find_call(g_par, "makesStore", "util.store.Other.create"));
    PASS();
}

TEST(relative_import_resolves_in_both_resolvers) {
    BOTH_MODES(
        pf_call_resolved_by(g_seq, "useRelativeImport", "util.date.formatDate", "import_map"),
        pf_call_resolved_by(g_par, "useRelativeImport", "util.date.formatDate", "import_map"));
    BOTH_MODES(!pf_find_call(g_seq, "useRelativeImport", "feature.date.formatDate"),
               !pf_find_call(g_par, "useRelativeImport", "feature.date.formatDate"));
    PASS();
}

/* Every import consumer shares one specifier → module mapping, so a relative
 * import means the same module to the usages and semantic passes too. */
TEST(relative_import_reference_is_a_usage_of_the_imported_function) {
    BOTH_MODES(pf_find_edge(g_seq, "referencesRelativeImport", "USAGE", "util.date.formatDate"),
               pf_find_edge(g_par, "referencesRelativeImport", "USAGE", "util.date.formatDate"));
    BOTH_MODES(
        !pf_find_edge(g_seq, "referencesRelativeImport", "USAGE", "feature.date.formatDate"),
        !pf_find_edge(g_par, "referencesRelativeImport", "USAGE", "feature.date.formatDate"));
    PASS();
}

TEST(relative_import_decorator_is_the_imported_one) {
    BOTH_MODES(pf_find_edge(g_seq, "handle", "DECORATES", "util.deco.register"),
               pf_find_edge(g_par, "handle", "DECORATES", "util.deco.register"));
    BOTH_MODES(!pf_find_edge(g_seq, "handle", "DECORATES", "feature.deco.register"),
               !pf_find_edge(g_par, "handle", "DECORATES", "feature.deco.register"));
    PASS();
}

/* The direct hit is for bare callees only: `client.get()` must not collapse
 * onto the imported `client` itself (upstream afcce2d4). */
TEST(member_call_on_imported_name_is_not_the_name_itself) {
    BOTH_MODES(pf_has_def(g_seq, "uses_client"), pf_has_def(g_par, "uses_client"));
    BOTH_MODES(!pf_find_call(g_seq, "uses_client", "util.client.client"),
               !pf_find_call(g_par, "uses_client", "util.client.client"));
    PASS();
}

static void import_resolution_teardown(void) {
    if (g_seq) {
        ctx_gbuf_free(g_seq);
        g_seq = NULL;
    }
    if (g_par) {
        ctx_gbuf_free(g_par);
        g_par = NULL;
    }
    if (g_ir_tmpdir[0]) {
        th_rmtree(g_ir_tmpdir);
        g_ir_tmpdir[0] = '\0';
    }
    g_ir_ready = false;
    g_ir_failed = false;
}

SUITE(import_resolution) {
    RUN_TEST(bare_imported_call_resolves_through_import_map);
    RUN_TEST(member_of_imported_class_resolves_exactly);
    RUN_TEST(relative_import_resolves_in_both_resolvers);
    RUN_TEST(relative_import_reference_is_a_usage_of_the_imported_function);
    RUN_TEST(relative_import_decorator_is_the_imported_one);
    RUN_TEST(member_call_on_imported_name_is_not_the_name_itself);
    import_resolution_teardown();
}

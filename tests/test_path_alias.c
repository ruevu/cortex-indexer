/*
 * test_path_alias.c — which repo paths a tsconfig / jsconfig alias names
 * (src/pipeline/path_alias.c; cortex-indexer todo T-fdya). The end-to-end
 * cases, an aliased import resolving a call, are in test_import_resolution.c;
 * these pin TypeScript's own rules for picking among patterns and configs.
 *
 * Suite: suite_path_alias
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include "foundation/platform.h"
#include "pipeline/path_alias.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_pa_tmpdir[256];
static ctx_path_aliases_t *g_pa;
static bool g_pa_tried;

static int pa_write(const char *rel, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_pa_tmpdir, rel);
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = '\0';
        th_mkdir_p(path);
        *slash = '/';
    }
    return th_write_file(path, content);
}

static int setup_pa_repo(void) {
    snprintf(g_pa_tmpdir, sizeof(g_pa_tmpdir), "/tmp/ctx_pa_XXXXXX");
    if (!ctx_mkdtemp(g_pa_tmpdir)) {
        return -1;
    }
    const char *const files[][2] = {
        /* The shorter and the wildcard patterns come first, so a lookup that
         * took the first match would pick them. */
        {"web/tsconfig.json", "{ \"compilerOptions\": {\n"
                              "  \"baseUrl\": \".\",\n"
                              "  \"paths\": {\n"
                              "    \"@x/*\": [\"x/*\"],\n"
                              "    \"@x/ui\": [\"ui/index.ts\"],\n"
                              "    \"@x/ui/*\": [\"ui/lib/*\"]\n"
                              "  }\n"
                              "} }\n"},
        /* A project of its own that declares no alias: the one above does
         * not reach into it. */
        {"web/plain/tsconfig.json", "{ \"compilerOptions\": {} }\n"},
        /* A base taken from a package is not followed; the file's own paths
         * still apply, anchored at its directory since there is no baseUrl. */
        {"pkg/tsconfig.json", "{ \"extends\": \"@tsconfig/node20/tsconfig.json\",\n"
                              "  \"compilerOptions\": { \"paths\": { \"~/*\": [\"src/*\"] } } }\n"},
        /* A config that extends itself, three times over: TypeScript rejects
         * the cycle; the loader must at least come back. */
        {"loop/tsconfig.json", "{ \"extends\": [\"./tsconfig.json\", \"./tsconfig.json\",\n"
                               "               \"./tsconfig.json\"],\n"
                               "  \"compilerOptions\": { \"paths\": { \"#l/*\": [\"l/*\"] } } }\n"},
        /* Unreadable as JSON: skipped, never fatal. */
        {"bad/tsconfig.json", "{ \"compilerOptions\": { \"paths\": \n"},
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (pa_write(files[i][0], files[i][1]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int ensure_pa(void) {
    if (!g_pa_tried) {
        g_pa_tried = true;
        if (setup_pa_repo() == 0) {
            g_pa = ctx_path_aliases_load(g_pa_tmpdir);
        }
    }
    return g_pa ? 0 : -1;
}

enum { PA_TEST_MAX = CTX_PATH_ALIAS_MAX_CANDIDATES };

/* The first candidate for specifier, seen from source_rel, copied into out
 * ("" when there is none); returns how many candidates there were. */
static int first_candidate(const char *source_rel, const char *specifier, char *out,
                           size_t out_size) {
    char *cands[PA_TEST_MAX];
    int n = ctx_path_aliases_candidates(g_pa, source_rel, specifier, cands, PA_TEST_MAX);
    snprintf(out, out_size, "%s", n > 0 ? cands[0] : "");
    for (int i = 0; i < n; i++) {
        free(cands[i]);
    }
    return n;
}

TEST(exact_pattern_beats_a_wildcard_that_also_matches) {
    ASSERT_EQ(ensure_pa(), 0);
    char first[256];
    ASSERT_GT(first_candidate("web/app.ts", "@x/ui", first, sizeof(first)), 0);
    ASSERT_STR_EQ(first, "web/ui/index.ts");
    PASS();
}

TEST(longest_wildcard_prefix_wins) {
    ASSERT_EQ(ensure_pa(), 0);
    char first[256];
    ASSERT_GT(first_candidate("web/app.ts", "@x/ui/button", first, sizeof(first)), 0);
    ASSERT_STR_EQ(first, "web/ui/lib/button");
    ASSERT_GT(first_candidate("web/app.ts", "@x/other", first, sizeof(first)), 0);
    ASSERT_STR_EQ(first, "web/x/other");
    PASS();
}

TEST(base_url_follows_the_pattern_targets) {
    ASSERT_EQ(ensure_pa(), 0);
    char *cands[PA_TEST_MAX];
    int n = ctx_path_aliases_candidates(g_pa, "web/app.ts", "@x/ui", cands, PA_TEST_MAX);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(cands[0], "web/ui/index.ts");
    ASSERT_STR_EQ(cands[1], "web/@x/ui");
    for (int i = 0; i < n; i++) {
        free(cands[i]);
    }
    PASS();
}

TEST(nearest_config_without_aliases_shadows_its_ancestor) {
    ASSERT_EQ(ensure_pa(), 0);
    char first[256];
    ASSERT_EQ(first_candidate("web/plain/app.ts", "@x/ui", first, sizeof(first)), 0);
    PASS();
}

TEST(only_script_files_are_aliased) {
    ASSERT_EQ(ensure_pa(), 0);
    char first[256];
    ASSERT_EQ(first_candidate("web/tool.py", "@x/ui", first, sizeof(first)), 0);
    ASSERT_EQ(first_candidate("web/app.ts", "./local", first, sizeof(first)), 0);
    PASS();
}

TEST(package_base_is_skipped_and_own_paths_anchor_at_the_config) {
    ASSERT_EQ(ensure_pa(), 0);
    char first[256];
    ASSERT_GT(first_candidate("pkg/main.ts", "~/thing", first, sizeof(first)), 0);
    ASSERT_STR_EQ(first, "pkg/src/thing");
    PASS();
}

/* Reaching this test at all is most of it: the repo above was loaded with a
 * self-extending config in it. */
TEST(self_extending_config_loads) {
    ASSERT_EQ(ensure_pa(), 0);
    char first[256];
    ASSERT_GT(first_candidate("loop/main.ts", "#l/a", first, sizeof(first)), 0);
    ASSERT_STR_EQ(first, "loop/l/a");
    PASS();
}

TEST(malformed_config_gives_no_aliases) {
    ASSERT_EQ(ensure_pa(), 0);
    char first[256];
    ASSERT_EQ(first_candidate("bad/main.ts", "@x/ui", first, sizeof(first)), 0);
    PASS();
}

SUITE(path_alias) {
    RUN_TEST(exact_pattern_beats_a_wildcard_that_also_matches);
    RUN_TEST(longest_wildcard_prefix_wins);
    RUN_TEST(base_url_follows_the_pattern_targets);
    RUN_TEST(nearest_config_without_aliases_shadows_its_ancestor);
    RUN_TEST(only_script_files_are_aliased);
    RUN_TEST(package_base_is_skipped_and_own_paths_anchor_at_the_config);
    RUN_TEST(self_extending_config_loads);
    RUN_TEST(malformed_config_gives_no_aliases);
    ctx_path_aliases_free(g_pa);
    g_pa = NULL;
    g_pa_tried = false;
    if (g_pa_tmpdir[0]) {
        th_rmtree(g_pa_tmpdir);
        g_pa_tmpdir[0] = '\0';
    }
}

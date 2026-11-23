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
        /* Saved with a UTF-8 byte order mark, as some Windows editors do;
         * TypeScript reads it. */
        {"bom/tsconfig.json", "\xEF\xBB\xBF"
                              "{ \"compilerOptions\": { \"paths\": { \"#b/*\": [\"b/*\"] } } }\n"},
        /* Unreadable as JSON: skipped, never fatal. */
        {"bad/tsconfig.json", "{ \"compilerOptions\": { \"paths\": \n"},
        /* What web's baseUrl reaches: a directory and a file. */
        {"web/shared/format.ts", "export const f = 1;\n"},
        {"web/legacy.ts", "export const l = 1;\n"},
        /* Workspace packages the tree declares, and one it only depends on. */
        {"libs/kit/package.json", "{ \"name\": \"@acme/kit\", \"version\": \"1.0.0\" }\n"},
        {"tools/cli/package.json", "{ \"name\": \"acme-cli\" }\n"},
        /* What the tree takes from the registry: the packages the tests below
         * expect to stay external, near-misses of its own names among them. */
        {"package.json", "{ \"private\": true,\n"
                         "  \"dependencies\": { \"react\": \"^18.0.0\", \"antd\": \"^5.0.0\",\n"
                         "    \"@scope/pkg\": \"^1.0.0\", \"JSONStream\": \"^1.3.5\",\n"
                         "    \"lodash.debounce\": \"^4.0.8\" },\n"
                         "  \"devDependencies\": { \"@acme/other\": \"^1.0.0\",\n"
                         "    \"@acme/ki\": \"^1.0.0\", \"acme-cli-extra\": \"^1.0.0\",\n"
                         "    \"@x/ui\": \"^1.0.0\", \"@sharedx/strings\": \"^1.0.0\" } }\n"},
        {"node_modules/antd/package.json", "{ \"name\": \"antd\" }\n"},
        /* Vite's template: the project's config only references the app's,
         * which holds the aliases; the loader scopes a config by directory,
         * not by references. */
        {"spa/tsconfig.json", "{ \"files\": [],\n"
                              "  \"references\": [{ \"path\": \"./tsconfig.app.json\" }] }\n"},
        {"spa/tsconfig.app.json", "{ \"compilerOptions\": { \"paths\": {\n"
                                  "  \"@shared/*\": [\"src/common/*\"],\n"
                                  "  \"*\": [\"src/*\"]\n"
                                  "} } }\n"},
        /* Dependencies the manifest points into the tree, by keys that are
         * not the target's name; and two that are not in the tree. */
        {"app/package.json", "{ \"name\": \"acme-app\",\n"
                             "  \"dependencies\": { \"shared\": \"file:../shared-lib\",\n"
                             "    \"kit2\": \"link:../kit\", \"left-pad\": \"^1.3.0\" },\n"
                             "  \"devDependencies\": { \"plug\": \"portal:../plug\",\n"
                             "    \"wk\": \"workspace:*\", \"lodash4\": \"npm:lodash@4\" } }\n"},
        /* Ignored by the root .gitignore; this tree has no .git directory,
         * so discovery indexes it all the same. */
        {".gitignore", "generated/\n"},
        {"generated/client/package.json", "{ \"name\": \"@acme/client\" }\n"},
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

TEST(config_with_byte_order_mark_loads) {
    ASSERT_EQ(ensure_pa(), 0);
    char first[256];
    ASSERT_GT(first_candidate("bom/main.ts", "#b/a", first, sizeof(first)), 0);
    ASSERT_STR_EQ(first, "bom/b/a");
    PASS();
}

TEST(malformed_config_gives_no_aliases) {
    ASSERT_EQ(ensure_pa(), 0);
    char first[256];
    ASSERT_EQ(first_candidate("bad/main.ts", "@x/ui", first, sizeof(first)), 0);
    PASS();
}

/* ── What the tree claims (cortex-indexer T-jnpg) ──────────────── */

/* A relative specifier is always the tree's own. */
TEST(relative_specifier_is_claimed) {
    ASSERT_EQ(ensure_pa(), 0);
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "web/app.ts", "./local"));
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "web/app.ts", "../up/thing"));
    ASSERT_TRUE(ctx_path_aliases_claims(NULL, "web/app.ts", "./local"));
    PASS();
}

/* A `paths` pattern of the file's own config claims what it matches, even
 * where its target does not exist. */
TEST(paths_pattern_claims_its_specifiers) {
    ASSERT_EQ(ensure_pa(), 0);
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "web/app.ts", "@x/ui"));
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "web/app.ts", "@x/not-there"));
    ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "web/plain/app.ts", "@x/ui"));
    PASS();
}

/* baseUrl claims a specifier whose first segment names an entry under it,
 * a directory or a file; not one that names nothing there. */
TEST(base_url_claims_what_exists_under_it) {
    ASSERT_EQ(ensure_pa(), 0);
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "web/app.ts", "shared/format"));
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "web/app.ts", "legacy"));
    ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "web/app.ts", "antd"));
    ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "web/app.ts", "node:path"));
    PASS();
}

/* A package.json in the tree claims its package name and every subpath of
 * it, from any script file; a dependency under node_modules claims nothing. */
TEST(workspace_package_claims_its_name) {
    ASSERT_EQ(ensure_pa(), 0);
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "apps/main.ts", "@acme/kit"));
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "apps/main.ts", "@acme/kit/src/use"));
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "web/app.ts", "acme-cli"));
    ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "apps/main.ts", "@acme/other"));
    ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "apps/main.ts", "@acme/ki"));
    ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "apps/main.ts", "antd"));
    ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "apps/main.ts", "acme-cli-extra"));
    PASS();
}

/* A specifier no npm package can have is an alias some tool resolves (Nuxt's
 * `~/`, Vite's `@/`, SvelteKit's `$lib`, Node's `#` subpath imports) or a
 * virtual module: never known to be external, with or without a tsconfig. */
TEST(specifier_no_package_can_have_is_claimed) {
    ASSERT_EQ(ensure_pa(), 0);
    const char *aliases_only[] = {"~/composables/counter", "~~/server/db", "@/components/Card",
                                  "@@/layers/base",       "#imports",      "#internal/util",
                                  "$lib/format",           "virtual:pwa-register"};
    for (size_t i = 0; i < sizeof(aliases_only) / sizeof(aliases_only[0]); i++) {
        ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "web/app.ts", aliases_only[i]));
        ASSERT_TRUE(ctx_path_aliases_claims(NULL, "web/app.ts", aliases_only[i]));
    }
    /* Package names, scoped or legacy-capitalised, that the tree depends on;
     * and Node's builtins. */
    const char *packages[] = {"react", "@scope/pkg/sub", "JSONStream", "lodash.debounce",
                              "node:path", "fs"};
    for (size_t i = 0; i < sizeof(packages) / sizeof(packages[0]); i++) {
        ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "apps/main.ts", packages[i]));
    }
    PASS();
}

/* External must be proven: a package-shaped specifier is a package only when a
 * manifest in the tree depends on that package from outside the tree, or it
 * names the runtime. Anything else may be an alias a bundler resolves into
 * the tree, which no config here declares: a webpack key cased unlike its
 * directory (`Utilities/`), a key that spells no path (`@app/` for src/), one
 * segment (`test-utils`, Jest's moduleDirectories), a Deno import map, which
 * keeps the extension. */
TEST(undeclared_package_is_claimed) {
    ASSERT_EQ(ensure_pa(), 0);
    const char *undeclared[] = {"Utilities/utility", "@app/utils/foo", "test-utils",
                                "utils/greet.ts",    "@core/x",        "react-dom"};
    for (size_t i = 0; i < sizeof(undeclared) / sizeof(undeclared[0]); i++) {
        ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "apps/main.ts", undeclared[i]));
        ASSERT_TRUE(ctx_path_aliases_claims(NULL, "apps/main.ts", undeclared[i]));
    }
    /* Declared by the root manifest or by app/'s, whole or as a subpath. */
    const char *declared[] = {"react", "react/jsx-runtime", "@scope/pkg/sub", "left-pad"};
    for (size_t i = 0; i < sizeof(declared) / sizeof(declared[0]); i++) {
        ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "apps/main.ts", declared[i]));
    }
    /* The runtime's own modules need no manifest. */
    const char *runtime[] = {"node:path", "fs",  "fs/promises",  "child_process",
                             "bun:test",  "bun", "npm:lodash@4", "jsr:@std/path"};
    for (size_t i = 0; i < sizeof(runtime) / sizeof(runtime[0]); i++) {
        ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "apps/main.ts", runtime[i]));
        ASSERT_FALSE(ctx_path_aliases_claims(NULL, "apps/main.ts", runtime[i]));
    }
    PASS();
}

/* Package specifiers are a JS-family notion: a Python module path is never
 * judged by them. */
TEST(non_script_file_claims_only_relative_specifiers) {
    ASSERT_EQ(ensure_pa(), 0);
    ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "web/tool.py", "@acme/kit"));
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "web/tool.py", "./sibling"));
    PASS();
}

/* A config the loader does not scope, such as the tsconfig.app.json that
 * Vite's template keeps its aliases in and the project's tsconfig.json only
 * references: its alias keys claim their specifiers from any file. A bare
 * "*" key is not one: it would claim every package. */
TEST(alias_key_of_an_unscoped_config_claims_its_specifiers) {
    ASSERT_EQ(ensure_pa(), 0);
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "spa/src/main.ts", "@shared/strings"));
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "apps/main.ts", "@shared/strings"));
    ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "spa/src/main.ts", "@sharedx/strings"));
    ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "spa/src/main.ts", "react"));
    PASS();
}

/* A dependency the manifest points into the tree (file:, link:, portal:,
 * workspace:) claims the key the code imports it by, whatever the package
 * there calls itself. A registry range or an npm: alias claims nothing. */
TEST(local_dependency_claims_its_key) {
    ASSERT_EQ(ensure_pa(), 0);
    const char *local[] = {"shared", "shared/sub", "kit2", "plug", "wk"};
    for (size_t i = 0; i < sizeof(local) / sizeof(local[0]); i++) {
        ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "apps/main.ts", local[i]));
    }
    ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "apps/main.ts", "left-pad"));
    ASSERT_FALSE(ctx_path_aliases_claims(g_pa, "apps/main.ts", "lodash4"));
    PASS();
}

/* A small repo of its own, loaded. git: 0 for no .git, 1 for a .git
 * directory, 2 for a .git file, as in a linked worktree. */
static ctx_path_aliases_t *pa_load_tree(char *dir, size_t dir_size, const char *const files[][2],
                                        size_t n, int git) {
    snprintf(dir, dir_size, "/tmp/ctx_pa_tree_XXXXXX");
    if (!ctx_mkdtemp(dir)) {
        dir[0] = '\0';
        return NULL;
    }
    if (git == 1) {
        th_mkdir_p(TH_PATH(dir, ".git"));
    } else if (git == 2) {
        th_write_file(TH_PATH(dir, ".git"), "gitdir: /elsewhere/.git/worktrees/x\n");
    }
    for (size_t i = 0; i < n; i++) {
        th_write_file(TH_PATH(dir, files[i][0]), files[i][1]);
    }
    return ctx_path_aliases_load(dir);
}

/* Package names follow what discovery indexes. Discovery applies the root
 * .gitignore only when .git is a directory: not in a linked worktree, whose
 * .git is a file, nor in a tree without one. There a gitignored package is
 * indexed, and its name is the tree's. (The trees below have no tsconfig at
 * all: package names load without one.) */
TEST(gitignored_package_counts_where_discovery_indexes_it) {
    ASSERT_EQ(ensure_pa(), 0);
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "apps/main.ts", "@acme/client"));
    /* The root depends on both by a registry range, as npm workspaces do: a
     * package the walk does not see would be judged external. */
    static const char *const files[][2] = {
        {".gitignore", "generated/\n"},
        {"package.json", "{ \"dependencies\": { \"@acme/client\": \"^1.0.0\",\n"
                         "  \"@acme/tools\": \"^1.0.0\" } }\n"},
        {"generated/client/package.json", "{ \"name\": \"@acme/client\" }\n"},
        {"tools/package.json", "{ \"name\": \"@acme/tools\" }\n"},
    };
    for (int git = 1; git <= 2; git++) {
        char dir[256];
        ctx_path_aliases_t *a = pa_load_tree(dir, sizeof(dir), files, 4, git);
        bool loaded = a != NULL;
        bool tools = ctx_path_aliases_claims(a, "apps/main.ts", "@acme/tools");
        bool client = ctx_path_aliases_claims(a, "apps/main.ts", "@acme/client");
        ctx_path_aliases_free(a);
        if (dir[0]) {
            th_rmtree(dir);
        }
        ASSERT_TRUE(loaded);
        ASSERT_TRUE(tools);
        ASSERT_EQ(client, git == 2);
    }
    PASS();
}

/* A walk that stops at a cap has missed configs and packages it would have
 * claimed, so it claims every package-shaped specifier: the rule never cuts
 * what it could not see. 1100 projects is past the 1024-config cap, and the
 * root depends on each of them, and on react, by a registry range. */
TEST(walk_stopped_at_a_cap_claims_every_package) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/ctx_pa_cap_XXXXXX");
    ASSERT_NOT_NULL(ctx_mkdtemp(dir));
    enum { ROOT_BYTES = 1100 * 32 + 64 };
    char *root = malloc(ROOT_BYTES);
    ASSERT_NOT_NULL(root);
    size_t len = (size_t)snprintf(root, ROOT_BYTES, "{ \"dependencies\": { \"react\": \"^18\"");
    for (int i = 0; i < 1100; i++) {
        len += (size_t)snprintf(root + len, ROOT_BYTES - len, ", \"@acme/d%04d\": \"*\"", i);
    }
    snprintf(root + len, ROOT_BYTES - len, " } }\n");
    th_write_file(TH_PATH(dir, "package.json"), root);
    free(root);
    for (int i = 0; i < 1100; i++) {
        char rel[64];
        char body[96];
        snprintf(rel, sizeof(rel), "d%04d/tsconfig.json", i);
        th_write_file(TH_PATH(dir, rel), "{}\n");
        snprintf(rel, sizeof(rel), "d%04d/package.json", i);
        snprintf(body, sizeof(body), "{ \"name\": \"@acme/d%04d\" }\n", i);
        th_write_file(TH_PATH(dir, rel), body);
    }
    ctx_path_aliases_t *a = ctx_path_aliases_load(dir);
    int unclaimed = 0;
    for (int i = 0; i < 1100; i++) {
        char spec[32];
        snprintf(spec, sizeof(spec), "@acme/d%04d", i);
        unclaimed += !ctx_path_aliases_claims(a, "d0000/x.ts", spec);
    }
    bool react = ctx_path_aliases_claims(a, "d0000/x.ts", "react");
    ctx_path_aliases_free(a);
    th_rmtree(dir);
    ASSERT_EQ(unclaimed, 0);
    ASSERT_TRUE(react);
    PASS();
}

/* Every manifest repeats the names it depends on: 600 packages that each
 * depend on 15 siblings write 9,600 names, past what the walk keeps, of 600
 * packages. The walk keeps names, not mentions, so it is complete, and what
 * the root takes from the registry stays external. */
TEST(repeated_dependencies_do_not_stop_the_walk) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/ctx_pa_dup_XXXXXX");
    ASSERT_NOT_NULL(ctx_mkdtemp(dir));
    th_write_file(TH_PATH(dir, "package.json"),
                  "{ \"devDependencies\": { \"react\": \"^18.0.0\" } }\n");
    enum { PACKAGES = 600, SIBLINGS = 15 };
    for (int i = 0; i < PACKAGES; i++) {
        char body[1024];
        size_t len = (size_t)snprintf(body, sizeof(body),
                                      "{ \"name\": \"@acme/p%03d\", \"dependencies\": { ", i);
        for (int s = 1; s <= SIBLINGS; s++) {
            len += (size_t)snprintf(body + len, sizeof(body) - len,
                                    "%s\"@acme/p%03d\": \"workspace:*\"", s > 1 ? ", " : "",
                                    (i + s) % PACKAGES);
        }
        snprintf(body + len, sizeof(body) - len, " } }\n");
        char rel[64];
        snprintf(rel, sizeof(rel), "p%03d/package.json", i);
        th_write_file(TH_PATH(dir, rel), body);
    }
    ctx_path_aliases_t *a = ctx_path_aliases_load(dir);
    int claimed = 0;
    for (int i = 0; i < PACKAGES; i++) {
        char spec[32];
        snprintf(spec, sizeof(spec), "@acme/p%03d", i);
        claimed += ctx_path_aliases_claims(a, "p000/x.ts", spec);
    }
    bool react = ctx_path_aliases_claims(a, "p000/x.ts", "react");
    ctx_path_aliases_free(a);
    th_rmtree(dir);
    ASSERT_EQ(claimed, PACKAGES);
    ASSERT_FALSE(react);
    PASS();
}

/* Discovery indexes a directory however deep it sits, and so does the walk:
 * one deep directory, 40 levels down, leaves the rest of the tree's verdicts
 * alone. */
TEST(deep_directory_does_not_stop_the_walk) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/ctx_pa_deep_XXXXXX");
    ASSERT_NOT_NULL(ctx_mkdtemp(dir));
    th_write_file(TH_PATH(dir, "package.json"),
                  "{ \"dependencies\": { \"@deep/pkg\": \"^1.0.0\", \"react\": \"^18.0.0\" } }\n");
    char rel[256];
    size_t len = 0;
    for (int level = 0; level < 40; level++) {
        len += (size_t)snprintf(rel + len, sizeof(rel) - len, "l%02d/", level);
    }
    snprintf(rel + len, sizeof(rel) - len, "package.json");
    th_write_file(TH_PATH(dir, rel), "{ \"name\": \"@deep/pkg\" }\n");
    ctx_path_aliases_t *a = ctx_path_aliases_load(dir);
    bool deep = ctx_path_aliases_claims(a, "app/x.ts", "@deep/pkg");
    bool react = ctx_path_aliases_claims(a, "app/x.ts", "react");
    ctx_path_aliases_free(a);
    th_rmtree(dir);
    ASSERT_TRUE(deep);
    ASSERT_FALSE(react);
    PASS();
}

/* A specifier too long to look up is claimed, not judged external. */
TEST(overlong_specifier_is_claimed) {
    ASSERT_EQ(ensure_pa(), 0);
    char spec[1400];
    int n = snprintf(spec, sizeof(spec), "@acme/kit/");
    memset(spec + n, 'a', 1300);
    spec[n + 1300] = '\0';
    ASSERT_TRUE(ctx_path_aliases_claims(g_pa, "apps/main.ts", spec));
    PASS();
}

/* An alias a bundler resolves usually spells the path it points at:
 * `@utils/money` for src/utils/money.js. Two segments at least, matched
 * whole, against the file (extension dropped) or a directory holding it. */
TEST(specifier_names_the_file_it_spells) {
    ASSERT_TRUE(ctx_path_aliases_specifier_names_file("@utils/money", "rn/src/utils/money.js"));
    ASSERT_TRUE(ctx_path_aliases_specifier_names_file("utils/tax", "rn/src/utils/tax.js"));
    ASSERT_TRUE(
        ctx_path_aliases_specifier_names_file("@utils/date", "rn/src/utils/date/format.js"));
    ASSERT_TRUE(ctx_path_aliases_specifier_names_file("@components/Button",
                                                      "src/components/Button/index.tsx"));
    ASSERT_TRUE(
        ctx_path_aliases_specifier_names_file("@acme/kit", "packages/@acme/kit/src/use.ts"));
    ASSERT_TRUE(ctx_path_aliases_specifier_names_file("utils/money.helpers",
                                                      "src/utils/money.helpers.ts"));
    /* One segment: a wrapper named after its package is not the package. */
    ASSERT_FALSE(ctx_path_aliases_specifier_names_file("dayjs", "lib/dayjs.ts"));
    ASSERT_FALSE(ctx_path_aliases_specifier_names_file("lodash/fp", "lib/clone.ts"));
    ASSERT_FALSE(ctx_path_aliases_specifier_names_file("utils/mon", "rn/src/utils/money.js"));
    ASSERT_FALSE(ctx_path_aliases_specifier_names_file("ils/money", "rn/src/utils/money.js"));
    ASSERT_FALSE(ctx_path_aliases_specifier_names_file("utils/money/x", "rn/src/utils/money.js"));
    ASSERT_FALSE(ctx_path_aliases_specifier_names_file("utils//money", "src/utils/money.ts"));
    ASSERT_FALSE(ctx_path_aliases_specifier_names_file("utils/", "src/utils/x.ts"));
    ASSERT_FALSE(ctx_path_aliases_specifier_names_file(NULL, "a/b.ts"));
    ASSERT_FALSE(ctx_path_aliases_specifier_names_file("a/b", NULL));
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
    RUN_TEST(config_with_byte_order_mark_loads);
    RUN_TEST(malformed_config_gives_no_aliases);
    RUN_TEST(relative_specifier_is_claimed);
    RUN_TEST(paths_pattern_claims_its_specifiers);
    RUN_TEST(base_url_claims_what_exists_under_it);
    RUN_TEST(workspace_package_claims_its_name);
    RUN_TEST(specifier_no_package_can_have_is_claimed);
    RUN_TEST(undeclared_package_is_claimed);
    RUN_TEST(non_script_file_claims_only_relative_specifiers);
    RUN_TEST(alias_key_of_an_unscoped_config_claims_its_specifiers);
    RUN_TEST(local_dependency_claims_its_key);
    RUN_TEST(gitignored_package_counts_where_discovery_indexes_it);
    RUN_TEST(walk_stopped_at_a_cap_claims_every_package);
    RUN_TEST(repeated_dependencies_do_not_stop_the_walk);
    RUN_TEST(deep_directory_does_not_stop_the_walk);
    RUN_TEST(overlong_specifier_is_claimed);
    RUN_TEST(specifier_names_the_file_it_spells);
    ctx_path_aliases_free(g_pa);
    g_pa = NULL;
    g_pa_tried = false;
    if (g_pa_tmpdir[0]) {
        th_rmtree(g_pa_tmpdir);
        g_pa_tmpdir[0] = '\0';
    }
}

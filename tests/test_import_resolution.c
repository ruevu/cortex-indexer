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
static char g_alias_tmpdir[256];

static int ir_write_in(const char *root, const char *rel, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = '\0';
        th_mkdir_p(path);
        *slash = '/';
    }
    return th_write_file(path, content);
}

static int ir_write(const char *rel, const char *content) {
    return ir_write_in(g_ir_tmpdir, rel, content);
}

static int setup_import_repo(void) {
    snprintf(g_ir_tmpdir, sizeof(g_ir_tmpdir), "/tmp/ctx_ir_XXXXXX");
    if (!ctx_mkdtemp(g_ir_tmpdir)) {
        return -1;
    }
    const char *const files[][2] = {
        /* Project functions named like what packages export (T-jnpg). */
        {"lib/render.ts", "export function render(): void {}\n"},
        {"lib/resolve.ts", "export function resolve(): string {\n"
                           "  return '';\n"
                           "}\n"},
        {"lib/dayjs.ts", "export function dayjs(): number {\n"
                         "  return 0;\n"
                         "}\n"},
        {"lib/Spin.tsx", "export function Spin() {\n"
                         "  return null;\n"
                         "}\n"},
        /* Callers that import those names from packages: npm, a Node builtin,
         * a default import, and a JSX component. */
        {"ui_tests/card.test.ts", "import { render } from '@testing-library/react';\n"
                                  "import { resolve } from 'node:path';\n"
                                  "import dayjs from 'dayjs';\n"
                                  "\n"
                                  "export function rendersCard(): void {\n"
                                  "  render();\n"
                                  "  resolve();\n"
                                  "  dayjs();\n"
                                  "}\n"},
        {"ui_tests/loading.tsx", "import { Spin } from 'antd';\n"
                                 "\n"
                                 "export function Loading() {\n"
                                 "  return <Spin />;\n"
                                 "}\n"},
        /* A workspace package the tree declares, imported by its name with no
         * alias for it: its code, so the guess stays. */
        {"packages/kit/package.json", "{ \"name\": \"@acme/kit\" }\n"},
        {"packages/kit/src/use_kit.ts", "export function useKit(): void {}\n"},
        {"apps/start.ts", "import { useKit } from '@acme/kit';\n"
                          "\n"
                          "export function startsApp(): void {\n"
                          "  useKit();\n"
                          "}\n"},
        /* Aliases only a framework resolves (Nuxt's `~/`, its `#imports`),
         * with no tsconfig to say so: no package has such a name, so the
         * guess stays. */
        {"composables/counter.ts", "export function useCounter(): void {}\n"},
        {"composables/theme.ts", "export function useTheme(): void {}\n"},
        {"pages/count.ts", "import { useCounter } from '~/composables/counter';\n"
                           "import { useTheme } from '#imports';\n"
                           "\n"
                           "export function countsUp(): void {\n"
                           "  useCounter();\n"
                           "  useTheme();\n"
                           "}\n"},
        /* Aliases the loader cannot see. Vite's template keeps them in a
         * tsconfig.app.json the project's tsconfig.json only references, and
         * a key need not spell the path it maps to. */
        {"web/tsconfig.json", "{ \"files\": [],\n"
                              "  \"references\": [{ \"path\": \"./tsconfig.app.json\" }] }\n"},
        {"web/tsconfig.app.json", "{ \"compilerOptions\": { \"paths\": {\n"
                                  "  \"@lib/*\": [\"src/lib/*\"],\n"
                                  "  \"@shared/*\": [\"src/common/*\"]\n"
                                  "} } }\n"},
        {"web/src/lib/format.ts", "export function formatDateA(): string {\n"
                                  "  return '';\n"
                                  "}\n"},
        {"web/src/common/strings.ts", "export function padA(): string {\n"
                                      "  return '';\n"
                                      "}\n"},
        {"web/src/app.ts", "import { formatDateA } from '@lib/format';\n"
                           "import { padA } from '@shared/strings';\n"
                           "\n"
                           "export function mainA(): void {\n"
                           "  formatDateA();\n"
                           "  padA();\n"
                           "}\n"},
        /* A JS app whose aliases live in a babel or webpack config, with no
         * tsconfig at all; each alias spells the path it points at, a file
         * or a directory that re-exports it. */
        {"rn/src/utils/money.js", "export function formatMoneyB() {\n"
                                  "  return 0;\n"
                                  "}\n"},
        {"rn/src/utils/tax.js", "export function taxB() {\n"
                                "  return 0;\n"
                                "}\n"},
        {"rn/src/utils/date/index.js", "export { formatDayB } from './format';\n"},
        {"rn/src/utils/date/format.js", "export function formatDayB() {\n"
                                        "  return '';\n"
                                        "}\n"},
        {"rn/src/screens/Home.js", "import { formatMoneyB } from '@utils/money';\n"
                                   "import { taxB } from 'utils/tax';\n"
                                   "import { formatDayB } from '@utils/date';\n"
                                   "\n"
                                   "export function homeB() {\n"
                                   "  formatMoneyB();\n"
                                   "  taxB();\n"
                                   "  formatDayB();\n"
                                   "}\n"},
        /* One name imported from a package and from a path the tree claims
         * but the resolver cannot follow, as a Vue file's two script blocks
         * can. */
        {"colors/tint.ts", "export function tintC(): string {\n"
                           "  return '';\n"
                           "}\n"},
        {"pages/swatch.ts", "import { tintC } from 'chroma-js';\n"
                            "import { tintC } from '~/colors/tint';\n"
                            "\n"
                            "export function swatchC(): string {\n"
                            "  return tintC();\n"
                            "}\n"},
        /* What the tree takes from the registry. A package-shaped specifier
         * is external only when a manifest depends on its package, or it
         * names the runtime (`node:path`). */
        {"package.json", "{ \"private\": true,\n"
                         "  \"devDependencies\": { \"@testing-library/react\": \"^14.0.0\",\n"
                         "    \"dayjs\": \"^1.11.0\", \"antd\": \"^5.0.0\",\n"
                         "    \"chroma-js\": \"^2.4.2\", \"lodash\": \"^4.17.21\" } }\n"},
        /* Bundler aliases no config declares, which spell no path the guess
         * lands on: a webpack key cased unlike its directory, a key naming
         * src/ itself, one segment (Jest's moduleDirectories), and a Deno
         * import map, which keeps the extension. No manifest depends on such
         * a package, so the guess stays. */
        {"wp/src/utilities/utility.js", "export function utilityE() {\n"
                                        "  return 0;\n"
                                        "}\n"},
        {"wp/src/utils/foo.js", "export function fooE() {\n"
                                "  return 0;\n"
                                "}\n"},
        {"wp/utils/test-utils.js", "export function renderWithE() {\n"
                                   "  return 0;\n"
                                   "}\n"},
        {"wp/src/main.js", "import { utilityE } from 'Utilities/utility';\n"
                           "import { fooE } from '@app/utils/foo';\n"
                           "import { renderWithE } from 'test-utils';\n"
                           "\n"
                           "export function mainE() {\n"
                           "  utilityE();\n"
                           "  fooE();\n"
                           "  renderWithE();\n"
                           "}\n"},
        {"deno/src/utils/greet.ts", "export function greetE(): string {\n"
                                    "  return '';\n"
                                    "}\n"},
        {"deno/main.ts", "import { greetE } from \"utils/greet.ts\";\n"
                         "\n"
                         "export function mainDenoE(): void {\n"
                         "  greetE();\n"
                         "}\n"},
        /* A package subpath of two segments that spells no project path. */
        {"lib/clone.ts", "export function cloneDeepD(): void {}\n"},
        {"ui_tests/copy.test.ts", "import { cloneDeepD } from 'lodash/fp';\n"
                                  "\n"
                                  "export function copiesD(): void {\n"
                                  "  cloneDeepD();\n"
                                  "}\n"},
        /* The function every caller means. */
        {"util/date.ts", "export function formatDate(d: string): string {\n"
                         "  return d;\n"
                         "}\n"
                         "export function parseDate(d: string): string {\n"
                         "  return d;\n"
                         "}\n"},
        /* A same-named function beside the relative importer: nearest by path,
         * but not what it imports. */
        {"feature/date.ts", "export function formatDate(d: string): string {\n"
                            "  return d + '!';\n"
                            "}\n"
                            "export function parseDate(d: string): string {\n"
                            "  return d + '!';\n"
                            "}\n"},
        /* A dotted file name, the Nx / Angular habit: `.mapper` is part of the
         * name, not an extension, and the sibling tag.ts is not what the
         * import names. */
        {"util/tag.mapper.ts", "export function mapTag(): void {}\n"},
        {"util/tag.ts", "export function mapTag(): void {}\n"},
        {"feature/tags.ts", "import { mapTag } from '../util/tag.mapper';\n"
                            "\n"
                            "export function usesMapper(): void {\n"
                            "  mapTag();\n"
                            "}\n"},
        /* Two names from one module: one import statement, one module, so one
         * IMPORTS edge in the graph, but two names in the import map. */
        {"feature/both.ts", "import { formatDate, parseDate } from '../util/date';\n"
                            "\n"
                            "export function usesBoth(): string {\n"
                            "  return formatDate(parseDate('z'));\n"
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
        /* An absolute Python import: the module path names the package
         * path and then the symbol (`util.deco.register`). */
        {"feature/abs_handlers.py", "from util.deco import register\n"
                                    "\n"
                                    "\n"
                                    "@register\n"
                                    "def handle_abs():\n"
                                    "    pass\n"},
        /* A barrel: ui/index.ts re-exports what the package's files define.
         * An import of the barrel names module `ui`, where none of these
         * symbols is defined. */
        {"ui/index.ts", "export * from './button';\n"
                        "export * from './panel';\n"
                        "export * from './format';\n"},
        {"ui/button.ts", "export function Button(): void {}\n"},
        {"ui/panel.ts", "export class Panel {\n"
                        "  static open(): void {}\n"
                        "}\n"
                        "export class Drawer {\n"
                        "  static open(): void {}\n"
                        "}\n"},
        /* Two `format`s inside the package: the shallower one is the
         * package's own, the deeper one an internal helper. */
        {"ui/format.ts", "export function format(): void {}\n"},
        {"ui/internal/deep/format.ts", "export function format(): void {}\n"},
        /* A barrel file beside the directory it re-exports: shop.ts and
         * shop/ share a qualified name, and the graph keeps the Folder. */
        {"shop.ts", "export { addItem } from './shop/cart';\n"},
        {"shop/cart.ts", "export function addItem(): void {}\n"},
        {"screens/shop_user.ts", "import { addItem } from '../shop.js';\n"
                                 "\n"
                                 "export function addsItem(): void {\n"
                                 "  addItem();\n"
                                 "}\n"},
        /* A component defined in its directory's index file. */
        {"kit/label/index.ts", "export function Label(): void {}\n"},
        {"screens/label_user.ts", "import { Label } from '../kit/label';\n"
                                  "\n"
                                  "export function rendersLabel(): void {\n"
                                  "  Label();\n"
                                  "}\n"},
        /* A component beside its stylesheet: kit/chip.tsx and kit/chip.css
         * share a module qn, and the import means the component. */
        {"kit/chip.tsx", "export function Chip(): void {}\n"},
        {"kit/chip.css", ".chip { color: red; }\n"},
        {"screens/chip_user.ts", "import { Chip } from '../kit/chip';\n"
                                 "\n"
                                 "export function showsChip(): void {\n"
                                 "  Chip();\n"
                                 "}\n"},
        /* A module beside a directory of the same name, defining what is
         * imported: feed.ts and feed/ share a qn, and `./feed` means feed.ts. */
        {"feed.ts", "export function loadFeed(): void {}\n"},
        {"feed/item.ts", "export function renderItem(): void {}\n"},
        {"screens/feed_user.ts", "import { loadFeed } from '../feed';\n"
                                 "\n"
                                 "export function showsFeed(): void {\n"
                                 "  loadFeed();\n"
                                 "}\n"},
        /* A function documented beside itself, VitePress style: index.md
         * opens with the heading "# useToggle", which shares a qn with the
         * function index.ts exports. */
        {"kit/useToggle/index.ts", "export function useToggle(): void {}\n"},
        {"kit/useToggle/index.md", "# useToggle\n\nToggles a value.\n"},
        {"screens/toggle_user.ts", "import { useToggle } from '../kit/useToggle';\n"
                                   "\n"
                                   "export function togglesIt(): void {\n"
                                   "  useToggle();\n"
                                   "}\n"},
        /* Two classes with a same-named method; the caller imports one of
         * them, through its package barrel, and sits beside the other. */
        {"board/index.ts", "export * from './actions';\n"},
        /* A package of two same-method classes; the first listed registers
         * first and wins an even tie. */
        {"yard/archive.ts", "export class YardArchive {\n"
                            "  sweep(): void {}\n"
                            "}\n"},
        {"yard/keeper.ts", "export class YardKeeper {\n"
                           "  sweep(): void {}\n"
                           "}\n"},
        {"yard/index.ts", "export * from './keeper';\n"
                          "export * from './archive';\n"},
        {"board/actions.ts", "export class BoardActions {\n"
                             "  reset(): void {}\n"
                             "}\n"
                             "export const boardActions = new BoardActions();\n"},
        {"other/actions.ts", "export class OtherActions {\n"
                             "  reset(): void {}\n"
                             "}\n"},
        {"other/board_user.ts", "import { boardActions } from '../board';\n"
                                "\n"
                                "export function resetsBoard(): void {\n"
                                "  boardActions.reset();\n"
                                "}\n"},
        /* The caller imports the package barrel and the one module whose
         * class it instantiates. */
        {"other/yard_user.ts", "import { YardKeeper } from '../yard/keeper';\n"
                               "import { yardTools } from '../yard';\n"
                               "\n"
                               "export function sweepsLocal(): void {\n"
                               "  const local = new YardKeeper();\n"
                               "  local.sweep();\n"
                               "  void yardTools;\n"
                               "}\n"},
        /* A package exporting an instance (`toast`, no definition of its own)
         * beside a class that happens to have a member of the called name. */
        {"kit/index.ts", "export * from './sheet';\n"},
        {"kit/sheet.ts", "export class Sheet {\n"
                         "  static show(): void {}\n"
                         "}\n"
                         "export const toast = makeToast();\n"
                         "export function hideAll(): void {}\n"},
        {"screens/kit_user.ts", "import { toast } from '../kit';\n"
                                "import * as kitNs from '../kit';\n"
                                "\n"
                                "export function showsToast(): void {\n"
                                "  toast.show();\n"
                                "}\n"
                                "\n"
                                "export function hidesAll(): void {\n"
                                "  kitNs.hideAll();\n"
                                "}\n"},
        /* Same-named definitions beside the importer: nearest by path, but
         * outside the imported package. */
        {"screens/button.ts", "export function Button(): void {}\n"},
        {"screens/format.ts", "export function format(): void {}\n"},
        {"screens/home.ts", "import { Button, Panel, format } from '../ui';\n"
                            "\n"
                            "export function rendersButton(): void {\n"
                            "  Button();\n"
                            "}\n"
                            "\n"
                            "export function opensPanel(): void {\n"
                            "  Panel.open();\n"
                            "}\n"
                            "\n"
                            "export function formatsHome(): void {\n"
                            "  format();\n"
                            "}\n"},
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

/* ── tsconfig path aliases ───────────────────────────────────────── */

/* A workspace laid out the way Nx lays one out: the aliases live in a root
 * tsconfig.base.json, which no tool reads on its own; each project's
 * tsconfig.json reaches it through `extends`. */
static int setup_alias_repo(void) {
    snprintf(g_alias_tmpdir, sizeof(g_alias_tmpdir), "/tmp/ctx_alias_XXXXXX");
    if (!ctx_mkdtemp(g_alias_tmpdir)) {
        return -1;
    }
    const char *const files[][2] = {
        /* Comments and a trailing comma: tsconfig is JSONC. */
        {"tsconfig.base.json", "{\n"
                               "  // shared by every project\n"
                               "  \"compilerOptions\": {\n"
                               "    \"baseUrl\": \".\",\n"
                               "    \"paths\": {\n"
                               "      \"@acme/ui\": [\"libs/ui/src/index.ts\"],\n"
                               "      \"@acme/util/*\": [\"libs/util/src/lib/*\"],\n"
                               "    },\n"
                               "  },\n"
                               "}\n"},
        {"libs/ui/tsconfig.json", "{ \"extends\": \"../../tsconfig.base.json\" }\n"},
        {"libs/ui/src/index.ts", "export * from './lib/button';\n"},
        {"libs/ui/src/lib/button.ts", "export function Button(): void {}\n"},
        /* Two hops, the middle one named without its .json. */
        {"libs/tsconfig.libs.json", "{ \"extends\": \"../tsconfig.base.json\" }\n"},
        {"libs/util/tsconfig.json", "{ \"extends\": \"../tsconfig.libs\" }\n"},
        {"libs/util/src/lib/date.ts", "export function formatDate(): void {}\n"},
        {"libs/util/src/lib/button.ts", "export function Button(): void {}\n"},
        {"libs/util/src/lib/decorate.ts", "import { Button } from '@acme/ui';\n"
                                          "\n"
                                          "export function decorate(): void {\n"
                                          "  Button();\n"
                                          "}\n"},
        {"apps/shop/tsconfig.json", "{\n"
                                    "  \"extends\": \"../../tsconfig.base.json\",\n"
                                    "  \"compilerOptions\": { \"strict\": true }\n"
                                    "}\n"},
        {"apps/shop/src/button.ts", "export function Button(): void {}\n"},
        {"apps/shop/src/date.ts", "export function formatDate(): void {}\n"},
        {"apps/shop/src/main.ts", "import { Button } from '@acme/ui';\n"
                                  "import { formatDate } from '@acme/util/date';\n"
                                  "\n"
                                  "export function render(): void {\n"
                                  "  Button();\n"
                                  "}\n"
                                  "\n"
                                  "export function stamp(): void {\n"
                                  "  formatDate();\n"
                                  "}\n"},
        /* Its own paths and no baseUrl: they resolve against the inherited
         * baseUrl (the repo root), not this file's directory. */
        {"apps/admin/tsconfig.json", "{\n"
                                     "  \"extends\": \"../../tsconfig.base.json\",\n"
                                     "  \"compilerOptions\": {\n"
                                     "    \"paths\": {\n"
                                     "      \"@admin/*\": [\"apps/admin/src/features/*\"]\n"
                                     "    }\n"
                                     "  }\n"
                                     "}\n"},
        {"apps/admin/src/features/orders.ts", "export function listOrders(): void {}\n"},
        {"apps/admin/src/orders.ts", "export function listOrders(): void {}\n"},
        {"apps/admin/src/main.ts", "import { listOrders } from '@admin/orders';\n"
                                   "\n"
                                   "export function showOrders(): void {\n"
                                   "  listOrders();\n"
                                   "}\n"},
        /* No extends; a baseUrl below the config, and a path relative to it. */
        {"apps/legacy/tsconfig.json", "{\n"
                                      "  \"compilerOptions\": {\n"
                                      "    \"baseUrl\": \"src\",\n"
                                      "    \"paths\": { \"~/*\": [\"*\"] }\n"
                                      "  }\n"
                                      "}\n"},
        {"apps/legacy/src/helpers/text.ts", "export function slugify(): void {}\n"
                                            "export function titleCase(): void {}\n"},
        {"apps/legacy/src/text.ts", "export function slugify(): void {}\n"
                                    "export function titleCase(): void {}\n"},
        {"apps/legacy/src/main.ts", "import { slugify } from 'helpers/text';\n"
                                    "import { titleCase } from '~/helpers/text';\n"
                                    "\n"
                                    "export function makeSlug(): void {\n"
                                    "  slugify();\n"
                                    "}\n"
                                    "\n"
                                    "export function makeTitle(): void {\n"
                                    "  titleCase();\n"
                                    "}\n"},
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (ir_write_in(g_alias_tmpdir, files[i][0], files[i][1]) != 0) {
            return -1;
        }
    }
    return 0;
}

static ctx_gbuf_t *g_alias_seq = NULL;
static ctx_gbuf_t *g_alias_par = NULL;
static bool g_alias_ready = false;
static bool g_alias_failed = false;

static int ensure_alias_graphs(void) {
    if (g_alias_ready) {
        return 0;
    }
    if (g_alias_failed || setup_alias_repo() != 0) {
        g_alias_failed = true;
        return -1;
    }
    ctx_discover_opts_t opts = {.mode = CTX_MODE_FULL};
    ctx_file_info_t *files = NULL;
    int file_count = 0;
    if (ctx_discover(g_alias_tmpdir, &opts, &files, &file_count) != 0) {
        g_alias_failed = true;
        return -1;
    }
    g_alias_seq = pf_run_sequential("alias-test", g_alias_tmpdir, files, file_count);
    g_alias_par = pf_run_parallel("alias-test", g_alias_tmpdir, files, file_count);
    ctx_discover_free(files, file_count);
    g_alias_ready = true;
    return 0;
}

#define ALIAS_BOTH_MODES(expr_seq, expr_par)                                                       \
    do {                                                                                           \
        if (ensure_alias_graphs() != 0) {                                                          \
            SKIP("setup failed");                                                                  \
        }                                                                                          \
        ASSERT_TRUE(expr_seq);                                                                     \
        ASSERT_TRUE(expr_par);                                                                     \
    } while (0)

/* An alias from an extended base config reaches the package's barrel, and
 * the barrel's package holds the definition. */
TEST(alias_from_extended_base_resolves_into_the_package) {
    ALIAS_BOTH_MODES(pf_call_resolved_by(g_alias_seq, "render", "libs.ui.src.lib.button.Button",
                                         "import_map_suffix"),
                     pf_call_resolved_by(g_alias_par, "render", "libs.ui.src.lib.button.Button",
                                         "import_map_suffix"));
    ALIAS_BOTH_MODES(!pf_find_call(g_alias_seq, "render", "apps.shop.src.button.Button"),
                     !pf_find_call(g_alias_par, "render", "apps.shop.src.button.Button"));
    PASS();
}

TEST(wildcard_alias_resolves_to_the_module) {
    ALIAS_BOTH_MODES(pf_call_resolved_by(g_alias_seq, "stamp", "libs.util.src.lib.date.formatDate",
                                         "import_map"),
                     pf_call_resolved_by(g_alias_par, "stamp", "libs.util.src.lib.date.formatDate",
                                         "import_map"));
    PASS();
}

/* libs/util/tsconfig.json → ../tsconfig.libs (no .json) → the base. */
TEST(alias_through_two_extends_hops) {
    ALIAS_BOTH_MODES(pf_call_resolved_by(g_alias_seq, "decorate", "libs.ui.src.lib.button.Button",
                                         "import_map_suffix"),
                     pf_call_resolved_by(g_alias_par, "decorate", "libs.ui.src.lib.button.Button",
                                         "import_map_suffix"));
    PASS();
}

TEST(child_paths_resolve_against_the_inherited_base_url) {
    const char *orders = "apps.admin.src.features.orders.listOrders";
    ALIAS_BOTH_MODES(pf_call_resolved_by(g_alias_seq, "showOrders", orders, "import_map"),
                     pf_call_resolved_by(g_alias_par, "showOrders", orders, "import_map"));
    PASS();
}

TEST(base_url_and_paths_relative_to_it) {
    ALIAS_BOTH_MODES(pf_call_resolved_by(g_alias_seq, "makeSlug",
                                         "apps.legacy.src.helpers.text.slugify", "import_map"),
                     pf_call_resolved_by(g_alias_par, "makeSlug",
                                         "apps.legacy.src.helpers.text.slugify", "import_map"));
    ALIAS_BOTH_MODES(pf_call_resolved_by(g_alias_seq, "makeTitle",
                                         "apps.legacy.src.helpers.text.titleCase", "import_map"),
                     pf_call_resolved_by(g_alias_par, "makeTitle",
                                         "apps.legacy.src.helpers.text.titleCase", "import_map"));
    PASS();
}

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

/* Every name a file imports from a module is in its import map, not only the
 * last: the graph keeps one IMPORTS edge per file and module, so a map read
 * back from those edges loses the rest. */
TEST(every_name_imported_from_one_module_resolves) {
    BOTH_MODES(pf_call_resolved_by(g_seq, "usesBoth", "util.date.formatDate", "import_map"),
               pf_call_resolved_by(g_par, "usesBoth", "util.date.formatDate", "import_map"));
    BOTH_MODES(pf_call_resolved_by(g_seq, "usesBoth", "util.date.parseDate", "import_map"),
               pf_call_resolved_by(g_par, "usesBoth", "util.date.parseDate", "import_map"));
    BOTH_MODES(!pf_find_call(g_seq, "usesBoth", "feature.date.formatDate"),
               !pf_find_call(g_par, "usesBoth", "feature.date.formatDate"));
    PASS();
}

/* An import of a package directory is an import of its index module, the
 * file that holds the re-exports, not of the Folder of the same name. */
TEST(barrel_import_edge_names_the_index_module) {
    BOTH_MODES(pf_find_edge(g_seq, "home.ts", "IMPORTS", "ui.index"),
               pf_find_edge(g_par, "home.ts", "IMPORTS", "ui.index"));
    PASS();
}

/* The file-name rules are TypeScript's. A Python module path's last dot
 * separates the module from the imported symbol, and the import names the
 * module, as it did before those rules existed. */
TEST(python_absolute_import_edge_names_the_module) {
    BOTH_MODES(pf_find_edge(g_seq, "abs_handlers.py", "IMPORTS", "util.deco"),
               pf_find_edge(g_par, "abs_handlers.py", "IMPORTS", "util.deco"));
    BOTH_MODES(!pf_find_edge(g_seq, "abs_handlers.py", "IMPORTS", "util.deco.register"),
               !pf_find_edge(g_par, "abs_handlers.py", "IMPORTS", "util.deco.register"));
    PASS();
}

/* The sequential pass creates IMPORTS edges once every file's definitions
 * exist. Before, a file's edges were made right after its own definitions, so
 * an import of a file later in the list found no module yet, or a sibling's:
 * here the importer sits between tag.ts and tag.mapper.ts. */
TEST(sequential_import_edge_does_not_depend_on_file_order) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/ctx_ord_XXXXXX");
    ASSERT_NOT_NULL(ctx_mkdtemp(dir));
    const char *const order[] = {"late/tag.ts", "early/uses.ts", "late/tag.mapper.ts"};
    const char *const content[] = {
        "export function mapTag(): void {}\n",
        "import { mapTag } from '../late/tag.mapper';\n"
        "\n"
        "export function usesLate(): void {\n"
        "  mapTag();\n"
        "}\n",
        "export function mapTag(): void {}\n",
    };
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ(ir_write_in(dir, order[i], content[i]), 0);
    }
    ctx_discover_opts_t opts = {.mode = CTX_MODE_FULL};
    ctx_file_info_t *found = NULL;
    int found_count = 0;
    ASSERT_EQ(ctx_discover(dir, &opts, &found, &found_count), 0);
    ASSERT_EQ(found_count, 3);
    ctx_file_info_t ordered[3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < found_count; j++) {
            if (strcmp(found[j].rel_path, order[i]) == 0) {
                ordered[i] = found[j];
            }
        }
    }
    ctx_gbuf_t *seq = pf_run_sequential("ord-test", dir, ordered, 3);
    bool to_mapper = pf_find_edge(seq, "uses.ts", "IMPORTS", "late.tag.mapper") != NULL;
    bool to_sibling = pf_find_edge(seq, "uses.ts", "IMPORTS", "late.tag") != NULL;
    ctx_gbuf_free(seq);
    ctx_discover_free(found, found_count);
    th_rmtree(dir);
    ASSERT(to_mapper);
    ASSERT(!to_sibling);
    PASS();
}

/* `../util/tag.mapper` names util/tag.mapper.ts: the last dot in a
 * specifier is not always an extension's. */
TEST(dotted_module_name_is_not_an_extension) {
    BOTH_MODES(pf_call_resolved_by(g_seq, "usesMapper", "util.tag.mapper.mapTag", "import_map"),
               pf_call_resolved_by(g_par, "usesMapper", "util.tag.mapper.mapTag", "import_map"));
    BOTH_MODES(!pf_find_call(g_seq, "usesMapper", "util.tag.mapTag"),
               !pf_find_call(g_par, "usesMapper", "util.tag.mapTag"));
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

/* A name imported from a barrel resolves to its definition inside the
 * imported package, never to a same-named one outside it. */
TEST(barrel_import_resolves_inside_the_package) {
    BOTH_MODES(
        pf_call_resolved_by(g_seq, "rendersButton", "ui.button.Button", "import_map_suffix"),
        pf_call_resolved_by(g_par, "rendersButton", "ui.button.Button", "import_map_suffix"));
    BOTH_MODES(!pf_find_call(g_seq, "rendersButton", "screens.button.Button"),
               !pf_find_call(g_par, "rendersButton", "screens.button.Button"));
    PASS();
}

/* `Panel.open()` on a class imported from a barrel is Panel's `open`, not the
 * first `open` a scan of the package happens to meet. */
TEST(barrel_import_member_is_the_named_class_member) {
    BOTH_MODES(
        pf_call_resolved_by(g_seq, "opensPanel", "ui.panel.Panel.open", "import_map_suffix"),
        pf_call_resolved_by(g_par, "opensPanel", "ui.panel.Panel.open", "import_map_suffix"));
    BOTH_MODES(!pf_find_call(g_seq, "opensPanel", "ui.panel.Drawer.open"),
               !pf_find_call(g_par, "opensPanel", "ui.panel.Drawer.open"));
    PASS();
}

/* Two `format`s in the package: the choice is deterministic (the shallower
 * one), and still never the one outside the package. */
TEST(barrel_import_with_two_candidates_prefers_the_shallower) {
    BOTH_MODES(pf_call_resolved_by(g_seq, "formatsHome", "ui.format.format", "import_map_suffix"),
               pf_call_resolved_by(g_par, "formatsHome", "ui.format.format", "import_map_suffix"));
    BOTH_MODES(!pf_find_call(g_seq, "formatsHome", "screens.format.format"),
               !pf_find_call(g_par, "formatsHome", "screens.format.format"));
    PASS();
}

/* A pick among several same-named definitions in the package is a tie-break,
 * not knowledge: it must not read as high confidence (0.7 and up). The single
 * candidate keeps its 0.85. */
TEST(barrel_import_with_two_candidates_is_not_high_confidence) {
    BOTH_MODES(pf_call_confidence(g_seq, "formatsHome", "ui.format.format") < 0.7,
               pf_call_confidence(g_par, "formatsHome", "ui.format.format") < 0.7);
    BOTH_MODES(pf_call_confidence(g_seq, "formatsHome", "ui.format.format") > 0,
               pf_call_confidence(g_par, "formatsHome", "ui.format.format") > 0);
    BOTH_MODES(pf_call_confidence(g_seq, "rendersButton", "ui.button.Button") >= 0.85,
               pf_call_confidence(g_par, "rendersButton", "ui.button.Button") >= 0.85);
    PASS();
}

/* `toast.show()` on a name the package defines nothing for: the import says
 * where `toast` comes from, not which `show` runs. Another class's member of
 * that name is a guess and must not be labelled as resolved by the import. */
/* With no module node to name, the Folder that took its qualified name is
 * still the package the import points into. */
TEST(barrel_file_beside_its_directory_resolves_through_the_folder) {
    BOTH_MODES(pf_call_resolved_by(g_seq, "addsItem", "shop.cart.addItem", "import_map_suffix"),
               pf_call_resolved_by(g_par, "addsItem", "shop.cart.addItem", "import_map_suffix"));
    PASS();
}

/* The file an IMPORTS edge from `from` to `qn_suffix` lands on, or NULL. */
static const char *import_target_file(const ctx_gbuf_t *gb, const char *from,
                                      const char *qn_suffix) {
    const ctx_gbuf_edge_t *e = pf_find_edge(gb, from, "IMPORTS", qn_suffix);
    const ctx_gbuf_node_t *t = e ? ctx_gbuf_find_by_id(gb, e->target_id) : NULL;
    return t ? t->file_path : NULL;
}

/* The label and file of the node holding `qn`, compared with `label`/`file`. */
static bool qn_held_by(const ctx_gbuf_t *gb, const char *qn, const char *label,
                       const char *file) {
    const ctx_gbuf_node_t *n = ctx_gbuf_find_by_qn(gb, qn);
    return n && n->label && n->file_path && strcmp(n->label, label) == 0 &&
           strcmp(n->file_path, file) == 0;
}

static bool str_is(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

/* The stylesheet beside a component must not take the component's module:
 * the import, and the module's definitions, belong to the .tsx. */
TEST(component_beside_its_stylesheet_keeps_its_module) {
    BOTH_MODES(qn_held_by(g_seq, "ir-test.kit.chip", "Module", "kit/chip.tsx"),
               qn_held_by(g_par, "ir-test.kit.chip", "Module", "kit/chip.tsx"));
    BOTH_MODES(str_is(import_target_file(g_seq, "chip_user.ts", "kit.chip"), "kit/chip.tsx"),
               str_is(import_target_file(g_par, "chip_user.ts", "kit.chip"), "kit/chip.tsx"));
    PASS();
}

/* A module beside a directory of the same name keeps its module node, and
 * the import of it lands there, not on the Folder. */
TEST(module_beside_its_directory_keeps_its_module) {
    BOTH_MODES(qn_held_by(g_seq, "ir-test.feed", "Module", "feed.ts"),
               qn_held_by(g_par, "ir-test.feed", "Module", "feed.ts"));
    BOTH_MODES(str_is(import_target_file(g_seq, "feed_user.ts", "feed"), "feed.ts"),
               str_is(import_target_file(g_par, "feed_user.ts", "feed"), "feed.ts"));
    BOTH_MODES(pf_call_resolved_by(g_seq, "showsFeed", "feed.loadFeed", "import_map"),
               pf_call_resolved_by(g_par, "showsFeed", "feed.loadFeed", "import_map"));
    PASS();
}

/* The label of the node the CALLS edge from `from` to `qn_suffix` lands on. */
static const char *call_target_label(const ctx_gbuf_t *gb, const char *from,
                                     const char *qn_suffix) {
    const ctx_gbuf_edge_t *e = pf_find_call(gb, from, qn_suffix);
    const ctx_gbuf_node_t *t = e ? ctx_gbuf_find_by_id(gb, e->target_id) : NULL;
    return t ? t->label : NULL;
}

/* A function keeps its qn over the doc heading beside it, and the call lands
 * on the function, not on a markdown heading. */
TEST(function_beside_its_doc_heading_keeps_its_qn) {
    BOTH_MODES(qn_held_by(g_seq, "ir-test.kit.useToggle.useToggle", "Function",
                          "kit/useToggle/index.ts"),
               qn_held_by(g_par, "ir-test.kit.useToggle.useToggle", "Function",
                          "kit/useToggle/index.ts"));
    const char *toggle = "kit.useToggle.useToggle";
    BOTH_MODES(str_is(call_target_label(g_seq, "togglesIt", toggle), "Function"),
               str_is(call_target_label(g_par, "togglesIt", toggle), "Function"));
    PASS();
}

/* A definition in the index file itself is what a directory import names:
 * definitions there are qualified by the directory, not by `index`. */
TEST(definition_in_the_index_file_is_a_direct_import_hit) {
    BOTH_MODES(pf_call_resolved_by(g_seq, "rendersLabel", "kit.label.Label", "import_map"),
               pf_call_resolved_by(g_par, "rendersLabel", "kit.label.Label", "import_map"));
    PASS();
}

/* When the import cannot name the method, the guess among same-named methods
 * still prefers the package the caller imported, barrel or not. */
TEST(barrel_import_makes_its_package_reachable_for_a_guess) {
    BOTH_MODES(pf_find_call(g_seq, "resetsBoard", "board.actions.BoardActions.reset"),
               pf_find_call(g_par, "resetsBoard", "board.actions.BoardActions.reset"));
    BOTH_MODES(!pf_find_call(g_seq, "resetsBoard", "other.actions.OtherActions.reset"),
               !pf_find_call(g_par, "resetsBoard", "other.actions.OtherActions.reset"));
    PASS();
}

/* A module the caller imports by name outranks the rest of a package it also
 * imports through the barrel: the guess keeps to the module. */
TEST(imported_module_outranks_its_package_for_a_guess) {
    BOTH_MODES(pf_find_call(g_seq, "sweepsLocal", "yard.keeper.YardKeeper.sweep"),
               pf_find_call(g_par, "sweepsLocal", "yard.keeper.YardKeeper.sweep"));
    BOTH_MODES(!pf_find_call(g_seq, "sweepsLocal", "yard.archive.YardArchive.sweep"),
               !pf_find_call(g_par, "sweepsLocal", "yard.archive.YardArchive.sweep"));
    PASS();
}

/* A namespace import of a barrel reaches the package's top-level functions. */
TEST(namespace_import_of_barrel_resolves_member_function) {
    BOTH_MODES(pf_call_resolved_by(g_seq, "hidesAll", "kit.sheet.hideAll", "import_map_suffix"),
               pf_call_resolved_by(g_par, "hidesAll", "kit.sheet.hideAll", "import_map_suffix"));
    PASS();
}

TEST(barrel_member_call_does_not_claim_another_owners_method) {
    BOTH_MODES(!pf_call_resolved_by(g_seq, "showsToast", "kit.sheet.Sheet.show", "import_map_suffix"),
               !pf_call_resolved_by(g_par, "showsToast", "kit.sheet.Sheet.show", "import_map_suffix"));
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
    if (g_alias_seq) {
        ctx_gbuf_free(g_alias_seq);
        g_alias_seq = NULL;
    }
    if (g_alias_par) {
        ctx_gbuf_free(g_alias_par);
        g_alias_par = NULL;
    }
    if (g_alias_tmpdir[0]) {
        th_rmtree(g_alias_tmpdir);
        g_alias_tmpdir[0] = '\0';
    }
    g_alias_ready = false;
    g_alias_failed = false;
}

/* A bare call to a name the file imports from a package is the package's,
 * never a project function guessed by name: npm, a Node builtin, a default
 * import, a JSX component (T-jnpg). */
TEST(call_to_a_package_import_is_not_guessed_onto_the_project) {
    BOTH_MODES(!pf_find_call(g_seq, "rendersCard", "lib.render.render"),
               !pf_find_call(g_par, "rendersCard", "lib.render.render"));
    BOTH_MODES(!pf_find_call(g_seq, "rendersCard", "lib.resolve.resolve"),
               !pf_find_call(g_par, "rendersCard", "lib.resolve.resolve"));
    BOTH_MODES(!pf_find_call(g_seq, "rendersCard", "lib.dayjs.dayjs"),
               !pf_find_call(g_par, "rendersCard", "lib.dayjs.dayjs"));
    BOTH_MODES(!pf_find_call(g_seq, "Loading", "lib.Spin.Spin"),
               !pf_find_call(g_par, "Loading", "lib.Spin.Spin"));
    PASS();
}

/* A package the tree itself declares is project code: a call into it by its
 * package name keeps its edge, guessed or not. */
TEST(call_to_a_workspace_package_import_is_kept) {
    BOTH_MODES(pf_find_call(g_seq, "startsApp", "kit.src.use_kit.useKit") != NULL,
               pf_find_call(g_par, "startsApp", "kit.src.use_kit.useKit") != NULL);
    PASS();
}

/* `~/composables/counter` and `#imports` cannot be npm packages: the guess
 * onto the project's own composable stays. */
TEST(call_through_a_framework_alias_is_kept) {
    BOTH_MODES(pf_find_call(g_seq, "countsUp", "composables.counter.useCounter") != NULL,
               pf_find_call(g_par, "countsUp", "composables.counter.useCounter") != NULL);
    BOTH_MODES(pf_find_call(g_seq, "countsUp", "composables.theme.useTheme") != NULL,
               pf_find_call(g_par, "countsUp", "composables.theme.useTheme") != NULL);
    PASS();
}

/* An alias the loader cannot see keeps its guess: one kept in a
 * tsconfig.app.json that the project's config only references, whether or
 * not its key spells the path; and a bundler alias that spells the path it
 * points at, a file or a directory re-exporting it. */
TEST(call_through_an_alias_the_loader_cannot_see_is_kept) {
    BOTH_MODES(pf_find_call(g_seq, "mainA", "lib.format.formatDateA") != NULL,
               pf_find_call(g_par, "mainA", "lib.format.formatDateA") != NULL);
    BOTH_MODES(pf_find_call(g_seq, "mainA", "common.strings.padA") != NULL,
               pf_find_call(g_par, "mainA", "common.strings.padA") != NULL);
    BOTH_MODES(pf_find_call(g_seq, "homeB", "utils.money.formatMoneyB") != NULL,
               pf_find_call(g_par, "homeB", "utils.money.formatMoneyB") != NULL);
    BOTH_MODES(pf_find_call(g_seq, "homeB", "utils.tax.taxB") != NULL,
               pf_find_call(g_par, "homeB", "utils.tax.taxB") != NULL);
    BOTH_MODES(pf_find_call(g_seq, "homeB", "date.format.formatDayB") != NULL,
               pf_find_call(g_par, "homeB", "date.format.formatDayB") != NULL);
    PASS();
}

/* An alias no config declares and whose specifier spells no path keeps its
 * guess, because nothing proves it external: no manifest in the tree depends
 * on a package of that name. */
TEST(call_through_an_undeclared_alias_is_kept) {
    BOTH_MODES(pf_has_def(g_seq, "mainE"), pf_has_def(g_par, "mainE"));
    BOTH_MODES(pf_find_call(g_seq, "mainE", "utilities.utility.utilityE") != NULL,
               pf_find_call(g_par, "mainE", "utilities.utility.utilityE") != NULL);
    BOTH_MODES(pf_find_call(g_seq, "mainE", "utils.foo.fooE") != NULL,
               pf_find_call(g_par, "mainE", "utils.foo.fooE") != NULL);
    BOTH_MODES(pf_has_call(g_seq, "mainE", "renderWithE"),
               pf_has_call(g_par, "mainE", "renderWithE"));
    BOTH_MODES(pf_find_call(g_seq, "mainDenoE", "utils.greet.greetE") != NULL,
               pf_find_call(g_par, "mainDenoE", "utils.greet.greetE") != NULL);
    PASS();
}

/* A name the file also imports from a path the tree claims is not the
 * package's alone: the guess stays. */
TEST(name_also_imported_from_the_tree_keeps_its_guess) {
    BOTH_MODES(pf_find_call(g_seq, "swatchC", "colors.tint.tintC") != NULL,
               pf_find_call(g_par, "swatchC", "colors.tint.tintC") != NULL);
    PASS();
}

/* A package subpath that spells no project path is still the package's. */
TEST(package_subpath_spelling_no_project_path_is_not_guessed) {
    BOTH_MODES(!pf_find_call(g_seq, "copiesD", "lib.clone.cloneDeepD"),
               !pf_find_call(g_par, "copiesD", "lib.clone.cloneDeepD"));
    PASS();
}

SUITE(import_resolution) {
    RUN_TEST(bare_imported_call_resolves_through_import_map);
    RUN_TEST(member_of_imported_class_resolves_exactly);
    RUN_TEST(relative_import_resolves_in_both_resolvers);
    RUN_TEST(every_name_imported_from_one_module_resolves);
    RUN_TEST(dotted_module_name_is_not_an_extension);
    RUN_TEST(barrel_import_edge_names_the_index_module);
    RUN_TEST(python_absolute_import_edge_names_the_module);
    RUN_TEST(sequential_import_edge_does_not_depend_on_file_order);
    RUN_TEST(relative_import_reference_is_a_usage_of_the_imported_function);
    RUN_TEST(relative_import_decorator_is_the_imported_one);
    RUN_TEST(barrel_import_resolves_inside_the_package);
    RUN_TEST(barrel_import_member_is_the_named_class_member);
    RUN_TEST(barrel_import_with_two_candidates_prefers_the_shallower);
    RUN_TEST(barrel_import_with_two_candidates_is_not_high_confidence);
    RUN_TEST(barrel_file_beside_its_directory_resolves_through_the_folder);
    RUN_TEST(component_beside_its_stylesheet_keeps_its_module);
    RUN_TEST(module_beside_its_directory_keeps_its_module);
    RUN_TEST(function_beside_its_doc_heading_keeps_its_qn);
    RUN_TEST(definition_in_the_index_file_is_a_direct_import_hit);
    RUN_TEST(barrel_import_makes_its_package_reachable_for_a_guess);
    RUN_TEST(imported_module_outranks_its_package_for_a_guess);
    RUN_TEST(namespace_import_of_barrel_resolves_member_function);
    RUN_TEST(barrel_member_call_does_not_claim_another_owners_method);
    RUN_TEST(member_call_on_imported_name_is_not_the_name_itself);
    RUN_TEST(alias_from_extended_base_resolves_into_the_package);
    RUN_TEST(wildcard_alias_resolves_to_the_module);
    RUN_TEST(alias_through_two_extends_hops);
    RUN_TEST(child_paths_resolve_against_the_inherited_base_url);
    RUN_TEST(base_url_and_paths_relative_to_it);
    RUN_TEST(call_to_a_package_import_is_not_guessed_onto_the_project);
    RUN_TEST(call_to_a_workspace_package_import_is_kept);
    RUN_TEST(call_through_a_framework_alias_is_kept);
    RUN_TEST(call_through_an_alias_the_loader_cannot_see_is_kept);
    RUN_TEST(call_through_an_undeclared_alias_is_kept);
    RUN_TEST(name_also_imported_from_the_tree_keeps_its_guess);
    RUN_TEST(package_subpath_spelling_no_project_path_is_not_guessed);
    import_resolution_teardown();
}

/*
 * path_alias.h — tsconfig / jsconfig path aliases, for import resolution.
 *
 * `import { Button } from '@acme/ui'` names no file by itself: the alias is
 * defined in a tsconfig `compilerOptions.paths` block, or the specifier is
 * resolved against `compilerOptions.baseUrl`. Without these, every aliased
 * import was read as a repo path, matched no module, and its calls fell back
 * to a project-wide name guess (cortex-indexer T-fdya).
 *
 * TypeScript's own rules, which this follows:
 *   - A source file belongs to the nearest tsconfig.json (or jsconfig.json)
 *     in its directory or above. That is approximate: TypeScript scopes a
 *     config by its include/files lists, which this does not read.
 *   - `extends` (a path, or an array of them; `.json` may be left off) is
 *     followed, so options set in a base file such as tsconfig.base.json,
 *     which no tool picks up on its own, still apply. A later base overrides
 *     an earlier one, and the file's own options override both. `paths` is
 *     replaced as a whole, never merged. A package name (`@tsconfig/node20`)
 *     is not followed: such bases do not know the repo's layout.
 *   - `paths` targets resolve against the effective baseUrl when there is
 *     one, otherwise against the directory of the file that declared
 *     `paths`. baseUrl resolves against the file that declared it.
 *   - The pattern with the longest prefix before its `*` wins; an exact
 *     pattern beats every wildcard one. Its targets are tried in order.
 *
 * All paths are repo-relative and computed from the repo path, never from
 * the process's working directory (cortex-indexer T-j4s5).
 */
#ifndef CTX_PATH_ALIAS_H
#define CTX_PATH_ALIAS_H

#include <stddef.h>

typedef struct ctx_path_aliases ctx_path_aliases_t;

/* Most targets kept for one `paths` pattern; a lookup yields at most these
 * plus the baseUrl candidate. */
enum { CTX_PATH_ALIAS_MAX_TARGETS = 8, CTX_PATH_ALIAS_MAX_CANDIDATES = 9 };

/* Walk repo_path for tsconfig.json / jsconfig.json files and load each one's
 * effective aliases. Directories discovery skips (.git, node_modules, ...)
 * and those the root .gitignore ignores are not walked. NULL when the repo
 * has no config with `paths` or `baseUrl`. Free with ctx_path_aliases_free. */
ctx_path_aliases_t *ctx_path_aliases_load(const char *repo_path);

/* NULL-safe. */
void ctx_path_aliases_free(ctx_path_aliases_t *aliases);

/* The repo-relative paths a non-relative specifier may name, seen from the
 * file at source_rel, in the order TypeScript tries them: the best-matching
 * `paths` pattern's targets, then the specifier under baseUrl. Each is written
 * into out (heap strings the caller frees); returns how many, at most max.
 * Returns 0 for a relative specifier, or when no config applies. */
int ctx_path_aliases_candidates(const ctx_path_aliases_t *aliases, const char *source_rel,
                                const char *specifier, char **out, int max);

#endif /* CTX_PATH_ALIAS_H */

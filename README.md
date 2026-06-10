# cortex-indexer

Native (C) code-graph indexer for [Cortex](https://github.com/ruevu/cortex).
Extracted from `cortex` (originally `codebase-memory-mcp`, MIT — see `LICENSE`).
Cortex consumes the prebuilt binary from this repo's GitHub Releases; see
cortex's `scripts/fetch-indexer.mjs`.

## Build

    make -f Makefile.indexer indexer      # -> build/c/cortex-indexer

`--version` emits JSON (`{"version","schema","protocol"}`) that cortex's
`ensureIndexer` guard parses.

## Release

Tag `vX.Y.Z` → CI builds the matrix (darwin/linux × arm64/x64), injects
`-DCTX_VERSION` from the tag, and publishes
`cortex-indexer-X.Y.Z-<os>-<arch>.tar.gz` (+ `.sha256`) to the GitHub Release.
The asset names and the `.sha256` format match what cortex's fetcher expects.

## CI

`ci.yml` builds + runs the version smoke test on all four targets on every
push/PR. The C unit-test suite runs as a **non-gating** step (it has a known
red baseline; rehabilitation is tracked separately).

#!/usr/bin/env bash
# Asserts `cortex-indexer --version` emits JSON with a non-empty .version.
set -euo pipefail
BIN="${1:-build/c/cortex-indexer}"
out="$("$BIN" --version)"
echo "version output: $out"
python3 -c "import json,sys; d=json.loads(sys.argv[1]); assert isinstance(d.get('version'),str) and d['version'], 'missing/empty .version'; print('OK schema=%s protocol=%s' % (d.get('schema'), d.get('protocol')))" "$out"

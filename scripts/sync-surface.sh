#!/usr/bin/env bash
# Generates and synchronizes WebIDL bindings, docs, and TypeScript types from brosurface into bro.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BRO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECTS_ROOT="$(cd "$BRO_ROOT/.." && pwd)"
SURFACE_ROOT="$PROJECTS_ROOT/brosurface"

if [[ ! -d "$SURFACE_ROOT" ]]; then
    echo "ERROR: brosurface repository not found at: $SURFACE_ROOT" >&2
    exit 1
fi

SYNC_SCRIPT="$SURFACE_ROOT/tools/sync_to_bro.mjs"
if [[ ! -f "$SYNC_SCRIPT" ]]; then
    echo "ERROR: brosurface sync script not found at: $SYNC_SCRIPT" >&2
    exit 1
fi

echo "== Generating and Synchronizing brosurface -> bro =="
node "$SYNC_SCRIPT" "$@"
echo "✅ Surface synchronization completed successfully."

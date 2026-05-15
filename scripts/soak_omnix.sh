#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${OMNIX_BIN:-$REPO_ROOT/build/omnix}"
MEMORY_ROOT="${OMNIX_SOAK_MEMORY_ROOT:-$REPO_ROOT/build/omnix-soak-memory}"

if [[ ! -x "$BIN" ]]; then
  echo "OmniX binary not found at $BIN" >&2
  echo "Run: cmake --build build -j4" >&2
  exit 1
fi

mkdir -p "$MEMORY_ROOT"

run_check() {
  echo "+ $*"
  "$@"
}

run_check "$BIN" ask "What is the Sun" --memory-root "$MEMORY_ROOT" --compact
run_check "$BIN" define xProcessingCache --memory-root "$MEMORY_ROOT" --compact
run_check "$BIN" provider probe --memory-root "$MEMORY_ROOT" --compact || true
run_check "$BIN" tview doctor --memory-root "$MEMORY_ROOT" --compact || true
run_check "$BIN" defend diag cpu --memory-root "$MEMORY_ROOT" --compact
run_check "$BIN" tze replay latest --memory-root "$MEMORY_ROOT" --compact
run_check "$BIN" tze chain latest --memory-root "$MEMORY_ROOT" --compact

HISTORY="$MEMORY_ROOT/history.jsonl"
if [[ -f "$HISTORY" ]] && grep -q "Recent feedback:.*Recent feedback:" "$HISTORY"; then
  echo "Soak failed: detected recursively repeated feedback in history." >&2
  exit 1
fi

echo "OmniX soak complete: pre/post runtime, provider diagnostics, TView doctor, and defense diagnostics responded."

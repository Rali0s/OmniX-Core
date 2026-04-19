#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
MODEL_NAME="${OMNIX_OLLAMA_MODEL:-deepnimsec-omni:latest}"

export OMNIX_REASONING_PROVIDER=ollama
export OMNIX_OLLAMA_MODEL="$MODEL_NAME"

if [ "${1:-}" = "--refresh-model" ] || [ "${1:-}" = "--rebuild-model" ]; then
  shift
  "$REPO_ROOT/scripts/create_deepnimsec_ollama_model.sh" "$MODEL_NAME"
  if [ $# -eq 0 ]; then
    exit 0
  fi
fi

if [ "${1:-}" = "--probe" ]; then
  shift
  exec "$REPO_ROOT/build/omnix" provider probe "$@"
fi

if [ "${1:-}" = "--ask" ]; then
  shift
  exec "$REPO_ROOT/build/omnix" ask --assist "$@"
fi

if [ "${1:-}" = "--shell" ] || [ $# -eq 0 ]; then
  if [ "${1:-}" = "--shell" ]; then
    shift
  fi
  exec "$REPO_ROOT/build/omnix" shell --assist "$@"
fi

exec "$REPO_ROOT/build/omnix" "$@"

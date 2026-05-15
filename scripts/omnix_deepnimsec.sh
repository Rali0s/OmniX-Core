#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
MODEL_NAME="${OMNIX_OLLAMA_MODEL:-deepnimsec-omni:latest}"

export OMNIX_REASONING_PROVIDER=ollama
export OMNIX_OLLAMA_MODEL="$MODEL_NAME"

usage() {
  cat <<EOF
DeepNimSec OmniX wrapper

Common commands:
  ./scripts/omnix_deepnimsec.sh start
  ./scripts/omnix_deepnimsec.sh probe --compact
  ./scripts/omnix_deepnimsec.sh provider probe --compact
  ./scripts/omnix_deepnimsec.sh --refresh-model
  ./scripts/omnix_deepnimsec.sh --ask "What should I check next?"
  ./scripts/omnix_deepnimsec.sh --shell

Raw Ollama start command:
  nohup ollama serve >/tmp/omnix-ollama-serve.log 2>&1 &

Configured environment:
  OMNIX_REASONING_PROVIDER=ollama
  OMNIX_OLLAMA_MODEL=$MODEL_NAME
EOF
}

ensure_ollama_server() {
  if ! command -v ollama >/dev/null 2>&1; then
    echo "ollama is required but was not found on PATH." >&2
    exit 1
  fi

  if ollama list >/dev/null 2>&1; then
    return 0
  fi

  echo "Starting local Ollama server for OmniX assist..." >&2
  nohup ollama serve >/tmp/omnix-ollama-serve.log 2>&1 &

  attempts=0
  while [ "$attempts" -lt 15 ]; do
    if ollama list >/dev/null 2>&1; then
      return 0
    fi
    attempts=$((attempts + 1))
    sleep 1
  done

  echo "Ollama server did not become ready. Check /tmp/omnix-ollama-serve.log." >&2
  exit 1
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ] || [ "${1:-}" = "help" ]; then
  usage
  exit 0
fi

if [ "${1:-}" = "start" ] || [ "${1:-}" = "--start" ] || [ "${1:-}" = "--start-ollama" ]; then
  ensure_ollama_server
  echo "ollama_ready: Local Ollama server is reachable. Log: /tmp/omnix-ollama-serve.log"
  exit 0
fi

if [ "${1:-}" = "--refresh-model" ] || [ "${1:-}" = "--rebuild-model" ]; then
  shift
  ensure_ollama_server
  "$REPO_ROOT/scripts/create_deepnimsec_ollama_model.sh" "$MODEL_NAME"
  "$REPO_ROOT/build/omnix" provider probe
  if [ $# -eq 0 ]; then
    exit 0
  fi
fi

if [ "${1:-}" = "--probe" ] || [ "${1:-}" = "probe" ] || [ "${1:-}" = "status" ]; then
  shift
  ensure_ollama_server
  exec "$REPO_ROOT/build/omnix" provider probe "$@"
fi

if [ "${1:-}" = "provider" ] && [ "${2:-}" = "probe" ]; then
  ensure_ollama_server
  exec "$REPO_ROOT/build/omnix" "$@"
fi

if [ "${1:-}" = "--ask" ]; then
  shift
  ensure_ollama_server
  exec "$REPO_ROOT/build/omnix" ask --assist "$@"
fi

if [ "${1:-}" = "--shell" ] || [ $# -eq 0 ]; then
  if [ "${1:-}" = "--shell" ]; then
    shift
  fi
  ensure_ollama_server
  exec "$REPO_ROOT/build/omnix" shell --assist "$@"
fi

exec "$REPO_ROOT/build/omnix" "$@"

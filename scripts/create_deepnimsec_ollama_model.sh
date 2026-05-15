#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
MODEL_NAME="${1:-deepnimsec-omni:latest}"
MODELFILE="${OMNIX_DEEPNIMSEC_MODELFILE:-$REPO_ROOT/models/DeepNimSec-Omni.Modelfile}"
OMNIX_BINARY="${OMNIX_DEEPNIMSEC_BINARY:-$REPO_ROOT/build/omnix}"
BASE_MODEL=""

rendered_modelfile=""
command_reference_file=""

cleanup() {
  [ -n "$rendered_modelfile" ] && [ -f "$rendered_modelfile" ] && rm -f "$rendered_modelfile"
  [ -n "$command_reference_file" ] && [ -f "$command_reference_file" ] && rm -f "$command_reference_file"
}

trap cleanup EXIT INT TERM

if ! command -v ollama >/dev/null 2>&1; then
  echo "ollama is required but was not found on PATH." >&2
  exit 1
fi

if ! ollama list >/dev/null 2>&1; then
  echo "Starting local Ollama server for model creation..." >&2
  nohup ollama serve >/tmp/omnix-ollama-serve.log 2>&1 &

  attempts=0
  while [ "$attempts" -lt 15 ]; do
    if ollama list >/dev/null 2>&1; then
      break
    fi
    attempts=$((attempts + 1))
    sleep 1
  done

  if ! ollama list >/dev/null 2>&1; then
    echo "Ollama server did not become ready. Check /tmp/omnix-ollama-serve.log." >&2
    exit 1
  fi
fi

if [ ! -f "$MODELFILE" ]; then
  echo "DeepNimSec Modelfile not found at $MODELFILE" >&2
  exit 1
fi

BASE_MODEL="$(awk '/^FROM[[:space:]]+/ { print $2; exit }' "$MODELFILE")"
if [ -z "$BASE_MODEL" ]; then
  echo "Unable to determine the base model from $MODELFILE" >&2
  exit 1
fi

if ! ollama show "$BASE_MODEL" >/dev/null 2>&1; then
  echo "Base model $BASE_MODEL is not currently available in Ollama." >&2
  echo "Recovery:" >&2
  echo "  ollama pull $BASE_MODEL" >&2
  exit 1
fi

if [ ! -x "$OMNIX_BINARY" ]; then
  echo "OmniX binary not found or not executable at $OMNIX_BINARY" >&2
  echo "Build OmniX first so the DeepNimSec model can learn the live command reference." >&2
  exit 1
fi

command_reference_file="$(mktemp)"
rendered_modelfile="$(mktemp)"

{
  printf '%s\n' "Usage:"
  "$OMNIX_BINARY" --help 2>&1 || true
  printf '\n%s\n' "Routing notes:"
  printf '%s\n' '- Prefer canonical OmniX commands over prose.'
  printf '%s\n' '- Use `provider probe` to validate the local model/provider.'
  printf '%s\n' '- Use `doctor` or `preflight` before `build` when readiness is uncertain.'
  printf '%s\n' '- Use `tool <name> -- <args...>` only for guarded allowlisted actions.'
  printf '%s\n' '- In shell mode, plain prompts are routed through the OmniX intent resolver.'
} > "$command_reference_file"

awk '
  FNR==NR {
    ref = ref $0 ORS
    next
  }
  {
    if ($0 == "{{OMNIX_COMMAND_REFERENCE}}") {
      printf "%s", ref
    } else {
      print
    }
  }
' "$command_reference_file" "$MODELFILE" > "$rendered_modelfile"

echo "Creating Ollama model $MODEL_NAME from rendered DeepNimSec Modelfile"
ollama create "$MODEL_NAME" -f "$rendered_modelfile"
echo "Created $MODEL_NAME"

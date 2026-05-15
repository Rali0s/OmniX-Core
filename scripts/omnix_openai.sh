#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ENV_FILE="${OMNIX_ENV_FILE:-$REPO_ROOT/.env}"
OMNIX_BIN="${OMNIX_BIN:-$REPO_ROOT/build/omnix}"

usage() {
  cat >&2 <<'USAGE'
Usage:
  ./scripts/omnix_openai.sh provider probe --compact
  ./scripts/omnix_openai.sh shell --assist
  ./scripts/omnix_openai.sh ask --assist "What should I do next?"
  ./scripts/omnix_openai.sh "Define Turning Scale"
  ./scripts/omnix_openai.sh omnix tview port 5000
  ./scripts/omnix_openai.sh defend diag cpu

Loads repo-local ./.env by default. Override with OMNIX_ENV_FILE=/path/to/.env.
USAGE
}

trim() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "$value"
}

strip_quotes() {
  local value="$1"
  if [[ "${#value}" -ge 2 ]]; then
    if [[ "${value:0:1}" == '"' && "${value: -1}" == '"' ]]; then
      value="${value:1:${#value}-2}"
    elif [[ "${value:0:1}" == "'" && "${value: -1}" == "'" ]]; then
      value="${value:1:${#value}-2}"
    fi
  fi
  printf '%s' "$value"
}

load_env_file() {
  local file="$1"
  local line key value

  if [[ ! -f "$file" ]]; then
    echo "OpenAI env file not found: $file" >&2
    echo "Create ./.env from .env.example or set OMNIX_ENV_FILE=/path/to/.env." >&2
    exit 1
  fi

  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%$'\r'}"
    [[ -z "$(trim "$line")" ]] && continue
    [[ "$(trim "$line")" == \#* ]] && continue
    if [[ "$line" == export[[:space:]]* ]]; then
      line="${line#export}"
    fi
    [[ "$line" == *"="* ]] || continue

    key="$(trim "${line%%=*}")"
    value="$(trim "${line#*=}")"
    value="$(strip_quotes "$value")"

    case "$key" in
      OMNIX_REASONING_PROVIDER|OPENAI_API_KEY|OMNIX_OPENAI_API_KEY|OPENAI_MODEL|OMNIX_OPENAI_MODEL|OPENAI_BASE_URL|OMNIX_OPENAI_BASE_URL|OPENAI_ORGANIZATION|OMNIX_OPENAI_ORGANIZATION|OPENAI_PROJECT|OMNIX_OPENAI_PROJECT|OMNIX_OPENAI_MODEL_LIST_FILE|OMNIX_OPENAI_ASSIST_FILE|OMNIX_OPENAI_TOOL_PLAN_FILE|OMNIX_OPENAI_BUILD_PLAN_FILE|OMNIX_OPENAI_COMMAND_PLAN_FILE|OMNIX_OPENAI_NEXT_STEP_PLAN_FILE|OMNIX_OPENAI_CASE_SUMMARY_PLAN_FILE|OMNIX_OPENAI_FREEFORM_FILE)
        if [[ -z "${!key:-}" ]]; then
          export "$key=$value"
        fi
        ;;
    esac
  done < "$file"
}

is_omnix_command() {
  case "${1:-}" in
    --version|ask|ingest|analyze|decide|defend|case|incident|define|explain|review|patch-proposal|build|recipe|preflight|doctor|provider|persona|tview|shell|memory|tze|tool|legacy|map|search|emit-cpp|build-cmake)
      return 0
      ;;
  esac
  return 1
}

normalize_invocation() {
  local -a args=("$@")

  if [[ "${args[0]:-}" == *[[:space:]]* ]]; then
    local first="${args[0]}"
    local -a split_head=()
    read -r -a split_head <<< "$first"
    if [[ "${split_head[0]:-}" == "omnix" ]]; then
      split_head=("${split_head[@]:1}")
    fi
    if is_omnix_command "${split_head[0]:-}"; then
      args=("${split_head[@]}" "${args[@]:1}")
    elif [[ "${#args[@]}" -eq 1 ]]; then
      args=(ask --assist "$first")
    else
      args=(ask --assist "$first" "${args[@]:1}")
    fi
  fi

  if [[ "${args[0]:-}" == "omnix" ]]; then
    args=("${args[@]:1}")
  fi

  if [[ "${#args[@]}" -gt 0 ]] && ! is_omnix_command "${args[0]:-}" && [[ "${args[0]:-}" != --* ]]; then
    local prompt="${args[*]}"
    args=(ask --assist "$prompt")
  fi

  local index
  for index in "${!args[@]}"; do
    case "${args[$index]}" in
      verbose|-verbose)
        args[$index]="--verbose"
        ;;
      compact|-compact)
        args[$index]="--compact"
        ;;
    esac
  done

  printf '%s\0' "${args[@]}"
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

if [[ $# -eq 0 ]]; then
  usage
  exit 2
fi

load_env_file "$ENV_FILE"
export OMNIX_REASONING_PROVIDER="${OMNIX_REASONING_PROVIDER:-openai}"

if [[ ! -x "$OMNIX_BIN" ]]; then
  echo "OmniX binary not found or not executable: $OMNIX_BIN" >&2
  echo "Build first with: cmake --build build -j4" >&2
  exit 1
fi

normalized_args=()
while IFS= read -r -d '' arg; do
  normalized_args+=("$arg")
done < <(normalize_invocation "$@")

exec "$OMNIX_BIN" "${normalized_args[@]}"

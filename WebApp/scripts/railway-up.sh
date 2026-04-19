#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEFAULT_PROJECT="${RAILWAY_DEPLOY_PROJECT:-e4aa5453-a9de-424c-9916-e741c361f673}"
DEFAULT_ENVIRONMENT="${RAILWAY_DEPLOY_ENVIRONMENT:-production}"
DEFAULT_SERVICE="${RAILWAY_DEPLOY_SERVICE:-secure-appreciation}"

SKIP_BUILD=0
CHECK_ONLY=0
FORWARD_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --check)
      CHECK_ONLY=1
      shift
      ;;
    *)
      FORWARD_ARGS+=("$1")
      shift
      ;;
  esac
done

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

require_command railway
require_command npm

cd "${APP_DIR}"

if [[ ! -f "package.json" ]]; then
  echo "package.json not found in ${APP_DIR}" >&2
  exit 1
fi

if [[ ! -f "Dockerfile" ]]; then
  echo "Dockerfile not found in ${APP_DIR}. Railway deploy expects the WebApp-local runner." >&2
  exit 1
fi

if ! railway whoami >/dev/null 2>&1; then
  echo "Railway CLI is not authenticated. Run: railway login" >&2
  exit 1
fi

if ! railway status --json >/dev/null 2>&1; then
  cat >&2 <<'EOF'
This directory is not linked to a Railway project/service yet.

Run one of:
  railway link
  railway init
EOF
  exit 1
fi

echo "Railway deploy helper"
echo "App directory: ${APP_DIR}"
echo "Deploy root: ${APP_DIR}"
echo "Project: ${DEFAULT_PROJECT}"
echo "Environment: ${DEFAULT_ENVIRONMENT}"
echo "Service: ${DEFAULT_SERVICE}"

if [[ ${SKIP_BUILD} -eq 0 ]]; then
  echo "Running local build check before deploy..."
  npm run build
else
  echo "Skipping local build check."
fi

if [[ ${CHECK_ONLY} -eq 1 ]]; then
  echo "Checks passed. No deploy triggered because --check was set."
  exit 0
fi

echo "Starting Railway deployment..."
if [[ ${#FORWARD_ARGS[@]} -gt 0 ]]; then
  railway up . \
    --path-as-root \
    --verbose \
    --project "${DEFAULT_PROJECT}" \
    --environment "${DEFAULT_ENVIRONMENT}" \
    --service "${DEFAULT_SERVICE}" \
    "${FORWARD_ARGS[@]}"
else
  railway up . \
    --path-as-root \
    --verbose \
    --project "${DEFAULT_PROJECT}" \
    --environment "${DEFAULT_ENVIRONMENT}" \
    --service "${DEFAULT_SERVICE}"
fi

cat <<'EOF'

If this is the first successful deploy, remember to:
  1. Attach a Railway volume mounted at /app/data
  2. Set SITE_ADMIN_USERNAME
  3. Set SITE_ADMIN_PASSWORD
  4. Set SITE_SESSION_SECRET
EOF

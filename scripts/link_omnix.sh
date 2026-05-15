#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

PREFIX="${OMNIX_LINK_PREFIX:-$HOME/.local/bin}"
OMNIX_BIN="${OMNIX_BIN:-$REPO_ROOT/build/omnix}"
WITH_TZE=1
WITH_GG=0
FORCE=0
DRY_RUN=0
UNLINK=0

usage() {
  cat <<'EOF'
Usage:
  ./scripts/link_omnix.sh [--prefix dir] [--bin path] [--force] [--dry-run]
  ./scripts/link_omnix.sh --unlink [--prefix dir]

Creates:
  omnix -> <repo>/build/omnix
  tze   -> shim for `omnix tze ...`

Options:
  --prefix dir   Link destination directory. Default: $HOME/.local/bin
  --bin path     OmniX binary to link. Default: <repo>/build/omnix
  --no-tze       Do not create the `tze` convenience shim.
  --with-gg      Also create `gg` shim for `omnix gg ...`.
  --force        Replace existing files at the target link paths.
  --dry-run      Print planned actions without writing files.
  --unlink       Remove OmniX-managed links/shims from the prefix.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)
      [[ $# -ge 2 ]] || { echo "--prefix requires a value." >&2; exit 2; }
      PREFIX="$2"
      shift 2
      ;;
    --bin)
      [[ $# -ge 2 ]] || { echo "--bin requires a value." >&2; exit 2; }
      OMNIX_BIN="$2"
      shift 2
      ;;
    --no-tze)
      WITH_TZE=0
      shift
      ;;
    --with-gg)
      WITH_GG=1
      shift
      ;;
    --force)
      FORCE=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --unlink)
      UNLINK=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

case "$PREFIX" in
  ~*) PREFIX="${HOME}${PREFIX#\~}" ;;
esac
case "$OMNIX_BIN" in
  ~*) OMNIX_BIN="${HOME}${OMNIX_BIN#\~}" ;;
esac

if [[ "$OMNIX_BIN" != /* ]]; then
  OMNIX_BIN="$(CDPATH= cd -- "$(dirname "$OMNIX_BIN")" && pwd)/$(basename "$OMNIX_BIN")"
fi
if [[ "$PREFIX" != /* ]]; then
  PREFIX="$(pwd)/$PREFIX"
fi

say() {
  printf '%s\n' "$*"
}

ensure_prefix() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    say "would create directory: $PREFIX"
    return
  fi
  mkdir -p "$PREFIX"
}

remove_path() {
  local path="$1"
  if [[ ! -e "$path" && ! -L "$path" ]]; then
    say "absent: $path"
    return
  fi
  if [[ "$DRY_RUN" -eq 1 ]]; then
    say "would remove: $path"
    return
  fi
  rm -f "$path"
  say "removed: $path"
}

install_symlink() {
  local name="$1"
  local link_path="$PREFIX/$name"
  if [[ -e "$link_path" || -L "$link_path" ]]; then
    if [[ -L "$link_path" && "$(readlink "$link_path")" == "$OMNIX_BIN" ]]; then
      say "linked: $link_path -> $OMNIX_BIN"
      return
    fi
    if [[ "$FORCE" -ne 1 ]]; then
      echo "refusing to replace existing path: $link_path" >&2
      echo "rerun with --force if you want OmniX to replace it." >&2
      exit 1
    fi
    [[ "$DRY_RUN" -eq 1 ]] || rm -f "$link_path"
  fi
  if [[ "$DRY_RUN" -eq 1 ]]; then
    say "would link: $link_path -> $OMNIX_BIN"
    return
  fi
  ln -s "$OMNIX_BIN" "$link_path"
  say "linked: $link_path -> $OMNIX_BIN"
}

install_namespace_shim() {
  local name="$1"
  local namespace="$2"
  local shim_path="$PREFIX/$name"
  if [[ -e "$shim_path" || -L "$shim_path" ]]; then
    if [[ "$FORCE" -ne 1 ]]; then
      echo "refusing to replace existing path: $shim_path" >&2
      echo "rerun with --force if you want OmniX to replace it." >&2
      exit 1
    fi
    [[ "$DRY_RUN" -eq 1 ]] || rm -f "$shim_path"
  fi
  if [[ "$DRY_RUN" -eq 1 ]]; then
    say "would create shim: $shim_path -> $OMNIX_BIN $namespace"
    return
  fi
  cat > "$shim_path" <<EOF
#!/usr/bin/env sh
# OmniX-managed shim. Recreate with scripts/link_omnix.sh.
exec "$OMNIX_BIN" $namespace "\$@"
EOF
  chmod +x "$shim_path"
  say "shimmed: $shim_path -> $OMNIX_BIN $namespace"
}

if [[ "$UNLINK" -eq 1 ]]; then
  remove_path "$PREFIX/omnix"
  remove_path "$PREFIX/tze"
  remove_path "$PREFIX/gg"
  exit 0
fi

if [[ ! -x "$OMNIX_BIN" ]]; then
  echo "OmniX binary not found or not executable: $OMNIX_BIN" >&2
  echo "Build it first with: cmake --build build -j4" >&2
  exit 1
fi

ensure_prefix
install_symlink "omnix"
if [[ "$WITH_TZE" -eq 1 ]]; then
  install_namespace_shim "tze" "tze"
fi
if [[ "$WITH_GG" -eq 1 ]]; then
  install_namespace_shim "gg" "gg"
fi

case ":$PATH:" in
  *":$PREFIX:"*) ;;
  *)
    say "path_hint: add this to your shell profile if needed:"
    say "export PATH=\"$PREFIX:\$PATH\""
    ;;
esac

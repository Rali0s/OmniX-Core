#!/bin/sh
set -eu

package_file=""
evidence_root=""
offline=0
skip_builds=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --package)
            package_file="$2"
            shift 2
            ;;
        --evidence-dir)
            evidence_root="$2"
            shift 2
            ;;
        --offline)
            offline=1
            shift
            ;;
        --skip-builds)
            skip_builds=1
            shift
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [ -z "$package_file" ]; then
    echo "--package is required." >&2
    exit 1
fi
if [ -z "$evidence_root" ]; then
    echo "--evidence-dir is required." >&2
    exit 1
fi

timestamp=$(date -u +"%Y%m%d-%H%M%S")
evidence_dir="$evidence_root/$timestamp"
unpack_dir="$evidence_dir/unpack"
memory_root="$evidence_dir/home"
mkdir -p "$unpack_dir" "$memory_root"

tar -xzf "$package_file" -C "$unpack_dir"
binary=$(find "$unpack_dir" -path '*/bin/omnix' -type f | head -n 1)
source_map=$(find "$unpack_dir" -path '*/share/omnix/res/tze.txt' -type f | head -n 1)
if [ -z "$binary" ] || [ ! -x "$binary" ]; then
    echo "Packaged omnix binary not found at $binary" >&2
    exit 1
fi
if [ -z "$source_map" ] || [ ! -f "$source_map" ]; then
    echo "Packaged source map not found in $unpack_dir" >&2
    exit 1
fi

uname -a > "$evidence_dir/host.txt"
"$binary" --version > "$evidence_dir/version.txt"

common_args="--memory-root $memory_root --source-map $unpack_dir/share/omnix/res/tze.txt"
offline_flag=""
if [ "$offline" -eq 1 ]; then
    offline_flag="--offline"
fi

"$binary" preflight nmap --memory-root "$memory_root" --source-map "$source_map" $offline_flag \
    > "$evidence_dir/preflight-nmap.txt" 2>&1
"$binary" doctor nmap --memory-root "$memory_root" --source-map "$source_map" $offline_flag \
    > "$evidence_dir/doctor-nmap.txt" 2>&1
"$binary" tool inspect-host --memory-root "$memory_root" -- --linux \
    > "$evidence_dir/inspect-host.txt" 2>&1

host_report=$(awk -F': ' '/^Produced artifact: / { print $2; exit }' "$evidence_dir/inspect-host.txt")
if [ -n "$host_report" ] && [ -f "$host_report" ]; then
    "$binary" analyze "$host_report" --memory-root "$memory_root" --source-map "$source_map" \
        > "$evidence_dir/analyze-host-report.txt" 2>&1
fi

for required in history.jsonl definitions.json preferences.json projects.json; do
    if [ ! -e "$memory_root/$required" ]; then
        echo "Missing bootstrapped memory file: $required" >&2
        exit 1
    fi
done

if [ "$skip_builds" -eq 0 ]; then
    "$binary" build nmap --memory-root "$memory_root" --source-map "$source_map" $offline_flag \
        > "$evidence_dir/build-nmap.txt" 2>&1
    "$binary" build fmt --memory-root "$memory_root" --source-map "$source_map" $offline_flag \
        > "$evidence_dir/build-fmt.txt" 2>&1
    "$binary" build tinyxml2 --memory-root "$memory_root" --source-map "$source_map" $offline_flag \
        > "$evidence_dir/build-tinyxml2.txt" 2>&1
    "$binary" build lua --memory-root "$memory_root" --source-map "$source_map" $offline_flag \
        > "$evidence_dir/build-lua.txt" 2>&1
fi

find "$memory_root" -maxdepth 3 -type f | sort > "$evidence_dir/evidence-files.txt"
find "$memory_root/installs" -type f 2>/dev/null | sort > "$evidence_dir/artifacts.txt" || true

echo "Smoke evidence captured in $evidence_dir"

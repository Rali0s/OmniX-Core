#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=${OMNIX_REPO_DIR:-$(CDPATH= cd -- "$script_dir/.." && pwd)}
omnix_bin=${OMNIX_BIN:-"$repo_dir/build/omnix"}
source_map=${OMNIX_SOURCE_MAP:-"$repo_dir/res/tze.txt"}
memory_root=${OMNIX_MEMORY_ROOT:-"$HOME/.omnix"}
output_root=${OMNIX_DAILY_OUTPUT_ROOT:-"$repo_dir/build/daily"}
case_id=${OMNIX_CASE_ID:-}
skip_report=0

usage() {
    cat <<EOF
Usage:
  sh scripts/daily_omnix.sh [--case-id id] [--memory-root dir] [--output-dir dir] [--bin path] [--source-map path] [--skip-report]

What it does:
  1. Lists known cases.
  2. Selects a working case, preferring one tied to the source map.
  3. Falls back to analyzing the source map if no usable case exists.
  4. Runs case inspect, decide, inspect-log, inspect-build, text-pipeline, and report-case.
  5. Saves each step under build/daily/<timestamp>/ by default.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --case-id)
            case_id="$2"
            shift 2
            ;;
        --memory-root)
            memory_root="$2"
            shift 2
            ;;
        --output-dir)
            output_root="$2"
            shift 2
            ;;
        --bin)
            omnix_bin="$2"
            shift 2
            ;;
        --source-map)
            source_map="$2"
            shift 2
            ;;
        --skip-report)
            skip_report=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [ ! -x "$omnix_bin" ]; then
    echo "OmniX binary not found or not executable: $omnix_bin" >&2
    exit 1
fi

timestamp=$(date +"%Y%m%d-%H%M%S")
run_dir="$output_root/$timestamp"
mkdir -p "$run_dir"

pipeline_target="$repo_dir/docs/smoke-tests.md"
if [ ! -f "$pipeline_target" ]; then
    pipeline_target="$source_map"
fi

run_step() {
    label="$1"
    shift
    output_file="$run_dir/$label.txt"
    echo "==> $label"
    "$@" | tee "$output_file"
}

run_omnix() {
    command="$1"
    shift
    if [ -n "$source_map" ]; then
        "$omnix_bin" "$command" "$@" --memory-root "$memory_root" --source-map "$source_map"
    else
        "$omnix_bin" "$command" "$@" --memory-root "$memory_root"
    fi
}

extract_case_id_from_list() {
    preferred_source="$1"
    basename_source=$(basename "$preferred_source")
    while IFS= read -r line; do
        case "$line" in
            " - case-"*" | "*)
                current_id=$(printf "%s\n" "$line" | awk '{print $2}')
                if printf "%s\n" "$line" | grep -F "source=$preferred_source" >/dev/null 2>&1; then
                    printf "%s\n" "$current_id"
                    return 0
                fi
                if printf "%s\n" "$line" | grep -F "source=$basename_source" >/dev/null 2>&1; then
                    printf "%s\n" "$current_id"
                    return 0
                fi
                if printf "%s\n" "$line" | grep -F "source=res/$(basename "$preferred_source")" >/dev/null 2>&1; then
                    printf "%s\n" "$current_id"
                    return 0
                fi
                if [ -z "${first_case_id:-}" ]; then
                    first_case_id="$current_id"
                fi
                ;;
        esac
    done

    if [ -n "${first_case_id:-}" ]; then
        printf "%s\n" "$first_case_id"
        return 0
    fi
    return 1
}

extract_case_id_from_output() {
    awk '/^Case id: / { print $3; exit }' "$1"
}

list_output="$run_dir/01-case-list.txt"
echo "==> 01-case-list"
run_omnix case list | tee "$list_output"

if [ -z "$case_id" ]; then
    case_id=$(extract_case_id_from_list "$source_map" < "$list_output" || true)
fi

if [ -z "$case_id" ]; then
    analyze_output="$run_dir/02-analyze-source-map.txt"
    echo "==> 02-analyze-source-map"
    run_omnix analyze "$source_map" | tee "$analyze_output"
    case_id=$(extract_case_id_from_output "$analyze_output")
fi

if [ -z "$case_id" ]; then
    echo "Unable to determine a working case id." >&2
    exit 1
fi

run_step "03-case-inspect" run_omnix case "$case_id"
run_step "04-decide" run_omnix decide "$case_id"
run_step "05-inspect-log" "$omnix_bin" tool inspect-log --memory-root "$memory_root" -- "$source_map"
run_step "06-inspect-build" "$omnix_bin" tool inspect-build --memory-root "$memory_root" -- "$repo_dir"
run_step "07-text-pipeline" "$omnix_bin" tool text-pipeline --memory-root "$memory_root" -- "$pipeline_target" --grep "nmap|build|doctor|case" --sed "s/nmap/NMAP/g"

report_path=
if [ "$skip_report" -eq 0 ]; then
    report_output="$run_dir/08-report-case.txt"
    echo "==> 08-report-case"
    "$omnix_bin" tool report-case --memory-root "$memory_root" -- "$case_id" | tee "$report_output"
    report_path=$(awk -F': ' '/^Produced artifact: / { print $2; exit }' "$report_output")
fi

summary_file="$run_dir/summary.txt"
cat > "$summary_file" <<EOF
repo_dir=$repo_dir
omnix_bin=$omnix_bin
memory_root=$memory_root
source_map=$source_map
case_id=$case_id
pipeline_target=$pipeline_target
report_path=$report_path
generated_at=$(date +"%Y-%m-%dT%H:%M:%S")
EOF

echo
echo "Daily Omni run complete."
echo "Case id: $case_id"
echo "Run directory: $run_dir"
if [ -n "$report_path" ]; then
    echo "Report path: $report_path"
fi

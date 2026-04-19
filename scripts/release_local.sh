#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir=${OMNIX_RELEASE_SOURCE_DIR:-$(CDPATH= cd -- "$script_dir/.." && pwd)}
build_dir=${OMNIX_RELEASE_BUILD_DIR:-"$source_dir/build"}
skip_tests=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir)
            build_dir="$2"
            shift 2
            ;;
        --skip-tests)
            skip_tests=1
            shift
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
fi

cmake --build "$build_dir" -j

if [ "$skip_tests" -eq 0 ]; then
    ctest --test-dir "$build_dir" --output-on-failure -E 'omnix_release_local|omnix_release_local_validate'
fi

cmake --build "$build_dir" --target validate_generated_xpp

release_dir="$build_dir/release"
rm -rf "$release_dir"
mkdir -p "$release_dir/packages"

cmake --install "$build_dir" --prefix "$release_dir/install"
cpack -G TGZ -B "$release_dir/packages" --config "$build_dir/CPackConfig.cmake"

package_file=$(find "$release_dir/packages" -maxdepth 1 -type f -name 'omnix-*.tar.gz' | head -n 1)
if [ -z "$package_file" ]; then
    echo "No OmniX release package was produced." >&2
    exit 1
fi

cp "$package_file" "$release_dir/"
package_name=$(basename "$package_file")
version=$("$build_dir/omnix" --version | awk '{print $2}')

checksum_file="$release_dir/SHA256SUMS.txt"
if command -v shasum >/dev/null 2>&1; then
    (cd "$release_dir" && shasum -a 256 "$package_name" > "$checksum_file")
elif command -v sha256sum >/dev/null 2>&1; then
    (cd "$release_dir" && sha256sum "$package_name" > "$checksum_file")
else
    echo "No SHA256 tool found (expected shasum or sha256sum)." >&2
    exit 1
fi

cat > "$release_dir/MANIFEST.txt" <<EOF
version=$version
package=$package_name
install_prefix=$release_dir/install
generated_at=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
EOF

echo "Release artifacts written to $release_dir"

#!/bin/sh
set -eu

REPOSITORY=${CPM_REPOSITORY:-https://github.com/Rana718/cpm}
PREFIX=${CPM_INSTALL_PREFIX:-"$HOME/.local"}
WITH_NIX=0

usage() {
    printf '%s\n' "Usage: ./install.sh [--prefix PATH] [--with-nix]" \
        "" \
        "Installs CPM without modifying system package directories." \
        "Default prefix: $PREFIX"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix)
            [ "$#" -ge 2 ] || { printf '%s\n' "missing value for --prefix" >&2; exit 2; }
            PREFIX=$2
            shift 2
            ;;
        --with-nix)
            WITH_NIX=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

missing=""
for command in git cmake c++; do
    command -v "$command" >/dev/null 2>&1 || missing="$missing $command"
done
if [ -n "$missing" ]; then
    printf 'missing required build tools:%s\n' "$missing" >&2
    printf '%s\n' "Install them with your OS package manager, then rerun this script." >&2
    exit 1
fi

source_temp=""
build_dir=""
cleanup() {
    [ -z "$source_temp" ] || rm -rf "$source_temp"
    [ -z "$build_dir" ] || rm -rf "$build_dir"
}
trap cleanup EXIT HUP INT TERM

if [ -f CMakeLists.txt ] && grep -q 'project(cpm' CMakeLists.txt; then
    source_dir=$(pwd)
else
    source_temp=$(mktemp -d "${TMPDIR:-/tmp}/cpm-install.XXXXXX")
    git clone --depth 1 --quiet "$REPOSITORY" "$source_temp/source"
    source_dir=$source_temp/source
fi

build_dir=$(mktemp -d "${TMPDIR:-/tmp}/cpm-build.XXXXXX")
cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$build_dir" --parallel
cmake --install "$build_dir" --prefix "$PREFIX"

printf 'Installed %s\n' "$PREFIX/bin/cpm"
case ":${PATH}:" in
    *":$PREFIX/bin:"*) ;;
    *) printf 'Add %s/bin to PATH.\n' "$PREFIX" ;;
esac

if [ "$WITH_NIX" -eq 1 ]; then
    "$PREFIX/bin/cpm" setup
fi

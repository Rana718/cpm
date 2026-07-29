#!/bin/sh
# CPM installer — downloads the latest release binary and installs it.
# Usage:
#   curl -fsSL https://cpm.rana718.dev/install.sh | sh
#   curl -fsSL https://cpm.rana718.dev/install.sh | sh -s -- --prefix ~/.local --with-nix
set -eu

REPO="Rana718/cpm"
PREFIX="${CPM_INSTALL_PREFIX:-$HOME/.local}"
WITH_NIX=0

usage() {
    cat << EOF
Usage: install.sh [options]

Options:
  --prefix PATH   Install cpm to PATH/bin  (default: $HOME/.local)
  --with-nix      Also install Nix after cpm is set up
  -h, --help      Show this help

Environment variables:
  CPM_INSTALL_PREFIX   Same as --prefix
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix)
            [ "$#" -ge 2 ] || { printf 'missing value for --prefix\n' >&2; exit 2; }
            PREFIX=$2; shift 2 ;;
        --with-nix)
            WITH_NIX=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            printf 'unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

OS=$(uname -s)

if [ "$OS" != "Linux" ]; then
    printf 'error: CPM only supports Linux at this time.\n' >&2
    exit 1
fi

# Universal static binary — runs on any Linux (musl, glibc, any arch)
ARTIFACT="cpm-linux"

printf 'Fetching latest CPM release...\n'

if command -v curl >/dev/null 2>&1; then
    FETCH="curl -fsSL"
elif command -v wget >/dev/null 2>&1; then
    FETCH="wget -qO-"
else
    printf 'error: curl or wget is required\n' >&2
    exit 1
fi

API_URL="https://api.github.com/repos/${REPO}/releases/latest"
TAG=$($FETCH "$API_URL" | grep '"tag_name"' | head -1 | sed 's/.*"tag_name": *"\([^"]*\)".*/\1/')

if [ -z "$TAG" ]; then
    printf 'error: could not determine latest release tag\n' >&2
    exit 1
fi

printf 'Installing CPM %s for %s/%s\n' "$TAG" "$OS" "$ARCH"

# Download and verify
BASE_URL="https://github.com/${REPO}/releases/download/${TAG}"
ARCHIVE="${ARTIFACT}.tar.gz"
CHECKSUM_FILE="${ARTIFACT}.tar.gz.sha256"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/cpm-install.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

$FETCH "${BASE_URL}/${ARCHIVE}"       > "$TMP/$ARCHIVE"
$FETCH "${BASE_URL}/${CHECKSUM_FILE}" > "$TMP/$CHECKSUM_FILE"

cd "$TMP"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum -c "$CHECKSUM_FILE"
elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 -c "$CHECKSUM_FILE"
else
    printf 'warning: cannot verify checksum (sha256sum/shasum not found)\n'
fi

tar -xzf "$ARCHIVE"

# Install binary
mkdir -p "${PREFIX}/bin"
cp cpm "${PREFIX}/bin/cpm"
chmod 755 "${PREFIX}/bin/cpm"

printf '\nInstalled: %s/bin/cpm\n' "$PREFIX"

# Remind user to add to PATH if needed
case ":${PATH}:" in
    *":${PREFIX}/bin:"*) ;;
    *)
        printf '\nAdd the following to your shell profile:\n'
        printf '  export PATH="%s/bin:$PATH"\n\n' "$PREFIX" ;;
esac

# Optionally install Nix
if [ "$WITH_NIX" -eq 1 ]; then
    printf 'Installing Nix...\n'
    curl --proto '=https' --tlsv1.2 -L https://nixos.org/nix/install | sh -s -- --daemon
    printf 'Nix installed. Restart your shell or source /etc/profile.d/nix.sh\n'
fi

printf 'Done! Run: cpm --help\n'

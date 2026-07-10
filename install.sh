#!/bin/bash
# CPM Installer — installs cpm + nix on any Linux distro
set -e

VERSION="0.1.0"
INSTALL_DIR="/usr/local/bin"
CPM_REPO="https://github.com/user/cpm"  # TODO: update with actual repo

echo ""
echo "  ██████╗██████╗ ███╗   ███╗"
echo " ██╔════╝██╔══██╗████╗ ████║"
echo " ██║     ██████╔╝██╔████╔██║"
echo " ██║     ██╔═══╝ ██║╚██╔╝██║"
echo " ╚██████╗██║     ██║ ╚═╝ ██║"
echo "  ╚═════╝╚═╝     ╚═╝     ╚═╝"
echo "  C/C++ Package Manager v$VERSION"
echo ""

# ─── Detect distro ────────────────────────────────────────────

detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        echo "$ID"
    elif [ -f /etc/arch-release ]; then
        echo "arch"
    elif [ -f /etc/debian_version ]; then
        echo "debian"
    elif [ -f /etc/fedora-release ]; then
        echo "fedora"
    else
        echo "unknown"
    fi
}

DISTRO=$(detect_distro)
echo "[cpm] Detected distro: $DISTRO"

# ─── Install system dependencies ──────────────────────────────

echo "[cpm] Installing build dependencies..."

case "$DISTRO" in
    arch|manjaro|endeavouros)
        sudo pacman -S --needed --noconfirm git cmake ninja gcc curl
        ;;
    ubuntu|debian|pop|linuxmint)
        sudo apt update -qq
        sudo apt install -y git cmake ninja-build g++ curl build-essential
        ;;
    fedora|rhel|centos|rocky|alma)
        sudo dnf install -y git cmake ninja-build gcc-c++ curl
        ;;
    opensuse*)
        sudo zypper install -y git cmake ninja gcc-c++ curl
        ;;
    void)
        sudo xbps-install -Sy git cmake ninja gcc curl
        ;;
    alpine)
        sudo apk add git cmake ninja g++ curl bash
        ;;
    *)
        echo "[cpm] Unknown distro '$DISTRO'. Please install manually: git cmake ninja g++ curl"
        echo "[cpm] Then re-run this script."
        exit 1
        ;;
esac

# ─── Install Nix (isolated package manager) ───────────────────

if command -v nix &>/dev/null; then
    echo "[cpm] Nix already installed: $(nix --version)"
else
    echo ""
    echo "[cpm] Installing Nix (for isolated C/C++ build environments)..."
    echo "[cpm] This enables reproducible builds without polluting your system."
    echo ""

    # Use Determinate Systems installer (faster, cleaner)
    if curl -sI "https://install.determinate.systems/nix" &>/dev/null; then
        curl --proto '=https' --tlsv1.2 -sSf -L \
            https://install.determinate.systems/nix | sh -s -- install --no-confirm
    else
        # Fallback to official nix installer
        sh <(curl -L https://nixos.org/nix/install) --daemon
    fi

    # Source nix in current shell
    if [ -f /etc/profile.d/nix-daemon.sh ]; then
        . /etc/profile.d/nix-daemon.sh
    elif [ -f "$HOME/.nix-profile/etc/profile.d/nix.sh" ]; then
        . "$HOME/.nix-profile/etc/profile.d/nix.sh"
    fi

    # Add nixpkgs channel
    if command -v nix-channel &>/dev/null; then
        nix-channel --add https://nixos.org/channels/nixpkgs-unstable nixpkgs
        nix-channel --update
    fi

    echo "[cpm] Nix installed: $(nix --version 2>/dev/null || echo 'restart shell to use')"
fi

# ─── Build and install cpm ─────────────────────────────────────

echo ""
echo "[cpm] Building cpm..."

# Check if we're in the cpm source directory
if [ -f "CMakeLists.txt" ] && grep -q "project(cpm" CMakeLists.txt 2>/dev/null; then
    SRC_DIR="."
else
    # Clone the repo
    TMPDIR=$(mktemp -d)
    echo "[cpm] Cloning cpm source..."
    git clone --depth 1 "$CPM_REPO" "$TMPDIR/cpm" 2>/dev/null || {
        echo "[cpm] ERROR: Could not clone cpm repo."
        echo "[cpm] If you have the source, run this script from the cpm directory."
        exit 1
    }
    SRC_DIR="$TMPDIR/cpm"
fi

# Build
mkdir -p "$SRC_DIR/build"
cd "$SRC_DIR/build"
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
cmake --build . -j$(nproc)

# Install
sudo cp cpm "$INSTALL_DIR/cpm"
sudo chmod +x "$INSTALL_DIR/cpm"

echo ""
echo "[cpm] ✓ Installed cpm to $INSTALL_DIR/cpm"
echo "[cpm] ✓ Version: $(cpm --version 2>/dev/null || echo $VERSION)"
echo ""
echo "[cpm] Quick start:"
echo "  mkdir myproject && cd myproject"
echo "  cpm init myapp"
echo "  cpm run"
echo ""
echo "[cpm] Or run a single file:"
echo "  cpm run hello.cpp"
echo ""

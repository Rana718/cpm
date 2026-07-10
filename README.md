# CPM — C/C++ Package Manager

A fast, isolated package manager for C and C++ projects. Like `uv` for Python, but for C++ and C.

```bash
# Install
curl -fsSL https://raw.githubusercontent.com/Rana718/cpm/main/install.sh | bash
```

## Features

- **`cpm run`** — install deps + build + run in one command
- **`cpm run file.cpp`** — compile and run single files (like `uv run`)
- **`cpm build -s`** — optimized production binary + portable bundle
- **Fully isolated** — never pollutes system (`/usr/local` untouched)
- **Nix-powered** — reproducible builds, correct compiler versions, all deps pre-built
- **Parallel downloads** with live progress
- **Auto-resolves** latest GitHub tags
- **Multi-file** projects supported (`src/` directory)
- **Editor support** — auto-generates `compile_commands.json` for clangd

## Quick Start

```bash
# Create a new project
mkdir myapp && cd myapp
cpm init myapp

# Add dependencies
# Edit cpm.toml:
#   [dependencies]
#   json = "github:nlohmann/json"

# Build and run
cpm run
```

## Install

### One-line install (any Linux)

```bash
curl -fsSL https://raw.githubusercontent.com/Rana718/cpm/main/install.sh | bash
```

This installs:
- System build tools (cmake, ninja, g++)
- Nix (for isolated builds)
- cpm binary to `/usr/local/bin/`

### Manual install

```bash
git clone https://github.com/Rana718/cpm.git
cd cpm
./install.sh
```

### From source (development)

```bash
git clone https://github.com/Rana718/cpm.git
cd cpm
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
sudo cp cpm /usr/local/bin/
```

### Verify

```bash
cpm --version
# cpm 0.1.0
```

## Usage

### Create a project

```bash
cpm init myapp
```

Creates:
```
myapp/
├── cpm.toml
├── main.cpp
├── .cpm/
└── compile_commands.json
```

### cpm.toml

```toml
[project]
name = "myapp"
version = "0.1.0"
cpp_standard = "20"       # 11, 14, 17, 20, 23, 26
compiler = "gcc-13"       # optional: gcc, gcc-13, clang-17
entry = "main.cpp"
output = "myapp"

[scripts]
start = "./myapp"

[dependencies]
# Header-only libraries (fast, just clone)
json = "github:nlohmann/json@v3.11.3"
fmt = "github:fmtlib/fmt"              # latest tag

[system-dependencies]
# Compiled libraries (built from source in nix-shell)
hiredis = "github:redis/hiredis"
seastar = "github:scylladb/seastar"
```

### Commands

```bash
cpm init <name>       # Create new project
cpm install           # Install all dependencies
cpm build             # Compile
cpm build -s          # Production build (optimized + portable bundle)
cpm run               # Install + build + run
cpm run file.cpp      # Compile and run a single file
cpm start             # Run the built binary
cpm add <pkg>         # Add: cpm add github:redis/hiredis
cpm remove <name>     # Remove package
cpm update            # Re-download all
cpm list              # Show installed
cpm setup             # Install nix backend
cpm --version         # Show version
```

### Run single files

```bash
# Without a project — uses system g++
cpm run hello.cpp
cpm run test.c

# Inside a project — uses project's dependencies
cpm run quick.cpp     # can use #include <nlohmann/json.hpp>
```

### Production build

```bash
cpm build -s
```

Produces a `dist/` folder:
```
dist/
├── myapp             # optimized, stripped binary
├── lib*.so           # bundled dependencies
└── run.sh            # portable launcher
```

Copy `dist/` to any Linux server and run:
```bash
./dist/run.sh
```

## How It Works

1. **Header-only packages** → git clone, symlink headers to `.cpm/include/`
2. **Compiled packages** → build inside `nix-shell` (provides correct compiler + all deps)
3. **All artifacts** → stored in `.cpm/` (local) and `~/.cpm/cache/` (global)
4. **Nothing** touches `/usr/local/` or system libraries
5. **Nix** provides pre-built binaries from `cache.nixos.org` — most deps don't need local compilation

## Requirements

- Linux (x86_64)
- `git`, `curl`
- `nix` (auto-installed by `install.sh`)

## Development
```bash
# Build
task

# Format code
task fmt

# Lint
task lint

# Install to system
task install

# Clean
task clean
```

## License

[MIT](LICENSE)

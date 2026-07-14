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
entry = "main.cpp"        # can be a .cpp or .h file
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
sdl3 = "github:libsdl-org/SDL@release-3.2.14"

[libs]
# System libraries resolved via nix (headers + .so linked into .cpm/)
glew   = "glew"
opengl = "libGL"
```

### Dependency Types

CPM supports three ways to add libraries depending on how they need to be consumed:

#### `[dependencies]` — Header-only libraries

For libraries that don't need compilation (just `#include`). CPM clones the repo and symlinks headers into `.cpm/include/`.

```toml
[dependencies]
json  = "github:nlohmann/json@v3.11.3"   # pinned version
fmt   = "github:fmtlib/fmt"              # latest release tag
glm   = "github:g-truc/glm@0.9.9.8"
imgui = "github:ocornut/imgui@docking"   # branch name works too
stb   = "github:nothings/stb"            # no tags → uses HEAD
```

**When to use:** Libraries that are purely headers (`.h` / `.hpp` files). No build step needed — fastest option.

**Common header-only libraries:**

| Package                 | cpm.toml entry                                       |
| ----------------------- | ---------------------------------------------------- |
| nlohmann/json           | `json = "github:nlohmann/json@v3.11.3"`              |
| fmtlib/fmt              | `fmt = "github:fmtlib/fmt@10.1.1"`                   |
| glm                     | `glm = "github:g-truc/glm@0.9.9.8"`                  |
| spdlog                  | `spdlog = "github:gabime/spdlog@v1.12.0"`            |
| imgui                   | `imgui = "github:ocornut/imgui@docking"`             |
| stb                     | `stb = "github:nothings/stb"`                        |
| entt                    | `entt = "github:skypjack/entt@v3.12.2"`              |
| toml++                  | `tomlplusplus = "github:marzer/tomlplusplus@v3.4.0"` |
| Catch2 (v3 header-only) | `catch2 = "github:catchorg/Catch2@v3.4.0"`           |

#### `[system-dependencies]` — Compiled libraries (built from source)

For libraries that produce `.a` / `.so` files and need to be compiled. CPM clones the source, builds it (using cmake/make/meson/autotools), and installs artifacts into `.cpm/lib/`.

```toml
[system-dependencies]
hiredis    = "github:redis/hiredis@v1.2.0"
sdl3       = "github:libsdl-org/SDL@release-3.2.14"
uwebsocket = "github:uNetworking/uWebSockets@v20.62.0"
```

**When to use:** Libraries that need compilation (have `CMakeLists.txt`, `Makefile`, `meson.build`, etc.) and produce binary artifacts.

**Build strategies (tried in order):**

1. `cooking.sh` (inside nix-shell if available)
2. `configure.py`
3. CMake
4. Makefile
5. Meson
6. Autotools (`./configure`)
7. Header-only fallback (just copies headers)

**Common compiled libraries:**

| Package | cpm.toml entry                                  |
| ------- | ----------------------------------------------- |
| SDL3    | `sdl3 = "github:libsdl-org/SDL@release-3.2.14"` |
| hiredis | `hiredis = "github:redis/hiredis@v1.2.0"`       |
| raylib  | `raylib = "github:raysan5/raylib@5.0"`          |
| GLFW    | `glfw = "github:glfw/glfw@3.3.9"`               |
| libcurl | `curl = "github:curl/curl@curl-8_5_0"`          |
| SQLite  | `sqlite = "github:sqlite/sqlite"`               |
| zlib    | `zlib = "github:madler/zlib@v1.3.1"`            |

#### `[libs]` — System libraries via Nix

For system-level libraries (OpenGL, Vulkan, audio drivers, etc.) that are best provided pre-built by the package manager. CPM uses `nix-build` to fetch pre-compiled binaries and symlinks their headers and `.so` files into `.cpm/`.

```toml
[libs]
glew    = "glew"
opengl  = "libGL"
vulkan  = "vulkan-loader"
sdl2    = "SDL2"
openal  = "openal"
freetype = "freetype"
```

**When to use:** System libraries that are complex to build from source, have many transitive dependencies, or are tightly coupled to the OS (graphics drivers, audio, etc.).

**Requires:** Nix installed (`cpm setup` to install).

**The value is the nixpkgs attribute name.** Find package names at https://search.nixos.org/packages.

**Common system libraries:**

| Package    | cpm.toml entry             | Provides              |
| ---------- | -------------------------- | --------------------- |
| OpenGL     | `opengl = "libGL"`         | `-lGL`                |
| GLEW       | `glew = "glew"`            | `-lGLEW`, `GL/glew.h` |
| Vulkan     | `vulkan = "vulkan-loader"` | `-lvulkan`            |
| SDL2       | `sdl2 = "SDL2"`            | `-lSDL2`              |
| FreeType   | `freetype = "freetype"`    | `-lfreetype`          |
| OpenAL     | `openal = "openal"`        | `-lopenal`            |
| ALSA       | `alsa = "alsa-lib"`        | `-lasound`            |
| PulseAudio | `pulse = "libpulseaudio"`  | `-lpulse`             |
| X11        | `x11 = "xorg.libX11"`      | `-lX11`               |
| Wayland    | `wayland = "wayland"`      | `-lwayland-client`    |

### Choosing the Right Section

```
┌─────────────────────────────────────────────────────────┐
│ Is it header-only? (no .cpp/.c to compile)              │
│   YES → [dependencies]                                  │
│   NO  ↓                                                 │
│ Is it a GitHub project you can build from source?       │
│   YES → [system-dependencies]                           │
│   NO  ↓                                                 │
│ Is it a system/OS-level library (GL, audio, drivers)?   │
│   YES → [libs] (needs nix)                              │
└─────────────────────────────────────────────────────────┘
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

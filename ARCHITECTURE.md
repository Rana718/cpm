# CPM Architecture

CPM is a C/C++ package manager built around **complete isolation** using **Nix** as the backend for reproducible builds.

---

## System Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         USER                                     │
│                                                                  │
│   cpm init ─── cpm install ─── cpm build ─── cpm run           │
│                                  │              │                │
│   cpm run file.cpp              cpm build -s   cpm start        │
│   (single file)                 (production)   (run binary)     │
└───────────────────────────────────┬─────────────────────────────┘
                                    │
┌───────────────────────────────────▼─────────────────────────────┐
│                        cpm.toml                                  │
│                                                                  │
│  [project]                                                       │
│  name, entry, output, cpp_standard, compiler                    │
│                                                                  │
│  [dependencies]        ──→ Header-only (git clone)              │
│  [system-dependencies] ──→ Compiled (nix-shell build)           │
│  [libs]                ──→ System libraries (nix-build)         │
└───────────────────────────────────┬─────────────────────────────┘
                                    │
        ┌───────────────────────────┼───────────────────────┐
        │                           │                        │
        ▼                           ▼                        ▼
┌───────────────┐     ┌─────────────────────┐    ┌──────────────────┐
│  Git Clone    │     │    Nix Shell         │    │  Nix Build       │
│  (--depth 1)  │     │  (isolated env)      │    │  (pre-built)     │
│               │     │                      │    │                  │
│ nlohmann/json │     │ ┌──────────────────┐ │    │  glew            │
│ fmt           │     │ │ gcc13 / clang17  │ │    │  libGL           │
│ imgui         │     │ │ cmake, ninja     │ │    │  vulkan-loader   │
│ glm           │     │ │ boost, fmt, gmp  │ │    │  SDL2            │
│ stb           │     │ └──────────────────┘ │    │  freetype        │
└───────┬───────┘     │                      │    └────────┬─────────┘
        │             │  Build strategies:   │             │
        │             │  1. cooking.sh       │             │
        │             │  2. configure.py     │             │
        │             │  3. CMake            │             │
        │             │  4. Makefile         │             │
        │             │  5. Meson            │             │
        │             │  6. Autotools        │             │
        │             │  7. Header-only      │             │
        │             └──────────┬───────────┘             │
        │                        │                         │
        ▼                        ▼                         ▼
┌─────────────────────────────────────────────────────────────────┐
│                         .cpm/ (project-local)                    │
│                                                                  │
│  ├── include/          ← symlinked headers (all sources)        │
│  │   ├── nlohmann/     ← from [dependencies] git clone          │
│  │   ├── SDL3/         ← from [system-dependencies] build       │
│  │   ├── GL/           ← from [libs] nix-build                  │
│  │   └── imgui.h       ← from [dependencies] git clone          │
│  ├── lib/              ← .a from builds + .so from nix          │
│  │   ├── libSDL3.a     ← [system-dependencies] built artifact   │
│  │   ├── libGL.so      ← [libs] nix symlink                     │
│  │   └── libGLEW.so    ← [libs] nix symlink                     │
│  ├── packages/         ← header-only package symlinks → cache   │
│  ├── bin/              ← build tools                            │
│  └── _entry.cpp        ← generated wrapper (if entry is .h)    │
│                                                                  │
│  compile_commands.json  ← generated for editor LSP (clangd)    │
└───────────────────────────────────┬─────────────────────────────┘
                                    │
┌───────────────────────────────────▼─────────────────────────────┐
│                    ~/.cpm/cache/ (global)                         │
│                                                                  │
│  ├── json-v3.12.0/           ← cloned once, symlinked to .cpm   │
│  ├── SDL-release-3.2.14-src/ ← downloaded source                │
│  ├── SDL-release-3.2.14-built/ ← compiled artifacts (cached)    │
│  │   ├── include/SDL3/                                          │
│  │   └── lib/libSDL3.a                                          │
│  └── fmt-10.1.1/             ← header-only package cache        │
└─────────────────────────────────────────────────────────────────┘
                                    │
┌───────────────────────────────────▼─────────────────────────────┐
│                     /nix/store/ (nix managed)                    │
│                                                                  │
│  Pre-built binaries cached from cache.nixos.org                 │
│  ├── glew-2.2.0/          ← [libs] → symlinked into .cpm/      │
│  │   ├── include/GL/glew.h                                      │
│  │   └── lib/libGLEW.so                                         │
│  ├── libglvnd-1.7.0/      ← [libs] opengl = "libGL"            │
│  │   └── lib/libGL.so                                           │
│  ├── gcc-13.4.0/           ← compiler for [system-dependencies] │
│  ├── boost-1.89.0/         ← transitive dep (auto-detected)     │
│  └── ... (all deps, downloaded once, never compiled locally)    │
└─────────────────────────────────────────────────────────────────┘
```

---

## Source Code Structure

```
src/
├── main.cpp              # CLI entry — command dispatch
├── package_manager.cpp   # Core logic (~2100 lines)
│                         #   install, build, run, add, remove, update
├── toml_parser.cpp       # Parses cpm.toml → ProjectConfig struct
├── resolver.cpp          # Symlinks package headers into .cpm/include/
├── environment.cpp       # Creates .cpm/ directory structure
├── nix_env.cpp           # Nix integration (shell.nix, dep detection)
├── config.cpp            # System info (compiler, arch, OS)
└── progress.cpp          # Parallel download progress display

include/cpm/
├── package_manager.hpp   # PackageManager class definition
├── toml_parser.hpp       # ProjectConfig, GitDependency, SystemDependency, NixLibrary
├── resolver.hpp          # Resolver — header export logic
├── environment.hpp       # Environment — .cpm/ directory management
├── nix_env.hpp           # NixEnv — nix-shell generation & detection
├── config.hpp            # Config — system detection utilities
└── progress.hpp          # ProgressDisplay — parallel task UI
```

---

## Data Flow

```
cpm.toml
    │
    ▼  TomlParser::parse()
ProjectConfig {
    git_dependencies[]       →  [dependencies]
    system_dependencies[]    →  [system-dependencies]
    nix_libraries[]          →  [libs]
    name, entry, output, compiler, cpp_standard
}
    │
    ▼  PackageManager::install()
    │
    ├──→ [dependencies]        : git clone → ~/.cpm/cache/ → symlink .cpm/packages/
    │                            Resolver::export_headers() → .cpm/include/
    │
    ├──→ [system-dependencies] : git clone → build (7 strategies) → .cpm/lib/ + .cpm/include/
    │
    └──→ [libs]                : nix-build → symlink /nix/store/.../include/ → .cpm/include/
                                             symlink /nix/store/.../lib/     → .cpm/lib/
    │
    ▼  PackageManager::build()
    │
    │  build_compile_command() builds a single g++ invocation:
    │    - Auto-discovers ALL .cpp files recursively (skips .cpm/, .git/, build/)
    │    - Auto-discovers ALL header directories (-I flags)
    │    - Adds platform defines (-DSEED_PLATFORM_LINUX)
    │    - Links .a files from .cpm/lib/
    │    - Links .so via -l flags from [libs] entries
    │    - Filters imgui backends by available dependencies
    │    - Wraps in nix-shell if system-deps used nix
    │
    ▼
  output binary
```

---

## How Each Dependency Type Works

### 1. Header-Only Packages (`[dependencies]`)

```toml
[dependencies]
json = "github:nlohmann/json@v3.11.3"
fmt  = "github:fmtlib/fmt"              # latest tag auto-resolved
```

**Flow:**
1. Resolve version: if `*` or empty → `git ls-remote --tags` → pick latest
2. Check global cache: `~/.cpm/cache/<name>-<version>/`
3. If not cached: `git clone --depth 1 --branch <version>` → cache
4. Symlink: cache → `.cpm/packages/<name>`
5. Resolver finds include root (`include/`, `single_include/`, or `src/`)
6. Symlinks headers into `.cpm/include/`

**Parallel:** Up to 4 downloads simultaneously with live progress display.

### 2. Compiled Libraries (`[system-dependencies]`)

```toml
[system-dependencies]
sdl3 = "github:libsdl-org/SDL@release-3.2.14"
```

**Flow:**
1. Resolve version (same as above)
2. Check built cache: `~/.cpm/cache/<name>-<version>-built/`
3. If not built:
   - Clone source to `~/.cpm/cache/<name>-<version>-src/`
   - Detect transitive deps from `CMakeLists.txt` → `find_package()` calls
   - If Nix available: generate `shell.nix`, build inside `nix-shell`
   - Try build strategies in order (cooking.sh → configure.py → CMake → Make → Meson → Autotools → header-only)
   - Install artifacts to built cache
4. Symlink headers → `.cpm/include/`
5. Copy `.a` files → `.cpm/lib/`

**Parallel:** Up to 2 builds simultaneously (they may share deps).

### 3. System Libraries via Nix (`[libs]`)

```toml
[libs]
glew   = "glew"        # nixpkgs attribute name
opengl = "libGL"       # nixpkgs attribute name
```

**Flow:**
1. `nix-build '<nixpkgs>' -A <attr>.dev` → get nix store path (headers)
2. `nix-build '<nixpkgs>' -A <attr>` → get nix store path (libraries)
3. Symlink `<store>/include/*` → `.cpm/include/`
4. Symlink `<store>/lib/*.so` → `.cpm/lib/`
5. At link time: add `-l<name>` flags (mapped from user-facing name)

**Name mapping (automatic):**
| cpm.toml name | Linker flag |
|---------------|-------------|
| `opengl`      | `-lGL`      |
| `glew`        | `-lGLEW`    |
| `vulkan`      | `-lvulkan`  |
| `sdl2`        | `-lSDL2`    |
| `sdl3`        | `-lSDL3`    |

---

## Build System

CPM compiles projects with a **single compiler invocation** (no separate object files/linking step):

```bash
g++ -std=c++20 \
    -DSEED_PLATFORM_LINUX \
    -I.cpm/include -I.cpm/include/SDL3 -I.cpm/include/backends \
    -ISeed/src -ISeed/src/renderer -ISeed/src/platform/Linux \
    main.cpp src/app.cpp src/renderer/opengl.cpp ... \
    -o myapp \
    -L.cpm/lib -lSDL3 -lGL -lGLEW \
    -lz -lpthread -ldl -lrt -latomic
```

**Key behaviors:**
- Auto-scans ALL `.cpp` files recursively in the project tree
- Auto-discovers ALL header directories (for `-I` flags)
- Filters imgui backends by checking if their platform dependencies exist
- When entry is a `.h` file with existing `.cpp` files: doesn't create wrapper (avoids duplicate `main()`)
- Wraps compile in `nix-shell` if system-deps used nix (for linker access)
- Links `.a` files from `.cpm/lib/` automatically
- Links `.so` only from explicit `[libs]` entries (avoids unwanted runtime deps)

### ImGui Backend Filtering

When imgui is a dependency and `.cpm/include/backends/` exists, CPM selectively compiles only backends whose requirements are met:

| Backend | Compiled when |
|---------|--------------|
| `imgui_impl_sdl3` | `SDL3/` exists in `.cpm/include/` |
| `imgui_impl_opengl3` | Always (uses bundled loader) |
| `imgui_impl_glfw` | `GLFW/` exists in `.cpm/include/` |
| `imgui_impl_vulkan` | `vulkan/` exists in `.cpm/include/` |
| `imgui_impl_sdlrenderer3` | `SDL3/` exists in `.cpm/include/` |
| Others (dx*, win32, android, allegro, glut, metal, osx) | Never (platform-specific) |

### compile_commands.json

Auto-generated for editor LSP (clangd) support. Includes:
- Every `.cpp` file in the project tree
- All auto-discovered `-I` paths (project headers + `.cpm/include/` + subdirs)
- Platform defines
- Extra defines from `.cpm/defines.txt`

Regenerated on `cpm install` and `cpm init`.

---

## Auto-Cleanup

On every `cpm install`, stale artifacts are removed:

| What | Cleanup rule |
|------|-------------|
| `.cpm/packages/<name>` | Removed if `<name>` not in `[dependencies]` |
| `.cpm/lib/<lib>.a` | Removed if doesn't match any `[system-dependencies]` or `[libs]` (case-insensitive) |
| `.cpm/include/<dir>` | Removed if doesn't belong to any dep, system-dep, or nix lib (checks symlink targets for nix store paths) |

---

## Nix Integration

Nix provides **isolated, reproducible build environments**:

- Each compiled library gets its own `shell.nix` with exact deps
- Compiler version from `cpm.toml` (`gcc13`, `clang-17`, etc.)
- Default: `gcc13` + C++20 (broad compatibility)
- Pre-built binaries from `cache.nixos.org` (no local compilation for most deps)
- Completely isolated from host system
- `[libs]` fetches pre-compiled system packages directly from nix store

```nix
# Auto-generated by cpm from CMakeLists.txt analysis
{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  buildInputs = with pkgs; [
    gcc13 cmake ninja pkg-config automake autoconf libtool
    boost fmt yaml-cpp lz4 gnutls gmp nettle
    liburing numactl hwloc c-ares protobuf
  ];
}
```

**Dep detection:** Parses `CMakeLists.txt` `find_package()` calls and maps CMake names to nixpkgs attributes (e.g., `Boost` → `boost`, `GnuTLS` → `gnutls`).

---

## `cpm run file.cpp` (Single File Execution)

Like `uv run script.py` — compile and run a single file:

```bash
# Without cpm.toml — uses system g++ directly
cpm run hello.cpp

# With cpm.toml — uses project's deps (.cpm/include/)
cpm run quick.cpp
```

- Detects `.c` vs `.cpp` → uses `gcc` or `g++`
- If `cpm.toml` exists: adds `-I.cpm/include` and defines
- Wraps in `nix-shell` if project uses nix
- Cleans up binary after execution

---

## Production Build (`cpm build -s`)

```bash
cpm build -s
```

Produces:

```
dist/
├── myapp           ← optimized, stripped binary
├── lib*.so         ← bundled nix .so files
└── run.sh          ← launcher (sets LD_LIBRARY_PATH)
```

- **-O3 -DNDEBUG -march=x86-64-v3** optimized + stripped
- **Portable bundle** — copy `dist/` to any x86_64 Linux
- **run.sh** sets `LD_LIBRARY_PATH` to `$DIR` for bundled .so files
- Uses `ldd` to find nix-provided .so files and copies them to dist/
- System libs (libc, libstdc++) NOT bundled

---

## Commands

| Command             | Description                                  |
| ------------------- | -------------------------------------------- |
| `cpm init <name>`   | Create project (cpm.toml + main.cpp + .cpm/) |
| `cpm install`       | Download + build all deps                    |
| `cpm build`         | Compile (runs in nix-shell if needed)        |
| `cpm build -s`      | Production build (optimized + dist/ bundle)  |
| `cpm run`           | install → build → run                        |
| `cpm run file.cpp`  | Compile + run single file                    |
| `cpm start`         | Run the built binary                         |
| `cpm add <pkg>`     | Add package: `cpm add github:redis/hiredis`  |
| `cpm remove <name>` | Remove package                               |
| `cpm update`        | Re-download + rebuild all                    |
| `cpm list`          | Show installed packages                      |
| `cpm setup`         | Install nix backend                          |
| `cpm --version`     | Show version                                 |

---

## cpm.toml Reference

```toml
[project]
name = "myapp"
version = "0.1.0"
cpp_standard = "20"        # 11, 14, 17, 20, 23, 26
compiler = "gcc-13"        # optional: gcc, gcc-13, clang-17
entry = "main.cpp"         # can be .cpp or .h (if .h, must be included by a .cpp)
output = "myapp"

[scripts]
start = "./myapp"

[dependencies]
# Header-only (git clone → symlink headers)
json  = "github:nlohmann/json@v3.11.3"
fmt   = "github:fmtlib/fmt"
glm   = "github:g-truc/glm@0.9.9.8"
imgui = "github:ocornut/imgui@docking"
stb   = "github:nothings/stb"

[system-dependencies]
# Compiled from source (git clone → build → .a + headers)
sdl3    = "github:libsdl-org/SDL@release-3.2.14"
hiredis = "github:redis/hiredis@v1.2.0"
raylib  = "github:raysan5/raylib@5.0"

[libs]
# System libraries via nix (nix-build → symlink .so + headers)
# Value = nixpkgs attribute name (search.nixos.org/packages)
glew   = "glew"
opengl = "libGL"
vulkan = "vulkan-loader"
```

---

## Isolation Guarantees

| What                     | Where           | Touches System?  |
| ------------------------ | --------------- | ---------------- |
| Package headers          | `.cpm/include/` | ❌               |
| Compiled libraries       | `.cpm/lib/`     | ❌               |
| Build tools              | `.cpm/bin/`     | ❌               |
| Nix deps (glew, GL, ...) | `/nix/store/`   | ❌ (nix-managed) |
| Source cache             | `~/.cpm/cache/` | ❌               |
| Production bundle        | `dist/`         | ❌               |
| System compiler          | `/usr/bin/g++`  | Read-only        |

**Nothing is ever installed to `/usr/local/`, `/usr/lib/`, or any system path.**

---

## Multi-File Projects

```
myproject/
├── cpm.toml
├── Seed/
│   ├── seed.h             ← umbrella header (includes init.h)
│   └── src/
│       ├── init.h         ← entry point (has main())
│       ├── app.cpp
│       ├── renderer/
│       │   ├── opengl.cpp
│       │   └── render.cpp
│       └── platform/
│           └── Linux/
│               └── linux_window.cpp
├── leaf/
│   └── src/
│       └── sandbox.cpp    ← app layer (includes seed.h → init.h → main())
└── .cpm/                  ← isolated environment
```

- **entry** in cpm.toml can be a `.h` file — CPM detects if project `.cpp` files already include it (avoids duplicate `main()`)
- ALL `.cpp` files in the project tree are auto-compiled (excluding `.cpm/`, `.git/`, `build/`)
- ALL directories containing `.h` files get `-I` flags automatically
- `compile_commands.json` lists all source files with full include paths for editor LSP

---

## Supported Distros

Works on any Linux x86_64 with:

- `git`, `curl` — for downloading
- `nix` — for isolated builds (`cpm setup` to install)
- `cmake` — for building cpm itself

Tested on: Arch Linux, Ubuntu, Debian, Fedora

---

## Performance

| Operation                         | Time                            |
| --------------------------------- | ------------------------------- |
| `cpm install` (cached)            | ~0.1s                           |
| `cpm install` (fresh, header-only)| ~3-5s                           |
| `cpm install` (fresh, compiled)   | ~30s-2min (nix downloads pre-built) |
| `cpm build`                       | ~2s                             |
| `cpm run file.cpp`                | ~1s                             |
| `cpm build -s` (production)       | ~5s                             |

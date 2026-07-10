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
└───────────────────────────────────┬─────────────────────────────┘
                                    │
        ┌───────────────────────────┼───────────────────────┐
        │                           │                        │
        ▼                           ▼                        ▼
┌───────────────┐     ┌─────────────────────┐    ┌──────────────────┐
│  Git Clone    │     │    Nix Shell         │    │  System Compiler │
│  (--depth 1)  │     │  (isolated env)      │    │  (fallback)      │
│               │     │                      │    │                  │
│ nlohmann/json │     │ ┌──────────────────┐ │    │  g++ / clang++   │
│ fmt           │     │ │ gcc13 / clang17  │ │    │                  │
│ uWebSockets   │     │ │ boost, fmt, gmp  │ │    │                  │
│               │     │ │ gnutls, hwloc... │ │    │                  │
└───────┬───────┘     │ └──────────────────┘ │    └────────┬─────────┘
        │             │                      │             │
        │             │  cmake/ninja/make    │             │
        │             │  configure.py        │             │
        │             │  cooking.sh          │             │
        │             └──────────┬───────────┘             │
        │                        │                         │
        ▼                        ▼                         ▼
┌─────────────────────────────────────────────────────────────────┐
│                         .cpm/ (project-local)                    │
│                                                                  │
│  ├── include/          ← symlinks to headers (all packages)     │
│  ├── lib/              ← compiled .a files                      │
│  ├── packages/         ← header-only package symlinks           │
│  ├── defines.txt       ← compile flags (-DSEASTAR_...)          │
│  ├── bin/              ← build tools (stow)                     │
│  └── compile_commands.json (for editor LSP)                     │
└───────────────────────────────────┬─────────────────────────────┘
                                    │
┌───────────────────────────────────▼─────────────────────────────┐
│                    ~/.cpm/cache/ (global)                         │
│                                                                  │
│  ├── json-v3.12.0/           ← cloned once, symlinked           │
│  ├── hiredis-v1.4.0-src/     ← source                          │
│  ├── hiredis-v1.4.0-built/   ← built artifacts (cached)        │
│  │   ├── include/hiredis/                                       │
│  │   ├── lib/libhiredis.a                                       │
│  │   └── defines.txt                                            │
│  └── seastar-25.05.0-src/    ← source + shell.nix              │
│      └── build/release/      ← nix-shell built artifacts        │
└─────────────────────────────────────────────────────────────────┘
                                    │
┌───────────────────────────────────▼─────────────────────────────┐
│                     /nix/store/ (nix managed)                    │
│                                                                  │
│  Pre-built binaries cached from cache.nixos.org                 │
│  ├── boost-1.89.0/                                              │
│  ├── fmt-10.2.1/                                                │
│  ├── gnutls-3.8.13/                                             │
│  ├── gcc-13.4.0/                                                │
│  └── ... (all deps, downloaded once)                            │
└─────────────────────────────────────────────────────────────────┘
```

---

## How It Works

### 1. Header-Only Packages (`[dependencies]`)

```toml
[dependencies]
json = "github:nlohmann/json"
fmt = "github:fmtlib/fmt@10.1.1"
```

- Cloned from GitHub (`git clone --depth 1`)
- Cached globally in `~/.cpm/cache/`
- Symlinked into `.cpm/include/`
- No compilation needed

### 2. Compiled System Libraries (`[system-dependencies]`)

```toml
[system-dependencies]
seastar = "github:scylladb/seastar"
hiredis = "github:redis/hiredis"
```

**When Nix is available (recommended):**

1. Detect deps from `CMakeLists.txt` → `find_package()` calls
2. Generate `shell.nix` with correct compiler + all deps
3. Build inside `nix-shell` → correct versions guaranteed
4. Copy headers/libs to `.cpm/`
5. `cpm build` also links inside nix-shell

**When Nix is NOT available (fallback):**

1. Download source from GitHub
2. Auto-detect build system (cmake/make/meson/autotools/cooking.sh)
3. Build from source into `~/.cpm/cache/<name>-built/`
4. Copy to `.cpm/`

---

## Nix Integration

Nix provides **isolated, reproducible build environments**:

- Each project gets its own `shell.nix` with exact deps
- Compiler version from `cpm.toml` (`gcc13`, `clang-17`, etc.)
- Default: `gcc13` + C++20 (broad compatibility)
- Pre-built binaries from `cache.nixos.org` (no local compilation for deps)
- Completely isolated from host system

```nix
# Auto-generated by cpm from CMakeLists.txt analysis
{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  buildInputs = with pkgs; [
    gcc13 cmake ninja pkg-config
    boost fmt yaml-cpp lz4 gnutls gmp nettle
    liburing numactl hwloc c-ares protobuf
  ];
}
```

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
- Cleans up binary after execution

---

## Production Build (`cpm build -s`)

```bash
cpm build -s
```

Produces:

```
dist/
├── countries-api    ← optimized, stripped binary
├── libfmt.so.10    ← bundled app-specific .so files
├── libboost_*.so   ← (excludes system libc/libstdc++)
└── run.sh          ← launcher (sets LD_LIBRARY_PATH)
```

- **O3 optimized** + stripped (no debug symbols)
- **Portable bundle** — copy `dist/` to any x86_64 Linux
- **patchelf** rewrites binary to use `$ORIGIN` rpath
- System libs (libc, libstdc++) NOT bundled — target provides its own
- Works on Ubuntu, Debian, Fedora, CentOS, Amazon Linux, Alpine (with gcompat)

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

## cpm.toml

```toml
[project]
name = "myapp"
version = "0.1.0"
cpp_standard = "20"        # 11, 14, 17, 20, 23, 26
compiler = "gcc-13"        # gcc, gcc-13, clang-17, or empty (default: gcc13 in nix)
entry = "main.cpp"
output = "myapp"

[scripts]
start = "./myapp --smp 1"

[dependencies]
# Header-only (just git clone)
json = "github:nlohmann/json@v3.11.3"
fmt = "github:fmtlib/fmt"           # latest tag auto-resolved

[system-dependencies]
# Compiled libraries (built from source in nix-shell)
hiredis = "github:redis/hiredis"
seastar = "github:scylladb/seastar"
```

---

## Isolation Guarantees

| What                     | Where           | Touches System?  |
| ------------------------ | --------------- | ---------------- |
| Package headers          | `.cpm/include/` | ❌               |
| Compiled libraries       | `.cpm/lib/`     | ❌               |
| Build tools              | `.cpm/bin/`     | ❌               |
| Nix deps (boost, gmp...) | `/nix/store/`   | ❌ (nix-managed) |
| Source cache             | `~/.cpm/cache/` | ❌               |
| Production bundle        | `dist/`         | ❌               |
| System compiler          | `/usr/bin/g++`  | Read-only        |

**Nothing is ever installed to `/usr/local/`, `/usr/lib/`, or any system path.**

---

## Multi-File Projects

```
myproject/
├── cpm.toml
├── main.cpp              ← entry point
├── src/
│   ├── db.hpp            ← auto-included in build
│   ├── handlers.hpp
│   └── utils.cpp         ← auto-compiled
└── .cpm/                 ← isolated environment
```

- `entry` in cpm.toml = main source file
- All `.cpp` files in `src/` auto-compiled
- `compile_commands.json` lists all source files for editor LSP

---

## Supported Distros

Works on any Linux with:

- `git`, `curl` — for downloading
- `nix` — for isolated builds (`cpm setup` to install)
- `cmake` — for building cpm itself

Tested on: Arch Linux, Ubuntu, Debian, Fedora

---

## Performance

| Operation                         | Time                            |
| --------------------------------- | ------------------------------- |
| `cpm install` (cached)            | ~0.1s                           |
| `cpm install` (fresh, simple lib) | ~5s                             |
| `cpm install` (fresh, seastar)    | ~2min (nix downloads pre-built) |
| `cpm build`                       | ~2s                             |
| `cpm run file.cpp`                | ~1s                             |
| `cpm build -s` (production)       | ~5s                             |

# CPM

CPM is a project-local C and C++ package and environment manager for Linux. It resolves Git dependencies, builds compiled libraries, can obtain system libraries through Nix, and keeps generated headers, libraries, tools, and build metadata inside `.cpm/`.

## Isolation Boundary

Normal CPM commands never install into `/usr`, `/usr/local`, or another system prefix. Package builds use a temporary prefix and are published into the project only after every dependency succeeds. A failed install leaves the previous `.cpm/` environment active.

CPM still uses the host kernel and, unless a Nix compiler is selected, the host compiler and C runtime. `cpm setup` is the one command that intentionally installs Nix on the machine. Production bundles remain subject to the target machine's libc and kernel ABI.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Install the CLI with:

```bash
cmake --install build --prefix "$HOME/.local"
```

Or use the user-local bootstrap (it does not run `sudo` or install OS packages):

```bash
./install.sh
./install.sh --prefix "$HOME/.local" --with-nix # explicit Nix opt-in
```

## Quick Start

```bash
mkdir hello && cd hello
cpm init hello
cpm add json=github:nlohmann/json@v3.11.3
cpm run
```

`cpm install` creates:

```text
.cpm/
  include/       exported compiled/Nix headers
  lib/           isolated library links
  packages/      links to header-package cache entries
  objects/       incremental project object cache
  bin/
  activate.sh
cpm.lock         requested refs and resolved commit SHAs
compile_commands.json
```

The global cache is selected in this order:

1. `CPM_CACHE_DIR`
2. `$XDG_CACHE_HOME/cpm`
3. `$HOME/.cache/cpm`

## Manifest

```toml
[project]
name = "service"
version = "0.1.0"
cpp_standard = "20"
compiler = "gcc-13" # optional; uses Nix when available
nixpkgs = "nixos-24.05" # recommended when using Nix
nix_config = "./shell.nix" # optional; path to user nix-shell file
entry = "src/main.cpp"
output = "service"

[scripts]
start = "./service --port 8080"

[dependencies]
json = "github:nlohmann/json@v3.11.3"
local_headers = "file:///workspace/headers@main"

[system-dependencies]
hiredis = "github:redis/hiredis@v1.2.0"
custom = "https://git.example.com/team/custom.git@release/2"

[libs]
ssl = "openssl"
compression = "zlib"

[build]
include_paths = ["include", "generated/include"]
sources = ["generated/schema.cpp"]
exclude_sources = ["tests", "benchmarks"]
defines = ["SERVICE_FEATURE=1"]
compile_options = ["-Wall", "-Wextra"]
link_libraries = ["m"]
link_options = ["-Wl,--as-needed"]
nix_deps = ["boost", "yaml-cpp"]
```

Arrays must contain quoted strings. Unknown keys are ignored for forward compatibility. Invalid package names, Nix attributes, C++ standards, duplicate dependency names, and project path traversal are rejected with a file and line diagnostic.

## Dependency Kinds

### Header Packages

Entries in `[dependencies]` are cloned into the global content cache and linked into `.cpm/packages/`. CPM adds the repository root and conventional `include/`, `single_include/`, and `src/` roots to compiler arguments without flattening one package over another.

```bash
cpm add json=github:nlohmann/json@v3.11.3
```

### Compiled Git Packages

Entries in `[system-dependencies]` are built with the first applicable standard adapter:

- CMake
- Meson
- Autotools `configure`
- `configure.py`
- Make
- `cooking.sh`
- header-only fallback

Artifacts are installed to a package-specific cache prefix, never a system prefix. CMake/Meson install metadata and `pkg-config --static` flags are retained for the project link.

```bash
cpm add --system hiredis=github:redis/hiredis@v1.2.0
```

CPM derives common CMake package names for a Nix build shell. Add ambiguous or differently named packages explicitly through `[build].nix_deps`.

### Nix Libraries

Entries in `[libs]` are resolved from nixpkgs and linked into `.cpm/include` and `.cpm/lib`.

```bash
cpm add --lib ssl=openssl
```

Pin `[project].nixpkgs` for repeatable Nix resolution. Without a pin, the configured host nixpkgs channel is used.

### User Nix Config

Set `nix_config` in `[project]` to point at your own `nix-shell` file (relative path, must end in `.nix`). CPM merges its auto-detected build dependencies into your shell without duplication — your `shellHook`, overlays, and nixpkgs pin are fully preserved.

```toml
[project]
nix_config = "./shell.nix"
```

```nix
# shell.nix — declare only what CPM won't auto-detect
{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  packages = with pkgs; [
    openssl       # TLS support
    postgresql    # libpq database client
    spdlog        # fast logging
  ];
  shellHook = "echo dev shell ready";
}
```

CPM reads the `with pkgs; [ … ]` block from your file, identifies which packages it would add from its own detection, removes the ones already declared, and merges only the difference via `overrideAttrs`. If your shell already covers everything, it is used as-is with no modifications.

You can also combine `nix_config` with `[build].nix_deps` — both are merged without duplication:

```toml
[project]
nix_config = "./shell.nix"

[build]
nix_deps = ["protobuf", "grpc"]   # added on top of shell.nix
```

## Git Sources and Versions

Supported source forms include:

```toml
a = "github:owner/repository@v1.2.3"
b = "https://host/owner/repository.git@release/1"
c = "git@host:owner/repository.git@main"
d = "file:///absolute/local/repository@commit-or-tag"
```

The ref may be a tag, branch, slash-containing branch, or full commit. If omitted, CPM resolves the highest remote version-sorted tag, falling back to `HEAD`. CPM writes the requested ref and immutable commit SHA to `cpm.lock`; later installs reuse that exact commit. `cpm update` resolves moving refs again. Commit `cpm.lock` to version control.

Cache keys include the source URL and encoded ref, preventing aliases with the same name/version from sharing unrelated content.

## Commands

```text
cpm init <name>
cpm install
cpm add [alias=]git-source
cpm add --system [alias=]git-source
cpm add --lib [alias=]nix-attribute
cpm remove <alias>
cpm update
cpm list
cpm build
cpm build -s
cpm run
cpm run file.cpp
cpm start
cpm info
cpm setup
```

`add` and `remove` update `cpm.toml` atomically and roll it back if installation fails. `remove` works across all three dependency sections. `run file.cpp` compiles into `.cpm/run/`, executes it, and removes the temporary binary.

## Build Performance

Project translation units compile in parallel. Objects are keyed by compiler flags and retained in `.cpm/objects`; unchanged sources and headers are not recompiled. Static archives are linked as a group to avoid declaration-order failures. Paths and compiler arguments are executed with `posix_spawn`, not interpolated into a shell command.

`cpm build -s` enables `-O3 -DNDEBUG`, copies required project-local/Nix shared libraries into `dist/`, sets an origin-relative rpath when `patchelf` is available, and writes `dist/run.sh`.

## Development Checks

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cmake -S . -B build-san -DCPM_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-san --output-on-failure
```

The core tests cover manifest parsing and mutation, path traversal rejection, shell-safe process arguments, environment/header isolation, paths containing spaces, and incremental object reuse.

# CPM — Release Notes

> ⚠️ **Beta Software** — CPM is under active development. Core features are stable
> and used in real projects, but APIs and manifest formats may change between
> releases. Feedback and bug reports are very welcome.

---

## What is CPM?

CPM is a **project-local C and C++ package and environment manager for Linux**.

The most important thing about CPM: **it is completely isolated**.

Every dependency — headers, compiled libraries, Nix packages — lives inside
`.cpm/` in your project directory. CPM **never touches `/usr`, `/usr/local`,
or any system prefix**. No `sudo`. No polluting other projects. No version
conflicts between projects on the same machine. A failed install leaves the
previous working environment untouched.

---

## Why CPM? How is it different?

Most C++ package managers have one or more of these problems:

- They install globally and break other projects
- They only work with CMake or one specific build system
- They require a central registry — you can't just point at any Git repo
- They have no concept of reproducibility or lockfiles
- They can't build dependencies that need a specific compiler or system library

CPM solves all of these:

|                                                  | CPM | vcpkg | Conan        |
| ------------------------------------------------ | --- | ----- | ------------ |
| **Fully isolated — never touches system**        | ✅  | ❌    | configurable |
| Any Git source, no registry needed               | ✅  | ❌    | ❌           |
| Builds any upstream build system automatically   | ✅  | ❌    | ❌           |
| Nix hermetic compiler + library selection        | ✅  | ❌    | ❌           |
| Lockfile with exact commit SHAs                  | ✅  | ✅    | ✅           |
| Atomic install with rollback on failure          | ✅  | ❌    | ❌           |
| Incremental parallel compilation                 | ✅  | ❌    | ❌           |
| Zero config to start                             | ✅  | ❌    | ❌           |
| Binary cache                                     | ✅  | ✅    | ✅           |
| Production bundle (`dist/` with all shared libs) | ✅  | ❌    | ❌           |

---

## Features

### Three kinds of dependencies

**Header-only** (`[dependencies]`) — cloned from Git, linked into `.cpm/packages/`.
No build step. Works with any layout (`include/`, `single_include/`, `src/`).

```toml
[dependencies]
json = "github:nlohmann/json@v3.11.3"
```

**Compiled libraries** (`[system-dependencies]`) — CPM auto-detects and drives
CMake, Meson, Autotools, Make, `configure.py`, or `cooking.sh`. Artifacts go
into the global binary cache, never a system prefix.

```toml
[system-dependencies]
hiredis = "github:redis/hiredis@v1.2.0"
```

**Nix libraries** (`[libs]`) — resolves packages from nixpkgs and links headers
and `.so`/`.a` files into `.cpm/include` and `.cpm/lib`.

```toml
[libs]
ssl = "openssl"
```

### Any Git source

```toml
a = "github:owner/repo@v1.2.3"
b = "https://host/owner/repo.git@release/1"
c = "git@host:owner/repo.git@main"
d = "file:///local/path@commit-or-tag"
```

Tags, branches, slash-containing branches, and full commit SHAs all work.

### Lockfile

CPM writes every requested ref and its resolved immutable commit SHA to
`cpm.lock`. Subsequent installs reuse the exact commit — no surprise upgrades.
Commit `cpm.lock` to version control for fully reproducible builds.

### Atomic installs with rollback

`cpm add` and `cpm remove` update `cpm.toml` atomically. If any dependency
fails to build, the manifest is rolled back and the previous `.cpm/` environment
stays active. You never end up in a broken half-installed state.

### Incremental parallel compilation

Source files compile in parallel. Object files are keyed by compiler flags and
source content — unchanged files are never recompiled. `compile_commands.json`
is written automatically for clangd / IDE integration.

### Production bundles

`cpm build -s` produces a `dist/` folder with:

- Optimised binary (`-O3 -DNDEBUG`)
- All required shared libraries copied in
- Origin-relative rpath set via `patchelf`
- A portable `dist/run.sh` launcher

### Shell-safe process execution

All compiler and linker invocations use `posix_spawn` with explicit argument
arrays — never shell string interpolation. Paths with spaces, special characters,
or unusual names all work correctly.

### Full command reference

```
cpm init <name>          create a new project
cpm install              install all dependencies
cpm add [alias=]source   add a header dependency
cpm add --system …       add a compiled dependency
cpm add --lib …          add a Nix library
cpm remove <alias>       remove a dependency
cpm update               re-resolve moving refs (branches)
cpm list                 show installed dependencies
cpm build                compile the project
cpm build -s             optimised production bundle → dist/
cpm run                  install + build + run
cpm run file.cpp         compile and run a single file
cpm start                run via [scripts].start
cpm info                 show project info
cpm setup                install Nix on the machine
```

---

## Install

```sh
curl -fsSL https://cpm.rana718.dev/install.sh | sh
```

> Want Nix support for hermetic builds?
>
> ```sh
> curl -fsSL https://cpm.rana718.dev/install.sh | sh -s -- --with-nix
> ```

### Quick Start

```sh
mkdir hello && cd hello
cpm init hello
cpm add json=github:nlohmann/json@v3.11.3
cpm run
```

---

## Downloads

| Platform                                               | Archive            |
| ------------------------------------------------------ | ------------------ |
| Linux (universal — x86_64, aarch64, armv7, riscv64, …) | `cpm-linux.tar.gz` |

> Statically linked (musl libc) — no dependencies, runs on any Linux.

---

## Roadmap

CPM is in **beta**. It works well for real projects today, and we are actively
making it more mature and feature-loaded. Planned:

- Windows and macOS support
- Private Git repository authentication
- Dependency version conflict resolution
- Public package discovery website
- `cpm publish` to share reusable packages
- IDE plugin integration (VSCode, CLion)
- Reproducible CI mode with locked environments
- Support for more build systems and adapters

The goal is to be the package manager C++ deserved from the start — simple,
**isolated**, and powerful enough for production.

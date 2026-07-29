# CPM Architecture

## Components

```text
main.cpp
  PackageManager             CLI-facing facade
    Installer                lock resolution and transactional environments
      Downloader             Git cache and generic source builds
      NixEnv                 Nix shell/package resolution
      Resolver               namespaced header exports
    Builder                  parallel incremental compile/link/run/bundle
    TomlParser               validated manifest and atomic mutations
    Environment              project-local directory and activation script
    Process                  posix_spawn execution and output capture
```

`cpm_core` is a static library containing these components. The `cpm` executable only handles command dispatch. `cpm_core_tests` exercises the library directly.

## Install Transaction

1. Parse and validate `cpm.toml`.
2. Acquire `.cpm.install.lock`.
3. Reuse matching source/requested-ref records from `cpm.lock`; resolve misses to commit SHAs.
4. Create `.cpm-transaction-<pid>/.cpm`.
5. Download header packages concurrently and build compiled packages concurrently.
6. Resolve declared Nix libraries.
7. Export headers in the staging environment.
8. Write a staged `cpm.lock`.
9. Rename the existing `.cpm` to a backup and publish the staged directory.
10. Preserve compatible object caches, rewrite activation paths, publish the lock, and remove the backup.

An exception before publication deletes staging and releases the lock. Package caches are published with their own temporary-directory rename, so an interrupted clone/build is never treated as complete.

## Cache Identity

Git cache paths contain:

- validated package alias
- encoded resolved commit SHA
- FNV-1a source URL identity
- artifact kind (`-src` or `-built`)

The URL identity prevents two repositories using the same alias/ref from sharing cache data. Unsafe ref path characters are encoded and hashed instead of becoming path components.

## Source Build Contract

Compiled packages build into an explicit cache prefix. Adapters use argument vectors and set install prefixes through the build system's supported interface. CPM never invokes `sudo`, `make install` without a prefix, or a package-provided OS dependency installer.

Nix is attempted when available. If Nix exists but has no usable channel, CPM may use host build tools while retaining the isolated install prefix. CMake `find_package` names are normalized as Nix hints; `[build].nix_deps` is the authoritative override for names that do not map directly.

Build success requires both a successful selected adapter and at least one installed header or library artifact. Header-only fallback is successful only when headers are actually present.

## Header Model

Header packages remain separate in `.cpm/packages/<alias>`. Builder include paths point directly at repository roots, avoiding destructive flattening. Resolver additionally creates:

- `.cpm/include/<alias>` as a stable namespaced view
- non-conflicting compatibility aliases for conventional `include/` layouts

Compiled-package export collisions are errors. They are not silently overwritten.

## Project Build

Builder constructs one normalized argument model, then separates compile and link arguments. Translation units compile concurrently into:

```text
.cpm/objects/<flags-hash>/<source-name>-<source-path-hash>.o
```

An object is reused when its flags key matches and it is newer than its source and project headers. The final link is skipped when objects, output, and linked file artifacts are unchanged. Static archives are enclosed in a linker group.

Direct compiler and tool execution uses `posix_spawnp`. Shell execution is limited to explicit script surfaces (`[scripts].start`) and the command string required by `nix-shell --run`; generated arguments are single-quoted before entering that interface.

## Isolation Limits

CPM guarantees that normal package operations do not write system include/library prefixes. This is filesystem isolation, not a VM or container security boundary. Builds can execute upstream build scripts, and those scripts run with the user's permissions. Use trusted package sources and pin refs.

The default compiler links the host C/C++ runtime. Nix compiler/library selection improves reproducibility but does not make a Linux binary independent of kernel/libc ABI. `cpm setup` explicitly changes the host by installing Nix and is outside the normal project-local boundary.

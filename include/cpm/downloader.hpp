#pragma once

#include "cpm/toml_parser.hpp"

#include <filesystem>
#include <string>

namespace cpm {

// Both header-only (git clone) and compiled (build from source) packages.
class Downloader {
  public:
    Downloader(std::filesystem::path local_cpm_dir, std::filesystem::path global_cache_dir);

    // Header-only: git clone → cache, symlink into .cpm/packages/
    void clone_git_dependency(const GitDependency &dep);

    // Compiled: clone source → detect deps → build → install artifacts
    void resolve_system_dependency(const SystemDependency &dep, const std::filesystem::path &project_root);

    // Cache helpers
    [[nodiscard]] bool is_cached(const std::string &name, const std::string &version) const;
    [[nodiscard]] std::filesystem::path get_cache_path(const std::string &name, const std::string &version) const;
    void link_from_cache(const std::string &name, const std::string &version);

    // Resolve the latest git tag for a GitHub URL
    std::string resolve_latest_tag(const std::string &github_url, const std::string &name);

  private:
    std::filesystem::path local_cpm_dir_;
    std::filesystem::path global_cache_dir_;

    // Build a library from source using whichever build system it provides.
    // Tries: cooking.sh → configure.py → CMake → Makefile → Meson → Autotools → header-only
    bool build_from_source(const std::string &name, const std::filesystem::path &src_path, const std::filesystem::path &install_prefix, const std::filesystem::path &project_root);

    // Symlink/copy built artifacts (headers + libs) into local .cpm/
    void install_built_library(const std::string &name, const std::filesystem::path &built_path);

    // Parse CMakeLists.txt find_package() calls and recursively fetch/build them
    void resolve_transitive_deps(const std::filesystem::path &src_path, const std::filesystem::path &install_prefix);

    // Search GitHub API for a package by name, return clone URL
    std::string search_github_repo(const std::string &package_name);

    // Build stow from source if not on PATH (needed by cooking.sh)
    void ensure_build_tools(const std::filesystem::path &bin_dir);

    // Derive the nix compiler attribute name from a cpm.toml compiler string
    // e.g. "gcc-13" → "gcc13", "clang-17" → "clang_17"
    static std::string compiler_to_nix_attr(const std::string &compiler);
};

} // namespace cpm

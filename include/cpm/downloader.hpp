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
    [[nodiscard]] bool is_cached(const std::string &name, const std::string &version, const std::string &source = {}) const;
    [[nodiscard]] std::filesystem::path get_cache_path(const std::string &name, const std::string &version, const std::string &source = {}) const;
    [[nodiscard]] std::filesystem::path get_source_cache_path(const std::string &name, const std::string &version, const std::string &source = {}) const;
    [[nodiscard]] std::filesystem::path get_built_cache_path(const std::string &name, const std::string &version, const std::string &source = {}) const;
    void link_from_cache(const std::string &name, const std::string &version, const std::string &source = {});

    // Resolve the latest git tag for a GitHub URL
    std::string resolve_latest_tag(const std::string &github_url, const std::string &name);
    std::string resolve_git_ref(const std::string &git_url, const std::string &requested_ref, const std::string &name);

  private:
    std::filesystem::path local_cpm_dir_;
    std::filesystem::path global_cache_dir_;

    // Build a library from source using whichever build system it provides.
    // Tries: cooking.sh → configure.py → CMake → Makefile → Meson → Autotools → header-only
    bool build_from_source(const std::string &name, const std::filesystem::path &src_path, const std::filesystem::path &install_prefix, const std::filesystem::path &project_root);

    // Symlink/copy built artifacts (headers + libs) into local .cpm/
    void install_built_library(const std::string &name, const std::filesystem::path &built_path);

    static std::string cache_component(const std::string &value);
};

} // namespace cpm

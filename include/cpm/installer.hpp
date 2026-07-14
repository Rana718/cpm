#pragma once

#include "cpm/toml_parser.hpp"

#include <filesystem>
#include <string>

namespace cpm {

// Handles installing, removing, updating, and listing packages.
// Orchestrates Downloader + NixEnv + Resolver.
class Installer {
  public:
    Installer(std::filesystem::path project_root, std::filesystem::path local_cpm_dir, std::filesystem::path global_cache_dir);

    // Install all dependencies declared in cpm.toml
    void install();

    // Add a single package by spec (e.g. "github:user/repo@v1.0")
    void install_package(const std::string &package_spec);

    // Remove a named package
    void remove_package(const std::string &package_name);

    // Nuke all caches and re-install everything
    void update();

    // List installed packages to stdout
    void list() const;

  private:
    std::filesystem::path project_root_;
    std::filesystem::path local_cpm_dir_;
    std::filesystem::path global_cache_dir_;

    // Remove .cpm/ entries that are no longer in cpm.toml
    void auto_remove_stale_packages(const ProjectConfig &config);
    void auto_remove_stale_libs(const ProjectConfig &config);
    void auto_remove_stale_includes(const ProjectConfig &config);

    // Resolve [libs] entries via nix-build, symlink into .cpm/
    void resolve_nix_libraries(const ProjectConfig &config);

    void ensure_directories() const;
};

} // namespace cpm

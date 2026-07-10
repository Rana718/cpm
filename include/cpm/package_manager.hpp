#pragma once

#include "cpm/toml_parser.hpp"

#include <filesystem>
#include <string>

namespace cpm {

class PackageManager {
  public:
    PackageManager();

    // Commands
    void init(const std::string &project_name);
    void install();
    void install_package(const std::string &package_spec);
    void remove_package(const std::string &package_name);
    void update();
    void list();
    int build(bool static_build = false);
    int run();
    int run_file(const std::string& file);
    int start();
    void setup_environment();

    [[nodiscard]] std::string get_include_flags() const;
    [[nodiscard]] std::string get_library_flags() const;

  private:
    std::filesystem::path project_root_;
    std::filesystem::path local_cpm_dir_;
    std::filesystem::path global_cache_dir_;

    void ensure_directories();

    // Header-only packages
    void install_header_package(const GitDependency &dep);
    void clone_git_dependency(const GitDependency &dep);

    // Compiled packages (uses nix)
    void install_system_package(const SystemDependency &dep, const ProjectConfig &config);
    void resolve_system_dependency(const SystemDependency &dep);
    bool build_from_source(const std::string &name, const std::filesystem::path &src_path, const std::filesystem::path &install_prefix);
    void install_built_library(const std::string &name, const std::filesystem::path &built_path);
    void resolve_transitive_deps(const std::filesystem::path &src_path, const std::filesystem::path &install_prefix);
    std::string search_github_repo(const std::string &package_name);
    void ensure_build_tools(const std::filesystem::path &bin_dir);

    // Utilities
    std::string resolve_latest_tag(const std::string &github_url, const std::string &name);
    void export_package_headers();
    void generate_compile_commands();
    void auto_remove_stale(const ProjectConfig &config);
    void link_from_cache(const std::string &name, const std::string &version);
    [[nodiscard]] bool is_cached(const std::string &name, const std::string &version) const;
    [[nodiscard]] std::filesystem::path get_cache_path(const std::string &name, const std::string &version) const;

    // Build
    [[nodiscard]] std::string detect_compiler(const ProjectConfig &config) const;
    [[nodiscard]] std::string build_compile_command(const ProjectConfig &config) const;
    [[nodiscard]] std::filesystem::path get_output_path(const ProjectConfig &config) const;
    [[nodiscard]] std::filesystem::path find_shell_nix() const;
};

} // namespace cpm

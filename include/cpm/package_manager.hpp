#pragma once

#include "cpm/toml_parser.hpp"
#include <string>
#include <vector>
#include <filesystem>

namespace cpm {

class PackageManager {
public:
    PackageManager();

    // Core commands
    void init(const std::string& project_name);
    void install();
    void install_package(const std::string& package_spec);
    void remove_package(const std::string& package_name);
    void update();
    void list();

    // Build & Run
    int build();
    int run();
    int start();

    // Environment
    void setup_environment();
    std::string get_include_flags() const;
    std::string get_library_flags() const;

private:
    std::filesystem::path project_root_;
    std::filesystem::path local_cpm_dir_;
    std::filesystem::path global_cache_dir_;

    void ensure_directories();
    void clone_git_dependency(const GitDependency& dep);
    void resolve_system_dependency(const SystemDependency& dep);
    bool build_from_source(const std::string& name,
                           const std::filesystem::path& src_path,
                           const std::filesystem::path& install_prefix);
    void install_built_library(const std::string& name,
                               const std::filesystem::path& built_path);
    void resolve_transitive_deps(const std::filesystem::path& src_path,
                                  const std::filesystem::path& install_prefix);
    std::string search_github_repo(const std::string& package_name);
    void ensure_build_tools(const std::filesystem::path& bin_dir);
    void export_package_headers();
    void generate_compile_commands();
    void link_from_cache(const std::string& package_name, const std::string& version);
    bool is_cached(const std::string& package_name, const std::string& version) const;
    std::filesystem::path get_cache_path(const std::string& package_name, const std::string& version) const;

    std::string detect_compiler(const ProjectConfig& config) const;
    std::string build_compile_command(const ProjectConfig& config) const;
    std::filesystem::path get_output_path(const ProjectConfig& config) const;
    std::string resolve_latest_tag(const std::string& github_url, const std::string& name);
};

} // namespace cpm

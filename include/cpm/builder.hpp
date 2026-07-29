#pragma once

#include "cpm/toml_parser.hpp"

#include <filesystem>
#include <set>
#include <string>

namespace cpm {

// Compiles and runs C/C++ projects.
// Owns all logic for building compile commands, detecting sources, linking.
class Builder {
  public:
    Builder(std::filesystem::path project_root, std::filesystem::path local_cpm_dir, std::filesystem::path global_cache_dir);

    // Compile the project.  static_build=true → -O3, stripped, dist/ bundle.
    int build(bool static_build = false);

    // install (if needed) → build → run
    int run();

    // Compile and run a single .c / .cpp file
    int run_file(const std::string &file);

    // Run the already-built binary (or build it first if missing)
    int start();

  private:
    std::filesystem::path project_root_;
    std::filesystem::path local_cpm_dir_;
    std::filesystem::path global_cache_dir_;

    // Return the compiler binary to use for a given config
    [[nodiscard]] std::string detect_compiler(const ProjectConfig &config) const;

    // Return the compiler invocation without shell interpolation.
    [[nodiscard]] std::vector<std::string> build_compile_arguments(const ProjectConfig &config, bool optimized) const;

    // Where the output binary goes
    [[nodiscard]] std::filesystem::path get_output_path(const ProjectConfig &config) const;

    // Return all header directories under project_root (skip .cpm, .git, build …)
    [[nodiscard]] std::set<std::string> collect_include_dirs(const ProjectConfig &config) const;

    // Return all .cpp source files under project_root (same skip list)
    [[nodiscard]] std::vector<std::filesystem::path> collect_source_files(const ProjectConfig &config, const std::string &entry_abs) const;

    [[nodiscard]] std::filesystem::path create_project_shell(const ProjectConfig &config) const;

    int compile_incrementally(const ProjectConfig &config, const std::vector<std::string> &arguments, const std::filesystem::path &shell_nix, std::string &output) const;

    // Build the dist/ production bundle after a successful static build
    void bundle_production(const ProjectConfig &config) const;
};

} // namespace cpm

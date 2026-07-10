#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace cpm {

// Manages isolated build environment using .cpm directory
// instead of system paths, similar to virtualenv or node_modules
class Environment {
  public:
    Environment(const std::filesystem::path &project_root);

    // Create/activate environment
    void create();
    [[nodiscard]] bool exists() const;

    // Environment variables that override system paths
    [[nodiscard]] std::map<std::string, std::string> get_env_vars() const;

    // Paths
    [[nodiscard]] std::filesystem::path get_include_dir() const;
    [[nodiscard]] std::filesystem::path get_lib_dir() const;
    [[nodiscard]] std::filesystem::path get_bin_dir() const;
    [[nodiscard]] std::filesystem::path get_packages_dir() const;

    // Generate shell activation script
    [[nodiscard]] std::string generate_activation_script() const;

  private:
    std::filesystem::path project_root_;
    std::filesystem::path cpm_dir_;
};

} // namespace cpm

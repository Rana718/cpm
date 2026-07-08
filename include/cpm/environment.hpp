#pragma once

#include <string>
#include <vector>
#include <map>
#include <filesystem>

namespace cpm {

// Manages isolated build environment using .cpm directory
// instead of system paths, similar to virtualenv or node_modules
class Environment {
public:
    Environment(const std::filesystem::path& project_root);

    // Create/activate environment
    void create();
    bool exists() const;

    // Environment variables that override system paths
    std::map<std::string, std::string> get_env_vars() const;

    // Paths
    std::filesystem::path get_include_dir() const;
    std::filesystem::path get_lib_dir() const;
    std::filesystem::path get_bin_dir() const;
    std::filesystem::path get_packages_dir() const;

    // Generate shell activation script
    std::string generate_activation_script() const;

private:
    std::filesystem::path project_root_;
    std::filesystem::path cpm_dir_;
};

} // namespace cpm

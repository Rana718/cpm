#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace cpm {

struct GitDependency {
    std::string name;
    std::string github_url;
    std::string version;  // tag, branch, or "*" for latest
};

struct SystemDependency {
    std::string name;
    std::string github_url;
    std::string version;
    // Nix packages needed for this dep (auto-detected or user-specified)
    std::vector<std::string> nix_deps;
};

struct ProjectConfig {
    std::string name;
    std::string version;
    std::string description;
    std::string cpp_standard;  // "17", "20", "23"
    std::string compiler;      // "gcc", "gcc-13", "clang-17", or empty (auto from deps)

    std::string entry;
    std::string output;
    std::string start_script;

    std::vector<GitDependency> git_dependencies;
    std::vector<SystemDependency> system_dependencies;

    // Nix configuration
    std::vector<std::string> extra_nix_deps; // user can add extra nix packages
};

class TomlParser {
public:
    static ProjectConfig parse(const std::filesystem::path& toml_path);
    static void create_default(const std::filesystem::path& toml_path, const std::string& project_name);

private:
    static std::string trim(const std::string& str);
    static std::pair<std::string, std::string> parse_key_value(const std::string& line);
};

} // namespace cpm

#pragma once

#include <string>
#include <vector>
#include <map>
#include <filesystem>

namespace cpm {

struct GitDependency {
    std::string name;
    std::string github_url;
    std::string version;  // tag, branch, or "*" for latest
};

// System/compiled dependencies — downloaded from source, built inside .cpm/
// These produce .so/.a files in .cpm/lib/ and headers in .cpm/include/
struct SystemDependency {
    std::string name;
    std::string github_url;
    std::string version;
    // Build system auto-detected: cmake, make, meson
};

struct ProjectConfig {
    std::string name;
    std::string version;
    std::string description;
    std::string cpp_standard;  // "17", "20", "23"
    std::string compiler;      // "gcc", "clang", "gcc@13", "clang@17" or empty for auto

    // Build config
    std::string entry;         // entry source file (e.g., "main.cpp")
    std::string output;        // output binary name (defaults to project name)

    // Scripts
    std::string start_script;  // command to run after build (defaults to ./output)

    // Header-only dependencies (just clone + expose headers)
    std::vector<GitDependency> git_dependencies;

    // Compiled dependencies (clone + build + install into .cpm/)
    std::vector<SystemDependency> system_dependencies;
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

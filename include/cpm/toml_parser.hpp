#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace cpm {

struct GitDependency {
    std::string name;
    std::string github_url;
    std::string version; // tag, branch, or "*" for latest
};

struct SystemDependency {
    std::string name;
    std::string github_url;
    std::string version;
    std::vector<std::string> nix_deps;
};

// A system library resolved via nix (e.g. glew, libGL, SDL3)
// User just writes the nix package name; cpm links headers+libs into .cpm/
struct NixLibrary {
    std::string name;       // user-facing name (e.g. "glew")
    std::string nix_attr;   // nixpkgs attribute (e.g. "glew", "libGL")
};

struct ProjectConfig {
    std::string name;
    std::string version;
    std::string description;
    std::string cpp_standard;
    std::string compiler;

    std::string entry;
    std::string output;
    std::string start_script;

    std::vector<GitDependency> git_dependencies;
    std::vector<SystemDependency> system_dependencies;

    // [libs] — nix-resolved system libraries
    std::vector<NixLibrary> nix_libraries;

    // kept for advanced use
    std::vector<std::string> extra_nix_deps;
    std::vector<std::string> include_paths;
    std::vector<std::string> extra_sources;
};

class TomlParser {
  public:
    static ProjectConfig parse(const std::filesystem::path &toml_path);
    static void create_default(const std::filesystem::path &toml_path, const std::string &project_name);

  private:
    static std::string trim(const std::string &str);
    static std::pair<std::string, std::string> parse_key_value(const std::string &line);
};

} // namespace cpm

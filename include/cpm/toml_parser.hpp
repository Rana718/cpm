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
};

// A system library resolved via nix (e.g. glew, libGL, SDL3)
struct NixLibrary {
    std::string name;
    std::string nix_attr;
};

struct ProjectConfig {
    std::string name;
    std::string version;
    std::string description;
    std::string cpp_standard;
    std::string compiler;
    std::string nixpkgs; // pin nixpkgs channel (e.g. "nixos-24.05", "nixos-23.11")

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
    std::vector<std::string> exclude_sources;
    std::vector<std::string> compile_options;
    std::vector<std::string> link_options;
    std::vector<std::string> link_libraries;
    std::vector<std::string> defines;
};

class TomlParser {
  public:
    static ProjectConfig parse(const std::filesystem::path &toml_path);
    static void create_default(const std::filesystem::path &toml_path, const std::string &project_name);
    static GitDependency parse_git_dependency(const std::string &name, const std::string &spec);
    static void upsert_dependency(const std::filesystem::path &toml_path, const GitDependency &dependency, bool compiled = false);
    static void upsert_nix_library(const std::filesystem::path &toml_path, const NixLibrary &library);
    static bool remove_dependency(const std::filesystem::path &toml_path, const std::string &name);

  private:
    static std::string trim(const std::string &str);
    static std::pair<std::string, std::string> parse_key_value(const std::string &line);
};

} // namespace cpm

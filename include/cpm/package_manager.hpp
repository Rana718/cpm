#pragma once

#include <filesystem>
#include <string>

namespace cpm {

// Thin public facade — delegates to Installer, Builder, and Downloader.
// This is the only type that main.cpp touches.
class PackageManager {
  public:
    PackageManager();

    // Project setup
    void init(const std::string &project_name);

    // Dependency management (delegates to Installer)
    void install();
    void install_package(const std::string &package_spec, const std::string &kind = "header");
    void remove_package(const std::string &package_name);
    void update();
    void list() const;

    // Build / run (delegates to Builder)
    int build(bool static_build = false);
    int run();
    int run_file(const std::string &file);
    int start();

    // Compiler flag helpers (used by main.cpp info command)
    [[nodiscard]] std::string get_include_flags() const;
    [[nodiscard]] std::string get_library_flags() const;

  private:
    std::filesystem::path project_root_;
    std::filesystem::path local_cpm_dir_;
    std::filesystem::path global_cache_dir_;

    // Regenerate compile_commands.json for clangd
    void generate_compile_commands() const;
};

} // namespace cpm

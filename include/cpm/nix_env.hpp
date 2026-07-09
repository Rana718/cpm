#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace cpm {

// NixEnv manages isolated build environments via nix-shell.
// Every compiled dependency gets built in a nix environment
// that provides the exact compiler + libraries it needs.
//
// This handles version mismatches perfectly:
// - Library A needs gcc-13 → gets a nix-shell with gcc13
// - Library B needs gcc-16 → gets a nix-shell with gcc16  
// - Both install headers/libs into .cpm/ and user links them together
class NixEnv {
public:
    NixEnv(const std::filesystem::path& cpm_dir,
           const std::filesystem::path& global_cache);

    // Check if nix is installed
    bool available() const;

    // Generate shell.nix content for a dependency build
    // Detects what the library needs from its CMakeLists.txt/configure
    std::string generate_shell_nix(const std::string& compiler,
                                    const std::string& cpp_standard,
                                    const std::vector<std::string>& extra_deps = {});

    // Build a library inside nix-shell and install to prefix
    bool build_in_shell(const std::filesystem::path& src_path,
                        const std::filesystem::path& install_prefix,
                        const std::string& shell_nix_content);

    // Resolve nix package names from CMakeLists.txt find_package() calls
    std::vector<std::string> detect_nix_deps(const std::filesystem::path& src_path);

    // Detect the best compiler for a project by checking its requirements
    std::string detect_compiler_for_project(const std::filesystem::path& src_path);

    // Link nix store packages into .cpm/include and .cpm/lib
    void link_nix_packages(const std::vector<std::string>& packages,
                           const std::filesystem::path& include_dir,
                           const std::filesystem::path& lib_dir);

    // Run a command inside nix-shell (used by cpm build for linking)
    int run_in_shell(const std::string& cmd,
                     const std::filesystem::path& shell_nix_path);

    // Run a command and get output
    std::string run_cmd(const std::string& cmd);

private:
    std::filesystem::path cpm_dir_;
    std::filesystem::path global_cache_;

    // Map find_package names to nix package names
    std::string cmake_to_nix(const std::string& cmake_name);
};

} // namespace cpm

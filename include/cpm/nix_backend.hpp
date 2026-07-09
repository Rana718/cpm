#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace cpm {

// Nix backend — creates isolated build environments.
// Instead of fighting compiler/library version mismatches on the host,
// nix provides the EXACT environment needed (right gcc, right boost, everything).
// Like a lightweight container but without Docker.
class NixBackend {
public:
    NixBackend(const std::filesystem::path& cpm_dir);

    // Check if nix is available
    bool is_available() const;

    // Install nix
    bool install_nix();

    // Map github repo to nix package name
    std::string resolve_nix_name(const std::string& github_repo);

    // Create a shell.nix for a project with specific compiler + deps
    std::string create_shell_nix(const std::string& project_name,
                                  const std::string& compiler,
                                  const std::string& cpp_standard,
                                  const std::vector<std::string>& deps);

    // Build a project inside the nix environment
    bool build_in_env(const std::filesystem::path& project_dir,
                      const std::string& build_cmd,
                      const std::string& shell_nix_content);

    // Copy build results into .cpm/
    bool install_to_cpm(const std::filesystem::path& build_result,
                        const std::filesystem::path& include_dir,
                        const std::filesystem::path& lib_dir);

    // Fetch a single pre-built package (simple case)
    bool fetch_package(const std::string& nix_pkg_name,
                       const std::filesystem::path& include_dir,
                       const std::filesystem::path& lib_dir);

    // Fetch multiple packages
    bool fetch_packages(const std::vector<std::string>& nix_pkg_names,
                        const std::filesystem::path& include_dir,
                        const std::filesystem::path& lib_dir);

    // Get dependencies
    std::vector<std::string> get_dependencies(const std::string& nix_pkg_name);

private:
    std::filesystem::path cpm_dir_;
    std::filesystem::path nix_store_link_;

public:
    // These need to be accessible from package_manager
    std::string run_nix(const std::string& cmd);
    void copy_from_store(const std::filesystem::path& store_path,
                         const std::filesystem::path& include_dir,
                         const std::filesystem::path& lib_dir);
};

} // namespace cpm

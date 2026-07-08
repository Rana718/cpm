#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace cpm {

// Resolver exports package headers into .cpm/include/
// and generates compile_commands.json so clangd (used by Zed, VS Code, etc.)
// resolves all includes without errors.
class Resolver {
public:
    Resolver(const std::filesystem::path& project_root);

    // Export all package headers into .cpm/include/
    void export_headers();

    // Get the include path for the compiler
    std::string get_include_path() const;

private:
    std::filesystem::path project_root_;
    std::filesystem::path include_dir_;  // .cpm/include
    std::filesystem::path packages_dir_; // .cpm/packages

    std::filesystem::path find_include_root(const std::filesystem::path& package_dir) const;
    void export_package(const std::string& package_name, const std::filesystem::path& package_dir);
};

} // namespace cpm

#pragma once

#include <string>
#include <filesystem>

namespace cpm {

// Manages isolated compiler toolchains inside .cpm/toolchain/
// Downloads prebuilt GCC/Clang binaries so projects can pin exact compiler versions.
//
// Usage in cpm.toml:
//   compiler = "gcc@13"       # downloads GCC 13 into .cpm/toolchain/
//   compiler = "clang@17"     # downloads Clang 17 into .cpm/toolchain/
//   compiler = "gcc"          # uses system gcc (default)
//   compiler = ""             # auto-detect
class Toolchain {
public:
    Toolchain(const std::filesystem::path& cpm_dir);

    // Parse compiler spec like "gcc@13" → (gcc, 13)
    struct CompilerSpec {
        std::string type;     // "gcc" or "clang"
        std::string version;  // "13", "17", "" (system default)
        bool pinned;          // true if version was specified
    };

    static CompilerSpec parse_compiler(const std::string& spec);

    // Ensure the specified compiler is available
    // Downloads into .cpm/toolchain/ if needed
    // Returns the path to the compiler binary (g++ or clang++)
    std::string ensure_compiler(const CompilerSpec& spec);

    // Get CC and CXX paths for a given spec
    std::string get_cc(const CompilerSpec& spec);
    std::string get_cxx(const CompilerSpec& spec);

    // Get environment variables to use this toolchain
    std::string get_path_prefix();

private:
    std::filesystem::path cpm_dir_;
    std::filesystem::path toolchain_dir_;

    // Download prebuilt compiler
    bool download_gcc(const std::string& version);
    bool download_clang(const std::string& version);

    // Get download URL for the platform
    std::string get_gcc_url(const std::string& version);
    std::string get_clang_url(const std::string& version);
};

} // namespace cpm

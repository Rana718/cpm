#pragma once

#include <filesystem>
#include <string>

namespace cpm {

struct Config {
    // Paths
    static std::filesystem::path get_global_cache_dir();
    static std::filesystem::path get_local_cpm_dir();
    static std::filesystem::path get_toml_path();
    static std::filesystem::path get_resolve_header_path();

    // System info
    static std::string get_architecture();
    static std::string get_os();
    static std::string get_compiler();
    static std::string get_compiler_version();
    static std::string get_cpp_standard();
};

} // namespace cpm

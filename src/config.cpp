#include "cpm/config.hpp"
#include <cstdlib>
#include <array>
#include <memory>
#include <stdexcept>

#ifdef __linux__
#include <sys/utsname.h>
#endif

namespace cpm {

static std::string exec_command(const std::string& cmd) {
    std::array<char, 256> buffer;
    std::string result;
    auto pipe = std::unique_ptr<FILE, int(*)(FILE*)>(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

std::filesystem::path Config::get_global_cache_dir() {
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    if (!home) throw std::runtime_error("Cannot determine home directory");
    return std::filesystem::path(home) / ".cpm" / "cache";
}

std::filesystem::path Config::get_local_cpm_dir() {
    return std::filesystem::current_path() / ".cpm";
}

std::filesystem::path Config::get_toml_path() {
    return std::filesystem::current_path() / "cpm.toml";
}

std::filesystem::path Config::get_resolve_header_path() {
    return std::filesystem::current_path() / "resolve.h";
}

std::string Config::get_architecture() {
#ifdef __linux__
    struct utsname buf;
    if (uname(&buf) == 0) {
        return buf.machine;
    }
#endif
    return exec_command("uname -m");
}

std::string Config::get_os() {
#ifdef __linux__
    return "linux";
#elif defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown";
#endif
}

std::string Config::get_compiler() {
#if defined(__clang__)
    return "clang";
#elif defined(__GNUC__)
    return "gcc";
#elif defined(_MSC_VER)
    return "msvc";
#else
    if (std::system("g++ --version > /dev/null 2>&1") == 0) return "gcc";
    if (std::system("clang++ --version > /dev/null 2>&1") == 0) return "clang";
    return "unknown";
#endif
}

std::string Config::get_compiler_version() {
    std::string compiler = get_compiler();
    if (compiler == "gcc") {
        return exec_command("g++ -dumpversion");
    } else if (compiler == "clang") {
        return exec_command("clang++ --version 2>&1 | head -1 | grep -oP '\\d+\\.\\d+\\.\\d+'");
    }
    return "unknown";
}

std::string Config::get_cpp_standard() {
#if __cplusplus >= 202302L
    return "23";
#elif __cplusplus >= 202002L
    return "20";
#elif __cplusplus >= 201703L
    return "17";
#elif __cplusplus >= 201402L
    return "14";
#else
    return "11";
#endif
}

} // namespace cpm

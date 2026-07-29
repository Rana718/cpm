#include "cpm/config.hpp"

#include "cpm/process.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <string>
#include <sys/utsname.h>

namespace cpm {
namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    const auto start = value.find_first_not_of(" \t\r\n");
    return start == std::string::npos ? std::string{} : value.substr(start);
}

std::string detected_cxx() {
    if (Process::command_exists("c++")) return "c++";
    if (Process::command_exists("g++")) return "g++";
    if (Process::command_exists("clang++")) return "clang++";
    return {};
}

} // namespace

std::filesystem::path Config::get_global_cache_dir() {
    if (const char *configured = std::getenv("CPM_CACHE_DIR"); configured && *configured) return configured;
    if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) return std::filesystem::path(xdg) / "cpm";
    if (const char *home = std::getenv("HOME"); home && *home) return std::filesystem::path(home) / ".cache" / "cpm";
    throw std::runtime_error("cannot determine the cache directory; set CPM_CACHE_DIR");
}

std::filesystem::path Config::get_local_cpm_dir() { return std::filesystem::current_path() / ".cpm"; }
std::filesystem::path Config::get_toml_path() { return std::filesystem::current_path() / "cpm.toml"; }
std::filesystem::path Config::get_resolve_header_path() { return std::filesystem::current_path() / "resolve.h"; }

std::string Config::get_architecture() {
    struct utsname information{};
    return uname(&information) == 0 ? information.machine : "unknown";
}

std::string Config::get_os() {
    struct utsname information{};
    if (uname(&information) != 0) return "unknown";
    std::string os = information.sysname;
    std::ranges::transform(os, os.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return os;
}

std::string Config::get_compiler() {
    auto compiler = detected_cxx();
    if (compiler.empty()) return "unknown";
    auto output = Process::run({compiler, "--version"}, {}, {}, true).output;
    std::ranges::transform(output, output.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (output.find("clang") != std::string::npos) return "clang";
    if (output.find("gcc") != std::string::npos || output.find("g++") != std::string::npos) return "gcc";
    return compiler;
}

std::string Config::get_compiler_version() {
    const auto compiler = detected_cxx();
    if (compiler.empty()) return "unknown";
    if (get_compiler() == "gcc") {
        const auto result = Process::run({compiler, "-dumpfullversion", "-dumpversion"}, {}, {}, true);
        if (result.exit_code == 0 && !trim(result.output).empty()) return trim(result.output);
    }
    const auto result = Process::run({compiler, "--version"}, {}, {}, true);
    static const std::regex version(R"(([0-9]+(?:\.[0-9]+){1,2}))");
    std::smatch match;
    return std::regex_search(result.output, match, version) ? match[1].str() : "unknown";
}

std::string Config::get_cpp_standard() {
#if __cplusplus >= 202600L
    return "26";
#elif __cplusplus >= 202302L
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

#include "cpm/nix_env.hpp"

#include "cpm/process.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cpm {
namespace fs = std::filesystem;
namespace {

bool valid_attribute(const std::string &attribute) {
    return !attribute.empty() && std::ranges::all_of(attribute, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; });
}

std::string normalized_package(std::string package) {
    std::ranges::transform(package, package.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::ranges::replace(package, '_', '-');
    return package;
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    const auto start = value.find_first_not_of(" \t\r\n");
    return start == std::string::npos ? std::string{} : value.substr(start);
}

} // namespace

NixEnv::NixEnv(fs::path cpm_dir, fs::path global_cache) : cpm_dir_(std::move(cpm_dir)), global_cache_(std::move(global_cache)) {}

bool NixEnv::available() const { return Process::command_exists("nix-shell") && Process::command_exists("nix-build"); }

std::vector<std::string> NixEnv::detect_nix_deps(const fs::path &source) const {
    std::set<std::string> dependencies;
    if (fs::exists(source / "CMakeLists.txt")) {
        dependencies.insert("cmake");
        dependencies.insert("ninja");
    }
    if (fs::exists(source / "meson.build")) {
        dependencies.insert("meson");
        dependencies.insert("ninja");
    }
    if (fs::exists(source / "configure") || fs::exists(source / "configure.ac")) {
        dependencies.insert("autoconf");
        dependencies.insert("automake");
        dependencies.insert("libtool");
    }
    if (fs::exists(source / "configure.py")) {
        dependencies.insert("python3");
        dependencies.insert("ninja");
        dependencies.insert("gnumake");
    }
    if (fs::exists(source / "cooking.sh")) {
        dependencies.insert("cmake");
        dependencies.insert("ninja");
    }
    if (fs::exists(source / "Makefile") || fs::exists(source / "makefile")) dependencies.insert("gnumake");
    if (fs::exists(source / "package.json")) dependencies.insert("nodejs");
    dependencies.insert("pkg-config");

    static const std::set<std::string> cmake_builtins = {"cmakepackageconfighelpers", "ctest", "doxygen", "gnuinstalldirs", "gnustandarddirs", "linuxmembarrier", "pkgconfig", "pthreadsetname", "rt",
        "sanitizers", "sourcelocation", "stdatomic", "systemtapsdt", "threads"};
    static const std::map<std::string, std::string> nix_names = {{"boost", "boost"}, {"c-ares", "c-ares"}, {"cryptopp", "cryptopp"}, {"fmt", "fmt"}, {"gnutls", "gnutls"}, {"hwloc", "hwloc"},
        {"liburing", "liburing"}, {"libxml2", "libxml2"}, {"lksctp-tools", "lksctp-tools"}, {"lz4", "lz4"}, {"openssl", "openssl"}, {"protobuf", "protobuf"}, {"python", "python3"},
        {"python3", "python3"}, {"ragel", "ragel"}, {"ucontext", "libucontext"}, {"valgrind", "valgrind"}, {"xfsprogs", "xfsprogs"}, {"yaml-cpp", "yaml-cpp"}, {"zlib", "zlib"}};
    static const std::regex find_package(R"((?:find_package|find_dependency|[A-Za-z0-9_]+_find_dep)\s*\(\s*([A-Za-z0-9_.+-]+))", std::regex::icase);
    const auto add_cmake_package = [&](std::string package) {
        package = normalized_package(std::move(package));
        std::string compact = package;
        compact.erase(std::remove(compact.begin(), compact.end(), '-'), compact.end());
        if (cmake_builtins.contains(compact)) return;
        if (const auto known = nix_names.find(package); known != nix_names.end()) dependencies.insert(known->second);
        if (package == "gnutls") {
            for (const auto dependency : {"gmp", "libidn2", "libtasn1", "libunistring", "nettle", "p11-kit", "zlib"}) dependencies.insert(dependency);
        }
    };
    // Map of include path prefixes (the directory component after <) to nix packages.
    // Covers headers that are required unconditionally in source files but never
    // declared via find_package() in the project's CMake files.
    static const std::map<std::string, std::string> include_prefix_to_nix = {
        {"xfs/", "xfsprogs"},
        {"liburing", "liburing"},
        {"dpdk/", "dpdk"},
        {"numa", "numactl"},
        {"libaio", "libaio"},
        {"sctp", "lksctp-tools"},
    };
    try {
        for (const auto &entry : fs::recursive_directory_iterator(source, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            const auto &path = entry.path();
            const auto filename = path.filename().string();
            const auto ext = path.extension().string();
            // Scan CMake files for find_package() calls.
            if (filename == "CMakeLists.txt" || ext == ".cmake") {
                std::ifstream input(path);
                const std::string contents((std::istreambuf_iterator<char>(input)), {});
                for (std::sregex_iterator match(contents.begin(), contents.end(), find_package), end; match != end; ++match) {
                    add_cmake_package((*match)[1].str());
                }
                if (filename.starts_with("Find") && filename.ends_with(".cmake")) add_cmake_package(filename.substr(4, filename.size() - 10));
                continue;
            }
            // Scan C/C++ source and header files for #include directives that
            // reference system headers not advertised via find_package().
            if (ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx") {
                static const std::regex system_include(R"(^\s*#\s*include\s*<([^>]+)>)", std::regex::multiline);
                std::ifstream input(path);
                if (!input) continue;
                const std::string contents((std::istreambuf_iterator<char>(input)), {});
                for (std::sregex_iterator match(contents.begin(), contents.end(), system_include), end; match != end; ++match) {
                    const auto header = (*match)[1].str();
                    for (const auto &[prefix, nix_pkg] : include_prefix_to_nix) {
                        if (header.starts_with(prefix)) {
                            dependencies.insert(nix_pkg);
                            break;
                        }
                    }
                }
            }
        }
    } catch (const fs::filesystem_error &) {
    }
    return {dependencies.begin(), dependencies.end()};
}

std::string NixEnv::generate_shell_nix(const std::string &compiler, const std::string & /*cpp_standard*/, const std::vector<std::string> &extra_deps) const {
    if (!compiler.empty() && !valid_attribute(compiler)) throw std::runtime_error("invalid Nix compiler attribute");
    std::set<std::string> packages;
    if (!compiler.empty()) packages.insert(compiler);
    for (const auto &dependency : extra_deps) {
        if (!valid_attribute(dependency)) throw std::runtime_error("invalid Nix dependency attribute: " + dependency);
        packages.insert(dependency);
    }
    std::ostringstream expression;
    expression << "{ pkgs ? import <nixpkgs> {} }:\n"
               << "pkgs.mkShell {\n  packages = with pkgs; [\n";
    for (const auto &package : packages) expression << "    " << package << '\n';
    expression << "  ];\n}\n";
    return expression.str();
}

std::string NixEnv::build_package(const std::string &attribute, const std::string &nixpkgs_pin) const {
    if (!valid_attribute(attribute)) throw std::runtime_error("invalid Nix package attribute: " + attribute);
    fs::create_directories(cpm_dir_);
    ProcessResult result;
    if (nixpkgs_pin.empty()) {
        result = Process::run({"nix-build", "<nixpkgs>", "-A", attribute, "--no-out-link"}, {}, {}, true);
    } else {
        const auto expression_path = cpm_dir_ / "resolve-package.nix";
        std::ofstream expression(expression_path, std::ios::trunc);
        expression << "let pkgs = import (builtins.fetchTarball { url = "
                   << "\"https://github.com/NixOS/nixpkgs/archive/" << nixpkgs_pin << ".tar.gz\"; }) {}; in pkgs." << attribute << '\n';
        expression.close();
        result = Process::run({"nix-build", expression_path.string(), "--no-out-link"}, {}, {}, true);
    }
    if (result.exit_code != 0) return {};
    std::istringstream lines(result.output);
    std::string line;
    std::string last;
    while (std::getline(lines, line))
        if (!trim(line).empty()) last = trim(line);
    return last;
}

} // namespace cpm

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

// Read an entire file into a string efficiently.
std::string read_file(const fs::path &path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

NixEnv::NixEnv(fs::path cpm_dir, fs::path global_cache) : cpm_dir_(std::move(cpm_dir)), global_cache_(std::move(global_cache)) {}

bool NixEnv::available() const { return Process::command_exists("nix-shell") && Process::command_exists("nix-build"); }

std::vector<std::string> NixEnv::detect_nix_deps(const fs::path &source) const {
    std::set<std::string> dependencies;

    // Build-system tool detection from well-known manifest files.
    struct {
        const char *file;
        std::initializer_list<const char *> pkgs;
    } build_tools[] = {
        {"CMakeLists.txt", {"cmake", "ninja"}},
        {"meson.build", {"meson", "ninja"}},
        {"configure.py", {"python3", "ninja", "gnumake"}},
        {"cooking.sh", {"cmake", "ninja"}},
        {"Makefile", {"gnumake"}},
        {"makefile", {"gnumake"}},
        {"package.json", {"nodejs"}},
    };
    for (const auto &[file, pkgs] : build_tools) {
        if (fs::exists(source / file))
            for (const char *p : pkgs) dependencies.insert(p);
    }
    if (fs::exists(source / "configure") || fs::exists(source / "configure.ac")) {
        for (const char *p : {"autoconf", "automake", "libtool"}) dependencies.insert(p);
    }
    dependencies.insert("pkg-config");

    static const std::set<std::string> cmake_builtins = {"cmakepackageconfighelpers", "ctest", "doxygen", "gnuinstalldirs", "gnustandarddirs", "linuxmembarrier", "pkgconfig", "pthreadsetname", "rt",
        "sanitizers", "sourcelocation", "stdatomic", "systemtapsdt", "threads"};
    static const std::map<std::string, std::string> nix_names = {
        {"boost", "boost"},
        {"c-ares", "c-ares"},
        {"cryptopp", "cryptopp"},
        {"fmt", "fmt"},
        {"gnutls", "gnutls"},
        {"hwloc", "hwloc"},
        {"liburing", "liburing"},
        {"libxml2", "libxml2"},
        {"lksctp-tools", "lksctp-tools"},
        {"lz4", "lz4"},
        {"openssl", "openssl"},
        {"protobuf", "protobuf"},
        {"python", "python3"},
        {"python3", "python3"},
        {"ragel", "ragel"},
        {"ucontext", "libucontext"},
        {"valgrind", "valgrind"},
        {"xfsprogs", "xfsprogs"},
        {"yaml-cpp", "yaml-cpp"},
        {"zlib", "zlib"},
    };
    // Maps #include <prefix…> to the nix package supplying those headers.
    static const std::map<std::string, std::string> include_prefix_to_nix = {
        {"xfs/", "xfsprogs"},
        {"liburing", "liburing"},
        {"dpdk/", "dpdk"},
        {"numa", "numactl"},
        {"libaio", "libaio"},
        {"sctp", "lksctp-tools"},
    };

    static const std::regex re_find_package(R"((?:find_package|find_dependency|[A-Za-z0-9_]+_find_dep)\s*\(\s*([A-Za-z0-9_.+-]+))", std::regex::icase);
    static const std::regex re_system_include(R"(^\s*#\s*include\s*<([^>]+)>)", std::regex::multiline);

    const auto add_cmake_package = [&](std::string package) {
        package = normalized_package(std::move(package));
        std::string compact = package;
        compact.erase(std::ranges::remove(compact, '-').begin(), compact.end());
        if (cmake_builtins.contains(compact)) return;
        if (const auto it = nix_names.find(package); it != nix_names.end()) dependencies.insert(it->second);
        if (package == "gnutls") {
            for (const char *dep : {"gmp", "libidn2", "libtasn1", "libunistring", "nettle", "p11-kit", "zlib"}) dependencies.insert(dep);
        }
    };

    try {
        for (const auto &entry : fs::recursive_directory_iterator(source, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            const auto &path = entry.path();
            const auto fname = path.filename().string();
            const auto ext = path.extension().string();

            if (fname == "CMakeLists.txt" || ext == ".cmake") {
                const auto contents = read_file(path);
                for (std::sregex_iterator m(contents.begin(), contents.end(), re_find_package), end; m != end; ++m) add_cmake_package((*m)[1].str());
                if (fname.starts_with("Find") && fname.ends_with(".cmake")) add_cmake_package(fname.substr(4, fname.size() - 10));
                continue;
            }

            // Scan C/C++ files for bare system includes not declared via find_package.
            static constexpr std::array<std::string_view, 8> c_exts = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"};
            if (std::ranges::find(c_exts, ext) == c_exts.end()) continue;
            std::ifstream input(path);
            if (!input) continue;
            const std::string contents((std::istreambuf_iterator<char>(input)), {});
            for (std::sregex_iterator m(contents.begin(), contents.end(), re_system_include), end; m != end; ++m) {
                const auto header = (*m)[1].str();
                for (const auto &[prefix, pkg] : include_prefix_to_nix) {
                    if (header.starts_with(prefix)) {
                        dependencies.insert(pkg);
                        break;
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
    for (const auto &dep : extra_deps) {
        if (!valid_attribute(dep)) throw std::runtime_error("invalid Nix dependency attribute: " + dep);
        packages.insert(dep);
    }

    std::ostringstream expr;
    expr << "{ pkgs ? import <nixpkgs> {} }:\npkgs.mkShell {\n  packages = with pkgs; [\n";
    for (const auto &pkg : packages) expr << "    " << pkg << '\n';
    expr << "  ];\n}\n";
    return expr.str();
}

std::string NixEnv::build_package(const std::string &attribute, const std::string &nixpkgs_pin) const {
    if (!valid_attribute(attribute)) throw std::runtime_error("invalid Nix package attribute: " + attribute);
    fs::create_directories(cpm_dir_);

    ProcessResult result;
    if (nixpkgs_pin.empty()) {
        result = Process::run({"nix-build", "<nixpkgs>", "-A", attribute, "--no-out-link"}, {}, {}, true);
    } else {
        const auto expr_path = cpm_dir_ / "resolve-package.nix";
        std::ofstream expr(expr_path, std::ios::trunc);
        expr << "let pkgs = import (builtins.fetchTarball { url = "
             << "\"https://github.com/NixOS/nixpkgs/archive/" << nixpkgs_pin << ".tar.gz\"; }) {}; in pkgs." << attribute << '\n';
        expr.close();
        result = Process::run({"nix-build", expr_path.string(), "--no-out-link"}, {}, {}, true);
    }
    if (result.exit_code != 0) return {};

    std::string last;
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line))
        if (const auto t = trim(line); !t.empty()) last = t;
    return last;
}

} // namespace cpm

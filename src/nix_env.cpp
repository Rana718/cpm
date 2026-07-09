#include "cpm/nix_env.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <array>
#include <memory>
#include <algorithm>
#include <set>

namespace cpm {

NixEnv::NixEnv(const std::filesystem::path& cpm_dir,
               const std::filesystem::path& global_cache)
    : cpm_dir_(cpm_dir)
    , global_cache_(global_cache)
{}

bool NixEnv::available() const {
    return std::system("nix --version > /dev/null 2>&1") == 0;
}

std::string NixEnv::run_cmd(const std::string& cmd) {
    std::array<char, 4096> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

std::string NixEnv::cmake_to_nix(const std::string& cmake_name) {
    // Map CMake find_package names to nixpkgs attribute names
    std::string lower = cmake_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Known mappings
    if (lower == "boost") return "boost";
    if (lower == "fmt") return "fmt_10"; // use fmt 10 for broad compat
    if (lower == "yaml-cpp" || lower == "yamlcpp") return "yaml-cpp";
    if (lower == "lz4") return "lz4";
    if (lower == "gnutls") return "gnutls";
    if (lower == "gmp") return "gmp";
    if (lower == "nettle") return "nettle";
    if (lower == "c-ares" || lower == "cares") return "c-ares";
    if (lower == "protobuf") return "protobuf";
    if (lower == "hwloc") return "hwloc";
    if (lower == "ragel") return "ragel";
    if (lower == "liburing" || lower == "uring") return "liburing";
    if (lower == "numactl" || lower == "numa") return "numactl";
    if (lower == "openssl") return "openssl";
    if (lower == "zlib") return "zlib";
    if (lower == "libxml2" || lower == "xml2") return "libxml2";
    if (lower == "valgrind") return "valgrind";
    if (lower == "lksctp-tools" || lower == "sctp") return "lksctp-tools";
    if (lower == "dpdk") return "dpdk";
    if (lower == "libtasn1") return "libtasn1";
    if (lower == "p11-kit") return "p11-kit";
    if (lower == "xfsprogs") return "xfsprogs";
    if (lower == "libidn2") return "libidn2";
    if (lower == "hiredis") return "hiredis";

    // Default: try the lowercase name
    return lower;
}

std::vector<std::string> NixEnv::detect_nix_deps(const std::filesystem::path& src_path) {
    namespace fs = std::filesystem;
    std::set<std::string> deps;

    // Always need build tools
    deps.insert("cmake");
    deps.insert("ninja");
    deps.insert("pkg-config");
    deps.insert("automake");
    deps.insert("autoconf");
    deps.insert("libtool");

    // Parse CMakeLists.txt for find_package calls
    auto cmake_file = src_path / "CMakeLists.txt";
    if (fs::exists(cmake_file)) {
        std::ifstream f(cmake_file);
        std::string line;
        while (std::getline(f, line)) {
            auto pos = line.find("find_package");
            if (pos == std::string::npos) continue;
            auto paren = line.find('(', pos);
            if (paren == std::string::npos) continue;
            auto end_paren = line.find(')', paren);
            if (end_paren == std::string::npos) continue;
            std::istringstream iss(line.substr(paren + 1, end_paren - paren - 1));
            std::string pkg_name;
            iss >> pkg_name;
            if (pkg_name.empty()) continue;

            // Skip cmake internals
            static const std::set<std::string> skip = {
                "Threads", "PkgConfig", "GnuInstallDirs",
                "CMakePackageConfigHelpers", "CTest", "Python3", "Doxygen"
            };
            if (skip.count(pkg_name)) continue;

            std::string nix_name = cmake_to_nix(pkg_name);
            if (!nix_name.empty()) deps.insert(nix_name);
        }
    }

    // Also check cmake/ dir for Find*.cmake
    auto cmake_dir = src_path / "cmake";
    if (fs::exists(cmake_dir)) {
        for (const auto& entry : fs::directory_iterator(cmake_dir)) {
            auto fname = entry.path().filename().string();
            if (fname.find("Find") == 0 && fname.find(".cmake") != std::string::npos) {
                auto name = fname.substr(4, fname.size() - 10);
                std::string nix_name = cmake_to_nix(name);
                if (!nix_name.empty()) deps.insert(nix_name);
            }
        }
    }

    // If has cooking.sh, add common seastar deps
    if (fs::exists(src_path / "cooking.sh")) {
        deps.insert("stow");
        deps.insert("ragel");
        deps.insert("python3Packages.pyelftools");
        deps.insert("xfsprogs");
        deps.insert("libtasn1");
        deps.insert("libidn2");
        deps.insert("p11-kit");
    }

    return std::vector<std::string>(deps.begin(), deps.end());
}

std::string NixEnv::detect_compiler_for_project(const std::filesystem::path& src_path) {
    namespace fs = std::filesystem;

    // Check CMakeLists.txt for CXX_STANDARD hints
    auto cmake_file = src_path / "CMakeLists.txt";
    if (fs::exists(cmake_file)) {
        std::ifstream f(cmake_file);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        // If project uses C++23, needs GCC >= 13
        if (content.find("CXX_STANDARD 23") != std::string::npos ||
            content.find("c++23") != std::string::npos) {
            return "gcc13";
        }
    }

    // Default: gcc13 (good compat with most libraries)
    return "gcc13";
}

std::string NixEnv::generate_shell_nix(const std::string& compiler,
                                        const std::string& cpp_standard,
                                        const std::vector<std::string>& extra_deps) {
    std::ostringstream nix;
    nix << "{ pkgs ? import <nixpkgs> {} }:\n";
    nix << "pkgs.mkShell {\n";
    nix << "  buildInputs = with pkgs; [\n";

    // Compiler
    if (!compiler.empty()) {
        nix << "    " << compiler << "\n";
    } else {
        nix << "    gcc13\n";
    }

    // Extra deps
    for (const auto& dep : extra_deps) {
        nix << "    " << dep << "\n";
    }

    nix << "  ];\n";
    nix << "}\n";

    return nix.str();
}

bool NixEnv::build_in_shell(const std::filesystem::path& src_path,
                             const std::filesystem::path& install_prefix,
                             const std::string& shell_nix_content) {
    namespace fs = std::filesystem;

    // Write shell.nix
    auto shell_nix_path = src_path / "shell.nix";
    { std::ofstream f(shell_nix_path); f << shell_nix_content; }

    fs::create_directories(install_prefix / "include");
    fs::create_directories(install_prefix / "lib");

    int ret = -1;

    // Strategy 1: configure.py (Seastar-style)
    if (fs::exists(src_path / "configure.py")) {
        std::string cmd =
            "cd " + src_path.string() + " && nix-shell --run '"
            "export MAKEFLAGS=\"-j$(nproc)\" && "
            "export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc) && "
            "./configure.py --mode=release --prefix=" + install_prefix.string() +
            " && ninja -C build/release -j$(nproc)"
            " && ninja -C build/release install"
            "' 2>&1";
        ret = std::system(cmd.c_str());
    }

    // Strategy 2: CMake
    if (ret != 0 && fs::exists(src_path / "CMakeLists.txt")) {
        auto build_dir = src_path / "_cpm_build";
        fs::create_directories(build_dir);
        std::string cmd =
            "cd " + src_path.string() + " && nix-shell --run '"
            "cd _cpm_build && cmake .. -GNinja"
            " -DCMAKE_INSTALL_PREFIX=" + install_prefix.string() +
            " -DCMAKE_BUILD_TYPE=Release"
            " -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF"
            " -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF"
            " && cmake --build . --parallel"
            " && cmake --install ."
            "' 2>&1";
        ret = std::system(cmd.c_str());
        if (fs::exists(build_dir)) fs::remove_all(build_dir);
    }

    // Strategy 3: Make
    if (ret != 0 && (fs::exists(src_path / "Makefile") || fs::exists(src_path / "makefile"))) {
        std::string cmd =
            "cd " + src_path.string() + " && nix-shell --run '"
            "make -j$(nproc) && make install PREFIX=" + install_prefix.string() +
            "' 2>&1";
        ret = std::system(cmd.c_str());
    }

    // Always copy source headers if build didn't install them
    auto src_inc = src_path / "include";
    if (fs::exists(src_inc) && (ret != 0 || fs::is_empty(install_prefix / "include"))) {
        std::string cp = "cp -r " + src_inc.string() + "/* " +
                         (install_prefix / "include").string() + "/ 2>/dev/null";
        std::system(cp.c_str());
    }

    // Copy .a files from build directories
    if (fs::exists(src_path / "build")) {
        std::string find_libs = "find " + (src_path / "build").string() +
                                " -name '*.a' -not -path '*/_cooking/*' -exec cp {} " +
                                (install_prefix / "lib").string() + "/ \\; 2>/dev/null";
        std::system(find_libs.c_str());

        // Copy generated headers (e.g. seastar's ragel-generated parsers)
        std::string find_gen = "find " + (src_path / "build").string() +
                               " -path '*/gen/include/*' -type f \\( -name '*.hh' -o -name '*.h' \\)"
                               " -exec sh -c '"
                               "rel=$(echo {} | sed \"s|.*/gen/include/||\")"
                               " && mkdir -p " + (install_prefix / "include").string() + "/$(dirname $rel)"
                               " && cp {} " + (install_prefix / "include").string() + "/$rel"
                               "' \\; 2>/dev/null";
        std::system(find_gen.c_str());
    }

    return (ret == 0) || !fs::is_empty(install_prefix / "include");
}

void NixEnv::link_nix_packages(const std::vector<std::string>& packages,
                                const std::filesystem::path& include_dir,
                                const std::filesystem::path& lib_dir) {
    namespace fs = std::filesystem;
    fs::create_directories(include_dir);
    fs::create_directories(lib_dir);

    for (const auto& pkg : packages) {
        // Get nix store path (try .dev first for headers)
        std::string store_path = run_cmd(
            "nix-build '<nixpkgs>' -A " + pkg + ".dev --no-out-link 2>/dev/null");
        if (store_path.empty()) {
            store_path = run_cmd(
                "nix-build '<nixpkgs>' -A " + pkg + " --no-out-link 2>/dev/null");
        }
        if (store_path.empty()) continue;

        // Symlink headers
        auto pkg_inc = std::filesystem::path(store_path) / "include";
        if (fs::exists(pkg_inc)) {
            for (const auto& entry : fs::directory_iterator(pkg_inc)) {
                auto target = include_dir / entry.path().filename();
                if (!fs::exists(target) && !fs::is_symlink(target)) {
                    fs::create_symlink(entry.path(), target);
                }
            }
        }

        // Symlink libs
        auto pkg_lib = std::filesystem::path(store_path) / "lib";
        if (!fs::exists(pkg_lib)) {
            // Try non-dev output for libs
            std::string lib_path = run_cmd(
                "nix-build '<nixpkgs>' -A " + pkg + " --no-out-link 2>/dev/null");
            if (!lib_path.empty()) pkg_lib = std::filesystem::path(lib_path) / "lib";
        }
        if (fs::exists(pkg_lib)) {
            for (const auto& entry : fs::directory_iterator(pkg_lib)) {
                if (!entry.is_regular_file() && !entry.is_symlink()) continue;
                auto ext = entry.path().extension().string();
                if (ext == ".a" || ext == ".so") {
                    auto target = lib_dir / entry.path().filename();
                    if (!fs::exists(target) && !fs::is_symlink(target)) {
                        fs::create_symlink(entry.path(), target);
                    }
                }
            }
        }
    }
}

int NixEnv::run_in_shell(const std::string& cmd,
                          const std::filesystem::path& shell_nix_path) {
    std::string nix_cmd = "nix-shell " + shell_nix_path.string() +
                          " --run '" + cmd + "' 2>&1";
    return std::system(nix_cmd.c_str());
}

} // namespace cpm

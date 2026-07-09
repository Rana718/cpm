#include "cpm/nix_backend.hpp"
#include <iostream>
#include <array>
#include <memory>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace cpm {

NixBackend::NixBackend(const std::filesystem::path& cpm_dir)
    : cpm_dir_(cpm_dir)
    , nix_store_link_(cpm_dir / "nix-result")
{}

std::string NixBackend::run_nix(const std::string& cmd) {
    std::array<char, 4096> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

bool NixBackend::is_available() const {
    return std::system("nix --version > /dev/null 2>&1") == 0;
}

bool NixBackend::install_nix() {
    std::cout << "[cpm] Installing nix...\n";
    int ret = std::system("curl --proto '=https' --tlsv1.2 -sSf -L "
                          "https://install.determinate.systems/nix | sh -s -- install --no-confirm 2>&1");
    return (ret == 0);
}

std::string NixBackend::resolve_nix_name(const std::string& github_repo) {
    std::string repo_name;
    auto slash = github_repo.find('/');
    if (slash != std::string::npos) {
        repo_name = github_repo.substr(slash + 1);
    } else {
        repo_name = github_repo;
    }
    std::transform(repo_name.begin(), repo_name.end(), repo_name.begin(), ::tolower);
    return repo_name;
}

bool NixBackend::fetch_package(const std::string& nix_pkg_name,
                                const std::filesystem::path& include_dir,
                                const std::filesystem::path& lib_dir) {
    return fetch_packages({nix_pkg_name}, include_dir, lib_dir);
}

bool NixBackend::fetch_packages(const std::vector<std::string>& nix_pkg_names,
                                 const std::filesystem::path& include_dir,
                                 const std::filesystem::path& lib_dir) {
    // This method is no longer used directly — we use create_env + build_in_env instead
    return false;
}

void NixBackend::copy_from_store(const std::filesystem::path& store_path,
                                  const std::filesystem::path& include_dir,
                                  const std::filesystem::path& lib_dir) {
    namespace fs = std::filesystem;
    auto src_include = store_path / "include";
    if (fs::exists(src_include)) {
        for (const auto& entry : fs::directory_iterator(src_include)) {
            auto target = include_dir / entry.path().filename();
            if (fs::exists(target) || fs::is_symlink(target)) fs::remove_all(target);
            fs::create_symlink(entry.path(), target);
        }
    }
    auto src_lib = store_path / "lib";
    if (fs::exists(src_lib)) {
        for (const auto& entry : fs::directory_iterator(src_lib)) {
            if (!entry.is_regular_file() && !entry.is_symlink()) continue;
            auto ext = entry.path().extension().string();
            if (ext == ".a" || ext == ".so" || entry.path().filename().string().find(".so.") != std::string::npos) {
                auto target = lib_dir / entry.path().filename();
                if (fs::exists(target) || fs::is_symlink(target)) fs::remove_all(target);
                fs::create_symlink(entry.path(), target);
            }
        }
    }
}

std::vector<std::string> NixBackend::get_dependencies(const std::string&) {
    return {};
}

// ─── The real power: create a nix shell environment for building ───

std::string NixBackend::create_shell_nix(const std::string& project_name,
                                          const std::string& compiler,
                                          const std::string& cpp_standard,
                                          const std::vector<std::string>& deps) {
    // Generate a shell.nix that provides an isolated build environment
    // with the exact compiler and deps needed
    std::ostringstream nix;
    nix << "{ pkgs ? import <nixpkgs> {} }:\n\n";
    nix << "pkgs.mkShell {\n";
    nix << "  name = \"" << project_name << "-env\";\n";
    nix << "  buildInputs = with pkgs; [\n";

    // Compiler
    if (compiler.find("clang") != std::string::npos) {
        nix << "    clang\n";
        nix << "    llvmPackages.libcxx\n";
    } else {
        // Parse gcc version if specified (e.g., "gcc-13" → gcc13)
        std::string gcc_pkg = "gcc";
        auto dash = compiler.find('-');
        if (dash != std::string::npos) {
            std::string ver = compiler.substr(dash + 1);
            gcc_pkg = "gcc" + ver;
        }
        nix << "    " << gcc_pkg << "\n";
    }

    // Build tools
    nix << "    cmake\n";
    nix << "    ninja\n";
    nix << "    pkg-config\n";

    // Dependencies
    for (const auto& dep : deps) {
        nix << "    " << dep << "\n";
    }

    nix << "  ];\n\n";

    // Environment variables
    nix << "  shellHook = ''\n";
    nix << "    export CPM_NIX_ENV=1\n";
    nix << "  '';\n";
    nix << "}\n";

    return nix.str();
}

bool NixBackend::build_in_env(const std::filesystem::path& project_dir,
                               const std::string& build_cmd,
                               const std::string& shell_nix_content) {
    namespace fs = std::filesystem;

    // Write shell.nix
    auto shell_nix_path = project_dir / "shell.nix";
    {
        std::ofstream f(shell_nix_path);
        f << shell_nix_content;
    }

    // Run the build command inside the nix-shell
    std::string cmd = "cd " + project_dir.string() +
                      " && nix-shell --run '" + build_cmd + "' 2>&1";

    std::cout << "[cpm] Building in nix environment...\n";
    int ret = std::system(cmd.c_str());
    return (ret == 0);
}

bool NixBackend::install_to_cpm(const std::filesystem::path& build_result,
                                 const std::filesystem::path& include_dir,
                                 const std::filesystem::path& lib_dir) {
    namespace fs = std::filesystem;
    fs::create_directories(include_dir);
    fs::create_directories(lib_dir);

    // Copy include/
    if (fs::exists(build_result / "include")) {
        std::string cp = "cp -r " + (build_result / "include").string() + "/* " +
                         include_dir.string() + "/ 2>/dev/null";
        std::system(cp.c_str());
    }

    // Copy lib/
    if (fs::exists(build_result / "lib")) {
        std::string cp = "cp -r " + (build_result / "lib").string() + "/* " +
                         lib_dir.string() + "/ 2>/dev/null";
        std::system(cp.c_str());
    }

    return true;
}

} // namespace cpm

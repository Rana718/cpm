#include "cpm/downloader.hpp"

#include "cpm/nix_env.hpp"
#include "cpm/toml_parser.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpm {

// ─── Helpers ────────────────────────────────────────────────────────────────

namespace fs = std::filesystem;

static bool run_cmd_ok(const std::string &cmd) { return std::system(cmd.c_str()) == 0; }

static std::string capture(const std::string &cmd) {
    std::array<char, 512> buf;
    std::string out;
    auto pipe = std::unique_ptr<FILE, int (*)(FILE *)>(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return {};
    while (fgets(buf.data(), buf.size(), pipe.get())) out += buf.data();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

static void cp_r(const fs::path &src, const fs::path &dst) { std::system(("cp -r " + src.string() + "/* " + dst.string() + "/ 2>/dev/null").c_str()); }

static void cp_rn(const fs::path &src, const fs::path &dst) { std::system(("cp -rn " + src.string() + "/* " + dst.string() + "/ 2>/dev/null").c_str()); }

// ─── Constructor ────────────────────────────────────────────────────────────

Downloader::Downloader(fs::path local_cpm_dir, fs::path global_cache_dir) : local_cpm_dir_(std::move(local_cpm_dir)), global_cache_dir_(std::move(global_cache_dir)) {}

// ─── Cache helpers ──────────────────────────────────────────────────────────

bool Downloader::is_cached(const std::string &name, const std::string &version) const { return fs::exists(get_cache_path(name, version)); }

fs::path Downloader::get_cache_path(const std::string &name, const std::string &version) const { return global_cache_dir_ / (name + "-" + version); }

void Downloader::link_from_cache(const std::string &name, const std::string &version) {
    auto cache_path = get_cache_path(name, version);
    auto local_path = local_cpm_dir_ / "packages" / name;

    if (fs::exists(local_path) || fs::is_symlink(local_path)) fs::remove_all(local_path);

    fs::create_directory_symlink(cache_path, local_path);
}

// ─── Tag resolution ─────────────────────────────────────────────────────────

std::string Downloader::resolve_latest_tag(const std::string &github_url, const std::string &name) {
    std::string cmd = "git ls-remote --tags --sort=-v:refname " + github_url + R"( 2>/dev/null | grep -v '\^{}' | head -1 | sed 's/.*refs\/tags\///')";
    std::string tag = capture(cmd);

    if (!tag.empty()) {
        std::cout << "[cpm] " << name << " → latest: " << tag << "\n";
        return tag;
    }

    std::cout << "[cpm] " << name << " → no tags, using HEAD\n";
    return "HEAD";
}

// ─── Header-only clone ──────────────────────────────────────────────────────

void Downloader::clone_git_dependency(const GitDependency &dep) {
    std::string version = dep.version;
    if (version == "*" || version.empty()) version = resolve_latest_tag(dep.github_url, dep.name);

    if (is_cached(dep.name, version)) {
        std::cout << "[cpm] " << dep.name << " (cached)\n";
        link_from_cache(dep.name, version);
        return;
    }

    auto cache_path = get_cache_path(dep.name, version);
    fs::create_directories(cache_path);

    std::string clone_cmd = "git -c advice.detachedHead=false clone --depth 1 --quiet ";
    if (version != "HEAD") clone_cmd += "--branch " + version + " ";
    clone_cmd += dep.github_url + " " + cache_path.string() + " 2>&1";

    std::cout << "[cpm] " << dep.name << " (downloading...)\n";
    if (!run_cmd_ok(clone_cmd)) {
        fs::remove_all(cache_path);
        throw std::runtime_error("Failed to clone " + dep.github_url);
    }

    auto git_dir = cache_path / ".git";
    if (fs::exists(git_dir)) fs::remove_all(git_dir);

    link_from_cache(dep.name, version);
}

// ─── Compiled dependency ────────────────────────────────────────────────────

void Downloader::resolve_system_dependency(const SystemDependency &dep, const fs::path &project_root) {
    std::string version = dep.version;
    if (version == "*" || version.empty()) version = resolve_latest_tag(dep.github_url, dep.name);

    auto cache_key = dep.name + "-" + version + "-built";
    auto built_cache = global_cache_dir_ / cache_key;

    // Validate cache: must have non-empty include/ or lib/
    auto cache_valid = [&]() -> bool {
        if (!fs::exists(built_cache)) return false;
        bool ok = (fs::exists(built_cache / "include") && !fs::is_empty(built_cache / "include")) || (fs::exists(built_cache / "lib") && !fs::is_empty(built_cache / "lib"));
        return ok;
    };

    if (cache_valid()) {
        std::cout << "[cpm] " << dep.name << " (cached, pre-built)\n";
        install_built_library(dep.name, built_cache);
        return;
    }
    // Remove broken/empty cache entry
    if (fs::exists(built_cache)) fs::remove_all(built_cache);

    // Download source
    std::cout << "[cpm] " << dep.name << " (downloading source...)\n";
    auto src_path = global_cache_dir_ / (dep.name + "-" + version + "-src");

    if (!fs::exists(src_path)) {
        fs::create_directories(src_path);
        std::string clone_cmd = "git -c advice.detachedHead=false clone --depth 1 --quiet "
                                "--recurse-submodules --shallow-submodules --jobs=4 ";
        if (version != "HEAD") clone_cmd += "--branch " + version + " ";
        clone_cmd += dep.github_url + " " + src_path.string() + " 2>&1";

        if (!run_cmd_ok(clone_cmd)) {
            fs::remove_all(src_path);
            throw std::runtime_error("Failed to clone " + dep.github_url);
        }
        auto git_dir = src_path / ".git";
        if (fs::exists(git_dir)) fs::remove_all(git_dir);
    }

    std::cout << "[cpm] " << dep.name << " (building from source...)\n";
    fs::create_directories(built_cache);

    if (!build_from_source(dep.name, src_path, built_cache, project_root)) {
        fs::remove_all(built_cache);
        throw std::runtime_error("Failed to build " + dep.name);
    }

    install_built_library(dep.name, built_cache);
    std::cout << "[cpm] " << dep.name << " (built and installed)\n";
}

// ─── install_built_library ───────────────────────────────────────────────────

void Downloader::install_built_library(const std::string & /*name*/, const fs::path &built_path) {
    auto dst_include = local_cpm_dir_ / "include";
    auto dst_lib = local_cpm_dir_ / "lib";

    // Symlink headers
    auto src_include = built_path / "include";
    if (fs::exists(src_include)) {
        for (const auto &entry : fs::directory_iterator(src_include)) {
            auto target = dst_include / entry.path().filename();
            if (fs::exists(target) || fs::is_symlink(target)) fs::remove_all(target);
            fs::create_symlink(fs::absolute(entry.path()), target);
        }
    }

    // Copy .a / .so files
    auto copy_libs = [&](const fs::path &src_lib) {
        if (!fs::exists(src_lib)) return;
        for (const auto &entry : fs::recursive_directory_iterator(src_lib)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext == ".a" || ext == ".so" || entry.path().filename().string().find(".so.") != std::string::npos) {
                auto target = dst_lib / entry.path().filename();
                if (fs::exists(target)) fs::remove(target);
                fs::copy(entry.path(), target);
            }
        }
    };

    copy_libs(built_path / "lib");
    copy_libs(built_path / "lib64");

    // Preserve defines.txt (compile-time flags needed by this library)
    auto src_defines = built_path / "defines.txt";
    if (fs::exists(src_defines)) fs::copy(src_defines, local_cpm_dir_ / "defines.txt", fs::copy_options::overwrite_existing);
}

// ─── compiler_to_nix_attr ───────────────────────────────────────────────────

std::string Downloader::compiler_to_nix_attr(const std::string &compiler) {
    if (compiler.empty()) return "gcc13";
    if (compiler.find("clang") != std::string::npos) {
        auto dash = compiler.find('-');
        return dash != std::string::npos ? "clang_" + compiler.substr(dash + 1) : "clang";
    }
    auto dash = compiler.find('-');
    return dash != std::string::npos ? "gcc" + compiler.substr(dash + 1) : "gcc";
}

// ─── ensure_build_tools ──────────────────────────────────────────────────────

void Downloader::ensure_build_tools(const fs::path &bin_dir) {
    auto stow_path = bin_dir / "stow";
    if (run_cmd_ok("which stow > /dev/null 2>&1") || fs::exists(stow_path)) return;

    std::cout << "[cpm]   → building stow from source...\n";
    auto stow_src = global_cache_dir_ / "tool-stow-src";

    if (!fs::exists(stow_src)) {
        run_cmd_ok("git -c advice.detachedHead=false clone --depth 1 --quiet "
                   "--branch v2.4.1 https://github.com/aspiers/stow " +
                   stow_src.string() + " 2>/dev/null");
    }

    auto tool_prefix = bin_dir.parent_path() / "tools" / "stow";
    fs::create_directories(tool_prefix);

    std::string build_cmd = "cd " + stow_src.string() +
                            " && sed -i 's/-Werror//g' configure.ac 2>/dev/null"
                            " && autoreconf -iv > /dev/null 2>&1"
                            " && ./configure --prefix=" +
                            tool_prefix.string() +
                            " > /dev/null 2>&1"
                            " && make install > /dev/null 2>&1";

    if (run_cmd_ok(build_cmd) && fs::exists(tool_prefix / "bin" / "stow")) {
        if (fs::exists(stow_path) || fs::is_symlink(stow_path)) fs::remove(stow_path);
        fs::create_symlink(tool_prefix / "bin" / "stow", stow_path);
    } else {
        std::cerr << "[cpm] WARNING: Could not build stow. Some packages may fail.\n";
    }
}

// ─── search_github_repo ──────────────────────────────────────────────────────

std::string Downloader::search_github_repo(const std::string &package_name) {
    std::string cmd = "curl -s 'https://api.github.com/search/repositories?q=" + package_name +
                      "&sort=stars&per_page=1' 2>/dev/null"
                      " | grep -oP '\"html_url\": \"https://github.com/[^/]+/[^\"]*\"'"
                      " | head -1 | grep -oP 'https://github.com/[^\"]*'";
    return capture(cmd);
}

// ─── resolve_transitive_deps ─────────────────────────────────────────────────

void Downloader::resolve_transitive_deps(const fs::path &src_path, const fs::path &install_prefix) {
    static const std::set<std::string> skip_modules = {"Threads", "PkgConfig", "PthreadSetName", "LinuxMembarrier", "Sanitizers", "SourceLocation", "StdAtomic", "SystemTap-SDT", "Valgrind",
        "ucontext", "rt", "GnuInstallDirs", "CMakePackageConfigHelpers", "FindPkgConfig", "CheckCXXSourceCompiles", "CTest", "Python3", "Doxygen"};

    std::vector<std::string> needed;

    // Parse find_package() calls in CMakeLists.txt
    auto cmake_file = src_path / "CMakeLists.txt";
    if (fs::exists(cmake_file)) {
        std::ifstream f(cmake_file);
        std::string line;
        while (std::getline(f, line)) {
            auto pos = line.find("find_package");
            if (pos == std::string::npos) continue;
            auto paren = line.find('(', pos);
            if (paren == std::string::npos) continue;
            auto end = line.find(')', paren);
            if (end == std::string::npos) continue;
            std::istringstream iss(line.substr(paren + 1, end - paren - 1));
            std::string pkg;
            iss >> pkg;
            if (!pkg.empty() && !skip_modules.count(pkg)) needed.push_back(pkg);
        }
    }

    // Also collect from cmake/Find*.cmake
    auto cmake_dir = src_path / "cmake";
    if (fs::exists(cmake_dir)) {
        for (const auto &entry : fs::directory_iterator(cmake_dir)) {
            auto fname = entry.path().filename().string();
            if (fname.starts_with("Find") && fname.ends_with(".cmake")) needed.push_back(fname.substr(4, fname.size() - 10));
        }
    }

    if (needed.empty()) {
        std::cout << "[cpm]   → no transitive deps detected\n";
        return;
    }

    std::cout << "[cpm]   → detected " << needed.size() << " dependencies from CMakeLists.txt\n";

    for (const auto &pkg : needed) {
        // Check if already satisfied in prefix or on system
        std::string pkg_lower = pkg;
        std::ranges::transform(pkg_lower, pkg_lower.begin(), ::tolower);

        bool found = false;
        if (fs::exists(install_prefix / "include")) {
            for (const auto &e : fs::directory_iterator(install_prefix / "include")) {
                std::string n = e.path().filename().string();
                std::ranges::transform(n, n.begin(), ::tolower);
                if (n.find(pkg_lower) != std::string::npos) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) found = run_cmd_ok("pkg-config --exists " + pkg_lower + " 2>/dev/null");
        if (found) continue;

        std::string github_url = search_github_repo(pkg);
        if (github_url.empty()) {
            std::cout << "[cpm]     ◦ " << pkg << " (not found on GitHub, skipping)\n";
            continue;
        }

        std::cout << "[cpm]     ◦ " << pkg << " → " << github_url << "\n";
        std::string version = resolve_latest_tag(github_url, pkg);

        auto dep_cache = global_cache_dir_ / (pkg + "-" + version + "-built");
        if (!fs::exists(dep_cache)) {
            auto dep_src = global_cache_dir_ / (pkg + "-" + version + "-src");
            if (!fs::exists(dep_src)) {
                fs::create_directories(dep_src);
                std::string clone_cmd = "git -c advice.detachedHead=false clone --depth 1 --quiet "
                                        "--recurse-submodules ";
                if (version != "HEAD") clone_cmd += "--branch " + version + " ";
                clone_cmd += github_url + " " + dep_src.string() + " 2>/dev/null";
                if (!run_cmd_ok(clone_cmd)) {
                    fs::remove_all(dep_src);
                    std::cerr << "[cpm]     ✗ failed to download " << pkg << "\n";
                    continue;
                }
                auto git_dir = dep_src / ".git";
                if (fs::exists(git_dir)) fs::remove_all(git_dir);
            }

            fs::create_directories(dep_cache);
            // Recursive build — pass empty project_root (no cooking.sh nix context)
            if (!build_from_source(pkg, dep_src, dep_cache, {})) {
                fs::remove_all(dep_cache);
                std::cerr << "[cpm]     ✗ failed to build " << pkg << "\n";
                continue;
            }
        }

        // Merge into install_prefix (non-clobbering)
        if (fs::exists(dep_cache / "include")) cp_rn(dep_cache / "include", install_prefix / "include");
        if (fs::exists(dep_cache / "lib")) cp_rn(dep_cache / "lib", install_prefix / "lib");
    }
}

// ─── build_from_source ───────────────────────────────────────────────────────
// Tries 7 build strategies in order. Returns true if at least headers are present.

bool Downloader::build_from_source(const std::string &name, const fs::path &src_path, const fs::path &install_prefix, const fs::path &project_root) {
    auto build_dir = src_path / "_cpm_build";
    fs::create_directories(build_dir);
    fs::create_directories(install_prefix / "include");
    fs::create_directories(install_prefix / "lib");

    int ret = -1;

    // ── Pre-pass: resolve transitive deps if needed ───────────────────────
    if (!fs::exists(src_path / "cooking.sh") && fs::exists(src_path / "install-dependencies.sh")) {
        std::cout << "[cpm]   → resolving transitive dependencies...\n";
        resolve_transitive_deps(src_path, install_prefix);
    }

    // ── Strategy 1: cooking.sh (Seastar-style self-contained build) ───────
    if (fs::exists(src_path / "cooking.sh")) {
        std::cout << "[cpm]   → using cooking.sh\n";

        auto bin_dir = local_cpm_dir_ / "bin";
        fs::create_directories(bin_dir);

        NixEnv nix(local_cpm_dir_, global_cache_dir_);
        if (nix.available()) {
            std::cout << "[cpm]   → building inside nix environment\n";

            // Determine compiler from project cpm.toml if available
            std::string nix_compiler = "gcc13";
            if (!project_root.empty()) {
                auto toml_path = project_root / "cpm.toml";
                if (fs::exists(toml_path)) {
                    auto cfg = TomlParser::parse(toml_path);
                    if (!cfg.compiler.empty()) nix_compiler = compiler_to_nix_attr(cfg.compiler);
                }
            }

            // Build shell.nix from CMakeLists.txt analysis
            auto detected_deps = nix.detect_nix_deps(src_path);
            std::vector<std::string> nix_deps = {nix_compiler};
            nix_deps.insert(nix_deps.end(), detected_deps.begin(), detected_deps.end());

            std::string shell_nix = "{ pkgs ? import <nixpkgs> {} }:\npkgs.mkShell {\n"
                                    "  buildInputs = with pkgs; [\n";
            for (const auto &d : nix_deps) shell_nix += "    " + d + "\n";
            shell_nix += "  ];\n}\n";

            auto shell_nix_path = src_path / "shell.nix";
            {
                std::ofstream f(shell_nix_path);
                f << shell_nix;
            }

            // Try configure.py first, then cooking.sh
            std::string prefix = install_prefix.string();
            std::string build_cmd = "cd " + src_path.string() +
                                    " && nix-shell --run '"
                                    "export MAKEFLAGS=\"-j$(nproc)\" && "
                                    "export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc) && "
                                    "./configure.py --mode=release --prefix=" +
                                    prefix +
                                    " && ninja -C build/release -j$(nproc)"
                                    " && ninja -C build/release install' 2>&1";
            ret = std::system(build_cmd.c_str());

            if (ret != 0) {
                std::string cook_cmd = "cd " + src_path.string() +
                                       " && nix-shell --run '"
                                       "bash ./cooking.sh -t Release -g Ninja"
                                       " -s CC=gcc -s CXX=g++"
                                       " && ninja -C build -j$(nproc)' 2>&1";
                ret = std::system(cook_cmd.c_str());
            }

            // Copy project headers
            if (fs::exists(src_path / "include")) cp_r(src_path / "include", install_prefix / "include");

            // Copy built .a files
            if (fs::exists(src_path / "build")) {
                std::string find_libs = "find " + (src_path / "build").string() +
                                        " -name '*.a' -not -path '*/_cooking/*'"
                                        " -exec cp {} " +
                                        (install_prefix / "lib").string() + "/ \\; 2>/dev/null";
                std::system(find_libs.c_str());
            }

            // Symlink all detected nix dep headers/libs into install_prefix
            for (const auto &pkg : detected_deps) {
                std::string store = nix.run_cmd("nix-build '<nixpkgs>' -A " + pkg + ".dev --no-out-link 2>/dev/null");
                if (store.empty()) store = nix.run_cmd("nix-build '<nixpkgs>' -A " + pkg + " --no-out-link 2>/dev/null");
                if (store.empty()) continue;

                auto pkg_inc = fs::path(store) / "include";
                if (fs::exists(pkg_inc)) {
                    for (const auto &e : fs::directory_iterator(pkg_inc)) {
                        auto tgt = install_prefix / "include" / e.path().filename();
                        if (!fs::exists(tgt) && !fs::is_symlink(tgt)) fs::create_symlink(e.path(), tgt);
                    }
                }
                auto pkg_lib = fs::path(store) / "lib";
                if (fs::exists(pkg_lib)) {
                    for (const auto &e : fs::directory_iterator(pkg_lib)) {
                        if (!e.is_regular_file() && !e.is_symlink()) continue;
                        auto ext = e.path().extension().string();
                        if (ext == ".a" || ext == ".so") {
                            auto tgt = install_prefix / "lib" / e.path().filename();
                            if (!fs::exists(tgt) && !fs::is_symlink(tgt)) fs::create_symlink(e.path(), tgt);
                        }
                    }
                }
            }

            // Seastar-specific compile defines
            if (name == "seastar") {
                std::ofstream df(install_prefix / "defines.txt");
                df << "-DSEASTAR_SCHEDULING_GROUPS_COUNT=16\n"
                   << "-DSEASTAR_API_LEVEL=7\n"
                   << "-DSEASTAR_SSTRING\n"
                   << "-DSEASTAR_LOGGER_COMPILE_TIME_FMT\n";
            }

            if (ret == 0 || (fs::exists(install_prefix / "include") && !fs::is_empty(install_prefix / "include"))) {
                if (fs::exists(build_dir)) fs::remove_all(build_dir);
                return true;
            }
        }

        // Fallback: cooking.sh without nix
        ensure_build_tools(bin_dir);

        std::string compiler_env = "export MAKEFLAGS=\"-j$(nproc)\" && "
                                   "export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc) && ";

        std::string sys_cc = "gcc", sys_cxx = "g++";
        if (!project_root.empty()) {
            auto toml_path = project_root / "cpm.toml";
            if (fs::exists(toml_path)) {
                auto cfg = TomlParser::parse(toml_path);
                if (!cfg.compiler.empty()) {
                    sys_cc = cfg.compiler;
                    sys_cxx = cfg.compiler;
                    compiler_env += "export CC=\"" + sys_cc +
                                    "\" && "
                                    "export CXX=\"" +
                                    sys_cxx + "\" && ";
                }
            }
        }

        // Patch broken download URLs in cooking_recipe.cmake
        auto recipe = src_path / "cooking_recipe.cmake";
        if (fs::exists(recipe)) {
            std::system(("sed -i "
                         "'s|https://gmplib.org/download/gmp/|https://ftp.gnu.org/gnu/gmp/|g' " +
                         recipe.string() + " 2>/dev/null")
                    .c_str());
        }

        std::string cook_cmd = compiler_env + "export PATH=\"" + bin_dir.string() +
                               ":$PATH\" && "
                               "cd " +
                               src_path.string() +
                               " && bash ./cooking.sh -t Release -g Ninja"
                               " -s CC=" +
                               sys_cc + " -s CXX=" + sys_cxx + " -- -DCMAKE_C_COMPILER=" + sys_cc + " -DCMAKE_CXX_COMPILER=" + sys_cxx + " 2>&1";
        ret = std::system(cook_cmd.c_str());

        if (ret == 0) {
            // Find the ninja build dir
            fs::path ninja_dir;
            if (fs::exists(src_path / "build" / "release" / "build.ninja"))
                ninja_dir = src_path / "build" / "release";
            else if (fs::exists(src_path / "build" / "build.ninja"))
                ninja_dir = src_path / "build";

            if (!ninja_dir.empty()) {
                std::string build_cmd2 = compiler_env + "export PATH=\"" + bin_dir.string() +
                                         ":$PATH\" && "
                                         "cd " +
                                         src_path.string() + " && ninja -C " + ninja_dir.string() + " -j$(nproc) 2>&1";
                std::system(build_cmd2.c_str());
            }

            // Copy artifacts
            if (fs::exists(src_path / "include")) cp_r(src_path / "include", install_prefix / "include");

            std::system(("find " + (src_path / "build").string() +
                         " -name '*.a' -not -path '*/_cooking/*'"
                         " -exec cp {} " +
                         (install_prefix / "lib").string() + "/ \\; 2>/dev/null")
                    .c_str());

            // Copy cooked deps
            for (const auto &cook_base : {src_path / "build", src_path / "build" / "release"}) {
                auto cooked = cook_base / "_cooking" / "installed";
                if (fs::exists(cooked / "include")) cp_r(cooked / "include", install_prefix / "include");
                if (fs::exists(cooked / "lib")) cp_r(cooked / "lib", install_prefix / "lib");
            }
        }

        // Partial success: headers available even if ninja failed
        if (fs::exists(install_prefix / "include") && !fs::is_empty(install_prefix / "include")) {
            if (fs::exists(build_dir)) fs::remove_all(build_dir);
            return true;
        }

        // Last resort: copy headers directly from source tree
        if (fs::exists(src_path / "include") && !fs::is_empty(src_path / "include")) {
            cp_r(src_path / "include", install_prefix / "include");
            for (const auto &cook_base : {src_path / "build", src_path / "build" / "release"}) {
                auto cooked = cook_base / "_cooking" / "installed";
                if (fs::exists(cooked / "include")) cp_r(cooked / "include", install_prefix / "include");
                if (fs::exists(cooked / "lib")) cp_r(cooked / "lib", install_prefix / "lib");

                auto ingredients = cook_base / "_cooking" / "ingredient";
                if (fs::exists(ingredients)) {
                    for (const auto &ing : fs::directory_iterator(ingredients)) {
                        if (!ing.is_directory()) continue;
                        auto boost_hdrs = ing.path() / "src" / "boost";
                        if (fs::exists(boost_hdrs)) {
                            fs::create_directories(install_prefix / "include" / "boost");
                            cp_r(boost_hdrs, install_prefix / "include" / "boost");
                        }
                        auto ing_inc = ing.path() / "src" / "include";
                        if (fs::exists(ing_inc)) cp_r(ing_inc, install_prefix / "include");
                    }
                }
            }
            std::cout << "[cpm]   → headers copied from source (full build failed)\n";
            if (fs::exists(build_dir)) fs::remove_all(build_dir);
            return true;
        }

        std::cerr << "[cpm] cooking.sh failed. Trying other build systems...\n";
        ret = -1;
    }

    // ── Strategy 2: configure.py ──────────────────────────────────────────
    if (ret != 0 && fs::exists(src_path / "configure.py")) {
        std::cout << "[cpm]   → using configure.py\n";
        bool has_ninja = run_cmd_ok("which ninja > /dev/null 2>&1");
        std::string tool = has_ninja ? "ninja" : "make";
        std::string cfg_cmd = "cd " + src_path.string() + " && python3 ./configure.py --mode=release --prefix=" + install_prefix.string() + " > /dev/null 2>&1";
        if (run_cmd_ok(cfg_cmd)) {
            ret = std::system(("cd " + src_path.string() + " && " + tool +
                               " -C build/release -j$(nproc) > /dev/null 2>&1"
                               " && " +
                               tool + " -C build/release install > /dev/null 2>&1")
                    .c_str());
        }
    }

    // ── Strategy 3: CMake ─────────────────────────────────────────────────
    if (ret != 0 && fs::exists(src_path / "CMakeLists.txt")) {
        std::cout << "[cpm]   → using cmake\n";
        bool has_ninja = run_cmd_ok("which ninja > /dev/null 2>&1");
        std::string gen = has_ninja ? " -GNinja" : "";
        std::string cmake_cmd = "cd " + build_dir.string() + " && cmake " + src_path.string() + gen + " -DCMAKE_INSTALL_PREFIX=" + install_prefix.string() +
                                " -DCMAKE_PREFIX_PATH=" + install_prefix.string() +
                                " -DCMAKE_BUILD_TYPE=Release"
                                " -DCMAKE_POSITION_INDEPENDENT_CODE=ON"
                                " -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF"
                                " -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARKS=OFF"
                                " > /dev/null 2>&1"
                                " && cmake --build . --parallel > /dev/null 2>&1"
                                " && cmake --install . > /dev/null 2>&1";
        ret = std::system(cmake_cmd.c_str());
    }

    // ── Strategy 4: Makefile ──────────────────────────────────────────────
    if (ret != 0 && (fs::exists(src_path / "Makefile") || fs::exists(src_path / "makefile"))) {
        std::cout << "[cpm]   → using make\n";
        ret = std::system(("cd " + src_path.string() + " && make -j$(nproc) > /dev/null 2>&1").c_str());
        if (ret == 0) {
            if (!run_cmd_ok("cd " + src_path.string() + " && make install PREFIX=" + install_prefix.string() + " > /dev/null 2>&1")) {
                // Manual copy if no install target
                for (const auto &e : fs::directory_iterator(src_path)) {
                    if (e.is_regular_file() && e.path().extension() == ".a") fs::copy(e.path(), install_prefix / "lib" / e.path().filename(), fs::copy_options::overwrite_existing);
                }
                auto hdr_src = fs::exists(src_path / "include") ? src_path / "include" : src_path / "src";
                if (fs::exists(hdr_src)) {
                    for (const auto &e : fs::recursive_directory_iterator(hdr_src)) {
                        if (!e.is_regular_file()) continue;
                        auto ext = e.path().extension().string();
                        if (ext == ".h" || ext == ".hpp" || ext == ".hxx") {
                            auto rel = fs::relative(e.path(), hdr_src);
                            auto dest = install_prefix / "include" / rel;
                            fs::create_directories(dest.parent_path());
                            fs::copy(e.path(), dest, fs::copy_options::overwrite_existing);
                        }
                    }
                }
            }
        }
    }

    // ── Strategy 5: Meson ─────────────────────────────────────────────────
    if (ret != 0 && fs::exists(src_path / "meson.build")) {
        std::cout << "[cpm]   → using meson\n";
        ret = std::system(("meson setup " + build_dir.string() + " " + src_path.string() + " --prefix=" + install_prefix.string() +
                           " --default-library=static > /dev/null 2>&1"
                           " && meson compile -C " +
                           build_dir.string() +
                           " > /dev/null 2>&1"
                           " && meson install -C " +
                           build_dir.string() + " > /dev/null 2>&1")
                .c_str());
    }

    // ── Strategy 6: Autotools ─────────────────────────────────────────────
    if (ret != 0 && fs::exists(src_path / "configure")) {
        std::cout << "[cpm]   → using autotools\n";
        ret = std::system(("cd " + src_path.string() + " && ./configure --prefix=" + install_prefix.string() +
                           " --enable-static --disable-shared > /dev/null 2>&1"
                           " && make -j$(nproc) > /dev/null 2>&1"
                           " && make install > /dev/null 2>&1")
                .c_str());
    }

    // ── Strategy 7: Header-only fallback ─────────────────────────────────
    if (ret != 0) {
        bool has_headers = false;
        for (const auto &dir : {src_path / "include", src_path / "src", src_path}) {
            if (!fs::exists(dir)) continue;
            for (const auto &e : fs::recursive_directory_iterator(dir)) {
                if (!e.is_regular_file()) continue;
                auto ext = e.path().extension().string();
                if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh") {
                    has_headers = true;
                    break;
                }
            }
            if (has_headers) break;
        }

        if (has_headers) {
            std::cout << "[cpm]   → header-only (no build needed)\n";
            auto hdr_src = fs::exists(src_path / "include") ? src_path / "include" : fs::exists(src_path / "src") ? src_path / "src" : src_path;
            for (const auto &e : fs::recursive_directory_iterator(hdr_src)) {
                if (!e.is_regular_file()) continue;
                auto ext = e.path().extension().string();
                if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh") {
                    auto rel = fs::relative(e.path(), hdr_src);
                    auto dest = install_prefix / "include" / rel;
                    fs::create_directories(dest.parent_path());
                    fs::copy(e.path(), dest, fs::copy_options::overwrite_existing);
                }
            }
            ret = 0;
        } else {
            std::cerr << "[cpm] ERROR: Cannot detect build system for " << name
                      << "\n[cpm]        Tried: cooking.sh, configure.py, cmake, "
                         "make, meson, autotools\n";
            if (fs::exists(build_dir)) fs::remove_all(build_dir);
            return false;
        }
    }

    if (fs::exists(build_dir)) fs::remove_all(build_dir);
    return (ret == 0);
}

} // namespace cpm

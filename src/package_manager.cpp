#include "cpm/package_manager.hpp"

#include "cpm/config.hpp"
#include "cpm/environment.hpp"
#include "cpm/nix_env.hpp"
#include "cpm/progress.hpp"
#include "cpm/resolver.hpp"
#include "cpm/toml_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <sys/wait.h>
#include <cstdlib>
#include <cstdio>
#include <sys/wait.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpm {

PackageManager::PackageManager() : project_root_(std::filesystem::current_path()), local_cpm_dir_(project_root_ / ".cpm"), global_cache_dir_(Config::get_global_cache_dir()) {}

void PackageManager::init(const std::string &project_name) {
    std::cout << "[cpm] Initializing project '" << project_name << "'...\n";

    std::cout << "[cpm] Architecture: " << Config::get_architecture() << "\n";
    std::cout << "[cpm] OS: " << Config::get_os() << "\n";
    std::cout << "[cpm] Compiler: " << Config::get_compiler() << " " << Config::get_compiler_version() << "\n";

    auto toml_path = project_root_ / "cpm.toml";
    if (!std::filesystem::exists(toml_path)) {
        TomlParser::create_default(toml_path, project_name);
        std::cout << "[cpm] Created cpm.toml\n";
    } else {
        std::cout << "[cpm] cpm.toml already exists, skipping.\n";
    }

    Environment env(project_root_);
    env.create();

    std::filesystem::create_directories(global_cache_dir_);

    auto main_file = project_root_ / "main.cpp";
    if (!std::filesystem::exists(main_file)) {
        std::ofstream f(main_file);
        f << "#include <iostream>\n\n";
        f << "int main() {\n";
        f << "    std::cout << \"Hello from " << project_name << "!\" << std::endl;\n";
        f << "    return 0;\n";
        f << "}\n";
        f.close();
        std::cout << "[cpm] Created main.cpp\n";
    }

    generate_compile_commands();

    std::cout << "\n[cpm] Done! You can now:\n";
    std::cout << "  cpm run       # install + build + run\n";
    std::cout << "  cpm build     # compile the project\n";
    std::cout << "  cpm start     # run the built binary\n";
}

void PackageManager::ensure_directories() {
    std::filesystem::create_directories(local_cpm_dir_ / "packages");
    std::filesystem::create_directories(local_cpm_dir_ / "include");
    std::filesystem::create_directories(local_cpm_dir_ / "lib");
    std::filesystem::create_directories(local_cpm_dir_ / "bin");
    std::filesystem::create_directories(global_cache_dir_);
}

bool PackageManager::is_cached(const std::string &package_name, const std::string &version) const { return std::filesystem::exists(get_cache_path(package_name, version)); }

std::filesystem::path PackageManager::get_cache_path(const std::string &package_name, const std::string &version) const { return global_cache_dir_ / (package_name + "-" + version); }

void PackageManager::link_from_cache(const std::string &package_name, const std::string &version) {
    auto cache_path = get_cache_path(package_name, version);
    auto local_path = local_cpm_dir_ / "packages" / package_name;

    if (std::filesystem::exists(local_path) || std::filesystem::is_symlink(local_path)) {
        std::filesystem::remove_all(local_path);
    }

    std::filesystem::create_directory_symlink(cache_path, local_path);
}

std::string PackageManager::resolve_latest_tag(const std::string &github_url, const std::string &name) {
    // Get all tags sorted by version (descending), filter out ^{} refs
    std::string tag_cmd = "git ls-remote --tags --sort=-v:refname " + github_url + R"( 2>/dev/null | grep -v '\^{}' | head -1 | sed 's/.*refs\/tags\///')";
    std::array<char, 256> buffer;
    std::string latest_tag;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(tag_cmd.c_str(), "r"), pclose);
    if (pipe) {
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            latest_tag += buffer.data();
        }
    }
    // Trim whitespace
    while (!latest_tag.empty() && (latest_tag.back() == '\n' || latest_tag.back() == '\r' || latest_tag.back() == ' ')) {
        latest_tag.pop_back();
    }

    if (!latest_tag.empty()) {
        std::cout << "[cpm] " << name << " → latest: " << latest_tag << "\n";
        return latest_tag;
    }

    // No tags found — use default branch
    std::cout << "[cpm] " << name << " → no tags, using HEAD\n";
    return "HEAD";
}

void PackageManager::clone_git_dependency(const GitDependency &dep) {
    std::string version = dep.version;

    // "*" or empty means latest release tag
    if (version == "*" || version.empty()) {
        version = resolve_latest_tag(dep.github_url, dep.name);
    }

    if (is_cached(dep.name, version)) {
        std::cout << "[cpm] " << dep.name << " (cached)\n";
        link_from_cache(dep.name, version);
        return;
    }

    auto cache_path = get_cache_path(dep.name, version);
    std::filesystem::create_directories(cache_path);

    std::string clone_cmd = "git -c advice.detachedHead=false clone --depth 1 --quiet ";
    if (version != "HEAD") {
        clone_cmd += "--branch " + version + " ";
    }
    clone_cmd += dep.github_url + " " + cache_path.string() + " 2>&1";

    std::cout << "[cpm] " << dep.name << " (downloading...)\n";
    int ret = std::system(clone_cmd.c_str());
    if (ret != 0) {
        std::filesystem::remove_all(cache_path);
        throw std::runtime_error("Failed to clone " + dep.github_url);
    }

    auto git_dir = cache_path / ".git";
    if (std::filesystem::exists(git_dir)) {
        std::filesystem::remove_all(git_dir);
    }

    link_from_cache(dep.name, version);
}

void PackageManager::resolve_system_dependency(const SystemDependency &dep) {
    namespace fs = std::filesystem;

    std::string version = dep.version;

    // "*" or empty means latest release tag
    if (version == "*" || version.empty()) {
        version = resolve_latest_tag(dep.github_url, dep.name);
    }

    // Check if already built in global cache
    auto cache_key = dep.name + "-" + version + "-built";
    auto built_cache = global_cache_dir_ / cache_key;

    // Check cache — but verify it actually has content (not a failed empty build)
    if (fs::exists(built_cache) && !fs::is_empty(built_cache)) {
        bool has_content = false;
        if (fs::exists(built_cache / "include") && !fs::is_empty(built_cache / "include")) has_content = true;
        if (fs::exists(built_cache / "lib") && !fs::is_empty(built_cache / "lib")) has_content = true;

        if (has_content) {
            std::cout << "[cpm] " << dep.name << " (cached, pre-built)\n";
            install_built_library(dep.name, built_cache);
            return;
        } else {
            // Empty/broken cache — remove and rebuild
            fs::remove_all(built_cache);
        }
    } else if (fs::exists(built_cache)) {
        fs::remove_all(built_cache);
    }

    // Download source
    std::cout << "[cpm] " << dep.name << " (downloading source...)\n";

    auto src_path = global_cache_dir_ / (dep.name + "-" + version + "-src");
    if (!fs::exists(src_path)) {
        fs::create_directories(src_path);

        // Use --jobs for parallel submodule fetch, --shallow-submodules to save
        // time
        std::string clone_cmd = "git -c advice.detachedHead=false clone --depth 1 --quiet "
                                "--recurse-submodules --shallow-submodules --jobs=4 ";
        if (version != "HEAD") {
            clone_cmd += "--branch " + version + " ";
        }
        clone_cmd += dep.github_url + " " + src_path.string() + " 2>&1";

        int ret = std::system(clone_cmd.c_str());
        if (ret != 0) {
            fs::remove_all(src_path);
            throw std::runtime_error("Failed to clone " + dep.github_url);
        }

        // Remove .git
        auto git_dir = src_path / ".git";
        if (fs::exists(git_dir)) fs::remove_all(git_dir);
    }

    // Build from source into the built cache
    std::cout << "[cpm] " << dep.name << " (building from source...)\n";
    fs::create_directories(built_cache);

    bool build_ok = build_from_source(dep.name, src_path, built_cache);
    if (!build_ok) {
        fs::remove_all(built_cache);
        throw std::runtime_error("Failed to build " + dep.name);
    }

    // Install built artifacts to local .cpm/
    install_built_library(dep.name, built_cache);
    std::cout << "[cpm] " << dep.name << " (built and installed)\n";
}

bool PackageManager::build_from_source(const std::string &name, const std::filesystem::path &src_path, const std::filesystem::path &install_prefix) {
    namespace fs = std::filesystem;

    auto build_dir = src_path / "_cpm_build";
    fs::create_directories(build_dir);
    fs::create_directories(install_prefix / "include");
    fs::create_directories(install_prefix / "lib");

    int ret = -1;

    // ─── Strategy 1: If project has cooking.sh, skip transitive deps ───
    // cooking.sh already downloads ALL dependencies from source internally.
    // It never touches the system. We just need to ensure build tools exist.
    if (fs::exists(src_path / "cooking.sh")) {
        // Don't bother with transitive dep resolution — cooking.sh handles it
        // Just go straight to cooking.sh strategy
    } else if (fs::exists(src_path / "install-dependencies.sh")) {
        // Has install-dependencies.sh but NO cooking.sh
        // Parse CMakeLists.txt and try to resolve deps
        std::cout << "[cpm]   → resolving transitive dependencies...\n";
        resolve_transitive_deps(src_path, install_prefix);
    }

    // ─── Strategy 2: cooking.sh (self-contained build) ───
    if (fs::exists(src_path / "cooking.sh")) {
        std::cout << "[cpm]   → using cooking.sh\n";

        auto bin_dir = local_cpm_dir_ / "bin";
        fs::create_directories(bin_dir);

        // If nix is available, run cooking.sh inside nix-shell
        // nix provides ALL deps (boost, gmp, gnutls, etc.) — no need to download/build them
        NixEnv nix(local_cpm_dir_, global_cache_dir_);
        if (nix.available()) {
            std::cout << "[cpm]   → building inside nix environment (all deps provided)\n";

            // Get compiler from toml (default gcc13 for compatibility)
            std::string nix_compiler = "gcc13";
            auto toml_path_nix = project_root_ / "cpm.toml";
            if (fs::exists(toml_path_nix)) {
                auto cfg = TomlParser::parse(toml_path_nix);
                if (!cfg.compiler.empty()) {
                    // Parse compiler: "gcc-13" → gcc13, "clang-17" → clang_17
                    std::string comp = cfg.compiler;
                    if (comp.find("clang") != std::string::npos) {
                        auto dash = comp.find('-');
                        nix_compiler = dash != std::string::npos ? "clang_" + comp.substr(dash + 1) : "clang";
                    } else {
                        auto dash = comp.find('-');
                        nix_compiler = dash != std::string::npos ? "gcc" + comp.substr(dash + 1) : "gcc";
                    }
                    // Parse compiler: "gcc-13" → gcc13, "clang-17" → clang_17
                }
            }

            // Detect nix packages from the project's CMakeLists.txt
            NixEnv nix_detector(local_cpm_dir_, global_cache_dir_);
            auto detected_deps = nix_detector.detect_nix_deps(src_path);
            std::vector<std::string> nix_deps = {nix_compiler};
            nix_deps.insert(nix_deps.end(), detected_deps.begin(), detected_deps.end());

            // Generate shell.nix
            std::string shell_nix_content = "{ pkgs ? import <nixpkgs> {} }:\n"
                                            "pkgs.mkShell {\n"
                                            "  buildInputs = with pkgs; [\n";
            for (const auto &dep_name : nix_deps) {
                shell_nix_content += "    " + dep_name + "\n";
            }
            shell_nix_content += "  ];\n}\n";

            auto shell_nix_path = src_path / "shell.nix";
            {
                std::ofstream f(shell_nix_path);
                f << shell_nix_content;
            }

            // Build inside nix-shell using configure.py (preferred) or cooking.sh
            std::string build_cmd = "cd " + src_path.string() +
                                    " && nix-shell --run '"
                                    "export MAKEFLAGS=\"-j$(nproc)\" && "
                                    "export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc) && "
                                    "./configure.py --mode=release --prefix=" +
                                    install_prefix.string() +
                                    " && "
                                    "ninja -C build/release -j$(nproc) && "
                                    "ninja -C build/release install"
                                    "' 2>&1";

            ret = std::system(build_cmd.c_str());

            if (ret != 0) {
                // Fallback: try cooking.sh inside nix-shell
                std::string cook_cmd = "cd " + src_path.string() +
                                       " && nix-shell --run '"
                                       "bash ./cooking.sh -t Release -g Ninja -s CC=gcc -s CXX=g++ && "
                                       "ninja -C build -j$(nproc)"
                                       "' 2>&1";
                ret = std::system(cook_cmd.c_str());
            }

            // ─── Copy project headers to install_prefix ───
            auto src_inc = src_path / "include";
            if (fs::exists(src_inc)) {
                fs::create_directories(install_prefix / "include");
                std::string cp = "cp -r " + src_inc.string() + "/* " + (install_prefix / "include").string() + "/ 2>/dev/null";
                std::system(cp.c_str());
            }

            // Copy built .a files
            if (fs::exists(src_path / "build")) {
                fs::create_directories(install_prefix / "lib");
                std::string find_libs = "find " + (src_path / "build").string() + " -name '*.a' -not -path '*/_cooking/*' -exec cp {} " + (install_prefix / "lib").string() + "/ \\; 2>/dev/null";
                std::system(find_libs.c_str());
            }

            // ─── Symlink ALL nix dependency headers/libs into install_prefix ───
            // This ensures cpm build can find boost, fmt, etc. without nix-shell
            // Link nix packages detected from the project
            auto link_pkgs = detected_deps;

            for (const auto &pkg : link_pkgs) {
                // Try .dev output first (has headers), then normal output
                std::string store_path = nix.run_cmd("nix-build '<nixpkgs>' -A " + pkg + ".dev --no-out-link 2>/dev/null");
                if (store_path.empty()) {
                    store_path = nix.run_cmd("nix-build '<nixpkgs>' -A " + pkg + " --no-out-link 2>/dev/null");
                }
                if (!store_path.empty()) {
                    // Symlink headers
                    auto pkg_inc = std::filesystem::path(store_path) / "include";
                    if (fs::exists(pkg_inc)) {
                        for (const auto &entry : fs::directory_iterator(pkg_inc)) {
                            auto target = install_prefix / "include" / entry.path().filename();
                            if (!fs::exists(target) && !fs::is_symlink(target)) {
                                fs::create_symlink(entry.path(), target);
                            }
                        }
                    }
                    // Symlink libs
                    auto pkg_lib = std::filesystem::path(store_path) / "lib";
                    if (fs::exists(pkg_lib)) {
                        for (const auto &entry : fs::directory_iterator(pkg_lib)) {
                            if (!entry.is_regular_file() && !entry.is_symlink()) continue;
                            auto ext = entry.path().extension().string();
                            if (ext == ".a" || ext == ".so") {
                                auto target = install_prefix / "lib" / entry.path().filename();
                                if (!fs::exists(target) && !fs::is_symlink(target)) {
                                    fs::create_symlink(entry.path(), target);
                                }
                            }
                        }
                    }
                }
            }

            // ─── Save compile defines needed by this library ───
            auto defines_file = install_prefix / "defines.txt";
            {
                std::ofstream df(defines_file);
                // Seastar-specific defines (detected from CMakeLists.txt)
                if (name == "seastar") {
                    df << "-DSEASTAR_SCHEDULING_GROUPS_COUNT=16\n";
                    df << "-DSEASTAR_API_LEVEL=7\n";
                    df << "-DSEASTAR_SSTRING\n";
                    df << "-DSEASTAR_LOGGER_COMPILE_TIME_FMT\n";
                }
            }

            if (ret == 0 || (fs::exists(install_prefix / "include") && !fs::is_empty(install_prefix / "include"))) {
                if (fs::exists(build_dir)) fs::remove_all(build_dir);
                return true;
            }
        }

        // ─── No nix: use old approach (direct cooking.sh) ───
        ensure_build_tools(bin_dir);

        // Check if user has pinned a compiler — use isolated toolchain
        std::string compiler_env;
        // Set parallel build env vars for all sub-builds
        compiler_env += "export MAKEFLAGS=\"-j$(nproc)\" && ";
        compiler_env += "export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc) && ";
        auto toml_path = project_root_ / "cpm.toml";
        if (fs::exists(toml_path)) {
            auto config = TomlParser::parse(toml_path);
            if (!config.compiler.empty()) {
                if (!config.compiler.empty()) {
                    std::string cxx = config.compiler;
                    std::string cc = config.compiler;
                    // Verify compiler exists
                    std::string check = "which " + cxx + " > /dev/null 2>&1";
                    if (std::system(check.c_str()) == 0) {
                        compiler_env += "export CC=\"" + cc + "\" && ";
                        compiler_env += "export CXX=\"" + cxx + "\" && ";
                    }
                }
            }
        }

        // Run cooking.sh — it fetches and builds all deps into _cooking/installed/
        std::string cook_cmd = compiler_env + "export PATH=\"" + bin_dir.string() +
                               ":$PATH\" && "
                               "cd " +
                               src_path.string() + " && bash ./cooking.sh -t Release";

        // Always pass the system compiler explicitly so cmake doesn't leave CC empty
        std::string sys_cc = "gcc";
        std::string sys_cxx = "g++";
        if (fs::exists(toml_path)) {
            auto config = TomlParser::parse(toml_path);
            if (!config.compiler.empty()) {
                sys_cc = config.compiler;
                sys_cxx = config.compiler;
            }
        }
        // Use -s to set CC/CXX as env vars (cooking.sh's -s flag = env vars)
        cook_cmd += " -s CC=" + sys_cc;
        cook_cmd += " -s CXX=" + sys_cxx;

        // Use ninja with max parallelism (-g must be before --)
        cook_cmd += " -g Ninja";

        // Pass cmake defines LAST via --
        cook_cmd += " -- -DCMAKE_C_COMPILER=" + sys_cc + " -DCMAKE_CXX_COMPILER=" + sys_cxx;

        // Auto-fix known incompatibilities in cooking recipes
        auto recipe_file = src_path / "cooking_recipe.cmake";
        if (fs::exists(recipe_file)) {
            // Fix broken download URLs (use mirrors for unreachable hosts)
            std::string patch_urls = "sed -i "
                                     "'s|https://gmplib.org/download/gmp/|https://ftp.gnu.org/gnu/gmp/|g' " +
                                     recipe_file.string() + " 2>/dev/null";
            std::system(patch_urls.c_str());

            // Auto-upgrade old deps if system compiler is too new
        }

        cook_cmd += " 2>&1";
        ret = std::system(cook_cmd.c_str());

        if (ret == 0) {
            // Find the actual build directory (cooking.sh may use build/ or
            // build/release/)
            std::filesystem::path ninja_build_dir;
            if (fs::exists(src_path / "build" / "release" / "build.ninja")) {
                ninja_build_dir = src_path / "build" / "release";
            } else if (fs::exists(src_path / "build" / "build.ninja")) {
                ninja_build_dir = src_path / "build";
            }

            int build_ret = -1;
            if (!ninja_build_dir.empty()) {
                // Build the actual project with the pinned compiler
                std::string build_cmd = compiler_env + "export PATH=\"" + bin_dir.string() +
                                        ":$PATH\" && "
                                        "cd " +
                                        src_path.string() + " && ninja -C " + ninja_build_dir.string() + " -j$(nproc) 2>&1";
                build_ret = std::system(build_cmd.c_str());
            }

            // Copy results to install_prefix regardless of build success
            // Headers from source
            auto src_inc = src_path / "include";
            if (fs::exists(src_inc)) {
                std::string cp = "cp -r " + src_inc.string() + "/* " + (install_prefix / "include").string() + "/ 2>/dev/null";
                std::system(cp.c_str());
            }

            // Built libraries — search in all build subdirs
            std::string find_libs = "find " + (src_path / "build").string() + " -name '*.a' -not -path '*/_cooking/*' -exec cp {} " + (install_prefix / "lib").string() + "/ \\; 2>/dev/null";
            std::system(find_libs.c_str());

            // Also copy installed deps headers/libs from _cooking/installed/
            // Check both build/ and build/release/ paths
            for (const auto &cook_base : {src_path / "build", src_path / "build" / "release"}) {
                auto cooked = cook_base / "_cooking" / "installed";
                if (fs::exists(cooked / "include")) {
                    std::string cp = "cp -r " + (cooked / "include").string() + "/* " + (install_prefix / "include").string() + "/ 2>/dev/null";
                    std::system(cp.c_str());
                }
                if (fs::exists(cooked / "lib")) {
                    std::string cp = "cp -r " + (cooked / "lib").string() + "/* " + (install_prefix / "lib").string() + "/ 2>/dev/null";
                    std::system(cp.c_str());
                }
            }

            if (build_ret == 0) {
                if (fs::exists(build_dir)) fs::remove_all(build_dir);
                return true;
            }
        }

        // Even if ninja build failed, if we got headers it's partial success
        if (fs::exists(install_prefix / "include") && !fs::is_empty(install_prefix / "include")) {
            std::cout << "[cpm]   → cooking.sh: headers available (build may be "
                         "incomplete)\n";
            if (fs::exists(build_dir)) fs::remove_all(build_dir);
            return true;
        }

        // Last resort: copy headers directly from source even if build failed
        auto src_inc = src_path / "include";
        if (fs::exists(src_inc) && !fs::is_empty(src_inc)) {
            fs::create_directories(install_prefix / "include");
            std::string cp = "cp -r " + src_inc.string() + "/* " + (install_prefix / "include").string() + "/ 2>/dev/null";
            std::system(cp.c_str());

            // Also grab any cooked deps that did succeed
            for (const auto &cook_base : {src_path / "build", src_path / "build" / "release"}) {
                auto cooked = cook_base / "_cooking" / "installed";
                if (fs::exists(cooked / "include")) {
                    std::string cp2 = "cp -r " + (cooked / "include").string() + "/* " + (install_prefix / "include").string() + "/ 2>/dev/null";
                    std::system(cp2.c_str());
                }
                if (fs::exists(cooked / "lib")) {
                    fs::create_directories(install_prefix / "lib");
                    std::string cp2 = "cp -r " + (cooked / "lib").string() + "/* " + (install_prefix / "lib").string() + "/ 2>/dev/null";
                    std::system(cp2.c_str());
                }

                // Grab headers from ingredient sources (e.g. Boost headers from source)
                auto ingredients = cook_base / "_cooking" / "ingredient";
                if (fs::exists(ingredients)) {
                    for (const auto &ing : fs::directory_iterator(ingredients)) {
                        if (!ing.is_directory()) continue;
                        auto ing_src = ing.path() / "src";
                        // Boost: headers at src/boost/
                        auto boost_hdrs = ing_src / "boost";
                        if (fs::exists(boost_hdrs)) {
                            fs::create_directories(install_prefix / "include" / "boost");
                            std::string cp3 = "cp -r " + boost_hdrs.string() + "/* " + (install_prefix / "include" / "boost").string() + "/ 2>/dev/null";
                            std::system(cp3.c_str());
                        }
                        // Other ingredients: headers at src/include/
                        auto ing_include = ing_src / "include";
                        if (fs::exists(ing_include)) {
                            std::string cp3 = "cp -r " + ing_include.string() + "/* " + (install_prefix / "include").string() + "/ 2>/dev/null";
                            std::system(cp3.c_str());
                        }
                    }
                }
            }

            std::cout << "[cpm]   → headers copied from source (full build failed)\n";
            if (fs::exists(build_dir)) fs::remove_all(build_dir);
            return true;
        }

        std::cout << "[cpm]   → cooking.sh failed, trying other methods...\n";
        std::cerr << "[cpm]\n";
        std::cerr << "[cpm] If the build failed due to compiler incompatibility, try:\n";
        std::cerr << "[cpm]   1. Add .compiler = \"gcc-XX\" (e.g. gcc-13, clang-17)' to [project] in cpm.toml\n";
        std::cerr << "[cpm]   2. Install it:\n";
        std::cerr << "[cpm]      Arch: sudo pacman -S gccXX (e.g. gcc13)\n";
        std::cerr << "[cpm]      Ubuntu: sudo apt install g++-13\n";
        std::cerr << "[cpm]      Fedora: sudo dnf install gcc-c++-13\n";
        std::cerr << "[cpm]\n";
        ret = -1;
    }

    // ─── Strategy 2: configure.py (Python-based configure) ───
    if (ret != 0 && fs::exists(src_path / "configure.py")) {
        std::cout << "[cpm]   → using configure.py\n";

        // Check if ninja is available
        bool has_ninja = (std::system("which ninja > /dev/null 2>&1") == 0);
        std::string build_tool = has_ninja ? "ninja" : "make";

        std::string cfg_cmd = "cd " + src_path.string() +
                              " && python3 ./configure.py --mode=release"
                              " --prefix=" +
                              install_prefix.string() + " > /dev/null 2>&1";

        int cfg_ret = std::system(cfg_cmd.c_str());
        if (cfg_ret == 0) {
            std::string build_cmd = "cd " + src_path.string() + " && " + build_tool +
                                    " -C build/release -j$(nproc) > /dev/null 2>&1"
                                    " && " +
                                    build_tool + " -C build/release install > /dev/null 2>&1";
            ret = std::system(build_cmd.c_str());
        }
    }

    // ─── Strategy 3: CMake ───
    if (ret != 0 && fs::exists(src_path / "CMakeLists.txt")) {
        std::cout << "[cpm]   → using cmake\n";

        bool has_ninja = (std::system("which ninja > /dev/null 2>&1") == 0);
        std::string generator = has_ninja ? " -GNinja" : "";

        // Pass CMAKE_PREFIX_PATH so deps can find each other inside .cpm/

        std::string cmake_cmd = "cd " + build_dir.string() + " && cmake " + src_path.string() + generator + " -DCMAKE_INSTALL_PREFIX=" + install_prefix.string() +
                                " -DCMAKE_PREFIX_PATH=" + install_prefix.string() +
                                " -DCMAKE_BUILD_TYPE=Release"
                                " -DCMAKE_POSITION_INDEPENDENT_CODE=ON"
                                " -DBUILD_SHARED_LIBS=OFF"
                                " -DBUILD_TESTING=OFF"
                                " -DBUILD_TESTS=OFF"
                                " -DBUILD_EXAMPLES=OFF"
                                " -DBUILD_BENCHMARKS=OFF"
                                " > /dev/null 2>&1"
                                " && cmake --build . --parallel > /dev/null 2>&1"
                                " && cmake --install . > /dev/null 2>&1";

        ret = std::system(cmake_cmd.c_str());
    }

    // ─── Strategy 4: Makefile ───
    if (ret != 0 && (fs::exists(src_path / "Makefile") || fs::exists(src_path / "makefile"))) {
        std::cout << "[cpm]   → using make\n";

        std::string make_cmd = "cd " + src_path.string() + " && make -j$(nproc) > /dev/null 2>&1";

        ret = std::system(make_cmd.c_str());

        if (ret == 0) {
            // Try make install
            std::string install_cmd = "cd " + src_path.string() + " && make install PREFIX=" + install_prefix.string() + " > /dev/null 2>&1";
            int install_ret = std::system(install_cmd.c_str());

            if (install_ret != 0) {
                // No install target — manually copy artifacts
                for (const auto &entry : fs::directory_iterator(src_path)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".a") {
                        fs::copy(entry.path(), install_prefix / "lib" / entry.path().filename(), fs::copy_options::overwrite_existing);
                    }
                }

                // Copy headers
                auto header_src = src_path / "include";
                if (!fs::exists(header_src)) header_src = src_path / "src";

                if (fs::exists(header_src)) {
                    for (const auto &entry : fs::recursive_directory_iterator(header_src)) {
                        if (!entry.is_regular_file()) continue;
                        auto ext = entry.path().extension().string();
                        if (ext == ".h" || ext == ".hpp" || ext == ".hxx") {
                            auto rel = fs::relative(entry.path(), header_src);
                            auto dest = install_prefix / "include" / rel;
                            fs::create_directories(dest.parent_path());
                            fs::copy(entry.path(), dest, fs::copy_options::overwrite_existing);
                        }
                    }
                }
            }
        }
    }

    // ─── Strategy 5: Meson ───
    if (ret != 0 && fs::exists(src_path / "meson.build")) {
        std::cout << "[cpm]   → using meson\n";

        std::string meson_cmd = "meson setup " + build_dir.string() + " " + src_path.string() + " --prefix=" + install_prefix.string() +
                                " --default-library=static"
                                " > /dev/null 2>&1"
                                " && meson compile -C " +
                                build_dir.string() +
                                " > /dev/null 2>&1"
                                " && meson install -C " +
                                build_dir.string() + " > /dev/null 2>&1";

        ret = std::system(meson_cmd.c_str());
    }

    // ─── Strategy 6: Autotools (./configure) ───
    if (ret != 0 && fs::exists(src_path / "configure")) {
        std::cout << "[cpm]   → using autotools\n";

        std::string auto_cmd = "cd " + src_path.string() + " && ./configure --prefix=" + install_prefix.string() +
                               " --enable-static --disable-shared"
                               " > /dev/null 2>&1"
                               " && make -j$(nproc) > /dev/null 2>&1"
                               " && make install > /dev/null 2>&1";

        ret = std::system(auto_cmd.c_str());
    }

    // ─── Strategy 7: Header-only (no build needed) ───
    if (ret != 0) {
        // Check if it's actually a header-only library with no build system
        bool has_headers = false;
        auto check_dirs = {src_path / "include", src_path / "src", src_path};
        for (const auto &dir : check_dirs) {
            if (!fs::exists(dir)) continue;
            for (const auto &entry : fs::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh") {
                    has_headers = true;
                    break;
                }
            }
            if (has_headers) break;
        }

        if (has_headers) {
            std::cout << "[cpm]   → header-only (no build needed)\n";
            // Just copy headers
            auto header_src = src_path / "include";
            if (!fs::exists(header_src)) header_src = src_path / "src";
            if (!fs::exists(header_src)) header_src = src_path;

            for (const auto &entry : fs::recursive_directory_iterator(header_src)) {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh") {
                    auto rel = fs::relative(entry.path(), header_src);
                    auto dest = install_prefix / "include" / rel;
                    fs::create_directories(dest.parent_path());
                    fs::copy(entry.path(), dest, fs::copy_options::overwrite_existing);
                }
            }
            ret = 0;
        } else {
            std::cerr << "[cpm] ERROR: Cannot detect build system for " << name << "\n";
            std::cerr << "[cpm]        Tried: cooking.sh, configure.py, cmake, make, "
                         "meson, autotools\n";
            return false;
        }
    }

    // Cleanup build dir
    if (fs::exists(build_dir)) {
        fs::remove_all(build_dir);
    }

    return (ret == 0);
}

void PackageManager::install_built_library(const std::string &name, const std::filesystem::path &built_path) {
    namespace fs = std::filesystem;

    // Copy/symlink headers from built_path/include/ → .cpm/include/
    auto src_include = built_path / "include";
    auto dst_include = local_cpm_dir_ / "include";

    if (fs::exists(src_include)) {
        for (const auto &entry : fs::directory_iterator(src_include)) {
            auto target = dst_include / entry.path().filename();
            if (fs::exists(target) || fs::is_symlink(target)) {
                fs::remove_all(target);
            }
            fs::create_symlink(fs::absolute(entry.path()), target);
        }
    }

    // Copy/symlink libraries from built_path/lib/ → .cpm/lib/
    auto src_lib = built_path / "lib";
    auto dst_lib = local_cpm_dir_ / "lib";

    if (fs::exists(src_lib)) {
        for (const auto &entry : fs::recursive_directory_iterator(src_lib)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext == ".a" || ext == ".so" || ext.find(".so.") != std::string::npos) {
                auto target = dst_lib / entry.path().filename();
                if (fs::exists(target)) fs::remove(target);
                fs::copy(entry.path(), target);
            }
        }
    }

    // Also check lib64/
    auto src_lib64 = built_path / "lib64";
    if (fs::exists(src_lib64)) {
        for (const auto &entry : fs::recursive_directory_iterator(src_lib64)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext == ".a" || ext == ".so" || ext.find(".so.") != std::string::npos) {
                auto target = dst_lib / entry.path().filename();
                if (fs::exists(target)) fs::remove(target);
                fs::copy(entry.path(), target);
            }
        }
    }

    // Copy defines.txt if present (compile flags needed by this library)
    auto src_defines = built_path / "defines.txt";
    auto dst_defines = local_cpm_dir_ / "defines.txt";
    if (fs::exists(src_defines)) {
        // Overwrite (not append) — install runs fresh each time
        fs::copy(src_defines, dst_defines, fs::copy_options::overwrite_existing);
    }
}

void PackageManager::resolve_transitive_deps(const std::filesystem::path &src_path, const std::filesystem::path &install_prefix) {
    namespace fs = std::filesystem;

    // Parse CMakeLists.txt to find find_package() calls
    std::vector<std::string> needed_packages;

    auto cmake_file = src_path / "CMakeLists.txt";
    if (fs::exists(cmake_file)) {
        std::ifstream f(cmake_file);
        std::string line;
        while (std::getline(f, line)) {
            // Match: find_package(PackageName ...) or find_package(PackageName
            // REQUIRED)
            auto pos = line.find("find_package");
            if (pos == std::string::npos) continue;

            auto paren = line.find('(', pos);
            if (paren == std::string::npos) continue;

            auto end_paren = line.find(')', paren);
            if (end_paren == std::string::npos) continue;

            std::string args = line.substr(paren + 1, end_paren - paren - 1);
            // First word is the package name
            std::istringstream iss(args);
            std::string pkg_name;
            iss >> pkg_name;

            if (!pkg_name.empty() && pkg_name != "Threads" && pkg_name != "PkgConfig") {
                // Skip cmake internal/utility modules (not real libraries)
                static const std::set<std::string> skip_modules = {"Threads", "PkgConfig", "PthreadSetName", "LinuxMembarrier", "Sanitizers", "SourceLocation", "StdAtomic", "SystemTap-SDT",
                    "Valgrind", "ucontext", "rt", "GnuInstallDirs", "CMakePackageConfigHelpers", "FindPkgConfig", "CheckCXXSourceCompiles", "CTest", "Python3", "Doxygen"};
                if (skip_modules.find(pkg_name) == skip_modules.end()) {
                    needed_packages.push_back(pkg_name);
                }
            }
        }
        f.close();
    }

    // Also scan cmake/ directory for Find*.cmake files that hint at deps
    auto cmake_dir = src_path / "cmake";
    if (fs::exists(cmake_dir)) {
        for (const auto &entry : fs::directory_iterator(cmake_dir)) {
            auto fname = entry.path().filename().string();
            if (fname.starts_with("Find") && fname.find(".cmake") != std::string::npos) {
                // FindYamlCpp.cmake → YamlCpp
                auto name = fname.substr(4, fname.size() - 10); // remove "Find" and ".cmake"
                needed_packages.push_back(name);
            }
        }
    }

    if (needed_packages.empty()) {
        std::cout << "[cpm]   → no transitive deps detected\n";
        return;
    }

    // For each needed package, search GitHub for it and build
    std::cout << "[cpm]   → detected " << needed_packages.size() << " dependencies from CMakeLists.txt\n";

    for (const auto &pkg : needed_packages) {
        // Check if already satisfied (in install prefix or system pkg-config)
        std::string pkg_lower = pkg;
        std::ranges::transform(pkg_lower, pkg_lower.begin(), ::tolower);

        // Check if already in install_prefix/lib or include
        bool found = false;
        if (fs::exists(install_prefix / "include")) {
            for (const auto &entry : fs::directory_iterator(install_prefix / "include")) {
                std::string name = entry.path().filename().string();
                std::string name_lower = name;
                std::ranges::transform(name_lower, name_lower.begin(), ::tolower);
                if (name_lower.find(pkg_lower) != std::string::npos) {
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            // Check system pkg-config (read-only check, doesn't install anything)
            std::string check = "pkg-config --exists " + pkg_lower + " 2>/dev/null";
            if (std::system(check.c_str()) == 0) {
                found = true;
            }
        }

        if (!found) {
            // Search GitHub for this package
            std::string github_url = search_github_repo(pkg);
            if (github_url.empty()) {
                std::cout << "[cpm]     ◦ " << pkg << " (not found on GitHub, skipping)\n";
                continue;
            }

            std::cout << "[cpm]     ◦ " << pkg << " → " << github_url << "\n";

            std::string version = resolve_latest_tag(github_url, pkg);

            // Check cache
            auto cache_key = pkg + "-" + version + "-built";
            auto dep_built_cache = global_cache_dir_ / cache_key;

            if (!fs::exists(dep_built_cache)) {
                // Download
                auto dep_src = global_cache_dir_ / (pkg + "-" + version + "-src");
                if (!fs::exists(dep_src)) {
                    fs::create_directories(dep_src);
                    std::string clone_cmd = "git -c advice.detachedHead=false clone --depth 1 --quiet --recurse-submodules ";
                    if (version != "HEAD") clone_cmd += "--branch " + version + " ";
                    clone_cmd += github_url + " " + dep_src.string() + " 2>/dev/null";
                    if (std::system(clone_cmd.c_str()) != 0) {
                        fs::remove_all(dep_src);
                        std::cerr << "[cpm]     ✗ failed to download " << pkg << "\n";
                        continue;
                    }
                    auto git_dir = dep_src / ".git";
                    if (fs::exists(git_dir)) fs::remove_all(git_dir);
                }

                // Build
                fs::create_directories(dep_built_cache);
                bool ok = build_from_source(pkg, dep_src, dep_built_cache);
                if (!ok) {
                    fs::remove_all(dep_built_cache);
                    std::cerr << "[cpm]     ✗ failed to build " << pkg << "\n";
                    continue;
                }
            }

            // Install to prefix
            auto dep_inc = dep_built_cache / "include";
            auto dep_lib = dep_built_cache / "lib";
            if (fs::exists(dep_inc)) {
                std::string cp_cmd = "cp -rn " + dep_inc.string() + "/* " + (install_prefix / "include").string() + "/ 2>/dev/null";
                std::system(cp_cmd.c_str());
            }
            if (fs::exists(dep_lib)) {
                std::string cp_cmd = "cp -rn " + dep_lib.string() + "/* " + (install_prefix / "lib").string() + "/ 2>/dev/null";
                std::system(cp_cmd.c_str());
            }
        }
    }
}

std::string PackageManager::search_github_repo(const std::string &package_name) {
    // Use GitHub API to search for the repository (top result by stars)
    std::string search_cmd = "curl -s 'https://api.github.com/search/repositories?q=" + package_name +
                             "&sort=stars&per_page=1' "
                             "2>/dev/null | grep -oP '\"html_url\": "
                             "\"https://github.com/[^/]+/[^\"]*\"' "
                             "| head -1 | grep -oP 'https://github.com/[^\"]*'";

    std::array<char, 512> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(search_cmd.c_str(), "r"), pclose);
    if (pipe) {
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }

    return result;
}

void PackageManager::ensure_build_tools(const std::filesystem::path &bin_dir) {
    namespace fs = std::filesystem;

    auto stow_path = bin_dir / "stow";
    if (std::system("which stow > /dev/null 2>&1") != 0 && !fs::exists(stow_path)) {
        std::cout << "[cpm]   → building stow from source...\n";

        auto stow_src = global_cache_dir_ / "tool-stow-src";
        if (!fs::exists(stow_src)) {
            std::string cmd = "git -c advice.detachedHead=false clone --depth 1 --quiet --branch v2.4.1 "
                              "https://github.com/aspiers/stow " +
                              stow_src.string() + " 2>/dev/null";
            std::system(cmd.c_str());
        }

        auto tool_prefix = bin_dir.parent_path() / "tools" / "stow";
        fs::create_directories(tool_prefix);

        // Build stow: remove -Werror from configure.ac, then autoreconf + configure + make install
        std::string build_cmd = "cd " + stow_src.string() +
                                " && sed -i 's/-Werror//g' configure.ac 2>/dev/null"
                                " && autoreconf -iv > /dev/null 2>&1"
                                " && ./configure --prefix=" +
                                tool_prefix.string() +
                                " > /dev/null 2>&1"
                                " && make install > /dev/null 2>&1";

        int ret = std::system(build_cmd.c_str());
        if (ret == 0 && fs::exists(tool_prefix / "bin" / "stow")) {
            if (fs::exists(stow_path) || fs::is_symlink(stow_path)) fs::remove(stow_path);
            fs::create_symlink(tool_prefix / "bin" / "stow", stow_path);
        } else {
            std::cerr << "[cpm] WARNING: Could not build stow. Some packages may fail to install.\n";
        }
    }
}

void PackageManager::export_package_headers() {
    Resolver resolver(project_root_);
    resolver.export_headers();
}

void PackageManager::generate_compile_commands() {
    namespace fs = std::filesystem;
    auto toml_path = project_root_ / "cpm.toml";
    if (!fs::exists(toml_path)) return;

    auto config = TomlParser::parse(toml_path);
    auto include_dir = local_cpm_dir_ / "include";

    // Build the flags string
    std::string compiler = detect_compiler(config);
    std::string flags = " -std=c++" + config.cpp_standard;
    if (fs::exists(include_dir)) {
        flags += " -I" + include_dir.string();
    }

    // Add defines from .cpm/defines.txt
    auto defines_file = local_cpm_dir_ / "defines.txt";
    if (fs::exists(defines_file)) {
        std::ifstream df(defines_file);
        std::string define;
        while (std::getline(df, define)) {
            if (!define.empty() && define[0] == '-') {
                flags += " " + define;
            }
        }
    }

    // Collect all source files (.cpp, .cc, .cxx)
    std::vector<fs::path> source_files;
    for (const auto &entry : fs::directory_iterator(project_root_)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
            source_files.push_back(entry.path());
        }
    }
    // Also scan src/ subdirectory if exists
    auto src_dir = project_root_ / "src";
    if (fs::exists(src_dir) && fs::is_directory(src_dir)) {
        for (const auto &entry : fs::recursive_directory_iterator(src_dir)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
                source_files.push_back(entry.path());
            }
        }
    }

    // If no sources found, at least add the entry file
    if (source_files.empty() && !config.entry.empty()) {
        source_files.push_back(project_root_ / config.entry);
    }

    // Write compile_commands.json with entry for each source file
    std::ofstream cc(project_root_ / "compile_commands.json");
    cc << "[\n";
    for (size_t i = 0; i < source_files.size(); ++i) {
        cc << "  {\n";
        cc << R"(    "directory": ")" << project_root_.string() << "\",\n";
        cc << R"(    "command": ")" << compiler << flags << " -c " << source_files[i].filename().string() << "\",\n";
        cc << R"(    "file": ")" << source_files[i].string() << "\"\n";
        cc << "  }";
        if (i + 1 < source_files.size()) cc << ",";
        cc << "\n";
    }
    cc << "]\n";
    cc.close();
}

void PackageManager::install() {
    auto toml_path = project_root_ / "cpm.toml";
    if (!std::filesystem::exists(toml_path)) {
        throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");
    }

    auto config = TomlParser::parse(toml_path);
    ensure_directories();

    // ─── Auto-remove packages not in cpm.toml anymore ───
    auto packages_dir = local_cpm_dir_ / "packages";
    if (std::filesystem::exists(packages_dir)) {
        // Collect names that SHOULD exist
        std::set<std::string> wanted;
        for (const auto &dep : config.git_dependencies) {
            wanted.insert(dep.name);
        }

        // Check what's currently installed and remove stale ones
        std::vector<std::string> to_remove;
        for (const auto &entry : std::filesystem::directory_iterator(packages_dir)) {
            if (!entry.is_directory() && !entry.is_symlink()) continue;
            std::string installed_name = entry.path().filename().string();
            if (wanted.find(installed_name) == wanted.end()) {
                to_remove.push_back(installed_name);
            }
        }

        for (const auto &name : to_remove) {
            std::cout << "[cpm] Removing " << name << " (not in cpm.toml)\n";
            std::filesystem::remove_all(packages_dir / name);
        }
    }

    // Auto-remove built libraries not in cpm.toml anymore
    auto lib_dir = local_cpm_dir_ / "lib";
    if (std::filesystem::exists(lib_dir)) {
        // Collect names of system deps that SHOULD exist
        std::set<std::string> wanted_libs;
        for (const auto &dep : config.system_dependencies) {
            wanted_libs.insert(dep.name);
        }

        // Check .cpm/lib/ for stale .a files
        std::vector<std::filesystem::path> libs_to_remove;
        for (const auto &entry : std::filesystem::directory_iterator(lib_dir)) {
            if (!entry.is_regular_file()) continue;
            auto filename = entry.path().filename().string();
            if (entry.path().extension() != ".a") continue;

            // libhiredis.a → hiredis
            std::string lib_name = filename;
            if (lib_name.starts_with("lib")) lib_name = lib_name.substr(3);
            auto dot = lib_name.find('.');
            if (dot != std::string::npos) lib_name = lib_name.substr(0, dot);

            bool found = false;
            for (const auto &dep_name : wanted_libs) {
                // Match: dep name "hiredis" matches lib "hiredis" or "usockets" etc.
                if (lib_name.find(dep_name) != std::string::npos || dep_name.find(lib_name) != std::string::npos) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                libs_to_remove.push_back(entry.path());
            }
        }

        for (const auto &lib_path : libs_to_remove) {
            std::cout << "[cpm] Removing " << lib_path.filename().string() << " (not in cpm.toml)\n";
            std::filesystem::remove(lib_path);
        }
    }

    // Auto-remove stale symlinks in .cpm/include/
    auto include_dir = local_cpm_dir_ / "include";
    if (std::filesystem::exists(include_dir)) {
        // Collect all wanted package names
        std::set<std::string> all_wanted;
        for (const auto &dep : config.git_dependencies) {
            all_wanted.insert(dep.name);
        }
        for (const auto &dep : config.system_dependencies) {
            all_wanted.insert(dep.name);
        }

        std::vector<std::filesystem::path> includes_to_remove;
        for (const auto &entry : std::filesystem::directory_iterator(include_dir)) {
            std::string inc_name = entry.path().filename().string();
            // Check if this include dir/symlink belongs to any wanted package
            bool belongs = false;
            for (const auto &wanted_name : all_wanted) {
                if (inc_name == wanted_name) {
                    belongs = true;
                    break;
                }
            }

            // Also check if it's a sub-include from a header-only package (e.g.,
            // "nlohmann" from "json") by checking if the symlink target points to a
            // wanted package
            if (!belongs && std::filesystem::is_symlink(entry.path())) {
                auto target = std::filesystem::read_symlink(entry.path()).string();
                for (const auto &dep : config.git_dependencies) {
                    if (target.find(dep.name) != std::string::npos) {
                        belongs = true;
                        break;
                    }
                }
                for (const auto &dep : config.system_dependencies) {
                    if (target.find(dep.name) != std::string::npos) {
                        belongs = true;
                        break;
                    }
                }
            }

            if (!belongs) {
                includes_to_remove.push_back(entry.path());
            }
        }

        for (const auto &path : includes_to_remove) {
            std::cout << "[cpm] Removing include: " << path.filename().string() << " (not in cpm.toml)\n";
            std::filesystem::remove_all(path);
        }
    }

    // ─── Ensure toolchain is ready BEFORE building anything ───
    if (!config.compiler.empty()) {
        if (!config.compiler.empty()) {
            config.compiler;
        }
    }

    // ─── Install what's in cpm.toml (PARALLEL) ───

    ProgressDisplay progress;
    std::mutex install_mutex;

    // Register all tasks
    std::vector<int> header_task_ids;
    header_task_ids.reserve(config.git_dependencies.size());
    for (const auto &dep : config.git_dependencies) {
        header_task_ids.push_back(progress.add_task(dep.name));
    }
    std::vector<int> sys_task_ids;
    sys_task_ids.reserve(config.system_dependencies.size());
    for (const auto &dep : config.system_dependencies) {
        sys_task_ids.push_back(progress.add_task(dep.name));
    }

    if (!config.git_dependencies.empty() || !config.system_dependencies.empty()) {
        progress.start();
    }

    // Download header-only deps in parallel
    if (!config.git_dependencies.empty()) {
        std::vector<std::function<void()>> download_tasks;
        download_tasks.reserve(config.git_dependencies.size());
        for (size_t i = 0; i < config.git_dependencies.size(); ++i) {
            download_tasks.emplace_back([&, i]() {
                const auto &dep = config.git_dependencies[i];
                int tid = header_task_ids[i];

                std::string version = dep.version;
                if (version == "*" || version.empty()) {
                    progress.set_status(tid, TaskStatus::Downloading, "resolving tag");
                    version = resolve_latest_tag(dep.github_url, dep.name);
                }

                if (is_cached(dep.name, version)) {
                    progress.set_status(tid, TaskStatus::Cached);
                    std::scoped_lock lock(install_mutex);
                    link_from_cache(dep.name, version);
                } else {
                    progress.set_status(tid, TaskStatus::Downloading);
                    try {
                        // Clone (thread-safe — different paths)
                        auto cache_path = get_cache_path(dep.name, version);
                        std::filesystem::create_directories(cache_path);

                        std::string clone_cmd = "git -c advice.detachedHead=false clone --depth 1 --quiet ";
                        if (version != "HEAD") clone_cmd += "--branch " + version + " ";
                        clone_cmd += dep.github_url + " " + cache_path.string() + " > /dev/null 2>&1";

                        int ret = std::system(clone_cmd.c_str());
                        if (ret != 0) {
                            std::filesystem::remove_all(cache_path);
                            progress.set_status(tid, TaskStatus::Failed);
                            return;
                        }
                        auto git_dir = cache_path / ".git";
                        if (std::filesystem::exists(git_dir)) std::filesystem::remove_all(git_dir);

                        std::scoped_lock lock(install_mutex);
                        link_from_cache(dep.name, version);
                        progress.set_status(tid, TaskStatus::Done);
                    } catch (...) {
                        progress.set_status(tid, TaskStatus::Failed);
                    }
                }
                if (header_task_ids[i] >= 0) {
                    auto &t = config.git_dependencies[i];
                    // Already set above
                }
            });
        }
        parallel_execute(download_tasks, 4);
    }

    // Build system deps in parallel (download parallel, build may be sequential
    // for complex ones)
    if (!config.system_dependencies.empty()) {
        std::vector<std::function<void()>> sys_tasks;
        sys_tasks.reserve(config.system_dependencies.size());
        for (size_t i = 0; i < config.system_dependencies.size(); ++i) {
            sys_tasks.emplace_back([&, i]() {
                const auto &dep = config.system_dependencies[i];
                int tid = sys_task_ids[i];
                progress.set_status(tid, TaskStatus::Downloading);

                try {
                    resolve_system_dependency(dep);
                    progress.set_status(tid, TaskStatus::Done);
                } catch (...) {
                    progress.set_status(tid, TaskStatus::Failed);
                }
            });
        }
        // System deps may depend on each other, so limit parallelism
        parallel_execute(sys_tasks, 2);
    }

    if (!config.git_dependencies.empty() || !config.system_dependencies.empty()) {
        progress.stop();
    }

    export_package_headers();
    generate_compile_commands();
    std::cout << "[cpm] Packages ready.\n";
}

// ─── BUILD SYSTEM ────────────────────────────────────────────

std::string PackageManager::detect_compiler(const ProjectConfig &config) const {
    if (!config.compiler.empty()) {
        // Check if it's a pinned version like "gcc@13" or "clang@17"
        if (!config.compiler.empty()) {
            return config.compiler;
        }

        if (config.compiler == "gcc") return "g++";
        if (config.compiler == "clang") return "clang++";
        if (config.compiler == "msvc") return "cl";
        return config.compiler;
    }

    std::string detected = Config::get_compiler();
    if (detected == "gcc") return "g++";
    if (detected == "clang") return "clang++";
    return "g++";
}

std::filesystem::path PackageManager::get_output_path(const ProjectConfig &config) const {
    std::string out_name = config.output.empty() ? config.name : config.output;
    return project_root_ / out_name;
}

std::string PackageManager::build_compile_command(const ProjectConfig &config) const {
    std::ostringstream cmd;

    cmd << detect_compiler(config);
    cmd << " -std=c++" << config.cpp_standard;

    // Include .cpm/include — all headers (both header-only and compiled libs)
    auto include_dir = local_cpm_dir_ / "include";
    if (std::filesystem::exists(include_dir)) {
        cmd << " -I" << include_dir.string();
    }

    // Read compile defines from .cpm/defines.txt (set by packages that need them)
    auto defines_file = local_cpm_dir_ / "defines.txt";
    if (!std::filesystem::exists(defines_file)) {
        // Also check in built cache
        defines_file = global_cache_dir_;
        // Search for any defines.txt in lib or include parents
        for (const auto &entry : std::filesystem::directory_iterator(local_cpm_dir_)) {
            auto df = entry.path() / "defines.txt";
            if (std::filesystem::exists(df)) {
                defines_file = df;
                break;
            }
        }
    }
    if (std::filesystem::exists(defines_file) && std::filesystem::is_regular_file(defines_file)) {
        std::ifstream df(defines_file);
        std::string define;
        while (std::getline(df, define)) {
            if (!define.empty() && define[0] == '-') {
                cmd << " " << define;
            }
        }
    }

    // Source files — entry file + any .cpp files in src/ directory
    cmd << " " << config.entry;

    // Also compile all .cpp files in src/ if it exists
    auto src_dir = project_root_ / "src";
    if (std::filesystem::exists(src_dir) && std::filesystem::is_directory(src_dir)) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(src_dir)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
                cmd << " " << entry.path().string();
            }
        }
    }

    // Output binary
    cmd << " -o " << get_output_path(config).string();

    // Link against .cpm/lib/
    auto lib_dir = local_cpm_dir_ / "lib";
    if (std::filesystem::exists(lib_dir)) {
        cmd << " -L" << lib_dir.string();

        // Link all .a files found in .cpm/lib/
        for (const auto &entry : std::filesystem::directory_iterator(lib_dir)) {
            if (!entry.is_regular_file()) continue;
            auto filename = entry.path().filename().string();
            auto ext = entry.path().extension().string();
            if (ext == ".a") {
                if (filename.starts_with("lib")) {
                    // libhiredis.a → -lhiredis
                    auto lib_name = filename.substr(3);
                    lib_name = lib_name.substr(0, lib_name.size() - 2);
                    cmd << " -l" << lib_name;
                } else {
                    // uSockets.a → link directly
                    cmd << " " << entry.path().string();
                }
            }
        }

        // Add common system libs that compiled deps often need
        cmd << " -lz -lpthread -ldl -lrt -latomic";

        // When linking seastar or complex libs, add their transitive deps
        // These are available in nix-shell PATH at link time
        if (std::filesystem::exists(lib_dir / "libseastar.a")) {
            cmd << " -lfmt -lboost_program_options -lboost_thread -lboost_filesystem -lboost_chrono";
            cmd << " -lyaml-cpp -lhwloc -lgnutls -luring -lnuma -lcares -lprotobuf -lsctp";
        }
    }

    return cmd.str();
}

int PackageManager::build(bool static_build) {
    auto toml_path = project_root_ / "cpm.toml";
    if (!std::filesystem::exists(toml_path)) {
        throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");
    }

    auto config = TomlParser::parse(toml_path);

    if (config.entry.empty()) {
        throw std::runtime_error("No 'entry' specified in [project] section of cpm.toml");
    }

    auto entry_path = project_root_ / config.entry;
    if (!std::filesystem::exists(entry_path)) {
        throw std::runtime_error("Entry file not found: " + config.entry);
    }

    std::string compiler = detect_compiler(config);
    std::string compile_cmd = build_compile_command(config);

    // ─── Production build (-s flag): optimized + self-contained bundle ───
    if (static_build) {
        std::cout << "[cpm] Building " << config.name << " (production)...\n";
        std::cout << "[cpm] " << compiler << " | c++" << config.cpp_standard << " | " << Config::get_architecture() << " | optimized\n";

        // Add optimization flags (no -flto if mixing compiler versions)
        auto spc = compile_cmd.find(' ');
        if (spc != std::string::npos) {
            compile_cmd.insert(spc, " -O3 -DNDEBUG -march=x86-64-v3");
        }

        // Find shell.nix for nix-shell linking
        NixEnv nix(local_cpm_dir_, global_cache_dir_);
        std::filesystem::path shell_nix_path;
        if (nix.available() && !config.system_dependencies.empty()) {
            for (const auto &entry : std::filesystem::directory_iterator(global_cache_dir_)) {
                auto snix = entry.path() / "shell.nix";
                if (std::filesystem::exists(snix) && entry.path().filename().string().find("-src") != std::string::npos) {
                    shell_nix_path = snix;
                    break;
                }
            }
        }

        std::cout.flush();
        int ret;
        // For production builds: use system compiler directly (same GCC that built .a files)
        // Don't use nix-shell here — it has a different GCC version causing LTO mismatch
        // Only use nix-shell if linking requires .so files not in .cpm/lib/
        bool has_seastar = std::filesystem::exists(local_cpm_dir_ / "lib" / "libseastar.a");
        if (!shell_nix_path.empty() && has_seastar) {
            // Seastar needs nix .so at link time — but WITHOUT -flto (version mismatch)
            std::string nix_cmd = "nix-shell " + shell_nix_path.string() + " --run \'" + compile_cmd + "\' 2>&1";
            ret = std::system(nix_cmd.c_str());
        } else {
            ret = std::system(compile_cmd.c_str());
        }

        if (ret != 0) {
            std::cerr << "[cpm] Production build failed.\n";
            return 1;
        }

        // Strip binary
        auto out = get_output_path(config);
        std::system(("strip " + out.string() + " 2>/dev/null").c_str());

        // Bundle shared libs into dist/ for portability
        auto dist_dir = project_root_ / "dist";
        std::filesystem::create_directories(dist_dir);
        std::filesystem::copy(out, dist_dir / out.filename(), std::filesystem::copy_options::overwrite_existing);

        // Copy required .so files
        std::string ldd_cmd = "ldd " + out.string() +
                              " 2>/dev/null | grep nix | awk \'{print $3}\' | "
                              "xargs -I{} cp {} " +
                              dist_dir.string() + "/ 2>/dev/null";
        std::system(ldd_cmd.c_str());

        // Create run script that sets LD_LIBRARY_PATH
        auto run_script = dist_dir / "run.sh";
        std::ofstream rs(run_script);
        rs << "#!/bin/bash\n";
        rs << "DIR=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n";
        rs << "export LD_LIBRARY_PATH=\"$DIR:$LD_LIBRARY_PATH\"\n";
        rs << "exec \"$DIR/" << out.filename().string() << "\" \"$@\"\n";
        rs.close();
        std::filesystem::permissions(run_script,
            std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec | std::filesystem::perms::others_read | std::filesystem::perms::others_exec);

        auto size = std::filesystem::file_size(out);
        std::cout << "[cpm] Built: " << out.filename().string() << " (" << (size / 1024 / 1024) << " MB, optimized)\n";
        std::cout << "[cpm] Bundle: dist/ (portable, copy to any Linux)\n";
        std::cout << "[cpm]   Run with: ./dist/run.sh\n";
        return 0;
    }

    std::cout << "[cpm] Building " << config.name << "...\n";
    std::cout << "[cpm] " << compiler << " | c++" << config.cpp_standard << " | " << Config::get_architecture() << "\n";

    // If any system dep was built with nix, run compile inside nix-shell
    // so linker can find all nix-provided shared libs
    NixEnv nix(local_cpm_dir_, global_cache_dir_);
    bool use_nix_shell = false;
    std::filesystem::path shell_nix_path;

    if (nix.available() && !config.system_dependencies.empty()) {
        // Check if any dep has a shell.nix in its source cache
        for (const auto &dep : config.system_dependencies) {
            std::string version = dep.version;
            if (version == "*" || version.empty()) version = "HEAD";
            // Search for shell.nix in cached source
            for (const auto &entry : std::filesystem::directory_iterator(global_cache_dir_)) {
                if (entry.path().filename().string().find(dep.name) != std::string::npos && entry.path().filename().string().find("-src") != std::string::npos) {
                    auto snix = entry.path() / "shell.nix";
                    if (std::filesystem::exists(snix)) {
                        shell_nix_path = snix;
                        use_nix_shell = true;
                        break;
                    }
                }
            }
            if (use_nix_shell) break;
        }
    }

    std::cout.flush();
    int ret;

    if (use_nix_shell) {
        // Wrap compile command in nix-shell for proper linking
        std::string nix_cmd = "nix-shell " + shell_nix_path.string() + " --run " + "'" + compile_cmd + "'" + " 2>&1";
        ret = std::system(nix_cmd.c_str());
    } else {
        ret = std::system(compile_cmd.c_str());
    }

    if (ret != 0) {
        std::cerr << "[cpm] Build failed.\n";
        return 1;
    }

    std::cout << "[cpm] Built: " << get_output_path(config).filename().string() << "\n";
    return 0;
}

int PackageManager::run() {
    auto toml_path = project_root_ / "cpm.toml";
    if (!std::filesystem::exists(toml_path)) {
        throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");
    }

    auto config = TomlParser::parse(toml_path);

    // Step 1: Install if needed
    bool needs_install = false;
    if (!config.git_dependencies.empty() || !config.system_dependencies.empty()) {
        auto packages_dir = local_cpm_dir_ / "packages";
        auto include_dir = local_cpm_dir_ / "include";

        if (!std::filesystem::exists(packages_dir) && !config.git_dependencies.empty()) {
            needs_install = true;
        } else if (!std::filesystem::exists(include_dir) || std::filesystem::is_empty(include_dir)) {
            needs_install = true;
        } else {
            for (const auto &dep : config.git_dependencies) {
                auto pkg_path = packages_dir / dep.name;
                if (!std::filesystem::exists(pkg_path) && !std::filesystem::is_symlink(pkg_path)) {
                    needs_install = true;
                    break;
                }
            }
        }
    }

    if (needs_install) {
        install();
    }

    // Step 2: Build
    int build_result = build();
    if (build_result != 0) {
        return build_result;
    }

    // Step 3: Run
    auto output_path = get_output_path(config);
    std::cout << "\n[cpm] Running " << output_path.filename().string() << "...\n";
    std::cout << "────────────────────────────────────────" << '\n';

    std::cout.flush();
    int ret = std::system(output_path.string().c_str());
    return WEXITSTATUS(ret);
}

int PackageManager::start() {
    auto toml_path = project_root_ / "cpm.toml";
    if (!std::filesystem::exists(toml_path)) {
        throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");
    }

    auto config = TomlParser::parse(toml_path);

    auto output_path = get_output_path(config);
    if (!std::filesystem::exists(output_path)) {
        std::cout << "[cpm] Binary not found, building...\n";
        int build_result = build();
        if (build_result != 0) return build_result;
    }

    std::string run_cmd;
    if (!config.start_script.empty()) {
        run_cmd = config.start_script;
    } else {
        run_cmd = output_path.string();
    }

    std::cout << "[cpm] Starting: " << run_cmd << "\n";
    std::cout << "────────────────────────────────────────" << '\n';

    std::cout.flush();
    int ret = std::system(run_cmd.c_str());
    return WEXITSTATUS(ret);
}

// ─── PACKAGE OPERATIONS ──────────────────────────────────────

void PackageManager::install_package(const std::string &package_spec) {
    ensure_directories();

    GitDependency dep;
    std::string spec = package_spec;

    if (spec.starts_with("github:")) {
        spec = spec.substr(7);
    }

    auto at_pos = spec.find('@');
    if (at_pos != std::string::npos) {
        std::string repo = spec.substr(0, at_pos);
        dep.version = spec.substr(at_pos + 1);
        dep.github_url = "https://github.com/" + repo;
        auto slash_pos = repo.find('/');
        dep.name = (slash_pos != std::string::npos) ? repo.substr(slash_pos + 1) : repo;
    } else {
        dep.github_url = "https://github.com/" + spec;
        dep.version = "*";
        auto slash_pos = spec.find('/');
        dep.name = (slash_pos != std::string::npos) ? spec.substr(slash_pos + 1) : spec;
    }

    std::cout << "[cpm] Adding " << dep.name << "...\n";
    clone_git_dependency(dep);
    export_package_headers();
    generate_compile_commands();
    std::cout << "[cpm] Added " << dep.name << "\n";
}

void PackageManager::remove_package(const std::string &package_name) {
    auto package_path = local_cpm_dir_ / "packages" / package_name;
    if (!std::filesystem::exists(package_path) && !std::filesystem::is_symlink(package_path)) {
        std::cerr << "[cpm] Package '" << package_name << "' not found.\n";
        return;
    }

    std::filesystem::remove_all(package_path);

    auto include_dir = local_cpm_dir_ / "include";
    if (std::filesystem::exists(include_dir)) {
        std::filesystem::remove_all(include_dir);
        std::filesystem::create_directories(include_dir);
    }

    export_package_headers();
    generate_compile_commands();
    std::cout << "[cpm] Removed " << package_name << "\n";
}

void PackageManager::update() {
    auto packages_dir = local_cpm_dir_ / "packages";
    if (std::filesystem::exists(packages_dir)) {
        std::filesystem::remove_all(packages_dir);
    }

    auto include_dir = local_cpm_dir_ / "include";
    if (std::filesystem::exists(include_dir)) {
        std::filesystem::remove_all(include_dir);
    }

    auto lib_dir = local_cpm_dir_ / "lib";
    if (std::filesystem::exists(lib_dir)) {
        std::filesystem::remove_all(lib_dir);
    }

    std::cout << "[cpm] Updating packages...\n";
    install();
}

void PackageManager::list() {
    auto packages_dir = local_cpm_dir_ / "packages";
    bool has_packages = false;

    std::cout << "[cpm] Installed packages:\n";

    if (std::filesystem::exists(packages_dir)) {
        for (const auto &entry : std::filesystem::directory_iterator(packages_dir)) {
            if (entry.is_directory() || entry.is_symlink()) {
                std::cout << "  [header] " << entry.path().filename().string() << "\n";
                has_packages = true;
            }
        }
    }

    auto lib_dir = local_cpm_dir_ / "lib";
    if (std::filesystem::exists(lib_dir)) {
        for (const auto &entry : std::filesystem::directory_iterator(lib_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".a") {
                std::cout << "  [lib]    " << entry.path().filename().string() << "\n";
                has_packages = true;
            }
        }
    }

    if (!has_packages) {
        std::cout << "  (none)\n";
    }
}

void PackageManager::setup_environment() {
    Environment env(project_root_);
    if (!env.exists()) {
        env.create();
    }
    std::cout << "[cpm] Environment ready. Run: source .cpm/activate.sh\n";
}

std::string PackageManager::get_include_flags() const { return "-I" + (local_cpm_dir_ / "include").string(); }

std::string PackageManager::get_library_flags() const {
    std::string flags;
    auto lib_dir = local_cpm_dir_ / "lib";
    if (std::filesystem::exists(lib_dir)) {
        flags += "-L" + lib_dir.string();
    }
    return flags;
}

} // namespace cpm

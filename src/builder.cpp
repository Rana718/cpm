#include "cpm/builder.hpp"

#include "cpm/config.hpp"
#include "cpm/installer.hpp"
#include "cpm/nix_env.hpp"
#include "cpm/progress.hpp"
#include "cpm/toml_parser.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace cpm {

namespace fs = std::filesystem;

// Return the nix import expression for a given nixpkgs pin.
// If pin is non-empty (e.g. "nixos-24.05"), use builtins.fetchTarball so the
// exact channel is used regardless of the host's nix channel configuration.
// This is critical: on nixpkgs-unstable, gcc13 resolves to GCC 15.
static std::string nix_pkgs_import(const std::string &pin) {
    if (!pin.empty()) {
        std::string url = "https://github.com/NixOS/nixpkgs/archive/" + pin + ".tar.gz";
        return "{ pkgs ? import (builtins.fetchTarball { url = \"" + url + "\"; }) {} }:\n";
    }
    return "{ pkgs ? import <nixpkgs> {} }:\n";
}

static const std::set<std::string> &skip_dirs() {
    static const std::set<std::string> dirs = {".cpm", ".git", "build", "_build", "_cpm_build", "dist", "node_modules"};
    return dirs;
}

Builder::Builder(fs::path project_root, fs::path local_cpm_dir, fs::path global_cache_dir)
    : project_root_(std::move(project_root)), local_cpm_dir_(std::move(local_cpm_dir)), global_cache_dir_(std::move(global_cache_dir)) {}


std::string Builder::detect_compiler(const ProjectConfig &config) const {
    if (!config.compiler.empty()) {
        std::string comp = config.compiler;
        // Map "gcc-13", "gcc-14", "gcc" → "g++" (nix-shell provides the right version)
        // Map "clang-17", "clang" → "clang++"
        if (comp == "gcc" || comp.starts_with("gcc-")) return "g++";
        if (comp == "clang" || comp.starts_with("clang-")) return "clang++";
        return comp;
    }
    std::string det = Config::get_compiler();
    if (det == "gcc") return "g++";
    if (det == "clang") return "clang++";
    return "g++";
}

fs::path Builder::get_output_path(const ProjectConfig &config) const { return project_root_ / (config.output.empty() ? config.name : config.output); }



std::set<std::string> Builder::collect_include_dirs(const ProjectConfig &config) const {
    std::set<std::string> seen;
    auto include_dir = local_cpm_dir_ / "include";

    // Seed with already-known dirs so we don't add them twice
    seen.insert(fs::weakly_canonical(include_dir).string());
    for (const auto &inc : config.include_paths) {
        auto p = fs::path(inc);
        if (p.is_relative()) p = project_root_ / p;
        seen.insert(fs::weakly_canonical(p).string());
    }

    try {
        for (const auto &entry : fs::recursive_directory_iterator(project_root_, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".h" && ext != ".hpp" && ext != ".hh" && ext != ".hxx") continue;
            auto rel = fs::relative(entry.path(), project_root_);
            if (skip_dirs().count(rel.begin()->string())) continue;
            seen.insert(fs::weakly_canonical(entry.path().parent_path()).string());
        }
    } catch (...) {
    }

    return seen;
}


std::vector<fs::path> Builder::collect_source_files(const std::string &entry_abs) const {
    std::vector<fs::path> sources;
    try {
        for (const auto &entry : fs::recursive_directory_iterator(project_root_, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".cc" && ext != ".cxx") continue;
            auto rel = fs::relative(entry.path(), project_root_);
            if (skip_dirs().count(rel.begin()->string())) continue;
            if (fs::weakly_canonical(entry.path()).string() == entry_abs) continue;
            sources.push_back(entry.path());
        }
    } catch (...) {
    }
    return sources;
}


bool Builder::backend_is_available(const std::string &stem) const {
    auto include_dir = local_cpm_dir_ / "include";
    if (stem == "imgui_impl_sdl3" || stem == "imgui_impl_sdlrenderer3" || stem == "imgui_impl_sdlgpu3") return fs::exists(include_dir / "SDL3");
    if (stem == "imgui_impl_opengl3") return true;
    if (stem == "imgui_impl_opengl2") return false; 
    if (stem == "imgui_impl_glfw") return fs::exists(include_dir / "GLFW");
    if (stem == "imgui_impl_vulkan") return fs::exists(include_dir / "vulkan");
    if (stem == "imgui_impl_null") return true;
    return false; // dx*, allegro5, android, metal, osx, win32, sdl2, wgpu
}


void Builder::auto_patch_sources() const {
    // Fix: `m_Matrix = (1.0f);` → `m_Matrix = glm::mat4(1.0f);`
    static const std::regex glm_mat_assign(R"((m_\w*[Mm]atrix\w*|m_\w*[Mm]at\w*)\s*=\s*\((\d+\.\d+f?)\)\s*;)");

    try {
        for (const auto &entry : fs::recursive_directory_iterator(project_root_, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".cc" && ext != ".cxx") continue;
            auto rel = fs::relative(entry.path(), project_root_);
            if (skip_dirs().count(rel.begin()->string())) continue;

            std::ifstream in(entry.path());
            std::string content((std::istreambuf_iterator<char>(in)), {});
            in.close();

            std::string patched = std::regex_replace(content, glm_mat_assign, "$1 = glm::mat4($2);");
            if (patched != content) {
                std::ofstream out(entry.path());
                out << patched;
                std::cout << "[cpm] auto-patched: " << entry.path().filename().string() << "\n";
            }
        }
    } catch (...) {
    }
}


std::string Builder::build_compile_command(const ProjectConfig &config) const {
    std::ostringstream cmd;
    auto include_dir = local_cpm_dir_ / "include";

    cmd << detect_compiler(config);
    cmd << " -std=c++" << config.cpp_standard;

    // Linux-only build
    cmd << " -DSEED_PLATFORM_LINUX";

    if (fs::exists(include_dir)) {
        cmd << " -I" << include_dir.string();
        for (const auto &entry : fs::directory_iterator(include_dir)) {
            if (entry.is_directory()) cmd << " -I" << entry.path().string();
        }
    }

    // Extra include paths from cpm.toml
    for (const auto &inc : config.include_paths) {
        auto p = fs::path(inc);
        if (p.is_relative()) p = project_root_ / p;
        cmd << " -I" << p.string();
    }

    // Auto-discovered header dirs
    for (const auto &dir : collect_include_dirs(config)) cmd << " -I" << dir;

    // defines.txt 
    auto defines_file = local_cpm_dir_ / "defines.txt";
    if (!fs::exists(defines_file)) {
        for (const auto &entry : fs::directory_iterator(local_cpm_dir_)) {
            if (!entry.is_directory()) continue;
            auto df = entry.path() / "defines.txt";
            if (fs::exists(df)) {
                defines_file = df;
                break;
            }
        }
    }
    if (fs::exists(defines_file) && fs::is_regular_file(defines_file)) {
        std::ifstream df(defines_file);
        std::string line;
        while (std::getline(df, line))
            if (!line.empty() && line[0] == '-') cmd << " " << line;
    }

    //  Entry file handling 
    std::string actual_entry;
    {
        auto entry_ext = fs::path(config.entry).extension().string();
        bool is_header = (entry_ext == ".h" || entry_ext == ".hpp" || entry_ext == ".hh");

        if (is_header) {
            // If there are existing .cpp files, they presumably include this header.
            // Don't create a wrapper to avoid duplicate main().
            bool has_cpp = !collect_source_files(fs::weakly_canonical(project_root_ / config.entry).string()).empty();

            if (!has_cpp) {
                auto wrapper = local_cpm_dir_ / "_entry.cpp";
                auto abs = fs::weakly_canonical(project_root_ / config.entry);
                std::ofstream wf(wrapper);
                wf << "#include \"" << abs.string() << "\"\n";
                actual_entry = wrapper.string();
            }
        } else {
            actual_entry = fs::weakly_canonical(project_root_ / config.entry).string();
        }
    }
    if (!actual_entry.empty()) cmd << " " << actual_entry;

    //  ImGui backends 
    auto backends_dir = include_dir / "backends";
    std::set<std::string> extra_src_abs;

    if (fs::exists(backends_dir)) {
        for (const auto &entry : fs::directory_iterator(backends_dir)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            auto stem = entry.path().stem().string();
            if (ext != ".cpp" && ext != ".cc" && ext != ".cxx") continue;
            if (!fs::exists(backends_dir / (stem + ".h"))) continue;
            if (!backend_is_available(stem)) continue;
            auto abs = fs::weakly_canonical(entry.path()).string();
            if (extra_src_abs.insert(abs).second) cmd << " " << entry.path().string();
        }
    }

    // imgui*.cpp files in .cpm/include/ root (imgui.cpp, imgui_draw.cpp, …)
    if (fs::exists(include_dir)) {
        for (const auto &entry : fs::directory_iterator(include_dir)) {
            if (!entry.is_regular_file()) continue;
            auto name = entry.path().filename().string();
            if (entry.path().extension() == ".cpp" && name.starts_with("imgui")) {
                auto abs = fs::weakly_canonical(entry.path()).string();
                if (extra_src_abs.insert(abs).second) cmd << " " << entry.path().string();
            }
        }
    }

    // Extra sources from cpm.toml
    for (const auto &src : config.extra_sources) {
        auto p = fs::path(src);
        if (p.is_relative()) p = project_root_ / p;
        auto abs = fs::weakly_canonical(p).string();
        if (extra_src_abs.insert(abs).second) cmd << " " << p.string();
    }

    // All project .cpp files (excluding .cpm/, build dirs, already-added)
    auto entry_abs = actual_entry.empty() ? fs::weakly_canonical(project_root_ / config.entry).string() : actual_entry;
    for (const auto &src : collect_source_files(entry_abs)) {
        auto abs = fs::weakly_canonical(src).string();
        if (!extra_src_abs.count(abs)) cmd << " " << src.string();
    }

    cmd << " -o " << get_output_path(config).string();

    //  Link .cpm/lib/ 
    auto lib_dir = local_cpm_dir_ / "lib";
    if (fs::exists(lib_dir)) {
        cmd << " -L" << lib_dir.string();

        std::set<std::string> linked;

        // Static .a files
        for (const auto &entry : fs::directory_iterator(lib_dir)) {
            if (!entry.is_regular_file() && !entry.is_symlink()) continue;
            if (entry.path().extension() != ".a") continue;
            auto filename = entry.path().filename().string();
            if (filename.starts_with("lib")) {
                auto lib_name = filename.substr(3, filename.size() - 5);
                if (linked.insert(lib_name).second) cmd << " -l" << lib_name;
            } else {
                cmd << " " << entry.path().string();
            }
        }

        // Explicit [libs] shared libraries (don't auto-link every .so)
        static const std::set<std::string> nix_name_map_keys = {"opengl", "glew", "sdl2", "SDL2", "sdl3", "SDL3", "vulkan"};
        for (const auto &nixlib : config.nix_libraries) {
            std::string lib_name = nixlib.name;
            if (lib_name == "opengl")
                lib_name = "GL";
            else if (lib_name == "glew")
                lib_name = "GLEW";
            else if (lib_name == "sdl2" || lib_name == "SDL2")
                lib_name = "SDL2";
            else if (lib_name == "sdl3" || lib_name == "SDL3")
                lib_name = "SDL3";
            else if (lib_name == "vulkan")
                lib_name = "vulkan";
            if (linked.insert(lib_name).second) cmd << " -l" << lib_name;
        }

        cmd << " -lz -lpthread -ldl -lrt -latomic";
    }

    return cmd.str();
}


void Builder::bundle_production(const ProjectConfig &config) const {
    auto out = get_output_path(config);
    auto dist_dir = project_root_ / "dist";
    fs::create_directories(dist_dir);

    fs::copy(out, dist_dir / out.filename(), fs::copy_options::overwrite_existing);

    // Strip binary
    std::system(("strip " + out.string() + " 2>/dev/null").c_str());

    // Bundle nix .so dependencies (exclude glibc and GPU driver interface)
    // Set LD_LIBRARY_PATH so ldd can resolve libs symlinked from .cpm/lib/
    std::string cpm_lib = (local_cpm_dir_ / "lib").string();
    std::string ldd_cmd = "LD_LIBRARY_PATH=" + cpm_lib + ":$LD_LIBRARY_PATH"
                          " ldd " + (dist_dir / out.filename()).string() +
                          " 2>/dev/null | grep '=>' | awk '{print $3}'"
                          " | grep -E '/nix/store/|" + cpm_lib + "'"
                          " | grep -v -E 'libc\\.so|libc-|libm\\.so|libm-|libpthread|libdl\\.so|librt\\.so"
                          "|libGL\\.so|libGLX\\.so|libGLdispatch|libBrokenLocale|libnss|libresolv|libmvec'"
                          " | while read lib; do"
                          "   real=$(readlink -f \"$lib\" 2>/dev/null || echo \"$lib\");"
                          "   [ -f \"$real\" ] && cp \"$real\" " + dist_dir.string() + "/ 2>/dev/null;"
                          // Also copy the soname symlink (e.g. libfoo.so.1 → libfoo.so.1.2.3)
                          // so the dynamic linker can find it by its soname.
                          "   soname=$(objdump -p \"$real\" 2>/dev/null | awk '/SONAME/{print $2}');"
                          "   if [ -n \"$soname\" ] && [ ! -e \"" + dist_dir.string() + "/$soname\" ]; then"
                          "     ln -s \"$(basename \"$real\")\" \"" + dist_dir.string() + "/$soname\" 2>/dev/null;"
                          "   fi;"
                          " done";
    std::system(ldd_cmd.c_str());

    // Patch interpreter and rpath with patchelf for portability
    auto dist_bin = dist_dir / out.filename();
    std::system(("patchelf --set-interpreter /lib64/ld-linux-x86-64.so.2 " + dist_bin.string() + " 2>/dev/null").c_str());
    std::system(("patchelf --set-rpath '$ORIGIN' " + dist_bin.string() + " 2>/dev/null").c_str());

    // run.sh launcher
    auto run_script = dist_dir / "run.sh";
    {
        std::ofstream rs(run_script);
        rs << "#!/bin/bash\n"
           << "DIR=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n"
           << "export LD_LIBRARY_PATH=\"$DIR:$LD_LIBRARY_PATH\"\n"
           << "exec \"$DIR/" << out.filename().string() << "\" \"$@\"\n";
    }
    fs::permissions(run_script, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec);

    auto size = fs::file_size(dist_dir / out.filename());
    std::cout << "[cpm] Built: " << out.filename().string() << " (" << (size / 1024 / 1024) << " MB, static)\n"
              << "[cpm] Bundle: dist/ (portable, copy to any Linux)\n"
              << "[cpm]   Run with: ./dist/run.sh\n";
}


int Builder::build(bool static_build) {
    auto toml_path = project_root_ / "cpm.toml";
    if (!fs::exists(toml_path)) throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");

    auto config = TomlParser::parse(toml_path);
    if (config.entry.empty()) throw std::runtime_error("No 'entry' specified in [project] in cpm.toml");

    auto entry_path = project_root_ / config.entry;
    if (!fs::exists(entry_path)) throw std::runtime_error("Entry file not found: " + config.entry);

    auto_patch_sources();
    std::string compile_cmd = build_compile_command(config);

    std::string compiler = detect_compiler(config);
    std::string detail = "c++" + config.cpp_standard + " · " + compiler;

    if (static_build) {
        auto spc = compile_cmd.find(' ');
        // Don't use -static-libgcc/-static-libstdc++ when system_dependencies are
        // present — packages like Seastar redefine unwinding symbols and conflict
        // with libgcc_eh.a, causing "multiple definition of _Unwind_RaiseException".
        bool has_sys_deps = !config.system_dependencies.empty();
        if (spc != std::string::npos)
            // Avoid -march=x86-64-v2 and stack protector flags when system_dependencies
            // are present: pre-built nix libraries (GCC 13) have a different stack
            // canary ABI than GCC 16, causing stack smashing false positives.
            compile_cmd.insert(spc, " -O3 -DNDEBUG -DGLEW_NO_GLU"
                + std::string(has_sys_deps ? " -fno-stack-protector" : " -march=x86-64-v2")
                + std::string(has_sys_deps ? "" : " -static-libgcc -static-libstdc++"));

        // Replace -l<name> with full .a path for static linking
        auto lib_dir = local_cpm_dir_ / "lib";
        if (fs::exists(lib_dir)) {
            for (const auto &entry : fs::directory_iterator(lib_dir)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() != ".a") continue;
                auto filename = entry.path().filename().string();
                if (!filename.starts_with("lib")) continue;
                auto lib_name = filename.substr(3, filename.size() - 5);
                std::string flag = "-l" + lib_name;
                auto pos = compile_cmd.find(flag);
                if (pos != std::string::npos) {
                    auto after = pos + flag.size();
                    if (after >= compile_cmd.size() || compile_cmd[after] == ' ') compile_cmd.replace(pos, flag.size(), entry.path().string());
                }
            }
        }

        BuildSpinner spinner(config.name, detail + " · optimized");
        NixEnv nix(local_cpm_dir_, global_cache_dir_);
        int ret = -1;

        if (nix.available()) {
            // Generate a minimal mkShell (not mkDerivation) so the compiler
            // is on PATH when the compile command runs inside nix-shell.
            auto prod_shell = local_cpm_dir_ / "prod_shell.nix";
            {
                std::string nix_compiler = "gcc13";
                if (!config.compiler.empty()) {
                    // compiler field like "gcc-13" → "gcc13", "clang-17" → "clang_17"
                    auto &c = config.compiler;
                    if (c.find("clang") != std::string::npos) {
                        auto d = c.find('-');
                        nix_compiler = d != std::string::npos ? "clang_" + c.substr(d + 1) : "clang";
                    } else {
                        auto d = c.find('-');
                        nix_compiler = d != std::string::npos ? "gcc" + c.substr(d + 1) : "gcc";
                    }
                }
                std::ofstream f(prod_shell);
                f << nix_pkgs_import(config.nixpkgs)
                  << "pkgs.mkShell {\n"
                  << "  buildInputs = with pkgs; [ " << nix_compiler << " pkg-config zlib";
                for (const auto &nixlib : config.nix_libraries) f << " " << nixlib.nix_attr;
                f << " ];\n}\n";
            }
            ret = std::system(("nix-shell " + prod_shell.string() + " --run '" + compile_cmd + "' 2>&1 >/dev/null").c_str());

            if (ret != 0) ret = std::system((compile_cmd + " 2>&1 >/dev/null").c_str());
        } else {
            ret = std::system((compile_cmd + " 2>&1 >/dev/null").c_str());
        }

        // Fallback: dynamic + bundle
        if (ret != 0) {
            compile_cmd = build_compile_command(config);
            auto spc2 = compile_cmd.find(' ');
            if (spc2 != std::string::npos) compile_cmd.insert(spc2, " -O3 -DNDEBUG"
                + std::string(config.system_dependencies.empty() ? " -march=x86-64-v2 -static-libgcc -static-libstdc++" : ""));
            ret = std::system((compile_cmd + " 2>&1 >/dev/null").c_str());
        }

        spinner.finish(ret == 0);

        if (ret != 0) return 1;
        bundle_production(config);
        return 0;
    }

    //  Regular build
    // All deps are already built and their artifacts are in .cpm/include and
    // .cpm/lib.  We only need a nix-shell wrapper for the final compile if:
    //   • nix is available, AND
    //   • the project uses [libs] or [system-dependencies], AND
    //   • the compiler requested in cpm.toml differs from the system compiler
    //     (i.e., the user wants a specific nix-provided compiler version).
    // In all other cases, run the compile command directly.
    NixEnv nix(local_cpm_dir_, global_cache_dir_);

    // Build a project-level shell.nix in .cpm/ so the correct nix compiler is
    // on PATH when linking against the artifacts in .cpm/lib.
    fs::path shell_nix_path;
    bool needs_nix_compiler = !config.compiler.empty() &&
                              (config.compiler.find('-') != std::string::npos); // e.g. "gcc-13"
    if (nix.available() && (!config.system_dependencies.empty() || !config.nix_libraries.empty() || needs_nix_compiler)) {
        // Derive nix compiler attr from cpm.toml compiler field
        std::string nix_compiler = "gcc13"; // default
        if (!config.compiler.empty()) {
            auto &c = config.compiler;
            if (c.find("clang") != std::string::npos) {
                auto d = c.find('-');
                nix_compiler = d != std::string::npos ? "clang_" + c.substr(d + 1) : "clang";
            } else {
                auto d = c.find('-');
                nix_compiler = d != std::string::npos ? "gcc" + c.substr(d + 1) : "gcc";
            }
        }
        auto proj_shell = local_cpm_dir_ / "project_shell.nix";
        {
            std::ofstream f(proj_shell);
            f << nix_pkgs_import(config.nixpkgs)
              << "pkgs.mkShell {\n"
              << "  buildInputs = with pkgs; [ " << nix_compiler << " pkg-config zlib";
            for (const auto &nixlib : config.nix_libraries) f << " " << nixlib.nix_attr;
            // Add transitive deps from each system_dependency's shell.nix so
            // the nix linker can find gnutls, hwloc, etc. during the user's build.
            for (const auto &dep : config.system_dependencies) {
                auto dep_shell = global_cache_dir_ / (dep.name + "-" + dep.version + "-src") / "shell.nix";
                if (!fs::exists(dep_shell)) {
                    // Try resolved version
                    for (const auto &entry : fs::directory_iterator(global_cache_dir_)) {
                        auto name = entry.path().filename().string();
                        if (name.rfind(dep.name + "-", 0) == 0 && name.find("-src") != std::string::npos) {
                            dep_shell = entry.path() / "shell.nix";
                            break;
                        }
                    }
                }
                if (!fs::exists(dep_shell)) continue;
                // Parse buildInputs from the dep's shell.nix and include them
                std::ifstream sf(dep_shell);
                std::string line;
                bool in_inputs = false;
                while (std::getline(sf, line)) {
                    if (line.find("buildInputs") != std::string::npos) { in_inputs = true; }
                    if (!in_inputs) continue;
                    if (line.find("];") != std::string::npos) { in_inputs = false; break; }
                    // Extract package names (words that look like nix attrs)
                    std::istringstream ss(line);
                    std::string tok;
                    while (ss >> tok) {
                        // Skip keywords and the compiler (already added)
                        if (tok == "with" || tok == "pkgs;" || tok == "[" || tok == "]"
                            || tok == "buildInputs" || tok == "=" || tok.find("gcc") == 0
                            || tok.find("clang") == 0 || tok == "cmake" || tok == "ninja"
                            || tok == "pkg-config" || tok == "automake" || tok == "autoconf"
                            || tok == "libtool" || tok == "python3" || tok == "git"
                            || tok == "stow" || tok == "meson" || tok == "valgrind"
                            || tok == "xorg.utilmacros" || tok == "xfsprogs"
                            || tok.find("pyelftools") != std::string::npos)
                            continue;
                        if (!tok.empty() && tok[0] != '#' && tok.find('"') == std::string::npos)
                            f << " " << tok;
                    }
                }
            }
            f << " ];\n}\n";
        }
        shell_nix_path = proj_shell;
    }

    BuildSpinner spinner(config.name, detail);

    int ret;
    if (!shell_nix_path.empty()) {
        ret = std::system(("nix-shell " + shell_nix_path.string() + " --run '" + compile_cmd + "' 2>&1 >/dev/null").c_str());
    } else {
        ret = std::system((compile_cmd + " 2>&1 >/dev/null").c_str());
    }

    spinner.finish(ret == 0);

    if (ret != 0) {
        // Re-run without output suppression so the user sees the compiler error
        std::cerr << "\n[cpm] Compiler output:\n";
        if (!shell_nix_path.empty()) {
            std::system(("nix-shell " + shell_nix_path.string() + " --run '" + compile_cmd + "'").c_str());
        } else {
            std::system(compile_cmd.c_str());
        }
        return 1;
    }
    return 0;
}


int Builder::run() {
    auto toml_path = project_root_ / "cpm.toml";
    if (!fs::exists(toml_path)) throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");

    auto config = TomlParser::parse(toml_path);

    // Install if any deps are missing
    bool needs_install = false;
    if (!config.git_dependencies.empty() || !config.system_dependencies.empty()) {
        auto packages_dir = local_cpm_dir_ / "packages";
        auto include_dir = local_cpm_dir_ / "include";

        if (!fs::exists(packages_dir) && !config.git_dependencies.empty()) {
            needs_install = true;
        } else if (!fs::exists(include_dir) || fs::is_empty(include_dir)) {
            needs_install = true;
        } else {
            for (const auto &dep : config.git_dependencies) {
                auto pkg = packages_dir / dep.name;
                if (!fs::exists(pkg) && !fs::is_symlink(pkg)) {
                    needs_install = true;
                    break;
                }
            }
        }
    }

    if (needs_install) {
        Installer installer(project_root_, local_cpm_dir_, global_cache_dir_);
        installer.install();
    }

    int build_result = build();
    if (build_result != 0) return build_result;

    auto output_path = get_output_path(config);
    std::cout << "\n[cpm] Running " << output_path.filename().string() << "...\n"
              << "────────────────────────────────────────\n";
    std::cout.flush();

    // Prepend .cpm/lib/ to LD_LIBRARY_PATH so nix-store .so symlinks are found
    // at runtime without needing a nix-shell or system-wide installation.
    std::string run_cmd = "LD_LIBRARY_PATH=" + (local_cpm_dir_ / "lib").string()
                        + ":$LD_LIBRARY_PATH " + output_path.string();
    int ret = std::system(run_cmd.c_str());
    return WEXITSTATUS(ret);
}


int Builder::run_file(const std::string &file) {
    auto file_path = project_root_ / file;
    if (!fs::exists(file_path)) {
        std::cerr << "[cpm] File not found: " << file << "\n";
        return 1;
    }

    auto ext = fs::path(file).extension().string();
    bool is_cpp = (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".C");
    bool is_c = (ext == ".c");

    if (!is_cpp && !is_c) {
        std::cerr << "[cpm] Unsupported file type: " << ext << " (use .c, .cpp, .cc, .cxx)\n";
        return 1;
    }

    auto out_path = project_root_ / fs::path(file).stem();

    std::string compiler, std_flag, include_flag, defines;
    auto toml_path = project_root_ / "cpm.toml";

    if (fs::exists(toml_path)) {
        auto config = TomlParser::parse(toml_path);
        compiler = is_cpp ? "g++" : "gcc";
        if (!config.compiler.empty() && config.compiler.find("clang") != std::string::npos) compiler = is_cpp ? "clang++" : "clang";

        std_flag = is_cpp ? " -std=c++" + config.cpp_standard : " -std=c11";

        auto include_dir = local_cpm_dir_ / "include";
        if (fs::exists(include_dir)) include_flag = " -I" + include_dir.string();

        auto defines_file = local_cpm_dir_ / "defines.txt";
        if (fs::exists(defines_file)) {
            std::ifstream df(defines_file);
            std::string line;
            while (std::getline(df, line))
                if (!line.empty() && line[0] == '-') defines += " " + line;
        }
    } else {
        compiler = is_cpp ? "g++" : "gcc";
        std_flag = is_cpp ? " -std=c++20" : " -std=c11";
    }

    std::string cmd = compiler + std_flag + include_flag + defines + " " + file + " -o " + out_path.string();

    // Wrap in nix-shell if project uses nix (use project-level shell, not a dep's shell)
    NixEnv nix(local_cpm_dir_, global_cache_dir_);
    fs::path shell_nix;
    if (nix.available() && fs::exists(toml_path)) {
        // Reuse project_shell.nix if already generated by cpm build/install
        auto proj_shell = local_cpm_dir_ / "project_shell.nix";
        if (fs::exists(proj_shell)) shell_nix = proj_shell;
    }

    BuildSpinner spinner(fs::path(file).stem().string(), std::string(is_cpp ? "c++" : "c") + " · " + compiler);
    int ret;
    if (!shell_nix.empty()) {
        ret = std::system(("nix-shell " + shell_nix.string() + " --run '" + cmd + "' >/dev/null 2>&1").c_str());
    } else {
        ret = std::system((cmd + " >/dev/null 2>&1").c_str());
    }

    spinner.finish(ret == 0);

    if (ret != 0) {
        std::cerr << "\n[cpm] Compiler output:\n";
        if (!shell_nix.empty())
            std::system(("nix-shell " + shell_nix.string() + " --run '" + cmd + "'").c_str());
        else
            std::system(cmd.c_str());
        return 1;
    }

    std::cout << "────────────────────────────────────────\n";
    std::cout.flush();
    ret = std::system(out_path.string().c_str());
    fs::remove(out_path);
    return WEXITSTATUS(ret);
}


int Builder::start() {
    auto toml_path = project_root_ / "cpm.toml";
    if (!fs::exists(toml_path)) throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");

    auto config = TomlParser::parse(toml_path);
    auto output_path = get_output_path(config);

    if (!fs::exists(output_path)) {
        std::cout << "[cpm] Binary not found, building...\n";
        int r = build();
        if (r != 0) return r;
    }

    std::string run_cmd = config.start_script.empty() ? output_path.string() : config.start_script;

    std::cout << "[cpm] Starting: " << run_cmd << "\n"
              << "────────────────────────────────────────\n";
    std::cout.flush();

    int ret = std::system(run_cmd.c_str());
    return WEXITSTATUS(ret);
}

} // namespace cpm

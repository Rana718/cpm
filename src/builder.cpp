#include "cpm/builder.hpp"

#include "cpm/config.hpp"
#include "cpm/installer.hpp"
#include "cpm/nix_env.hpp"
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

static const std::set<std::string> &skip_dirs() {
    static const std::set<std::string> dirs = {".cpm", ".git", "build", "_build", "_cpm_build", "dist", "node_modules"};
    return dirs;
}

Builder::Builder(fs::path project_root, fs::path local_cpm_dir, fs::path global_cache_dir)
    : project_root_(std::move(project_root)), local_cpm_dir_(std::move(local_cpm_dir)), global_cache_dir_(std::move(global_cache_dir)) {}

// ─── detect_compiler ────────────────────────────────────────────────────────

std::string Builder::detect_compiler(const ProjectConfig &config) const {
    if (!config.compiler.empty()) return config.compiler;
    std::string det = Config::get_compiler();
    if (det == "gcc") return "g++";
    if (det == "clang") return "clang++";
    return "g++";
}

fs::path Builder::get_output_path(const ProjectConfig &config) const { return project_root_ / (config.output.empty() ? config.name : config.output); }

// ─── find_shell_nix ─────────────────────────────────────────────────────────

fs::path Builder::find_shell_nix() const {
    if (!fs::exists(global_cache_dir_)) return {};
    for (const auto &entry : fs::directory_iterator(global_cache_dir_)) {
        if (entry.path().filename().string().find("-src") == std::string::npos) continue;
        auto snix = entry.path() / "shell.nix";
        if (fs::exists(snix)) return snix;
    }
    return {};
}

// ─── collect_include_dirs ───────────────────────────────────────────────────

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

// ─── collect_source_files ───────────────────────────────────────────────────

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

// ─── backend_is_available ───────────────────────────────────────────────────

bool Builder::backend_is_available(const std::string &stem) const {
    auto include_dir = local_cpm_dir_ / "include";
    if (stem == "imgui_impl_sdl3" || stem == "imgui_impl_sdlrenderer3" || stem == "imgui_impl_sdlgpu3") return fs::exists(include_dir / "SDL3");
    if (stem == "imgui_impl_opengl3") return true;
    if (stem == "imgui_impl_opengl2") return false; // conflicts with opengl3
    if (stem == "imgui_impl_glfw") return fs::exists(include_dir / "GLFW");
    if (stem == "imgui_impl_vulkan") return fs::exists(include_dir / "vulkan");
    if (stem == "imgui_impl_null") return true;
    return false; // dx*, allegro5, android, metal, osx, win32, sdl2, wgpu
}

// ─── auto_patch_sources ─────────────────────────────────────────────────────

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

// ─── build_compile_command ───────────────────────────────────────────────────

std::string Builder::build_compile_command(const ProjectConfig &config) const {
    std::ostringstream cmd;
    auto include_dir = local_cpm_dir_ / "include";

    cmd << detect_compiler(config);
    cmd << " -std=c++" << config.cpp_standard;

#if defined(__linux__)
    cmd << " -DSEED_PLATFORM_LINUX";
#elif defined(_WIN32)
    cmd << " -DSEED_PLATFORM_WINDOWS";
#elif defined(__APPLE__)
    cmd << " -DSEED_PLATFORM_MACOS";
#endif

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

    // defines.txt (e.g. Seastar flags)
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

    // ── Entry file handling ───────────────────────────────────────────────
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
            // else: no entry file — .cpp files include the header themselves
        } else {
            actual_entry = fs::weakly_canonical(project_root_ / config.entry).string();
        }
    }
    if (!actual_entry.empty()) cmd << " " << actual_entry;

    // ── ImGui backends ────────────────────────────────────────────────────
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

    // ── Output ────────────────────────────────────────────────────────────
    cmd << " -o " << get_output_path(config).string();

    // ── Link .cpm/lib/ ────────────────────────────────────────────────────
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

// ─── bundle_production ───────────────────────────────────────────────────────

void Builder::bundle_production(const ProjectConfig &config) const {
    auto out = get_output_path(config);
    auto dist_dir = project_root_ / "dist";
    fs::create_directories(dist_dir);

    fs::copy(out, dist_dir / out.filename(), fs::copy_options::overwrite_existing);

    // Strip binary
    std::system(("strip " + out.string() + " 2>/dev/null").c_str());

    // Bundle nix .so dependencies (exclude glibc and GPU driver interface)
    std::string ldd_cmd = "ldd " + (dist_dir / out.filename()).string() +
                          " 2>/dev/null | grep '=>' | awk '{print $3}'"
                          " | grep -E '/nix/store/'"
                          " | grep -v -E 'libc\\.so|libc-|libm\\.so|libm-|libpthread|libdl\\.so|librt\\.so"
                          "|libGL\\.so|libGLX\\.so|libGLdispatch|libBrokenLocale|libnss|libresolv|libmvec'"
                          " | while read lib; do"
                          "   [ -f \"$lib\" ] && cp \"$lib\" " +
                          dist_dir.string() +
                          "/ 2>/dev/null;"
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

// ─── build ───────────────────────────────────────────────────────────────────

int Builder::build(bool static_build) {
    auto toml_path = project_root_ / "cpm.toml";
    if (!fs::exists(toml_path)) throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");

    auto config = TomlParser::parse(toml_path);
    if (config.entry.empty()) throw std::runtime_error("No 'entry' specified in [project] in cpm.toml");

    auto entry_path = project_root_ / config.entry;
    if (!fs::exists(entry_path)) throw std::runtime_error("Entry file not found: " + config.entry);

    auto_patch_sources();
    std::string compile_cmd = build_compile_command(config);

    // ── Production build ──────────────────────────────────────────────────
    if (static_build) {
        std::cout << "[cpm] Building " << config.name << " (production, static)...\n[cpm] " << detect_compiler(config) << " | c++" << config.cpp_standard << " | " << Config::get_architecture()
                  << " | optimized\n";

        // Insert optimisation flags right after the compiler name
        auto spc = compile_cmd.find(' ');
        if (spc != std::string::npos)
            compile_cmd.insert(spc, " -O3 -DNDEBUG -DGLEW_NO_GLU -march=x86-64-v2"
                                    " -static-libgcc -static-libstdc++");

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

        NixEnv nix(local_cpm_dir_, global_cache_dir_);
        int ret = -1;

        if (nix.available()) {
            // Pinned nixos-24.05 for glibc 2.39 compatibility
            auto prod_shell = local_cpm_dir_ / "prod_shell.nix";
            {
                std::ofstream f(prod_shell);
                f << "{ pkgs ? import (fetchTarball {\n"
                  << "    url = \"https://github.com/NixOS/nixpkgs/archive/nixos-24.05.tar.gz\";\n"
                  << "  }) {} }:\n"
                  << "pkgs.mkShell {\n"
                  << "  buildInputs = with pkgs; [ gcc13 pkg-config zlib";
                for (const auto &nixlib : config.nix_libraries) f << " " << nixlib.nix_attr;
                f << " ];\n}\n";
            }
            std::string nix_cmd = "nix-shell " + prod_shell.string() + " --run '" + compile_cmd + "' 2>&1";
            ret = std::system(nix_cmd.c_str());
            if (ret != 0) {
                std::cout << "[cpm] Nix production build failed, "
                             "falling back to system compiler...\n";
                ret = std::system(compile_cmd.c_str());
            }
        } else {
            ret = std::system(compile_cmd.c_str());
        }

        // Fallback to dynamic+bundle if full static fails
        if (ret != 0) {
            std::cout << "[cpm] Static linking failed, falling back to dynamic + bundle...\n";
            compile_cmd = build_compile_command(config);
            auto spc2 = compile_cmd.find(' ');
            if (spc2 != std::string::npos) compile_cmd.insert(spc2, " -O3 -DNDEBUG -march=x86-64-v2 -static-libgcc -static-libstdc++");
            ret = std::system(compile_cmd.c_str());
        }

        if (ret != 0) {
            std::cerr << "[cpm] Production build failed.\n";
            return 1;
        }

        bundle_production(config);
        return 0;
    }

    // ── Regular build ─────────────────────────────────────────────────────
    std::cout << "[cpm] Building " << config.name << "...\n"
              << "[cpm] " << detect_compiler(config) << " | c++" << config.cpp_standard << " | " << Config::get_architecture() << "\n";

    // Wrap in nix-shell if a system-dep used nix (for linker access)
    NixEnv nix(local_cpm_dir_, global_cache_dir_);
    fs::path shell_nix_path;
    if (nix.available() && !config.system_dependencies.empty()) shell_nix_path = find_shell_nix();

    std::cout.flush();
    int ret;
    if (!shell_nix_path.empty()) {
        ret = std::system(("nix-shell " + shell_nix_path.string() + " --run '" + compile_cmd + "' 2>&1").c_str());
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

// ─── run ─────────────────────────────────────────────────────────────────────

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

    int ret = std::system(output_path.string().c_str());
    return WEXITSTATUS(ret);
}

// ─── run_file ────────────────────────────────────────────────────────────────

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

    std::cout << "[cpm] " << compiler << " " << file << "\n";

    // Wrap in nix-shell if project uses nix
    NixEnv nix(local_cpm_dir_, global_cache_dir_);
    fs::path shell_nix;
    if (nix.available() && fs::exists(toml_path)) shell_nix = find_shell_nix();

    int ret;
    if (!shell_nix.empty())
        ret = std::system(("nix-shell " + shell_nix.string() + " --run '" + cmd + "' 2>&1").c_str());
    else
        ret = std::system(cmd.c_str());

    if (ret != 0) {
        std::cerr << "[cpm] Compilation failed.\n";
        return 1;
    }

    std::cout << "────────────────────────────────────────\n";
    std::cout.flush();
    ret = std::system(out_path.string().c_str());
    fs::remove(out_path);
    return WEXITSTATUS(ret);
}

// ─── start ───────────────────────────────────────────────────────────────────

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

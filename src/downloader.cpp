#include "cpm/downloader.hpp"

#include "cpm/nix_env.hpp"
#include "cpm/toml_parser.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpm {

// Helpers 

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

static void cp_r(const fs::path &src, const fs::path &dst) {
    if (!fs::exists(src)) return;
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (const auto &entry : fs::directory_iterator(src, ec)) {
        if (ec) break;
        fs::copy(entry.path(), dst / entry.path().filename(),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
}

static void cp_rn(const fs::path &src, const fs::path &dst) {
    if (!fs::exists(src)) return;
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (const auto &entry : fs::directory_iterator(src, ec)) {
        if (ec) break;
        auto target = dst / entry.path().filename();
        if (fs::exists(target)) continue;
        fs::copy(entry.path(), target,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
}

// Constructor 

Downloader::Downloader(fs::path local_cpm_dir, fs::path global_cache_dir) : local_cpm_dir_(std::move(local_cpm_dir)), global_cache_dir_(std::move(global_cache_dir)) {}

// Cache helpers 

bool Downloader::is_cached(const std::string &name, const std::string &version) const { return fs::exists(get_cache_path(name, version)); }

fs::path Downloader::get_cache_path(const std::string &name, const std::string &version) const { return global_cache_dir_ / (name + "-" + version); }

void Downloader::link_from_cache(const std::string &name, const std::string &version) {
    auto cache_path = get_cache_path(name, version);
    auto local_path = local_cpm_dir_ / "packages" / name;

    if (fs::exists(local_path) || fs::is_symlink(local_path)) fs::remove_all(local_path);

    fs::create_directory_symlink(cache_path, local_path);
}


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

// Header-only clone 

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

// Compiled dependency 

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
        // Re-link nix packages for cached builds
        NixEnv nix(local_cpm_dir_, global_cache_dir_);
        if (nix.available()) {
            auto src_cache = global_cache_dir_ / (dep.name + "-" + version + "-src");
            auto deps = nix.detect_nix_deps(src_cache);
            nix.link_nix_packages(deps, local_cpm_dir_ / "include", local_cpm_dir_ / "lib");
        }
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

    // Nix provides dependency headers (boost, fmt, yaml-cpp, etc.) that
    // aren't in the built cache. Symlink those nix store paths into .cpm/include/
    // so user project compiles can find them without nix-shell.
    NixEnv nix(local_cpm_dir_, global_cache_dir_);
    if (nix.available()) {
        auto deps = nix.detect_nix_deps(built_cache);
        // Re-detect from source to get Boost which might not be in built_path
        auto src_cache = global_cache_dir_ / (dep.name + "-" + dep.version + "-src");
        if (fs::exists(src_cache)) {
            auto src_deps = nix.detect_nix_deps(src_cache);
            deps.insert(deps.end(), src_deps.begin(), src_deps.end());
        }
        nix.link_nix_packages(deps, local_cpm_dir_ / "include", local_cpm_dir_ / "lib");
    }

    std::cout << "[cpm] " << dep.name << " (built and installed)\n";
}

// install_built_library 

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

    // Read -D flags from the build cache's defines.txt.
    // Skip any -l or other non-compile flags that may have been written by a
    // previous install run — the cache should only hold -D and -I flags.
    std::vector<std::string> all_flags;
    std::set<std::string> seen_flags;

    auto src_defines = built_path / "defines.txt";
    if (fs::exists(src_defines)) {
        std::ifstream df(src_defines);
        std::string fl;
        while (std::getline(df, fl)) {
            if (fl.empty()) continue;
            // Only keep compile-time flags: -D and -I
            if (fl.rfind("-D", 0) != 0 && fl.rfind("-I", 0) != 0) continue;
            if (seen_flags.insert(fl).second) all_flags.push_back(fl);
        }
    }

    // Parse .pc files from the installed package and collect transitive
    // compile flags (-I, -D).  For packages like Seastar the .pc files reference
    // nix store paths that the user's compiler can't find without explicit flags.
    auto pc_dir = built_path / "lib" / "pkgconfig";
    {
        std::vector<std::string> pc_flags;
        std::vector<fs::path>   nix_incs;
        collect_pc_flags(pc_dir, pc_flags, nix_incs);

        // Append new flags that weren't already in defines.txt
        for (const auto &f : pc_flags)
            if (seen_flags.insert(f).second) all_flags.push_back(f);

        // Symlink each nix-store include dir's top-level entries into .cpm/include/
        // so the user's project compiles without a nix environment active.
        auto dst_include = local_cpm_dir_ / "include";
        fs::create_directories(dst_include);
        for (const auto &nix_inc : nix_incs) {
            try {
                for (const auto &sub : fs::directory_iterator(nix_inc)) {
                    auto target = dst_include / sub.path().filename();
                    if (fs::exists(target) || fs::is_symlink(target)) continue;
                    if (sub.is_directory())
                        fs::create_symlink(fs::absolute(sub.path()), target);
                    else
                        fs::copy(sub.path(), target, fs::copy_options::skip_existing);
                }
            } catch (...) {}
        }

        // Symlink only the specific nix-store .so/.a files referenced as absolute
        // paths in Libs:/Libs.private: lines, plus their unversioned .so siblings.
        // Also emit -l<name> flags into defines.txt so the builder links them
        // without having to scan .cpm/lib/ for symlinks.
        for (const auto &f : pc_flags) {
            if (f.rfind("/nix/store/", 0) != 0) continue;
            fs::path so(f);
            if (!fs::exists(so)) continue;
            auto ext   = so.extension().string();
            auto fname = so.filename().string();
            bool is_lib = ext == ".a" || ext == ".so" ||
                          fname.find(".so.") != std::string::npos;
            if (!is_lib) continue;

            // Symlink the exact file
            auto target = dst_lib / fname;
            if (!fs::exists(target) && !fs::is_symlink(target))
                fs::create_symlink(fs::absolute(so), target);

            // Also symlink every sibling .so* in the same dir (unversioned links)
            // so the linker can find libfmt.so when we pass -lfmt.
            try {
                for (const auto &sib : fs::directory_iterator(so.parent_path())) {
                    auto sfname = sib.path().filename().string();
                    auto sext   = sib.path().extension().string();
                    bool sib_lib = sext == ".so" || sfname.find(".so.") != std::string::npos;
                    if (!sib_lib) continue;
                    // Only take siblings with the same base name (e.g. libfmt.so*)
                    // to avoid pulling in unrelated libs from the same nix dir.
                    auto base = fname.substr(0, fname.find(".so"));
                    if (sfname.rfind(base, 0) != 0) continue;
                    auto sibtgt = dst_lib / sfname;
                    if (!fs::exists(sibtgt) && !fs::is_symlink(sibtgt))
                        fs::create_symlink(fs::absolute(sib.path()), sibtgt);
                }
            } catch (...) {}

            // Emit a -l flag for the unversioned name (libfmt.so → -lfmt)
            if (fname.starts_with("lib")) {
                // Strip "lib" prefix and ".so*" suffix
                auto base = fname.substr(3); // remove "lib"
                auto dot = base.find(".so");
                if (dot != std::string::npos) base = base.substr(0, dot);
                auto lflag = "-l" + base;
                if (seen_flags.insert(lflag).second) all_flags.push_back(lflag);
            }
        }

    }

    // Ensure every unversioned .so symlink in .cpm/lib/ has a matching -l flag.
    // These symlinks were created above for nix-store transitive deps.
    try {
        for (const auto &entry : fs::directory_iterator(dst_lib)) {
            if (!entry.is_symlink()) continue;
            if (entry.path().extension() != ".so") continue;
            auto fname = entry.path().filename().string();
            if (!fname.starts_with("lib")) continue;
            auto base = fname.substr(3, fname.size() - 6); // strip "lib" + ".so"
            if (base.empty()) continue;
            auto lflag = "-l" + base;
            if (seen_flags.insert(lflag).second) all_flags.push_back(lflag);
        }
    } catch (...) {}

    // Write the merged flag set to .cpm/defines.txt for the builder to pick up.
    // Also write back only -D/-I flags to the cache's defines.txt so the cache
    // stays clean and doesn't accumulate -l flags across installs.
    if (!all_flags.empty()) {
        std::ofstream df(local_cpm_dir_ / "defines.txt");
        for (const auto &f : all_flags) df << f << "\n";

        // Keep cache clean: only -D and -I flags
        std::ofstream cdf(built_path / "defines.txt");
        for (const auto &f : all_flags)
            if (f.rfind("-D", 0) == 0 || f.rfind("-I", 0) == 0) cdf << f << "\n";
    }
}


std::string Downloader::compiler_to_nix_attr(const std::string &compiler) {
    if (compiler.empty()) return "gcc13";
    if (compiler.find("clang") != std::string::npos) {
        auto dash = compiler.find('-');
        return dash != std::string::npos ? "clang_" + compiler.substr(dash + 1) : "clang";
    }
    auto dash = compiler.find('-');
    return dash != std::string::npos ? "gcc" + compiler.substr(dash + 1) : "gcc";
}


// Parse all .pc files in pc_dir, resolve ${variable} substitutions, and
// return every unique compiler flag (-I, -D) found in Cflags lines.
// Also separately returns the nix-store include paths for symlinking.
// Done in pure C++ — no pkg-config invocation needed.
std::vector<fs::path> Downloader::collect_pc_nix_includes(const fs::path &pc_dir) {
    // Thin wrapper kept for any callers; real logic is in collect_pc_flags.
    std::vector<std::string> flags;
    std::vector<fs::path> nix_incs;
    collect_pc_flags(pc_dir, flags, nix_incs);
    return nix_incs;
}

// Parse a single .pc file, expand variables, extract flags and required packages.
static void parse_one_pc(const fs::path &pc_file,
                          std::map<std::string, std::string> &vars,
                          std::vector<std::string> &lines,
                          std::vector<std::string> &requires_list) {
    std::ifstream f(pc_file);
    if (!f) return;

    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
        auto colon = line.find(':');
        auto eq    = line.find('=');
        // Variable assignment: has = before any :
        if (eq != std::string::npos && (colon == std::string::npos || eq < colon)) {
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            while (!key.empty() && key.back() == ' ') key.pop_back();
            while (!val.empty() && val.front() == ' ') val = val.substr(1);
            vars[key] = val;
        }
        // Requires / Requires.private: collect package names
        if (line.rfind("Requires", 0) == 0 && colon != std::string::npos) {
            std::string reqs = line.substr(colon + 1);
            // Split on commas and spaces, strip version constraints
            std::istringstream ss(reqs);
            std::string tok;
            while (ss >> tok) {
                // Skip version operators and numbers
                if (tok == ">=" || tok == "<=" || tok == ">" || tok == "<" || tok == "=")
                    continue;
                if (!tok.empty() && (std::isdigit(tok[0]) || tok[0] == '.'))
                    continue;
                // Strip trailing comma
                if (!tok.empty() && tok.back() == ',') tok.pop_back();
                if (!tok.empty()) requires_list.push_back(tok);
            }
        }
    }
}

// Search nix store for a .pc file for the given package name.
// Returns empty path if not found.
static fs::path find_nix_pc(const std::string &pkg_name,
                              std::set<std::string> &searched,
                              const std::map<std::string, fs::path> &nix_pc_index) {
    if (!searched.insert(pkg_name).second) return {};
    auto it = nix_pc_index.find(pkg_name);
    if (it != nix_pc_index.end()) return it->second;
    return {};
}

// Build an index of package-name → .pc file path for the entire nix store.
// Only called once per install; scans /nix/store/*/lib/pkgconfig/*.pc
static std::map<std::string, fs::path> build_nix_pc_index() {
    std::map<std::string, fs::path> index;
    if (!fs::exists("/nix/store")) return index;
    try {
        for (const auto &store_entry : fs::directory_iterator("/nix/store")) {
            if (!store_entry.is_directory()) continue;
            for (const auto &subdir : {"lib/pkgconfig", "share/pkgconfig"}) {
                auto pc_dir = store_entry.path() / subdir;
                if (!fs::exists(pc_dir)) continue;
                try {
                    for (const auto &pc_entry : fs::directory_iterator(pc_dir)) {
                        if (pc_entry.path().extension() != ".pc") continue;
                        auto stem = pc_entry.path().stem().string();
                        // Only insert first occurrence (prefer earlier store entries)
                        index.emplace(stem, pc_entry.path());
                    }
                } catch (...) {}
            }
        }
    } catch (...) {}
    return index;
}

void Downloader::collect_pc_flags(const fs::path &pc_dir,
                                  std::vector<std::string> &out_flags,
                                  std::vector<fs::path> &out_nix_incs) {
    if (!fs::exists(pc_dir)) return;

    std::set<std::string> seen_flags;
    std::set<std::string> seen_incs;
    std::set<std::string> searched_pkgs;

    // Build nix-store .pc index once (fast: ~100ms for 5000 store paths)
    static std::map<std::string, fs::path> nix_pc_index = build_nix_pc_index();

    // Queue of .pc files to process (seeded from pc_dir, extended by Requires deps)
    std::vector<fs::path> pc_queue;
    for (const auto &entry : fs::directory_iterator(pc_dir)) {
        if (entry.path().extension() == ".pc")
            pc_queue.push_back(entry.path());
    }

    auto expand_vars = [](const std::map<std::string, std::string> &vars,
                           std::string s) -> std::string {
        for (int iter = 0; iter < 10; ++iter) {
            bool changed = false;
            for (const auto &[k, v] : vars) {
                std::string ph = "${" + k + "}";
                size_t pos = 0;
                while ((pos = s.find(ph, pos)) != std::string::npos) {
                    s.replace(pos, ph.size(), v);
                    pos += v.size();
                    changed = true;
                }
            }
            if (!changed) break;
        }
        return s;
    };

    std::set<std::string> visited_pc;

    for (size_t qi = 0; qi < pc_queue.size(); ++qi) {
        const auto &pc_file = pc_queue[qi];
        if (!visited_pc.insert(pc_file.string()).second) continue;

        std::map<std::string, std::string> vars;
        std::vector<std::string> lines;
        std::vector<std::string> requires_list;
        parse_one_pc(pc_file, vars, lines, requires_list);

        // Queue Requires deps from nix store.
        // Skip packages that are either baked into the .a at build time,
        // or are compiler tools not needed at user link time.
        static const std::set<std::string> skip_transitive = {
            "protobuf",       // baked into libseastar.a; nix index picks wrong version
            "protobuf-lite",
            "gnutls",         // use system — nix version needs newer glibc ABI
            "nettle",
            "hogweed",
            "p11-kit-1",
            "libtasn1",
            "libidn2",
            "libunistring",
            "libunbound",
        };
        for (const auto &req : requires_list) {
            if (skip_transitive.count(req)) continue;
            auto nix_pc = find_nix_pc(req, searched_pkgs, nix_pc_index);
            if (!nix_pc.empty()) pc_queue.push_back(nix_pc);
        }

        // Extract flags from expanded lines
        for (const auto &ln : lines) {
            std::string expanded = expand_vars(vars, ln);
            std::istringstream ss(expanded);
            std::string tok;
            while (ss >> tok) {
                if (tok.size() < 2) continue;
                if (tok.find("${") != std::string::npos) continue;

                bool is_inc    = tok.rfind("-I", 0) == 0;
                bool is_def    = tok.rfind("-D", 0) == 0;
                bool is_nixlib = tok.rfind("/nix/store/", 0) == 0 &&
                                 (tok.find(".so") != std::string::npos ||
                                  tok.find(".a")  != std::string::npos);
                // -L/nix/store/... tells us where to find .so files for this pkg
                bool is_nixlibdir = tok.rfind("-L/nix/store/", 0) == 0;

                // Skip -L flags for packages we want to link from the system
                if (is_nixlibdir) {
                    static const std::vector<std::string> system_pkg_patterns = {
                        "gnutls", "nettle", "p11-kit", "tasn1", "idn2", "unbound", "unistring"
                    };
                    for (const auto &pat : system_pkg_patterns) {
                        if (tok.find(pat) != std::string::npos) { is_nixlibdir = false; break; }
                    }
                }

                if (!is_inc && !is_def && !is_nixlib && !is_nixlibdir) continue;

                if (seen_flags.insert(tok).second)
                    out_flags.push_back(tok);

                if (is_inc) {
                    fs::path inc(tok.substr(2));
                    if (fs::exists(inc) && seen_incs.insert(inc.string()).second)
                        out_nix_incs.push_back(inc);
                }
                if (is_nixlibdir) {
                    // system_link_libs: skip nix symlink but still emit -l so
                    // the system linker finds the compatible version.
                    static const std::set<std::string> skip_libs = {
                        "libprotoc.so",       // protobuf compiler tool
                        "libgtest.so",        // test framework
                        "libgmock.so",
                    };
                    static const std::set<std::string> system_link_libs = {
                        "libgnutls.so",       // nix version needs newer glibc ABI — use system
                        "libgnutls-dane.so",
                        "libgnutlsxx.so",
                        "libhogweed.so",
                        "libnettle.so",
                        "libp11-kit.so",
                        "libtasn1.so",
                        "libidn2.so",
                        "libunbound.so",
                        "libunistring.so",
                    };
                    fs::path lib_dir(tok.substr(2));
                    if (fs::exists(lib_dir)) {
                        try {
                            for (const auto &e : fs::directory_iterator(lib_dir)) {
                                if (e.path().extension() != ".so") continue;
                                auto fname = e.path().filename().string();
                                if (!fname.starts_with("lib")) continue;
                                if (skip_libs.count(fname)) continue;
                                if (system_link_libs.count(fname)) {
                                    // Emit -l flag only (no nix symlink) — system provides it
                                    auto base = fname.substr(3, fname.size() - 6);
                                    auto lflag = "-l" + base;
                                    if (seen_flags.insert(lflag).second)
                                        out_flags.push_back(lflag);
                                    continue;
                                }
                                auto sp = e.path().string();
                                if (seen_flags.insert(sp).second)
                                    out_flags.push_back(sp);
                            }
                        } catch (...) {}
                    }
                }
            }
        }
    }
}

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


std::string Downloader::search_github_repo(const std::string &package_name) {
    std::string cmd = "curl -s 'https://api.github.com/search/repositories?q=" + package_name +
                      "&sort=stars&per_page=1' 2>/dev/null"
                      " | grep -oP '\"html_url\": \"https://github.com/[^/]+/[^\"]*\"'"
                      " | head -1 | grep -oP 'https://github.com/[^\"]*'";
    return capture(cmd);
}


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

// ─── patch_broken_urls ───────────────────────────────────────────────────────
// Rewrites known-dead download URLs in cooking_recipe.cmake AND any already-
// generated ExternalProject stamp files under build/_cooking/.
// Must run BEFORE cooking.sh so the recipe is fixed even if a prior partial
// build already baked the old URL into generated stamp files.

static void patch_broken_urls(const fs::path &src_path) {
    // { old_string, replacement }
    // Covers both URL fixes (dead hosts) and version upgrades needed for
    // newer compilers (e.g. GMP 6.1.2 configure fails with GCC 16+).
    static const std::pair<std::string,std::string> kFixes[] = {
        // gmplib.org is frequently unreachable — use GNU FTP mirror
        { "https://gmplib.org/download/gmp/", "https://ftp.gnu.org/gnu/gmp/" },
        { "http://gmplib.org/download/gmp/",  "https://ftp.gnu.org/gnu/gmp/" },
        // GMP 6.1.2 configure fails with GCC 16+ (C strict-prototype errors).
        // Upgrade to 6.3.0 which is compatible.
        { "gmp-6.1.2.tar.bz2",                "gmp-6.3.0.tar.bz2" },
        { "8ddbb26dc3bd4e2302984debba1406a5",  "c1cd6ef33085e9cb818b9b08371f9000" },
    };

    // Build a single sed expression
    std::string expr;
    for (const auto &[old_url, new_url] : kFixes)
        expr += "s|" + old_url + "|" + new_url + "|g;";
    if (expr.empty()) return;

    // Patch cooking_recipe.cmake: wrap dpdk ingredient in if(Seastar_DPDK)
    // so cooking.sh skips it when -DSeastar_DPDK=OFF is passed.
    auto recipe = src_path / "cooking_recipe.cmake";
    if (fs::exists(recipe)) {
        std::ifstream fin(recipe);
        std::string content((std::istreambuf_iterator<char>(fin)), {});
        fin.close();
        // Only patch if not already wrapped
        if (content.find("if (Seastar_DPDK)") == std::string::npos &&
            content.find("cooking_ingredient (dpdk") != std::string::npos) {
            // Find the dpdk ingredient block and wrap it
            std::string dpdk_marker = "cooking_ingredient (dpdk";
            auto pos = content.find(dpdk_marker);
            if (pos != std::string::npos) {
                // Find the closing paren of this ingredient block
                int depth = 0;
                size_t end = pos;
                for (size_t i = pos; i < content.size(); ++i) {
                    if (content[i] == '(') depth++;
                    else if (content[i] == ')') { depth--; if (depth == 0) { end = i + 1; break; } }
                }
                // Insert guard around the block
                content.insert(end, "\nendif()");
                content.insert(pos, "if (Seastar_DPDK)\n");
                std::ofstream fout(recipe);
                fout << content;
            }
        }
    }

    // Also apply URL fixes to the recipe
    if (fs::exists(recipe))
        std::system(("sed -i '" + expr + "' " + recipe.string() + " 2>/dev/null").c_str());
    auto cooking_dir = src_path / "build" / "_cooking";
    if (!fs::exists(cooking_dir)) return;
    try {
        for (const auto &entry : fs::recursive_directory_iterator(cooking_dir)) {
            if (!entry.is_regular_file()) continue;
            auto fname = entry.path().filename().string();
            if (entry.path().extension() != ".cmake") continue;
            if (!fname.starts_with("download-") && !fname.starts_with("verify-")) continue;
            std::system(("sed -i '" + expr + "' " + entry.path().string() + " 2>/dev/null").c_str());
        }
    } catch (...) {}
}

// apply_cooking_patches
// Called AFTER cooking.sh configure has generated build.ninja and downloaded
// ingredient tarballs. Does only what cooking.sh cannot do itself:
//
// 1. Creates versioned automake shims (aclocal-1.15, automake-1.16, etc.)
// 2. Runs autoreconf -fi on every autotools ingredient, then back-dates
//    configure.ac/Makefile.am so make doesn't re-invoke automake during build
//
// Everything else (building GMP, nettle, GnuTLS, Boost, protobuf, c-ares...)
// is handled by cooking.sh itself — we stay out of its way.
//
// Returns the shim directory path to prepend to PATH.

static std::string apply_cooking_patches(const fs::path &src_path) {
    // 1. Versioned automake/gtkdocize shims
    auto shim_dir = src_path / "build" / "_cpm_autotools_shims";
    fs::create_directories(shim_dir);

    for (const std::string &tool : {"aclocal", "automake"}) {
        for (const std::string &ver : {"1.14", "1.15", "1.16", "1.17", "1.18"}) {
            auto shim = shim_dir / (tool + "-" + ver);
            if (!fs::exists(shim)) {
                std::ofstream f(shim);
                f << "#!/bin/sh\nexec " << tool << " \"$@\"\n";
                fs::permissions(shim, fs::perms::owner_all |
                                      fs::perms::group_read | fs::perms::group_exec |
                                      fs::perms::others_read | fs::perms::others_exec);
            }
        }
    }
    // gtkdocize stub — GnuTLS autoreconf calls it but we don't need docs
    {
        auto shim = shim_dir / "gtkdocize";
        if (!fs::exists(shim)) {
            std::ofstream f(shim);
            f << "#!/bin/sh\nexit 0\n";
            fs::permissions(shim, fs::perms::owner_all |
                                  fs::perms::group_read | fs::perms::group_exec |
                                  fs::perms::others_read | fs::perms::others_exec);
        }
    }

    auto ing_base = src_path / "build" / "_cooking" / "ingredient";
    auto shell_nix = src_path / "shell.nix";

    if (!fs::exists(ing_base) || !fs::exists(shell_nix)) return shim_dir.string();

    // 2. yaml-cpp: missing <cstdint> with GCC >= 15 (precaution)
    auto yaml_file = ing_base / "yaml-cpp" / "src" / "src" / "emitterutils.cpp";
    if (fs::exists(yaml_file))
        std::system(("grep -q '#include <cstdint>' " + yaml_file.string() +
                     " || sed -i '1s|^|#include <cstdint>\\n|' " + yaml_file.string()).c_str());

    // 3. GnuTLS: remove gtkdoc/manpages from build system so autoreconf succeeds
    auto gnutls_src = ing_base / "GnuTLS" / "src";
    if (fs::exists(gnutls_src / "configure.ac")) {
        std::system(("sed -i 's/GTK_DOC_CHECK.*//g' " +
                     (gnutls_src / "configure.ac").string() + " 2>/dev/null").c_str());
        std::system(("sed -i 's/ doc\\b//g; s/\\bdoc //g; s/ manpages\\b//g; s/\\bmanpages //g' " +
                     (gnutls_src / "Makefile.am").string() + " 2>/dev/null").c_str());
        for (const auto &stub : {gnutls_src / "gtk-doc.make",
                                  gnutls_src / "doc" / "Makefile.am",
                                  gnutls_src / "doc" / "reference" / "Makefile.am"}) {
            fs::create_directories(stub.parent_path());
            std::ofstream f(stub); f << "EXTRA_DIST =\n";
        }
        fs::create_directories(gnutls_src / "manpages");
    }

    // 4. autoreconf -fi on every autotools ingredient, then fix timestamps
    //    so make doesn't re-invoke automake (which would fail with version mismatch)

    // liburing 2.1 fix: needs <linux/openat2.h> — GCC 16+ rejects
    // forward-declared struct open_how in parameter lists.
    {
        auto liburing_h = ing_base / "liburing" / "src" / "src" / "include" / "liburing.h";
        if (fs::exists(liburing_h)) {
            std::string sed_cmd = "sed -i '1s|^|#include <linux/openat2.h>\\n|' "
                + liburing_h.string() + " 2>/dev/null";
            std::system(sed_cmd.c_str());
        }
    }

    // numactl: prevent make from trying to rebuild autotools files
    // autoreconf above touched Makefile.in to current time, which triggers
    // make to try rebuilding aclocal.m4 and calling aclocal-1.15.
    // Back-date everything in the source dir so the build dir's Makefile
    // (generated by configure) is newer than all autotools inputs.
    {
        auto numactl_src = ing_base / "numactl" / "src";
        if (fs::exists(numactl_src / "configure")) {
            std::system(("touch -t 202001010000 " + numactl_src.string() +
                "/configure.ac "
                + numactl_src.string() + "/aclocal.m4 "
                + numactl_src.string() + "/configure "
                + numactl_src.string() + "/Makefile.in "
                + numactl_src.string() + "/Makefile.am "
                + numactl_src.string() + "/config.h.in 2>/dev/null").c_str());
        }
    }

    for (const auto &entry : fs::directory_iterator(ing_base)) {
        if (!entry.is_directory()) continue;
        auto ing_src = entry.path() / "src";
        if (!fs::exists(ing_src / "configure.ac") &&
            !fs::exists(ing_src / "configure.in")) continue;

        std::string cmd =
            "nix-shell " + shell_nix.string() +
            " --run 'export PATH=" + shim_dir.string() + ":$PATH"
            " && cd " + ing_src.string() +
            " && autoreconf -fi 2>/dev/null"
            // Back-date configure.ac/Makefile.am so make sees Makefile.in as newer
            " && touch -t 202001010000 configure.ac configure.in Makefile.am aclocal.m4 2>/dev/null"
            // Then touch all Makefile.in to be current (newer than configure.ac)
            " && find . -name Makefile.in -exec touch {} \\; 2>/dev/null"
            "' 2>/dev/null";
        std::system(cmd.c_str());

        // Wipe configure/build/install stamps so ninja reruns with fresh files
        auto stamp_dir = entry.path() / "stamp";
        if (fs::exists(stamp_dir)) {
            auto ing_name = entry.path().filename().string();
            for (const std::string &step : {"configure", "build", "install"}) {
                auto sf = stamp_dir / ("ingredient_" + ing_name + "-" + step);
                if (fs::exists(sf)) fs::remove(sf);
            }
        }
    }

    // 5. Fix .pc file prefixes AND clean up any manually-copied stow files.
    //    If .pc files or other files were copied (not symlinked) into installed/,
    //    stow will refuse to stow over them. Remove real files that stow should own
    //    so stow can create its symlinks cleanly.
    {
        auto installed_dir = src_path / "build" / "_cooking" / "installed";
        auto stow_dir      = src_path / "build" / "_cooking" / "stow";
        if (!fs::exists(installed_dir) || !fs::exists(stow_dir)) goto done_pc_fix;

        // Remove any real (non-symlink) files in installed/ that have a
        // corresponding file in any stow/<pkg>/ directory.
        try {
            for (const auto &pkg_entry : fs::directory_iterator(stow_dir)) {
                if (!pkg_entry.is_directory()) continue;
                auto pkg_stow = pkg_entry.path();
                try {
                    for (const auto &f : fs::recursive_directory_iterator(pkg_stow)) {
                        if (!f.is_regular_file()) continue;
                        auto rel = fs::relative(f.path(), pkg_stow);
                        auto installed_copy = installed_dir / rel;
                        // If it exists as a real file (not symlink) → remove it
                        if (fs::exists(installed_copy) && !fs::is_symlink(installed_copy)) {
                            fs::remove(installed_copy);
                        }
                    }
                } catch (...) {}
            }
        } catch (...) {}

        // Fix .pc prefixes: rewrite stow/<pkg>/ → installed/ in all .pc files
        {
            auto pkgconfig_dir = installed_dir / "lib" / "pkgconfig";
            if (fs::exists(pkgconfig_dir)) {
                std::string sed_expr;
                try {
                    for (const auto &e : fs::directory_iterator(stow_dir)) {
                        if (!e.is_directory()) continue;
                        sed_expr += "s|" + stow_dir.string() + "/" +
                                    e.path().filename().string() + "/|" +
                                    installed_dir.string() + "/|g;";
                    }
                } catch (...) {}
                if (!sed_expr.empty()) {
                    try {
                        for (const auto &e : fs::directory_iterator(pkgconfig_dir)) {
                            if (e.path().extension() != ".pc") continue;
                            std::system(("sed -i '" + sed_expr + "' " +
                                         e.path().string() + " 2>/dev/null").c_str());
                        }
                    } catch (...) {}
                }
            }
        }
    }
    done_pc_fix:;

    return shim_dir.string();
}

bool Downloader::build_from_source(const std::string &name, const fs::path &src_path, const fs::path &install_prefix, const fs::path &project_root) {
    auto build_dir = src_path / "_cpm_build";
    fs::create_directories(build_dir);
    fs::create_directories(install_prefix / "include");
    fs::create_directories(install_prefix / "lib");

    int ret = -1;

    if (!fs::exists(src_path / "cooking.sh") && fs::exists(src_path / "install-dependencies.sh")) {
        std::cout << "[cpm]   → resolving transitive dependencies...\n";
        resolve_transitive_deps(src_path, install_prefix);
    }

    // Strategy 1: cooking.sh
    if (fs::exists(src_path / "cooking.sh")) {
        std::cout << "[cpm]   → using cooking.sh\n";
        auto bin_dir = local_cpm_dir_ / "bin";
        fs::create_directories(bin_dir);

        NixEnv nix(local_cpm_dir_, global_cache_dir_);
        if (nix.available()) {
            std::cout << "[cpm]   → building inside isolated nix environment\n";

            std::string nix_compiler = "gcc13";
            std::string nixpkgs_pin;
            std::string cpp_std = "20";
            if (!project_root.empty()) {
                auto toml_path = project_root / "cpm.toml";
                if (fs::exists(toml_path)) {
                    auto cfg = TomlParser::parse(toml_path);
                    if (!cfg.compiler.empty())     nix_compiler = compiler_to_nix_attr(cfg.compiler);
                    if (!cfg.nixpkgs.empty())      nixpkgs_pin  = cfg.nixpkgs;
                    if (!cfg.cpp_standard.empty()) cpp_std      = cfg.cpp_standard;
                }
            }

            std::string nix_import;
            if (!nixpkgs_pin.empty()) {
                std::string url = "https://github.com/NixOS/nixpkgs/archive/" + nixpkgs_pin + ".tar.gz";
                nix_import = "{ pkgs ? import (builtins.fetchTarball { url = \"" + url + "\"; }) {} }:\n";
            } else {
                nix_import = "{ pkgs ? import <nixpkgs> {} }:\n";
            }

            // Detect library deps from CMakeLists.txt + cmake/*.cmake
            auto detected_deps = nix.detect_nix_deps(src_path);

            std::vector<std::string> shell_deps = {
                nix_compiler,
                "cmake", "ninja", "pkg-config",
                "automake", "autoconf", "libtool",
                "python3", "python3Packages.pyelftools",
                "git", "stow", "meson",
                "xorg.utilmacros", "xfsprogs", "valgrind",
                // fmt_10 is required by seastar and other projects that use fmtlib ≥ 8.
                // Must be present so cmake find_package(fmt) resolves to the nix store
                // rather than falling through to a missing system package.
                "fmt_10"
            };

            std::set<std::string> seen(shell_deps.begin(), shell_deps.end());
            for (const auto& d : detected_deps)
                if (seen.insert(d).second) shell_deps.push_back(d);

            std::string shell_nix_content = nix_import + "pkgs.mkShell {\n  buildInputs = with pkgs; [\n";
            for (const auto& dep : shell_deps)
                shell_nix_content += "    " + dep + "\n";
            // Work around nix-shell GCC wrapper passing bare -rpath flag
            shell_nix_content += "  ];\n  NIX_CFLAGS_LINK = \"\";\n}\n";

            auto shell_nix_path = src_path / "shell.nix";
            { std::ofstream f(shell_nix_path); f << shell_nix_content; }

            patch_broken_urls(src_path);

            {
                auto jam = src_path / "build" / "cook_boost.jam";
                if (fs::exists(jam)) fs::remove(jam);
                for (const auto &sub : {"build", "stamp"}) {
                    auto d = src_path / "build" / "_cooking" / "ingredient" / "Boost" / sub;
                    if (fs::exists(d)) fs::remove_all(d);
                }
            }

            std::string nix_gcc = nix.run_cmd(
                "nix-shell " + shell_nix_path.string() + " --run 'which gcc' 2>/dev/null");
            std::string nix_gxx = nix.run_cmd(
                "nix-shell " + shell_nix_path.string() + " --run 'which g++' 2>/dev/null");
            // Resolve the nix-provided protoc so cmake uses the same version as
            // libprotobuf from the nix store.  Without this, cmake would pick up the
            // system protoc (which can be a much newer version), producing .pb.h files
            // that are incompatible with the nix-pinned libprotobuf headers.
            std::string nix_protoc = nix.run_cmd(
                "nix-shell " + shell_nix_path.string() + " --run 'which protoc' 2>/dev/null");
            if (nix_gcc.empty()) nix_gcc = "gcc";
            if (nix_gxx.empty()) nix_gxx = "g++";
            if (nix_protoc.empty()) nix_protoc = "protoc";

            auto shim_dir = src_path / "build" / "_cpm_autotools_shims";
            fs::create_directories(shim_dir);
            for (const std::string &tool : {"aclocal", "automake"}) {
                for (const std::string &ver : {"1.14", "1.15", "1.16", "1.17", "1.18"}) {
                    auto shim = shim_dir / (tool + "-" + ver);
                    if (!fs::exists(shim)) {
                        std::ofstream f(shim);
                        f << "#!/bin/sh\nexec " << tool << " \"$@\"\n";
                        fs::permissions(shim, fs::perms::owner_all |
                            fs::perms::group_read | fs::perms::group_exec |
                            fs::perms::others_read | fs::perms::others_exec);
                    }
                }
            }
            {
                auto shim = shim_dir / "gtkdocize";
                if (!fs::exists(shim)) {
                    std::ofstream f(shim);
                    f << "#!/bin/sh\nexit 0\n";
                    fs::permissions(shim, fs::perms::owner_all |
                        fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec);
                }
            }

            // Pre-compute nix flags once instead of nested nix-shell calls
            std::string nix_cflags = nix.run_cmd(
                "nix-shell " + shell_nix_path.string() + " --run 'echo $NIX_CFLAGS_COMPILE' 2>/dev/null");
            std::string nix_ldflags = nix.run_cmd(
                "nix-shell " + shell_nix_path.string() + " --run 'echo $NIX_LDFLAGS' 2>/dev/null");

            std::string env_setup =
                "export PATH=" + shim_dir.string() + ":$PATH && "
                // Prepend cooking installed/ to PKG_CONFIG_PATH so autoconf-based
                // ingredient builds (GnuTLS etc.) find cooking-built deps via pkg-config.
                "export PKG_CONFIG_PATH=" + (src_path / "build" / "_cooking" / "installed" / "lib" / "pkgconfig").string() + ":$PKG_CONFIG_PATH && "
                "export CC=\"" + nix_gcc + "\" && "
                "export CXX=\"" + nix_gxx + "\" && "
                // Nix isolation: propagate nix-provided include/lib paths
                "export CFLAGS=\"$CFLAGS " + nix_cflags + "\" && "
                "export CXXFLAGS=\"$CXXFLAGS " + nix_cflags + "\" && "
                "export LDFLAGS=\"$LDFLAGS " + nix_ldflags + "\" && "
                "export MAKEFLAGS=\"-j$(nproc)\" && "
                "export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc) && "
                "export CMAKE_POLICY_VERSION_MINIMUM=3.5";

            {
                auto doc_cmake = src_path / "doc" / "CMakeLists.txt";
                if (fs::exists(doc_cmake)) {
                    std::ofstream f(doc_cmake);
                    f << "# Patched by cpm — docs disabled\n";
                }
            }

            // Skip configure.py — it uses hardcoded g++ and /usr/local prefix.
            // Go directly to cooking.sh which respects our cmake flags.
            ret = 1; // force cooking.sh path

            if (ret != 0) {
                // cooking.sh approach: it generates build.ninja that builds all
                // ingredients, then Seastar.  Ingredients often fail on modern GCC.
                // We use a direct CMake build instead — nix already provides all
                // deps via shell.nix; we just need Seastar's own CMake to find them.
                // The cooking.sh step is still run once to download the ingredient
                // source tarballs (for headers even if builds fail).

                {
                    auto partial_installed = src_path / "build" / "_cooking" / "installed";
                    if (fs::exists(partial_installed)) fs::remove_all(partial_installed);
                    fs::create_directories(partial_installed);
                }

                // Run cooking.sh one time to download ingredient source tarballs
                // (needed for ingredient headers like boost, fmt, yaml-cpp etc.)
                std::string cook_dl_cmd =
                    "cd " + src_path.string() +
                    " && nix-shell " + shell_nix_path.string() +
                    " --run '" + env_setup +
                    " && bash ./cooking.sh -t Release -g Ninja"
                    " -s CC=\"" + nix_gcc + "\""
                    " -s CXX=\"" + nix_gxx + "\""
                    " -- -DCMAKE_C_COMPILER=\"" + nix_gcc + "\""
                    " -DCMAKE_CXX_COMPILER=\"" + nix_gxx + "\""
                    " -DCMAKE_CXX_STANDARD=" + cpp_std +
                    " -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
                    " -DSeastar_DPDK=OFF"
                    " -DSeastar_DPDK_MACHINE=none' 2>&1";
                std::system(cook_dl_cmd.c_str());

                // Patch any downloaded sources for GCC 16 compatibility
                apply_cooking_patches(src_path);

                // Collect ingredient headers from the cooking download area
                for (const auto &cook_base : {src_path / "build", src_path / "build" / "release"}) {
                    auto ingredients = cook_base / "_cooking" / "ingredient";
                    if (fs::exists(ingredients)) {
                        for (const auto &ing : fs::directory_iterator(ingredients)) {
                            if (!ing.is_directory()) continue;
                            // Boost headers
                            auto bh = ing.path() / "src" / "boost";
                            if (fs::exists(bh)) {
                                fs::create_directories(install_prefix / "include" / "boost");
                                cp_r(bh, install_prefix / "include" / "boost");
                            }
                            auto ih = ing.path() / "src" / "include";
                            if (fs::exists(ih)) cp_r(ih, install_prefix / "include");
                        }
                    }
                }

                // Build Seastar directly with CMake (not cooking.sh) inside nix-shell.
                // Nix already provides all deps via shell.nix — CMake should find them.
                auto seastar_build = src_path / "_cpm_build";
                fs::create_directories(seastar_build);
                std::string cmake_build_cmd =
                    "nix-shell " + shell_nix_path.string() +
                    " --run 'cd " + seastar_build.string() +
                    " && export MAKEFLAGS=\"-j$(nproc)\""
                    " && export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)"
                    " && export CMAKE_POLICY_VERSION_MINIMUM=3.5"
                    " && cmake " + src_path.string() + " -GNinja"
                    " -DCMAKE_BUILD_TYPE=Release"
                    " -DCMAKE_INSTALL_PREFIX=" + install_prefix.string() +
                    " -DCMAKE_CXX_STANDARD=" + cpp_std +
                    " -DCMAKE_C_COMPILER=" + nix_gcc +
                    " -DCMAKE_CXX_COMPILER=" + nix_gxx +
                    " -DProtobuf_PROTOC_EXECUTABLE=" + nix_protoc +
                    " -DSeastar_DPDK=OFF"
                    " -DSeastar_DPDK_MACHINE=none"
                    " -DBUILD_TESTING=OFF"
                    " && ninja -j$(nproc) -k 0"
                    " && cmake --install ."
                    " || ninja -j$(nproc) seastar -k 0"
                    "' 2>&1";
                ret = std::system(cmake_build_cmd.c_str());
            }

            // Collect build artifacts into install_prefix
            if (fs::exists(src_path / "include"))
                cp_r(src_path / "include", install_prefix / "include");

            // Collect generated headers from ALL gen/include directories recursively
            // (cooking build dir + _cpm_build direct CMake dir)
            {
                std::string find_gen = "find " + (src_path / "build").string() + " " +
                    (src_path / "_cpm_build").string() +
                    " -path '*/gen/include' -type d 2>/dev/null";
                auto pipe = popen(find_gen.c_str(), "r");
                if (pipe) {
                    std::array<char, 4096> buf;
                    std::string p;
                    while (fgets(buf.data(), buf.size(), pipe)) {
                        p = buf.data();
                        while (!p.empty() && (p.back() == '\n' || p.back() == '\r'))
                            p.pop_back();
                        if (!p.empty() && fs::exists(p))
                            cp_r(p, install_prefix / "include");
                    }
                    pclose(pipe);
                }
            }

            if (fs::exists(src_path / "build")) {
                std::system(("find " + (src_path / "build").string() +
                    " -name '*.a' -not -path '*/_cooking/*'"
                    " -exec cp {} " + (install_prefix / "lib").string() + "/ \\; 2>/dev/null").c_str());
            }

            // Collect headers/libs from cooking sub-deps
            for (const auto &cook_base : {src_path / "build", src_path / "build" / "release"}) {
                auto cooked = cook_base / "_cooking" / "installed";
                if (fs::exists(cooked / "include")) cp_r(cooked / "include", install_prefix / "include");
                if (fs::exists(cooked / "lib"))     cp_r(cooked / "lib",     install_prefix / "lib");

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

            // Use pkg-config inside the nix-shell to get the full set of
            // compile flags (including Requires.private transitive deps like
            // hwloc, gnutls, liburing, etc.) and write them to defines.txt
            // in the install cache so install_built_library can use them.
            {
                auto pc_dir = install_prefix / "lib" / "pkgconfig";
                std::string pc_names;
                if (fs::exists(pc_dir)) {
                    for (const auto &e : fs::directory_iterator(pc_dir)) {
                        if (e.path().extension() == ".pc")
                            pc_names += " " + e.path().stem().string();
                    }
                }

                std::vector<std::string> defines_flags;

                // Hard-coded -D flags that CMake bakes in but don't appear in .pc
                if (name == "seastar") {
                    for (const auto &f : {
                        "-DSEASTAR_SCHEDULING_GROUPS_COUNT=16",
                        "-DSEASTAR_API_LEVEL=7",
                        "-DSEASTAR_SSTRING",
                        "-DSEASTAR_LOGGER_COMPILE_TIME_FMT",
                        "-DFMT_SHARED"
                    }) defines_flags.push_back(f);
                }

                // Run pkg-config --cflags --libs --static inside nix-shell
                // to resolve all transitive deps including Requires.private.
                if (!pc_names.empty()) {
                    std::string pkgcfg_cmd =
                        "nix-shell " + shell_nix_path.string() +
                        " --run 'PKG_CONFIG_PATH=" + pc_dir.string() +
                        ":$PKG_CONFIG_PATH pkg-config --cflags --libs --static" +
                        pc_names + " 2>/dev/null'";
                    std::string pkgcfg_out = nix.run_cmd(pkgcfg_cmd);

                    std::set<std::string> seen(defines_flags.begin(), defines_flags.end());
                    std::istringstream ss(pkgcfg_out);
                    std::string tok;
                    while (ss >> tok) {
                        if (tok.size() < 2) continue;
                        bool is_flag = tok.rfind("-I", 0) == 0 ||
                                       tok.rfind("-D", 0) == 0 ||
                                       tok.rfind("-l", 0) == 0 ||
                                       tok.rfind("/nix/store/", 0) == 0;
                        if (is_flag && seen.insert(tok).second)
                            defines_flags.push_back(tok);
                    }
                }

                if (!defines_flags.empty()) {
                    std::ofstream df(install_prefix / "defines.txt");
                    for (const auto &f : defines_flags) df << f << "\n";
                }
            }

            if (ret == 0 || (fs::exists(install_prefix / "include") &&
                             !fs::is_empty(install_prefix / "include"))) {
                if (fs::exists(build_dir)) fs::remove_all(build_dir);
                return true;
            }
        }

        // Fallback: cooking.sh without nix
        ensure_build_tools(bin_dir);

        std::string compiler_env = "export MAKEFLAGS=\"-j$(nproc)\" && "
                                   "export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc) && "
                                   "export CMAKE_POLICY_VERSION_MINIMUM=3.5 && ";

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

        // Patch broken download URLs (cooking_recipe.cmake + any generated stamp files)
        patch_broken_urls(src_path);

        std::string cook_cmd = compiler_env + "export PATH=\"" + bin_dir.string() +
                               ":$PATH\" && "
                               "cd " +
                               src_path.string() +
                               " && bash ./cooking.sh -t Release -g Ninja"
                               " -s CC=" +
                               sys_cc + " -s CXX=" + sys_cxx + " -- -DCMAKE_C_COMPILER=" + sys_cc + " -DCMAKE_CXX_COMPILER=" + sys_cxx + " 2>&1";
        ret = std::system(cook_cmd.c_str());

        {
            // Collect ingredient source headers that cooking.sh downloaded
            // (Boost, fmt, yaml-cpp, etc.) before attempting any build.
            for (const auto &cook_base : {src_path / "build", src_path / "build" / "release"}) {
                auto ingredients = cook_base / "_cooking" / "ingredient";
                if (fs::exists(ingredients)) {
                    for (const auto &ing : fs::directory_iterator(ingredients)) {
                        if (!ing.is_directory()) continue;
                        auto bh = ing.path() / "src" / "boost";
                        if (fs::exists(bh)) {
                            fs::create_directories(install_prefix / "include" / "boost");
                            cp_r(bh, install_prefix / "include" / "boost");
                        }
                        auto ih = ing.path() / "src" / "include";
                        if (fs::exists(ih)) cp_r(ih, install_prefix / "include");
                    }
                }
            }

            // Try a direct CMake build to compile Seastar itself.
            auto seastar_cmake_build = src_path / "_cpm_build";
            fs::create_directories(seastar_cmake_build);
            std::string cmake_cmd =
                "cd " + seastar_cmake_build.string() +
                " && cmake " + src_path.string() + " -GNinja"
                " -DCMAKE_BUILD_TYPE=Release"
                " -DCMAKE_INSTALL_PREFIX=" + install_prefix.string() +
                " -DCMAKE_CXX_STANDARD=20"
                " -DSeastar_DPDK=OFF"
                " -DSeastar_DPDK_MACHINE=none"
                " -DBUILD_TESTING=OFF"
                " 2>&1"
                " && ninja -j$(nproc) -k 0 2>&1"
                " || ninja -j$(nproc) -k 0 seastar 2>&1";
            ret = std::system(cmake_cmd.c_str());

            if (ret == 0 || fs::exists(seastar_cmake_build / "gen")) {
                // Copy artifacts from direct CMake build
                if (fs::exists(src_path / "include"))
                    cp_r(src_path / "include", install_prefix / "include");
                if (fs::exists(seastar_cmake_build)) {
                    std::system(("find " + seastar_cmake_build.string() +
                                 " -name '*.a' -not -path '*/_cooking/*'"
                                 " -exec cp {} " + (install_prefix / "lib").string() + "/ \\; 2>/dev/null").c_str());
                }
            }
        }

        // Collect generated headers from ALL build directories
        {
            std::string find_gen = "find " + (src_path / "build").string() + " " +
                (src_path / "_cpm_build").string() +
                " -path '*/gen/include' -type d 2>/dev/null";
            auto pipe = popen(find_gen.c_str(), "r");
            if (pipe) {
                std::array<char, 4096> buf;
                std::string p;
                while (fgets(buf.data(), buf.size(), pipe)) {
                    p = buf.data();
                    while (!p.empty() && (p.back() == '\n' || p.back() == '\r'))
                        p.pop_back();
                    if (!p.empty() && fs::exists(p))
                        cp_r(p, install_prefix / "include");
                }
                pclose(pipe);
            }
        }

        // Copy cooked deps
        for (const auto &cook_base : {src_path / "build", src_path / "build" / "release"}) {
            auto cooked = cook_base / "_cooking" / "installed";
            if (fs::exists(cooked / "include")) cp_r(cooked / "include", install_prefix / "include");
            if (fs::exists(cooked / "lib"))     cp_r(cooked / "lib",     install_prefix / "lib");

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
            // Collect generated headers even on partial builds
            {
                std::string find_gen = "find " + (src_path / "build").string() +
                    " -path '*/gen/include' -type d 2>/dev/null";
                auto pipe = popen(find_gen.c_str(), "r");
                if (pipe) {
                    std::array<char, 4096> buf;
                    std::string p;
                    while (fgets(buf.data(), buf.size(), pipe)) {
                        p = buf.data();
                        while (!p.empty() && (p.back() == '\n' || p.back() == '\r'))
                            p.pop_back();
                        if (!p.empty() && fs::exists(p))
                            cp_r(p, install_prefix / "include");
                    }
                    pclose(pipe);
                }
            }
            std::cout << "[cpm]   → headers copied from source (full build failed)\n";
            if (fs::exists(build_dir)) fs::remove_all(build_dir);
            return true;
        }

        std::cerr << "[cpm] cooking.sh failed. Trying other build systems...\n";
        ret = -1;
    }

    //  Strategy 2: configure.py 
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

    //  Strategy 3: CMake 
    if (ret != 0 && fs::exists(src_path / "CMakeLists.txt")) {
        std::cout << "[cpm]   → using cmake\n";
        bool has_ninja = run_cmd_ok("which ninja > /dev/null 2>&1");
        std::string gen = has_ninja ? " -GNinja" : "";
        std::string cmake_cmd = "cd " + build_dir.string() + " && cmake " + src_path.string() + gen +
                                " -DCMAKE_INSTALL_PREFIX=" + install_prefix.string() +
                                " -DCMAKE_PREFIX_PATH=" + install_prefix.string() +
                                " -DCMAKE_BUILD_TYPE=Release"
                                " -DCMAKE_POSITION_INDEPENDENT_CODE=ON"
                                " -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
                                " -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF"
                                " -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARKS=OFF"
                                " > /dev/null 2>&1"
                                " && cmake --build . --parallel > /dev/null 2>&1"
                                " && cmake --install . > /dev/null 2>&1";
        ret = std::system(cmake_cmd.c_str());
    }

    //  Strategy 4: Makefile 
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

    //  Strategy 5: Meson 
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

    //  Strategy 6: Autotools 
    if (ret != 0 && fs::exists(src_path / "configure")) {
        std::cout << "[cpm]   → using autotools\n";
        ret = std::system(("cd " + src_path.string() + " && ./configure --prefix=" + install_prefix.string() +
                           " --enable-static --disable-shared > /dev/null 2>&1"
                           " && make -j$(nproc) > /dev/null 2>&1"
                           " && make install > /dev/null 2>&1")
                .c_str());
    }

    //  Strategy 7: Header-only fallback 
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

#include "cpm/installer.hpp"

#include "cpm/downloader.hpp"
#include "cpm/nix_env.hpp"
#include "cpm/progress.hpp"
#include "cpm/resolver.hpp"
#include "cpm/toml_parser.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpm {

namespace fs = std::filesystem;

Installer::Installer(fs::path project_root, fs::path local_cpm_dir, fs::path global_cache_dir)
    : project_root_(std::move(project_root)), local_cpm_dir_(std::move(local_cpm_dir)), global_cache_dir_(std::move(global_cache_dir)) {}

void Installer::ensure_directories() const {
    fs::create_directories(local_cpm_dir_ / "packages");
    fs::create_directories(local_cpm_dir_ / "include");
    fs::create_directories(local_cpm_dir_ / "lib");
    fs::create_directories(local_cpm_dir_ / "bin");
    fs::create_directories(global_cache_dir_);
}

// ─── Stale cleanup ──────────────────────────────────────────────────────────

void Installer::auto_remove_stale_packages(const ProjectConfig &config) {
    auto packages_dir = local_cpm_dir_ / "packages";
    if (!fs::exists(packages_dir)) return;

    std::set<std::string> wanted;
    for (const auto &dep : config.git_dependencies) wanted.insert(dep.name);

    for (const auto &entry : fs::directory_iterator(packages_dir)) {
        if (!entry.is_directory() && !entry.is_symlink()) continue;
        std::string name = entry.path().filename().string();
        if (!wanted.count(name)) {
            std::cout << "[cpm] Removing " << name << " (not in cpm.toml)\n";
            fs::remove_all(entry.path());
        }
    }
}

void Installer::auto_remove_stale_libs(const ProjectConfig &config) {
    auto lib_dir = local_cpm_dir_ / "lib";
    if (!fs::exists(lib_dir)) return;

    std::set<std::string> wanted;
    auto add_lower = [&](const std::string &s) {
        std::string l = s;
        std::ranges::transform(l, l.begin(), ::tolower);
        wanted.insert(l);
    };
    for (const auto &dep : config.system_dependencies) add_lower(dep.name);
    for (const auto &lib : config.nix_libraries) {
        add_lower(lib.name);
        add_lower(lib.nix_attr);
    }

    for (const auto &entry : fs::directory_iterator(lib_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".a") continue;

        std::string filename = entry.path().filename().string();
        std::string lib_name = filename.starts_with("lib") ? filename.substr(3, filename.size() - 5) : filename;
        std::ranges::transform(lib_name, lib_name.begin(), ::tolower);

        bool found = false;
        for (const auto &dep_name : wanted) {
            if (lib_name.find(dep_name) != std::string::npos || dep_name.find(lib_name) != std::string::npos) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::cout << "[cpm] Removing " << filename << " (not in cpm.toml)\n";
            fs::remove(entry.path());
        }
    }
}

void Installer::auto_remove_stale_includes(const ProjectConfig &config) {
    auto include_dir = local_cpm_dir_ / "include";
    if (!fs::exists(include_dir)) return;

    std::set<std::string> all_wanted;
    for (const auto &dep : config.git_dependencies) all_wanted.insert(dep.name);
    for (const auto &dep : config.system_dependencies) all_wanted.insert(dep.name);
    for (const auto &lib : config.nix_libraries) {
        all_wanted.insert(lib.name);
        all_wanted.insert(lib.nix_attr);
    }

    for (const auto &entry : fs::directory_iterator(include_dir)) {
        std::string inc_name = entry.path().filename().string();

        bool belongs = all_wanted.count(inc_name) > 0;

        // Symlink targets pointing to a known package or /nix/store/ are fine
        if (!belongs && fs::is_symlink(entry.path())) {
            auto target = fs::read_symlink(entry.path()).string();
            if (target.find("/nix/store/") != std::string::npos) {
                belongs = true;
            } else {
                for (const auto &dep : config.git_dependencies) {
                    if (target.find(dep.name) != std::string::npos) {
                        belongs = true;
                        break;
                    }
                }
                if (!belongs) {
                    for (const auto &dep : config.system_dependencies) {
                        if (target.find(dep.name) != std::string::npos) {
                            belongs = true;
                            break;
                        }
                    }
                }
            }
        }

        if (!belongs) {
            std::cout << "[cpm] Removing include: " << inc_name << " (not in cpm.toml)\n";
            fs::remove_all(entry.path());
        }
    }
}

// ─── Nix [libs] resolution ──────────────────────────────────────────────────

void Installer::resolve_nix_libraries(const ProjectConfig &config) {
    if (config.nix_libraries.empty()) return;

    NixEnv nix(local_cpm_dir_, global_cache_dir_);
    if (!nix.available()) {
        std::cerr << "[cpm] warning: nix not available, skipping [libs] resolution\n";
        return;
    }

    auto inc_dir = local_cpm_dir_ / "include";
    auto lib_dir = local_cpm_dir_ / "lib";
    fs::create_directories(inc_dir);
    fs::create_directories(lib_dir);

    for (const auto &nixlib : config.nix_libraries) {
        std::cout << "[cpm] resolving lib: " << nixlib.name << " (" << nixlib.nix_attr << ")\n";

        std::string dev_path = nix.run_cmd("nix-build '<nixpkgs>' -A " + nixlib.nix_attr + ".dev --no-out-link 2>/dev/null");
        std::string lib_path = nix.run_cmd("nix-build '<nixpkgs>' -A " + nixlib.nix_attr + " --no-out-link 2>/dev/null");

        if (dev_path.empty() && lib_path.empty()) {
            std::cerr << "[cpm] warning: nix package '" << nixlib.nix_attr << "' not found in nixpkgs\n";
            continue;
        }

        // Symlink headers from .dev/include/
        auto hdr_root = fs::path(dev_path.empty() ? lib_path : dev_path) / "include";
        if (fs::exists(hdr_root)) {
            for (const auto &e : fs::directory_iterator(hdr_root)) {
                auto tgt = inc_dir / e.path().filename();
                if (!fs::exists(tgt) && !fs::is_symlink(tgt)) fs::create_symlink(e.path(), tgt);
            }
        }

        // Symlink .so/.a from lib/
        if (!lib_path.empty()) {
            auto so_root = fs::path(lib_path) / "lib";
            if (fs::exists(so_root)) {
                for (const auto &e : fs::directory_iterator(so_root)) {
                    if (!e.is_regular_file() && !e.is_symlink()) continue;
                    auto ext = e.path().extension().string();
                    bool is_so = (ext == ".so" || ext == ".a" || e.path().filename().string().find(".so.") != std::string::npos);
                    if (!is_so) continue;
                    auto tgt = lib_dir / e.path().filename();
                    if (!fs::exists(tgt) && !fs::is_symlink(tgt)) fs::create_symlink(e.path(), tgt);
                }
            }
        }

        std::cout << "[cpm] " << nixlib.name << " linked\n";
    }
}

// ─── install ─────────────────────────────────────────────────────────────────

void Installer::install() {
    auto toml_path = project_root_ / "cpm.toml";
    if (!fs::exists(toml_path)) throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");

    auto config = TomlParser::parse(toml_path);
    ensure_directories();

    // Remove stale artifacts first
    auto_remove_stale_packages(config);
    auto_remove_stale_libs(config);
    auto_remove_stale_includes(config);

    // ── Parallel download/install ──────────────────────────────────────────
    ProgressDisplay progress;
    std::mutex install_mutex;

    std::vector<int> header_ids, sys_ids;
    header_ids.reserve(config.git_dependencies.size());
    sys_ids.reserve(config.system_dependencies.size());

    for (const auto &dep : config.git_dependencies) header_ids.push_back(progress.add_task(dep.name));
    for (const auto &dep : config.system_dependencies) sys_ids.push_back(progress.add_task(dep.name));

    const bool has_work = !config.git_dependencies.empty() || !config.system_dependencies.empty();
    if (has_work) progress.start();

    // Header-only deps — up to 4 in parallel
    if (!config.git_dependencies.empty()) {
        Downloader dl(local_cpm_dir_, global_cache_dir_);
        std::vector<std::function<void()>> tasks;
        tasks.reserve(config.git_dependencies.size());

        for (size_t i = 0; i < config.git_dependencies.size(); ++i) {
            tasks.emplace_back([&, i]() {
                const auto &dep = config.git_dependencies[i];
                int tid = header_ids[i];

                std::string version = dep.version;
                if (version == "*" || version.empty()) {
                    progress.set_status(tid, TaskStatus::Downloading, "resolving tag");
                    version = dl.resolve_latest_tag(dep.github_url, dep.name);
                }

                if (dl.is_cached(dep.name, version)) {
                    progress.set_status(tid, TaskStatus::Cached);
                    std::scoped_lock lk(install_mutex);
                    dl.link_from_cache(dep.name, version);
                    return;
                }

                progress.set_status(tid, TaskStatus::Downloading);
                try {
                    auto cache_path = dl.get_cache_path(dep.name, version);
                    fs::create_directories(cache_path);

                    std::string clone_cmd = "git -c advice.detachedHead=false clone --depth 1 --quiet ";
                    if (version != "HEAD") clone_cmd += "--branch " + version + " ";
                    clone_cmd += dep.github_url + " " + cache_path.string() + " > /dev/null 2>&1";

                    if (std::system(clone_cmd.c_str()) != 0) {
                        fs::remove_all(cache_path);
                        progress.set_status(tid, TaskStatus::Failed);
                        return;
                    }
                    auto git_dir = cache_path / ".git";
                    if (fs::exists(git_dir)) fs::remove_all(git_dir);

                    std::scoped_lock lk(install_mutex);
                    dl.link_from_cache(dep.name, version);
                    progress.set_status(tid, TaskStatus::Done);
                } catch (...) {
                    progress.set_status(tid, TaskStatus::Failed);
                }
            });
        }
        parallel_execute(tasks, 4);
    }

    // Compiled deps — up to 2 in parallel (they may share nix deps)
    if (!config.system_dependencies.empty()) {
        Downloader dl(local_cpm_dir_, global_cache_dir_);
        std::vector<std::function<void()>> tasks;
        tasks.reserve(config.system_dependencies.size());

        for (size_t i = 0; i < config.system_dependencies.size(); ++i) {
            tasks.emplace_back([&, i]() {
                const auto &dep = config.system_dependencies[i];
                int tid = sys_ids[i];
                progress.set_status(tid, TaskStatus::Building);
                try {
                    dl.resolve_system_dependency(dep, project_root_);
                    progress.set_status(tid, TaskStatus::Done);
                } catch (...) {
                    progress.set_status(tid, TaskStatus::Failed);
                }
            });
        }
        parallel_execute(tasks, 2);
    }

    if (has_work) progress.stop();

    // Nix [libs]
    resolve_nix_libraries(config);

    // Export headers + regenerate compile_commands.json
    Resolver resolver(project_root_);
    resolver.export_headers();

    std::cout << "[cpm] Packages ready.\n";
}

// ─── install_package ─────────────────────────────────────────────────────────

void Installer::install_package(const std::string &package_spec) {
    ensure_directories();

    GitDependency dep;
    std::string spec = package_spec;
    if (spec.starts_with("github:")) spec = spec.substr(7);

    auto at_pos = spec.find('@');
    if (at_pos != std::string::npos) {
        std::string repo = spec.substr(0, at_pos);
        dep.version = spec.substr(at_pos + 1);
        dep.github_url = "https://github.com/" + repo;
        auto slash = repo.find('/');
        dep.name = (slash != std::string::npos) ? repo.substr(slash + 1) : repo;
    } else {
        dep.github_url = "https://github.com/" + spec;
        dep.version = "*";
        auto slash = spec.find('/');
        dep.name = (slash != std::string::npos) ? spec.substr(slash + 1) : spec;
    }

    std::cout << "[cpm] Adding " << dep.name << "...\n";
    Downloader dl(local_cpm_dir_, global_cache_dir_);
    dl.clone_git_dependency(dep);

    Resolver resolver(project_root_);
    resolver.export_headers();

    std::cout << "[cpm] Added " << dep.name << "\n";
}

// ─── remove_package ──────────────────────────────────────────────────────────

void Installer::remove_package(const std::string &package_name) {
    auto package_path = local_cpm_dir_ / "packages" / package_name;
    if (!fs::exists(package_path) && !fs::is_symlink(package_path)) {
        std::cerr << "[cpm] Package '" << package_name << "' not found.\n";
        return;
    }

    fs::remove_all(package_path);

    // Rebuild include/ from scratch
    auto include_dir = local_cpm_dir_ / "include";
    if (fs::exists(include_dir)) {
        fs::remove_all(include_dir);
        fs::create_directories(include_dir);
    }

    Resolver resolver(project_root_);
    resolver.export_headers();

    std::cout << "[cpm] Removed " << package_name << "\n";
}

// ─── update ──────────────────────────────────────────────────────────────────

void Installer::update() {
    for (const auto &sub : {"packages", "include", "lib"}) {
        auto p = local_cpm_dir_ / sub;
        if (fs::exists(p)) fs::remove_all(p);
    }
    std::cout << "[cpm] Updating packages...\n";
    install();
}

// ─── list ────────────────────────────────────────────────────────────────────

void Installer::list() const {
    std::cout << "[cpm] Installed packages:\n";
    bool any = false;

    auto packages_dir = local_cpm_dir_ / "packages";
    if (fs::exists(packages_dir)) {
        for (const auto &entry : fs::directory_iterator(packages_dir)) {
            if (entry.is_directory() || entry.is_symlink()) {
                std::cout << "  [header] " << entry.path().filename().string() << "\n";
                any = true;
            }
        }
    }

    auto lib_dir = local_cpm_dir_ / "lib";
    if (fs::exists(lib_dir)) {
        for (const auto &entry : fs::directory_iterator(lib_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".a") {
                std::cout << "  [lib]    " << entry.path().filename().string() << "\n";
                any = true;
            }
        }
    }

    if (!any) std::cout << "  (none)\n";
}

} // namespace cpm

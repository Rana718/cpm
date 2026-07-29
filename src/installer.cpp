#include "cpm/installer.hpp"

#include "cpm/downloader.hpp"
#include "cpm/environment.hpp"
#include "cpm/nix_env.hpp"
#include "cpm/progress.hpp"
#include "cpm/resolver.hpp"
#include "cpm/toml_parser.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace cpm {

namespace fs = std::filesystem;

namespace {
struct LockedDependency {
    std::string kind;
    std::string name;
    std::string source;
    std::string requested;
    std::string resolved;
};

std::string read_file(const fs::path &path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void replace_file(const fs::path &path, const std::string &contents) {
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << contents;
    output.close();
    fs::rename(temporary, path);
}

std::vector<LockedDependency> read_lock(const fs::path &path) {
    std::vector<LockedDependency> dependencies;
    std::ifstream input(path);
    if (!input) return dependencies;
    std::string kind;
    unsigned int format = 0;
    while (input >> kind) {
        if (kind == "version") {
            input >> format;
            if (format != 1 && format != 2) throw std::runtime_error("unsupported cpm.lock version");
            continue;
        }
        if (format == 0) throw std::runtime_error("cpm.lock has no format version");
        LockedDependency dependency;
        dependency.kind = kind;
        if (!(input >> std::quoted(dependency.name) >> std::quoted(dependency.source))) {
            throw std::runtime_error("invalid cpm.lock entry");
        }
        if (format == 1) {
            dependency.requested = "__legacy__";
            if (!(input >> std::quoted(dependency.resolved))) throw std::runtime_error("invalid cpm.lock entry");
        } else if (!(input >> std::quoted(dependency.requested) >> std::quoted(dependency.resolved))) {
            throw std::runtime_error("invalid cpm.lock entry");
        }
        dependencies.emplace_back(std::move(dependency));
    }
    return dependencies;
}

void write_lock(const fs::path &path, const ProjectConfig &requested, const ProjectConfig &resolved) {
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write " + temporary);
    output << "version 2\n";
    for (size_t i = 0; i < resolved.git_dependencies.size(); ++i) {
        const auto &dependency = resolved.git_dependencies[i];
        output << "header " << std::quoted(dependency.name) << ' ' << std::quoted(dependency.github_url) << ' ' << std::quoted(requested.git_dependencies[i].version) << ' '
               << std::quoted(dependency.version) << '\n';
    }
    for (size_t i = 0; i < resolved.system_dependencies.size(); ++i) {
        const auto &dependency = resolved.system_dependencies[i];
        output << "system " << std::quoted(dependency.name) << ' ' << std::quoted(dependency.github_url) << ' ' << std::quoted(requested.system_dependencies[i].version) << ' '
               << std::quoted(dependency.version) << '\n';
    }
    output.close();
    fs::rename(temporary, path);
}

void acquire_install_lock(const fs::path &lock) {
    if (!fs::create_directory(lock)) {
        std::ifstream owner_file(lock / "pid");
        pid_t owner = 0;
        owner_file >> owner;
        if (owner > 0 && (::kill(owner, 0) == 0 || errno == EPERM)) {
            throw std::runtime_error("another cpm install is already running in this project (pid " + std::to_string(owner) + ")");
        }
        std::error_code error;
        fs::remove_all(lock, error);
        if (error || !fs::create_directory(lock)) {
            throw std::runtime_error("cannot reclaim stale install lock: " + lock.string());
        }
    }
    std::ofstream owner_file(lock / "pid", std::ios::trunc);
    if (!owner_file) {
        fs::remove_all(lock);
        throw std::runtime_error("cannot write install lock: " + lock.string());
    }
    owner_file << ::getpid() << '\n';
}
} // namespace

Installer::Installer(fs::path project_root, fs::path local_cpm_dir, fs::path global_cache_dir)
    : project_root_(std::move(project_root)), local_cpm_dir_(std::move(local_cpm_dir)), global_cache_dir_(std::move(global_cache_dir)) {}

// Nix [libs] resolution

void Installer::resolve_nix_libraries(const ProjectConfig &config, const fs::path &target_cpm_dir) {
    if (config.nix_libraries.empty()) return;

    NixEnv nix(target_cpm_dir, global_cache_dir_);
    if (!nix.available()) {
        throw std::runtime_error("[libs] requires Nix; run 'cpm setup' or remove the declared Nix libraries");
    }

    auto inc_dir = target_cpm_dir / "include";
    auto lib_dir = target_cpm_dir / "lib";
    fs::create_directories(inc_dir);
    fs::create_directories(lib_dir);

    for (const auto &nixlib : config.nix_libraries) {
        std::cout << "[cpm] resolving lib: " << nixlib.name << " (" << nixlib.nix_attr << ")\n";

        std::string dev_path = nix.build_package(nixlib.nix_attr + ".dev", config.nixpkgs);
        std::string lib_path = nix.build_package(nixlib.nix_attr, config.nixpkgs);

        if (dev_path.empty() && lib_path.empty()) {
            throw std::runtime_error("Nix package '" + nixlib.nix_attr + "' was not found");
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

// install

void Installer::install() { install_impl(false); }

void Installer::install_impl(bool refresh_lock) {
    auto toml_path = project_root_ / "cpm.toml";
    if (!fs::exists(toml_path)) throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");

    auto config = TomlParser::parse(toml_path);
    const auto requested_config = config;
    fs::create_directories(global_cache_dir_);

    const auto lock = project_root_ / ".cpm.install.lock";
    acquire_install_lock(lock);
    struct Cleanup {
        fs::path lock;
        fs::path staging;
        ~Cleanup() {
            std::error_code error;
            if (!staging.empty()) fs::remove_all(staging, error);
            fs::remove_all(lock, error);
        }
    } cleanup{lock, project_root_ / (".cpm-transaction-" + std::to_string(::getpid()))};
    const auto backup = project_root_ / ".cpm-backup";
    if (fs::exists(backup)) {
        if (!fs::exists(local_cpm_dir_))
            fs::rename(backup, local_cpm_dir_);
        else
            fs::remove_all(backup);
    }
    if (fs::exists(cleanup.staging)) fs::remove_all(cleanup.staging);
    Environment(cleanup.staging).create();
    const auto target_cpm = cleanup.staging / ".cpm";

    const auto locked = refresh_lock ? std::vector<LockedDependency>{} : read_lock(project_root_ / "cpm.lock");
    Downloader version_resolver(target_cpm, global_cache_dir_);
    auto resolve_version = [&](const std::string &kind, const std::string &name, const std::string &source, std::string &version) {
        const auto requested = version.empty() ? std::string("*") : version;
        const auto found = std::ranges::find_if(locked, [&](const auto &entry) { return entry.kind == kind && entry.name == name && entry.source == source && entry.requested == requested; });
        if (found != locked.end())
            version = found->resolved;
        else
            version = version_resolver.resolve_git_ref(source, requested, name);
    };
    for (auto &dependency : config.git_dependencies) {
        resolve_version("header", dependency.name, dependency.github_url, dependency.version);
    }
    for (auto &dependency : config.system_dependencies) {
        resolve_version("system", dependency.name, dependency.github_url, dependency.version);
    }

    // Parallel download/install
    ProgressDisplay progress;
    std::atomic<size_t> failures = 0;
    std::mutex error_mutex;
    std::vector<std::string> errors;

    std::vector<int> header_ids, sys_ids;
    header_ids.reserve(config.git_dependencies.size());
    sys_ids.reserve(config.system_dependencies.size());

    for (const auto &dep : config.git_dependencies) header_ids.emplace_back(progress.add_task(dep.name));
    for (const auto &dep : config.system_dependencies) sys_ids.emplace_back(progress.add_task(dep.name));

    const bool has_work = !config.git_dependencies.empty() || !config.system_dependencies.empty();
    if (has_work) progress.start();

    // Header-only deps — up to 4 in parallel
    if (!config.git_dependencies.empty()) {
        Downloader dl(target_cpm, global_cache_dir_);
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

                if (dl.is_cached(dep.name, version, dep.github_url)) {
                    progress.set_status(tid, TaskStatus::Cached);
                    dl.link_from_cache(dep.name, version, dep.github_url);
                    return;
                }

                progress.set_status(tid, TaskStatus::Downloading);
                try {
                    GitDependency resolved = dep;
                    resolved.version = version;
                    dl.clone_git_dependency(resolved);
                    progress.set_status(tid, TaskStatus::Done);
                } catch (const std::exception &e) {
                    ++failures;
                    std::scoped_lock error_lock(error_mutex);
                    errors.emplace_back(dep.name + ": " + e.what());
                    progress.set_status(tid, TaskStatus::Failed);
                }
            });
        }
        parallel_execute(tasks, 4);
    }

    // Compiled deps — up to 2 in parallel (they may share nix deps)
    if (!config.system_dependencies.empty()) {
        Downloader dl(target_cpm, global_cache_dir_);
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
                } catch (const std::exception &e) {
                    ++failures;
                    std::scoped_lock error_lock(error_mutex);
                    errors.emplace_back(dep.name + ": " + e.what());
                    progress.set_status(tid, TaskStatus::Failed);
                }
            });
        }
        parallel_execute(tasks, 2);
    }

    if (has_work) progress.stop();

    if (failures != 0) {
        std::ostringstream message;
        message << failures << " package installation(s) failed";
        for (const auto &error : errors) message << "\n  - " << error;
        throw std::runtime_error(message.str());
    }

    // Nix [libs]
    resolve_nix_libraries(config, target_cpm);

    // Export headers + regenerate compile_commands.json
    Resolver resolver(cleanup.staging);
    resolver.export_headers();
    const auto staged_lock = cleanup.staging / "cpm.lock";
    write_lock(staged_lock, requested_config, config);

    if (fs::exists(backup)) fs::remove_all(backup);
    std::error_code publish_error;
    if (fs::exists(local_cpm_dir_)) fs::rename(local_cpm_dir_, backup, publish_error);
    if (publish_error) throw std::runtime_error("cannot stage existing environment: " + publish_error.message());
    fs::rename(target_cpm, local_cpm_dir_, publish_error);
    if (publish_error) {
        if (fs::exists(backup)) fs::rename(backup, local_cpm_dir_);
        throw std::runtime_error("cannot publish environment: " + publish_error.message());
    }
    try {
        Environment(project_root_).create();
        fs::rename(staged_lock, project_root_ / "cpm.lock");
    } catch (...) {
        fs::remove_all(local_cpm_dir_);
        if (fs::exists(backup)) fs::rename(backup, local_cpm_dir_);
        throw;
    }
    if (fs::is_directory(backup / "objects") && !fs::exists(local_cpm_dir_ / "objects")) {
        fs::rename(backup / "objects", local_cpm_dir_ / "objects", publish_error);
        publish_error.clear(); 
    }
    fs::remove_all(backup);

    std::cout << "[cpm] Packages ready.\n";
}

// install_package

void Installer::install_package(const std::string &package_spec, const std::string &kind) {
    const auto toml_path = project_root_ / "cpm.toml";
    if (!fs::exists(toml_path)) throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");
    std::string spec = package_spec;
    const auto original_manifest = read_file(toml_path);
    if (kind == "nix") {
        const auto equals = spec.find('=');
        const auto attribute = equals == std::string::npos ? spec : spec.substr(equals + 1);
        const auto name = equals == std::string::npos ? spec : spec.substr(0, equals);
        TomlParser::upsert_nix_library(toml_path, {name, attribute});
        try {
            install();
        } catch (...) {
            replace_file(toml_path, original_manifest);
            throw;
        }
        std::cout << "[cpm] Added " << name << "\n";
        return;
    }
    if (kind != "header" && kind != "system") throw std::runtime_error("unknown package kind: " + kind);
    std::string name;
    const auto equals = spec.find('=');
    if (equals != std::string::npos) {
        name = spec.substr(0, equals);
        spec = spec.substr(equals + 1);
    } else {
        auto repository = spec;
        size_t ref = std::string::npos;
        if (repository.starts_with("github:")) {
            ref = repository.find('@');
        } else if (const auto scheme = repository.find("://"); scheme != std::string::npos) {
            const auto path = repository.find('/', scheme + 3);
            if (path != std::string::npos) ref = repository.find('@', path);
        } else if (repository.starts_with("git@")) {
            const auto path = repository.find(':');
            if (path != std::string::npos) ref = repository.find('@', path);
        } else {
            ref = repository.find('@');
        }
        if (ref != std::string::npos) repository.erase(ref);
        const auto slash = repository.find_last_of("/:");
        name = slash == std::string::npos ? repository : repository.substr(slash + 1);
        if (name.ends_with(".git")) name.erase(name.size() - 4);
    }
    auto dep = TomlParser::parse_git_dependency(name, spec);

    std::cout << "[cpm] Adding " << dep.name << "...\n";
    TomlParser::upsert_dependency(toml_path, dep, kind == "system");
    try {
        install();
    } catch (...) {
        replace_file(toml_path, original_manifest);
        throw;
    }

    std::cout << "[cpm] Added " << dep.name << "\n";
}

// remove_package

void Installer::remove_package(const std::string &package_name) {
    const auto toml_path = project_root_ / "cpm.toml";
    if (!fs::exists(toml_path)) throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");
    const auto original_manifest = read_file(toml_path);
    if (!TomlParser::remove_dependency(toml_path, package_name)) {
        std::cerr << "[cpm] Package '" << package_name << "' not found.\n";
        return;
    }

    try {
        install();
    } catch (...) {
        replace_file(toml_path, original_manifest);
        throw;
    }

    std::cout << "[cpm] Removed " << package_name << "\n";
}

void Installer::update() {
    std::cout << "[cpm] Updating packages...\n";
    install_impl(true);
}

void Installer::list() const {
    const auto manifest = project_root_ / "cpm.toml";
    if (!fs::exists(manifest)) throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");
    const auto config = TomlParser::parse(manifest);
    const auto lock = read_lock(project_root_ / "cpm.lock");
    const auto locked_version = [&](const std::string &kind, const std::string &name, const std::string &source, const std::string &declared) {
        const auto requested = declared.empty() ? std::string("*") : declared;
        const auto found = std::ranges::find_if(lock, [&](const auto &entry) { return entry.kind == kind && entry.name == name && entry.source == source && entry.requested == requested; });
        return found == lock.end() ? declared : found->resolved;
    };

    std::cout << "[cpm] Dependencies" << (fs::exists(local_cpm_dir_ / ".cpm_env") ? ":\n" : " (not installed):\n");
    for (const auto &dependency : config.git_dependencies) {
        std::cout << "  [header] " << dependency.name << " @ " << locked_version("header", dependency.name, dependency.github_url, dependency.version) << '\n';
    }
    for (const auto &dependency : config.system_dependencies) {
        std::cout << "  [system] " << dependency.name << " @ " << locked_version("system", dependency.name, dependency.github_url, dependency.version) << '\n';
    }
    for (const auto &library : config.nix_libraries) {
        std::cout << "  [nix]    " << library.name << " = " << library.nix_attr << '\n';
    }
    if (config.git_dependencies.empty() && config.system_dependencies.empty() && config.nix_libraries.empty()) {
        std::cout << "  (none)\n";
    }
}

} // namespace cpm

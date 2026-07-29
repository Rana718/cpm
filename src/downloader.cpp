#include "cpm/downloader.hpp"

#include "cpm/nix_env.hpp"
#include "cpm/process.hpp"
#include "cpm/toml_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cpm {
namespace fs = std::filesystem;
namespace {

bool library_file(const fs::path &path) {
    const auto name = path.filename().string();
    return path.extension() == ".a" || path.extension() == ".so" || name.find(".so.") != std::string::npos || path.extension() == ".dylib" || path.extension() == ".lib";
}

bool header_file(const fs::path &path) {
    const auto extension = path.extension().string();
    return extension == ".h" || extension == ".hh" || extension == ".hpp" || extension == ".hxx" || extension == ".inc" || extension == ".ipp" || extension == ".tpp";
}

std::string shell_quote(const std::string &value) {
    std::string result = "'";
    for (const char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}

std::string shell_join(const std::vector<std::string> &arguments) {
    std::ostringstream command;
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i) command << ' ';
        command << shell_quote(arguments[i]);
    }
    return command.str();
}

std::vector<std::string> split_flags(const std::string &value) {
    std::vector<std::string> flags;
    std::string current;
    char quote = 0;
    bool escaped = false;
    for (const char c : value) {
        if (escaped) {
            current.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (quote != 0) {
            if (c == quote)
                quote = 0;
            else
                current.push_back(c);
        } else if (c == '\'' || c == '"')
            quote = c;
        else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                flags.emplace_back(std::move(current));
                current.clear();
            }
        } else
            current.push_back(c);
    }
    if (!current.empty()) flags.emplace_back(std::move(current));
    return flags;
}

void copy_tree(const fs::path &source, const fs::path &destination, bool overwrite = true) {
    if (!fs::exists(source)) return;
    std::error_code error;
    fs::create_directories(destination, error);
    if (error) throw fs::filesystem_error("cannot create artifact directory", destination, error);
    for (const auto &entry : fs::recursive_directory_iterator(source, fs::directory_options::skip_permission_denied)) {
        const auto relative = entry.path().lexically_relative(source);
        const auto target = destination / relative;
        if (entry.is_symlink()) {
            fs::create_directories(target.parent_path());
            if (fs::exists(target) || fs::is_symlink(target)) {
                if (!overwrite) continue;
                fs::remove_all(target);
            }
            fs::create_symlink(fs::read_symlink(entry.path()), target);
        } else if (entry.is_directory())
            fs::create_directories(target);
        else if (entry.is_regular_file()) {
            fs::create_directories(target.parent_path());
            const auto options = overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::skip_existing;
            fs::copy_file(entry.path(), target, options);
        }
    }
}

std::string unique_suffix() { return ".tmp-" + std::to_string(::getpid()) + "-" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())); }

bool source_matches(const fs::path &path, const std::string &source) {
    if (source.empty()) return true;
    std::ifstream metadata(path / ".cpm-source");
    std::string recorded;
    std::getline(metadata, recorded);
    return recorded == source;
}

bool has_source_metadata(const fs::path &path) { return fs::is_regular_file(path / ".cpm-source"); }

bool current_source_cache(const fs::path &path) {
    std::ifstream version(path / ".cpm-cache-version");
    unsigned int value = 0;
    return (version >> value) && value == 2;
}

void write_source(const fs::path &path, const std::string &source) {
    std::ofstream metadata(path / ".cpm-source", std::ios::trunc);
    metadata << source << '\n';
    std::ofstream version(path / ".cpm-cache-version", std::ios::trunc);
    version << "2\n";
}

bool cmake_has_unexpanded_version(const fs::path &path) {
    std::ifstream input(path);
    const std::string contents((std::istreambuf_iterator<char>(input)), {});
    static const std::regex placeholder(R"(\bVERSION\s+([_@][A-Za-z0-9_.@-]*[_@]))", std::regex::icase);
    return std::regex_search(contents, placeholder);
}

std::vector<std::string> disabled_cmake_features(const fs::path &path) {
    std::ifstream input(path);
    const std::string contents((std::istreambuf_iterator<char>(input)), {});
    static const std::regex option(R"(option\s*\(\s*([A-Za-z0-9_]+))", std::regex::icase);
    static constexpr std::array<std::string_view, 10> disabled = {"_APP", "_APPS", "_BENCHMARK", "_BENCHMARKS", "_DEMO", "_DEMOS", "_DOC", "_DOCS", "_TEST", "_TESTING"};
    std::vector<std::string> arguments;
    for (std::sregex_iterator match(contents.begin(), contents.end(), option), end; match != end; ++match) {
        const auto name = (*match)[1].str();
        std::string upper = name;
        std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (std::ranges::any_of(disabled, [&](const auto suffix) { return upper.ends_with(suffix); })) arguments.emplace_back("-D" + name + "=OFF");
    }
    std::ranges::sort(arguments);
    arguments.erase(std::unique(arguments.begin(), arguments.end()), arguments.end());
    return arguments;
}

bool clone_repository(const std::string &url, const std::string &version, const fs::path &destination, bool submodules, std::string &diagnostic) {
    std::vector<std::string> clone = {"git", "-c", "advice.detachedHead=false", "clone", "--depth", "1", "--quiet"};
    if (version != "HEAD") {
        clone.emplace_back("--branch");
        clone.emplace_back(version);
    }
    clone.emplace_back(url);
    clone.emplace_back(destination.string());
    auto result = Process::run(clone, {}, {}, true);

    // exact ref into an otherwise empty checkout.
    if (result.exit_code != 0 && version != "HEAD") {
        fs::remove_all(destination);
        result = Process::run({"git", "clone", "--quiet", "--filter=blob:none", "--no-checkout", url, destination.string()}, {}, {}, true);
        if (result.exit_code == 0) {
            result = Process::run({"git", "fetch", "--depth", "1", "origin", version}, destination, {}, true);
            if (result.exit_code == 0) result = Process::run({"git", "checkout", "--quiet", "FETCH_HEAD"}, destination, {}, true);
        }
    }
    if (result.exit_code != 0) {
        diagnostic = std::move(result.output);
        return false;
    }
    if (submodules && fs::exists(destination / ".gitmodules")) {
        result = Process::run({"git", "submodule", "update", "--init", "--recursive", "--depth", "1"}, destination, {}, true);
        if (result.exit_code != 0) {
            result = Process::run({"git", "submodule", "update", "--init", "--recursive"}, destination, {}, true);
        }
        if (result.exit_code != 0) {
            diagnostic = std::move(result.output);
            return false;
        }
    }
    return true;
}

void publish_directory(const fs::path &temporary, const fs::path &destination) {
    std::error_code error;
    fs::rename(temporary, destination, error);
    if (!error) return;
    if (fs::is_directory(destination) && !fs::is_empty(destination)) {
        fs::remove_all(temporary);
        return;
    }
    fs::remove_all(temporary);
    throw std::runtime_error("cannot publish cache entry '" + destination.string() + "': " + error.message());
}

// Rewrite the prefix= line in every .pc file under a built cache directory.
// cmake --install bakes the temporary build prefix into pkg-config files; after
// publish_directory renames the temp dir we must update those paths so that
// pkg-config --cflags/--libs resolves correctly.
void rewrite_pkgconfig_prefix(const fs::path &built, const std::string &old_prefix) {
    const auto new_prefix = built.string();
    if (old_prefix == new_prefix) return;
    for (const auto &pc_root : {built / "lib" / "pkgconfig", built / "lib64" / "pkgconfig", built / "share" / "pkgconfig"}) {
        if (!fs::is_directory(pc_root)) continue;
        for (const auto &entry : fs::directory_iterator(pc_root)) {
            if (entry.path().extension() != ".pc") continue;
            std::ifstream in(entry.path());
            if (!in) continue;
            std::string contents((std::istreambuf_iterator<char>(in)), {});
            in.close();
            std::string updated;
            updated.reserve(contents.size());
            std::istringstream lines(contents);
            std::string line;
            bool changed = false;
            while (std::getline(lines, line)) {
                if (line.rfind("prefix=", 0) == 0 && line.substr(7) == old_prefix) {
                    updated += "prefix=" + new_prefix + '\n';
                    changed = true;
                } else {
                    std::string::size_type pos = 0;
                    while ((pos = line.find(old_prefix, pos)) != std::string::npos) {
                        line.replace(pos, old_prefix.size(), new_prefix);
                        pos += new_prefix.size();
                        changed = true;
                    }
                    updated += line + '\n';
                }
            }
            if (!changed) continue;
            const auto tmp = entry.path().string() + ".tmp";
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) continue;
            out << updated;
            out.close();
            fs::rename(tmp, entry.path());
        }
    }
}

} // namespace

Downloader::Downloader(fs::path local_cpm_dir, fs::path global_cache_dir) : local_cpm_dir_(std::move(local_cpm_dir)), global_cache_dir_(std::move(global_cache_dir)) {}

std::string Downloader::cache_component(const std::string &value) {
    std::string safe;
    uint64_t hash = 1469598103934665603ULL;
    bool changed = false;
    for (const unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-')
            safe.push_back(static_cast<char>(c));
        else {
            safe.push_back('_');
            changed = true;
        }
    }
    if (safe.empty()) safe = "default";
    if (changed) {
        std::ostringstream suffix;
        suffix << std::hex << hash;
        safe += "-" + suffix.str();
    }
    return safe;
}

static std::string source_suffix(const std::string &source) {
    if (source.empty()) return {};
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : source) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream suffix;
    suffix << '-' << std::hex << hash;
    return suffix.str();
}

fs::path Downloader::get_cache_path(const std::string &name, const std::string &version, const std::string &source) const {
    return global_cache_dir_ / (cache_component(name) + "-" + cache_component(version) + source_suffix(source));
}

fs::path Downloader::get_source_cache_path(const std::string &name, const std::string &version, const std::string &source) const {
    auto path = get_cache_path(name, version, source);
    path += "-src";
    return path;
}

fs::path Downloader::get_built_cache_path(const std::string &name, const std::string &version, const std::string &source) const {
    auto path = get_cache_path(name, version, source);
    path += "-built";
    return path;
}

bool Downloader::is_cached(const std::string &name, const std::string &version, const std::string &source) const {
    std::error_code error;
    const auto path = get_cache_path(name, version, source);
    return fs::is_directory(path, error) && !fs::is_empty(path, error) && source_matches(path, source);
}

void Downloader::link_from_cache(const std::string &name, const std::string &version, const std::string &source) {
    const auto cache = get_cache_path(name, version, source);
    if (!is_cached(name, version, source)) throw std::runtime_error("incomplete cache entry: " + cache.string());
    const auto local = local_cpm_dir_ / "packages" / name;
    fs::create_directories(local.parent_path());
    if (fs::exists(local) || fs::is_symlink(local)) fs::remove_all(local);
    fs::create_directory_symlink(fs::absolute(cache), local);
}

std::string Downloader::resolve_latest_tag(const std::string &url, const std::string &name) {
    const auto result = Process::run({"git", "ls-remote", "--refs", "--tags", "--sort=-v:refname", url}, {}, {}, true);
    if (result.exit_code == 0) {
        const auto first_line = result.output.substr(0, result.output.find('\n'));
        constexpr std::string_view marker = "refs/tags/";
        const auto position = first_line.find(marker);
        if (position != std::string::npos) {
            const auto tag = first_line.substr(position + marker.size());
            std::cout << "[cpm] " << name << " -> latest: " << tag << '\n';
            return tag;
        }
    }
    std::cout << "[cpm] " << name << " -> no tags, using HEAD\n";
    return "HEAD";
}

std::string Downloader::resolve_git_ref(const std::string &url, const std::string &requested_ref, const std::string &name) {
    std::string ref = requested_ref.empty() || requested_ref == "*" ? resolve_latest_tag(url, name) : requested_ref;
    const bool hexadecimal = !ref.empty() && std::ranges::all_of(ref, [](unsigned char c) { return std::isxdigit(c); });
    if (hexadecimal && (ref.size() == 40 || ref.size() == 64)) return ref;

    std::vector<std::string> arguments = {"git", "ls-remote", url};
    if (ref == "HEAD") {
        arguments.emplace_back("HEAD");
    } else {
        arguments.emplace_back(ref);
        arguments.emplace_back("refs/heads/" + ref);
        arguments.emplace_back("refs/tags/" + ref);
        arguments.emplace_back("refs/tags/" + ref + "^{}");
    }
    const auto result = Process::run(arguments, {}, {}, true);
    if (result.exit_code != 0) throw std::runtime_error("failed to resolve Git ref '" + ref + "' for " + name + ":\n" + result.output);

    std::string selected;
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto separator = line.find_first_of(" \t");
        if (separator == std::string::npos) continue;
        const auto commit = line.substr(0, separator);
        const auto ref_start = line.find_first_not_of(" \t", separator);
        if (ref_start == std::string::npos) continue;
        const auto remote_ref = line.substr(ref_start);
        if (remote_ref.ends_with("^{}")) return commit;
        if (selected.empty()) selected = commit;
    }
    if (selected.empty()) throw std::runtime_error("Git ref '" + ref + "' was not found for " + name);
    std::cout << "[cpm] " << name << " -> " << ref << " @ " << selected.substr(0, std::min<size_t>(12, selected.size())) << '\n';
    return selected;
}

void Downloader::clone_git_dependency(const GitDependency &dependency) {
    const auto version = dependency.version.empty() || dependency.version == "*" ? resolve_git_ref(dependency.github_url, dependency.version, dependency.name) : dependency.version;
    if (is_cached(dependency.name, version, dependency.github_url)) {
        link_from_cache(dependency.name, version, dependency.github_url);
        return;
    }
    fs::create_directories(global_cache_dir_);
    const auto cache = get_cache_path(dependency.name, version, dependency.github_url);
    if (fs::exists(cache)) {
        if (has_source_metadata(cache) && !source_matches(cache, dependency.github_url)) {
            throw std::runtime_error("cache identity collision for " + dependency.name);
        }
        fs::remove_all(cache); // Incomplete or legacy cache entry.
    }
    auto temporary = cache;
    temporary += unique_suffix();
    if (fs::exists(temporary)) fs::remove_all(temporary);
    std::string diagnostic;
    if (!clone_repository(dependency.github_url, version, temporary, true, diagnostic)) {
        fs::remove_all(temporary);
        throw std::runtime_error("failed to clone " + dependency.github_url + (diagnostic.empty() ? "" : ":\n" + diagnostic));
    }
    fs::remove_all(temporary / ".git");
    write_source(temporary, dependency.github_url);
    publish_directory(temporary, cache);
    link_from_cache(dependency.name, version, dependency.github_url);
}

void Downloader::resolve_system_dependency(const SystemDependency &dependency, const fs::path &project_root) {
    const auto version = dependency.version.empty() || dependency.version == "*" ? resolve_git_ref(dependency.github_url, dependency.version, dependency.name) : dependency.version;
    const auto source = get_source_cache_path(dependency.name, version, dependency.github_url);
    const auto built = get_built_cache_path(dependency.name, version, dependency.github_url);
    const auto valid_artifacts = [&] {
        std::error_code error;
        return fs::is_directory(built, error) && source_matches(built, dependency.github_url) &&
               ((fs::is_directory(built / "include", error) && !fs::is_empty(built / "include", error)) || (fs::is_directory(built / "lib", error) && !fs::is_empty(built / "lib", error)));
    };

    if (!valid_artifacts()) {
        if (fs::exists(built) && has_source_metadata(built) && !source_matches(built, dependency.github_url)) {
            throw std::runtime_error("cache identity collision for " + dependency.name);
        }
        if (fs::exists(built)) fs::remove_all(built);
        if (fs::exists(source) && has_source_metadata(source) && !source_matches(source, dependency.github_url)) {
            throw std::runtime_error("source cache identity collision for " + dependency.name);
        }
        if (!fs::is_directory(source) || fs::is_empty(source) || !has_source_metadata(source) || !current_source_cache(source)) {
            if (fs::exists(source)) fs::remove_all(source);
            auto temporary = source;
            temporary += unique_suffix();
            std::string diagnostic;
            if (!clone_repository(dependency.github_url, version, temporary, true, diagnostic)) {
                fs::remove_all(temporary);
                throw std::runtime_error("failed to clone " + dependency.github_url + (diagnostic.empty() ? "" : ":\n" + diagnostic));
            }
            fs::remove_all(temporary / ".git");
            write_source(temporary, dependency.github_url);
            publish_directory(temporary, source);
        }

        auto temporary = built;
        temporary += unique_suffix();
        if (fs::exists(temporary)) fs::remove_all(temporary);
        fs::create_directories(temporary);
        if (!build_from_source(dependency.name, source, temporary, project_root)) {
            fs::remove_all(temporary);
            throw std::runtime_error("failed to build " + dependency.name);
        }
        write_source(temporary, dependency.github_url);
        const auto old_prefix = temporary.string();
        publish_directory(temporary, built);
        rewrite_pkgconfig_prefix(built, old_prefix);
    }
    install_built_library(dependency.name, built);
}

bool Downloader::build_from_source(const std::string &name, const fs::path &source_cache, const fs::path &prefix, const fs::path &project_root) {
    fs::create_directories(prefix / "include");
    fs::create_directories(prefix / "lib");

    // Build scripts are allowed to generate or rewrite files, so never run them
    // in the immutable source cache shared by projects.
    const auto source = prefix / ".cpm-work";
    const auto build = prefix / ".cpm-build";
    copy_tree(source_cache, source);

    ProjectConfig config;
    if (!project_root.empty() && fs::exists(project_root / "cpm.toml")) config = TomlParser::parse(project_root / "cpm.toml");
    NixEnv nix(local_cpm_dir_, global_cache_dir_);
    fs::path shell;
    const auto shell_file = prefix / ".cpm-shell.nix";
    std::string shell_diagnostic;
    if (nix.available()) {
        auto dependencies = nix.detect_nix_deps(source);
        dependencies.insert(dependencies.end(), config.extra_nix_deps.begin(), config.extra_nix_deps.end());
        std::ranges::sort(dependencies);
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
        std::string compiler = config.compiler;
        if (compiler.starts_with("gcc-"))
            compiler = "gcc" + compiler.substr(4);
        else if (compiler.starts_with("clang-"))
            compiler = "clang_" + compiler.substr(6);
        else if (compiler.empty() || compiler == "gcc")
            compiler = "gcc";
        else if (compiler == "clang")
            compiler = "clang";
        std::string expression = nix.generate_shell_nix(compiler, config.cpp_standard, dependencies,
            (!config.nix_config.empty() && !project_root.empty()) ? project_root / config.nix_config : fs::path{});
        if (!config.nixpkgs.empty()) {
            const auto import = expression.find("import <nixpkgs>");
            if (import != std::string::npos) {
                expression.replace(import, std::string("import <nixpkgs>").size(),
                    "import (builtins.fetchTarball { url = \"https://github.com/NixOS/nixpkgs/archive/" + config.nixpkgs + ".tar.gz\"; })");
            }
        }
        shell = shell_file;
        std::ofstream output(shell);
        output << expression;
        output.close();
        const auto check = Process::run({"nix-shell", shell.string(), "--run", "true"}, {}, {}, true);
        if (check.exit_code != 0) {
            const bool required = !config.extra_nix_deps.empty() || !config.nix_libraries.empty() || config.compiler.starts_with("gcc-") || config.compiler.starts_with("clang-");
            if (required) throw std::runtime_error("cannot create required Nix build shell:\n" + check.output);
            shell_diagnostic = check.output;
            shell.clear();
        }
    }

    std::map<std::string, std::string> build_environment;
    if (shell.empty() && (config.compiler.starts_with("gcc-") || config.compiler.starts_with("clang-"))) {
        const bool gcc = config.compiler.starts_with("gcc-");
        const auto version = config.compiler.substr(gcc ? 4 : 6);
        const auto c_compiler = std::string(gcc ? "gcc-" : "clang-") + version;
        const auto cxx_compiler = std::string(gcc ? "g++-" : "clang++-") + version;
        if (!Process::command_exists(c_compiler) || !Process::command_exists(cxx_compiler)) {
            throw std::runtime_error("requested compiler is unavailable: " + config.compiler);
        }
        build_environment = {{"CC", c_compiler}, {"CXX", cxx_compiler}};
    }

    auto execute = [&](const std::vector<std::string> &arguments, const fs::path &cwd, bool capture = true) {
        if (shell.empty()) return Process::run(arguments, cwd, build_environment, capture);
        return Process::run({"nix-shell", shell.string(), "--run", "cd " + shell_quote(cwd.string()) + " && " + shell_join(arguments)}, {}, {}, capture);
    };

    const auto jobs = "-j" + std::to_string(std::max(1u, std::thread::hardware_concurrency()));
    bool attempted = false;
    bool succeeded = false;
    std::vector<std::pair<std::string, std::string>> diagnostics;
    auto attempt = [&](const std::string &adapter, auto &&operation) {
        if (succeeded) return;
        attempted = true;
        std::cout << "[cpm] " << name << ": trying " << adapter << '\n';
        auto candidate = operation();
        if (candidate.exit_code == 0) {
            succeeded = true;
            return;
        }
        diagnostics.emplace_back(adapter, std::move(candidate.output));
    };

    auto build_configured_tree = [&](ProcessResult configured, const fs::path &root) {
        if (configured.exit_code != 0) return configured;
        std::vector<fs::path> trees;
        try {
            for (const auto &entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file() && entry.path().filename() == "CMakeCache.txt") trees.emplace_back(entry.path().parent_path());
            }
        } catch (const fs::filesystem_error &) {
        }
        std::ranges::sort(trees, [](const auto &left, const auto &right) { return left.native().size() < right.native().size(); });
        if (trees.empty()) return ProcessResult{1, "configure completed but did not create a CMake build tree"};
        ProcessResult result{1, "no configured build tree succeeded"};
        for (const auto &tree : trees) {
            result = execute({"cmake", "--build", tree.string(), "--parallel"}, source);
            if (result.exit_code != 0) continue;
            const auto installed = execute({"cmake", "--install", tree.string()}, source);
            if (installed.exit_code != 0) std::cout << "[cpm] " << name << ": no install target; collecting build artifacts\n";
            return result;
        }
        return result;
    };

    if (fs::exists(source / "configure.py")) {
        attempt("configure.py", [&] {
            const auto help = execute({"python3", (source / "configure.py").string(), "--help"}, source);
            std::vector<std::string> arguments = {"python3", (source / "configure.py").string(), "--prefix=" + prefix.string()};
            const auto supports = [&](std::string_view option) { return help.output.contains(option); };
            const auto configure_build = source / ".cpm-configure-build";
            if (supports("--mode")) arguments.emplace_back("--mode=release");
            if (supports("--build-root")) arguments.emplace_back("--build-root=" + configure_build.string());
            for (const auto option : {"--without-tests", "--without-apps", "--without-demos", "--without-docs", "--disable-dpdk", "--disable-docs"})
                if (supports(option)) arguments.emplace_back(option);
            return build_configured_tree(execute(arguments, source), source);
        });
    }

    if (fs::exists(source / "CMakeLists.txt") && !cmake_has_unexpanded_version(source / "CMakeLists.txt")) {
        attempt("CMake", [&] {
            std::vector<std::string> configure = {"cmake", "-S", source.string(), "-B", build.string(), "-DCMAKE_INSTALL_PREFIX=" + prefix.string(), "-DCMAKE_PREFIX_PATH=" + prefix.string(),
                "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", "-DBUILD_TESTING=OFF", "-DBUILD_TESTS=OFF", "-DBUILD_EXAMPLES=OFF"};
            const auto feature_options = disabled_cmake_features(source / "CMakeLists.txt");
            configure.insert(configure.end(), feature_options.begin(), feature_options.end());
            if (Process::command_exists("ninja")) {
                configure.emplace_back("-G");
                configure.emplace_back("Ninja");
            }
            auto result = execute(configure, source);
            if (result.exit_code == 0) result = execute({"cmake", "--build", build.string(), "--parallel"}, source);
            if (result.exit_code == 0) {
                const auto installed = execute({"cmake", "--install", build.string()}, source);
                if (installed.exit_code != 0) std::cout << "[cpm] " << name << ": no install target; collecting build artifacts\n";
            }
            return result;
        });
    } else if (fs::exists(source / "CMakeLists.txt")) {
        diagnostics.emplace_back("CMake", "top-level CMakeLists.txt contains an unexpanded release-version placeholder");
    }

    if (fs::exists(source / "cooking.sh")) {
        attempt("cooking.sh", [&] {
            const auto cooking_build = source / ".cpm-cooking-build";
            std::vector<std::string> arguments = {"bash", (source / "cooking.sh").string(), "-t", "Release", "-g", "Ninja", "-d", cooking_build.string(), "-p", (prefix / ".cpm-cooking").string(),
                "--", "-DCMAKE_INSTALL_PREFIX=" + prefix.string(), "-DBUILD_TESTING=OFF", "-DBUILD_TESTS=OFF", "-DBUILD_EXAMPLES=OFF"};
            const auto feature_options = disabled_cmake_features(source / "CMakeLists.txt");
            arguments.insert(arguments.end(), feature_options.begin(), feature_options.end());
            auto result = execute(arguments, source);
            if (result.exit_code != 0) return result;
            result = execute({"cmake", "--build", cooking_build.string(), "--parallel"}, source);
            if (result.exit_code == 0) {
                const auto installed = execute({"cmake", "--install", cooking_build.string()}, source);
                if (installed.exit_code != 0) std::cout << "[cpm] " << name << ": no install target; collecting build artifacts\n";
            }
            return result;
        });
    }

    if (fs::exists(source / "meson.build")) {
        attempt("Meson", [&] {
            auto result = execute({"meson", "setup", build.string(), source.string(), "--prefix", prefix.string(), "--buildtype", "release"}, source);
            if (result.exit_code == 0) result = execute({"meson", "compile", "-C", build.string()}, source);
            if (result.exit_code == 0) {
                const auto installed = execute({"meson", "install", "-C", build.string()}, source);
                if (installed.exit_code != 0) std::cout << "[cpm] " << name << ": Meson install failed; collecting build artifacts\n";
            }
            return result;
        });
    }

    if (fs::exists(source / "configure")) {
        attempt("configure", [&] {
            auto result = execute({(source / "configure").string(), "--prefix=" + prefix.string()}, source);
            if (result.exit_code == 0) result = execute({"make", jobs}, source);
            if (result.exit_code == 0) {
                const auto installed = execute({"make", "install"}, source);
                if (installed.exit_code != 0) std::cout << "[cpm] " << name << ": no make install target; collecting build artifacts\n";
            }
            return result;
        });
    }

    if (fs::exists(source / "Makefile") || fs::exists(source / "makefile")) {
        attempt("Make", [&] {
            if (fs::exists(source / "package.json") && fs::exists(source / "package-lock.json")) {
                auto prepared = execute({"npm", "ci", "--no-audit", "--no-fund"}, source);
                if (prepared.exit_code != 0) return prepared;
                prepared = execute({"npm", "run", "build", "--if-present"}, source);
                if (prepared.exit_code != 0) return prepared;
            }
            auto result = execute({"make", jobs}, source);
            if (result.exit_code == 0) {
                const auto installed = execute({"make", "install", "PREFIX=" + prefix.string()}, source);
                if (installed.exit_code != 0) std::cout << "[cpm] " << name << ": no make install target; collecting build artifacts\n";
            }
            return result;
        });
    }

    // Standard installs are preferred. Also collect build outputs for projects
    // without an install target and generated headers produced during the build.
    if (fs::is_directory(source / "include") && fs::is_empty(prefix / "include")) copy_tree(source / "include", prefix / "include");
    if (!attempted && fs::is_empty(prefix / "include")) {
        const auto header_root = fs::is_directory(source / "single_include") ? source / "single_include" : fs::is_directory(source / "src") ? source / "src" : source;
        for (const auto &entry : fs::recursive_directory_iterator(header_root, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file() || !header_file(entry.path())) continue;
            const auto target = prefix / "include" / fs::relative(entry.path(), header_root);
            fs::create_directories(target.parent_path());
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        }
    }
    for (const auto &root : {build, source / "build", source / ".cpm-configure-build", source / ".cpm-cooking-build"}) {
        if (!fs::is_directory(root)) continue;
        for (const auto &entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            if (library_file(entry.path())) fs::copy_file(entry.path(), prefix / "lib" / entry.path().filename(), fs::copy_options::overwrite_existing);
            if (header_file(entry.path())) {
                const auto marker = entry.path().generic_string().find("/include/");
                if (marker != std::string::npos) {
                    const auto target = prefix / "include" / entry.path().generic_string().substr(marker + 9);
                    fs::create_directories(target.parent_path());
                    fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
                }
            }
        }
    }

    // Ask pkg-config for the exact public/private flags exported by the package.
    // The package's own pkgconfig directory is prepended to whatever PKG_CONFIG_PATH
    // the build environment already exports so that transitive Requires entries
    // (which may live in the nix store) remain resolvable.
    std::set<std::string> flags;
    for (const auto &pc_root : {prefix / "lib" / "pkgconfig", prefix / "lib64" / "pkgconfig", prefix / "share" / "pkgconfig"}) {
        if (!fs::is_directory(pc_root)) continue;
        for (const auto &entry : fs::directory_iterator(pc_root)) {
            if (entry.path().extension() != ".pc") continue;
            const auto package = entry.path().stem().string();
            ProcessResult metadata;
            if (shell.empty()) {
                // Prepend the package's pc dir to the host PKG_CONFIG_PATH so
                const char *host_pkgcfg = std::getenv("PKG_CONFIG_PATH");
                const std::string combined = host_pkgcfg && *host_pkgcfg ? pc_root.string() + ":" + host_pkgcfg : pc_root.string();
                const std::map<std::string, std::string> environment = {{"PKG_CONFIG_PATH", combined}};
                metadata = Process::run({"pkg-config", "--cflags", "--libs", "--static", package}, {}, environment, true);
            } else {
                // Inside the nix shell PKG_CONFIG_PATH already covers every nix-store
                // package.  Prepend the package's own pc dir and keep the rest intact.
                const std::string prepend_cmd = "PKG_CONFIG_PATH=" + shell_quote(pc_root.string()) + ":${PKG_CONFIG_PATH:-} pkg-config --cflags --libs --static " + shell_quote(package);
                metadata = Process::run({"nix-shell", shell.string(), "--run", prepend_cmd}, {}, {}, true);
            }
            if (metadata.exit_code == 0)
                for (auto &flag : split_flags(metadata.output)) {
                    // Only keep tokens that are compiler/linker arguments or absolute
                    // paths to library files. Discard stray output such as locale
                    if (!flag.empty() && (flag.front() == '-' || flag.front() == '/')) flags.insert(std::move(flag));
                }
        }
    }
    if (!flags.empty()) {
        std::ofstream metadata(prefix / "flags.txt");
        for (const auto &flag : flags) metadata << flag << '\n';
    }

    fs::remove_all(source);
    fs::remove_all(build);
    fs::remove(shell_file);
    fs::remove_all(prefix / ".cpm-cooking");
    const bool has_headers = fs::is_directory(prefix / "include") && !fs::is_empty(prefix / "include");
    const bool has_libraries = fs::is_directory(prefix / "lib") && !fs::is_empty(prefix / "lib");
    if ((!attempted || succeeded) && (has_headers || has_libraries)) return true;

    if (!shell_diagnostic.empty()) std::cerr << "[cpm] " << name << ": Nix build shell unavailable; host fallback was used\n" << shell_diagnostic;
    for (const auto &[adapter, diagnostic] : diagnostics) {
        std::cerr << "[cpm] " << name << ": " << adapter << " failed";
        if (!diagnostic.empty()) std::cerr << ":\n" << diagnostic;
        if (diagnostic.empty() || diagnostic.back() != '\n') std::cerr << '\n';
    }
    return false;
}

void Downloader::install_built_library(const std::string &name, const fs::path &built) {
    static std::mutex mutex;
    const std::scoped_lock lock(mutex);
    const auto includes = local_cpm_dir_ / "include";
    const auto libraries = local_cpm_dir_ / "lib";
    fs::create_directories(includes);
    fs::create_directories(libraries);

    if (fs::is_directory(built / "include")) {
        for (const auto &entry : fs::directory_iterator(built / "include")) {
            const auto target = includes / entry.path().filename();
            if (fs::exists(target) || fs::is_symlink(target)) {
                if (fs::is_symlink(target) && fs::canonical(target) == fs::canonical(entry.path())) continue;
                throw std::runtime_error("header export collision at " + target.string() + " while installing " + name);
            }
            if (entry.is_directory())
                fs::create_directory_symlink(fs::absolute(entry.path()), target);
            else
                fs::create_symlink(fs::absolute(entry.path()), target);
        }
    }
    for (const auto &directory : {built / "lib", built / "lib64"}) {
        if (!fs::is_directory(directory)) continue;
        for (const auto &entry : fs::recursive_directory_iterator(directory)) {
            if ((!entry.is_regular_file() && !entry.is_symlink()) || !library_file(entry.path())) continue;
            const auto target = libraries / entry.path().filename();
            if (fs::exists(target) || fs::is_symlink(target)) {
                throw std::runtime_error("library export collision at " + target.string() + " while installing " + name);
            }
            fs::create_symlink(fs::absolute(entry.path()), target);
        }
    }

    std::set<std::string> flags;
    const auto installed_flags = local_cpm_dir_ / "flags.txt";
    for (const auto &file : {installed_flags, built / "flags.txt"}) {
        if (!fs::is_regular_file(file)) continue;
        std::ifstream input(file);
        std::string flag;
        while (std::getline(input, flag)) {
            if (flag.empty()) continue;
            if (flag.front() == '-') {
                flags.insert(flag);
            } else if (flag.front() == '/') {
                const fs::path p(flag);
                // Absolute path to a shared library: symlink it and all versioned
                // so LD_LIBRARY_PATH=.cpm/lib is sufficient at runtime. Skip paths
                // that no longer exist (e.g. stale temp-dir references).
                if (!fs::exists(p)) continue;
                if (library_file(p)) {
                    // The stem before the first ".so" is the base name.
                    const auto filename = p.filename().string();
                    const auto so_pos = filename.find(".so");
                    const auto base = so_pos != std::string::npos ? filename.substr(0, so_pos) : filename;
                    // Symlink every file in the same directory that shares this base.
                    std::error_code ec;
                    for (const auto &sibling : fs::directory_iterator(p.parent_path(), ec)) {
                        if (!sibling.is_regular_file() && !sibling.is_symlink()) continue;
                        const auto sname = sibling.path().filename().string();
                        if (sname.substr(0, base.size()) != base) continue;
                        if (!library_file(sibling.path())) continue;
                        const auto target = libraries / sibling.path().filename();
                        if (!fs::exists(target) && !fs::is_symlink(target)) fs::create_symlink(fs::absolute(sibling.path()), target);
                    }
                    flags.insert(flag);
                }
            }
        }
    }
    if (!flags.empty()) {
        std::ofstream output(installed_flags, std::ios::trunc);
        for (const auto &flag : flags) output << flag << '\n';
    }
}

} // namespace cpm

#include "cpm/builder.hpp"

#include "cpm/config.hpp"
#include "cpm/installer.hpp"
#include "cpm/nix_env.hpp"
#include "cpm/process.hpp"
#include "cpm/progress.hpp"
#include "cpm/toml_parser.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace cpm {
namespace fs = std::filesystem;
namespace {

const std::set<std::string> &skip_directories() {
    static const std::set<std::string> directories = {".cpm", ".git", "build", "_build", "_cpm_build", "dist", "node_modules", "test", "tests", "example", "examples", "benchmark", "benchmarks"};
    return directories;
}

bool source_extension(const fs::path &path) {
    const auto extension = path.extension().string();
    return extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".C";
}

bool header_extension(const fs::path &path) {
    const auto extension = path.extension().string();
    return extension == ".h" || extension == ".hpp" || extension == ".hh" || extension == ".hxx";
}

bool skipped(const fs::path &root, const fs::path &path) {
    std::error_code error;
    const auto relative = fs::relative(path, root, error);
    if (error || relative.empty()) return false;
    const auto first = *relative.begin();
    return skip_directories().contains(first.string());
}

std::string normalize_library(std::string name) {
    if (name.starts_with("lib")) name.erase(0, 3);
    const auto suffix = name.find(".so");
    if (suffix != std::string::npos)
        name.erase(suffix);
    else if (name.ends_with(".a"))
        name.erase(name.size() - 2);
    name.erase(std::ranges::remove_if(name, [](unsigned char c) { return !std::isalnum(c); }).begin(), name.end());
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

std::string library_name(const fs::path &path) {
    const auto file = path.filename().string();
    if (!file.starts_with("lib")) return {};
    const auto so = file.find(".so");
    if (so != std::string::npos) return file.substr(3, so - 3);
    if (file.ends_with(".a")) return file.substr(3, file.size() - 5);
    return {};
}

// Append flags from flags.txt (or defines.txt) to an argument vector.
// Accepts dash-prefixed flags and absolute library paths; skips everything else.
void append_package_flags(std::vector<std::string> &args, const fs::path &local_cpm_dir) {
    const auto flags_file = local_cpm_dir / "flags.txt";
    const auto legacy_file = local_cpm_dir / "defines.txt";
    const auto metadata = fs::is_regular_file(flags_file) ? flags_file : legacy_file;
    if (!fs::is_regular_file(metadata)) return;
    std::ifstream input(metadata);
    std::string flag;
    while (std::getline(input, flag)) {
        if (flag.empty()) continue;
        const bool is_flag = flag.front() == '-';
        const bool is_abs_lib = flag.front() == '/' && fs::exists(flag);
        if (is_flag || is_abs_lib) args.emplace_back(std::move(flag));
    }
}

// Append -L, archive, and -l flags for everything under library_dir.
void append_library_flags(std::vector<std::string> &args, const fs::path &library_dir, const std::vector<NixLibrary> &nix_libraries, const std::vector<std::string> &link_libraries,
    std::set<std::string> &linked) {
    if (!fs::is_directory(library_dir)) {
        for (const auto &lib : link_libraries) {
            const auto flag = lib.starts_with("-") || lib.find('/') != std::string::npos ? lib : "-l" + lib;
            if (linked.insert(flag).second) args.emplace_back(flag);
        }
        return;
    }

    args.emplace_back("-L" + library_dir.string());

    std::vector<fs::path> archives;
    for (const auto &entry : fs::directory_iterator(library_dir)) {
        if ((entry.is_regular_file() || entry.is_symlink()) && entry.path().extension() == ".a") archives.emplace_back(entry.path());
    }
    std::ranges::sort(archives);
    if (!archives.empty()) args.emplace_back("-Wl,--start-group");
    for (const auto &archive : archives) args.emplace_back(archive.string());
    if (!archives.empty()) args.emplace_back("-Wl,--end-group");

    for (const auto &library : nix_libraries) {
        const std::array candidates = {normalize_library(library.name), normalize_library(library.nix_attr)};
        for (const auto &entry : fs::directory_iterator(library_dir)) {
            const auto link_name = library_name(entry.path());
            if (link_name.empty()) continue;
            const auto normalized = normalize_library(link_name);
            const auto flag = "-l" + link_name;
            if (std::ranges::any_of(candidates, [&](const auto &c) { return !c.empty() && (normalized == c || normalized.contains(c) || c.contains(normalized)); }) && linked.insert(flag).second)
                args.emplace_back(flag);
        }
    }
    for (const auto &lib : link_libraries) {
        const auto flag = lib.starts_with("-") || lib.find('/') != std::string::npos ? lib : "-l" + lib;
        if (linked.insert(flag).second) args.emplace_back(flag);
    }
}

std::string build_runtime_library_path(const fs::path &local_cpm_dir) {
    std::set<std::string> directories;
    directories.insert((local_cpm_dir / "lib").string());
    const auto flags_file = local_cpm_dir / "flags.txt";
    if (fs::is_regular_file(flags_file)) {
        std::ifstream input(flags_file);
        std::string flag;
        while (std::getline(input, flag)) {
            if (flag.starts_with("-L") && flag.size() > 2) {
                if (auto dir = flag.substr(2); fs::is_directory(dir)) directories.insert(std::move(dir));
            }
        }
    }
    std::string result;
    for (const auto &dir : directories) {
        if (!result.empty()) result += ':';
        result += dir;
    }
    return result;
}

std::string shell_quote(const std::string &value) {
    if (value.empty()) return "''";
    std::string quoted = "'";
    for (const char c : value) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted.push_back(c);
    }
    quoted.push_back('\'');
    return quoted;
}

std::string shell_join(const std::vector<std::string> &arguments) {
    std::ostringstream command;
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i != 0) command << ' ';
        command << shell_quote(arguments[i]);
    }
    return command.str();
}

std::string nix_compiler(const std::string &compiler) {
    if (compiler.empty()) return "gcc";
    if (compiler.starts_with("gcc-")) return "gcc" + compiler.substr(4);
    if (compiler == "gcc" || compiler == "g++") return "gcc";
    if (compiler.starts_with("clang-")) return "clang_" + compiler.substr(6);
    if (compiler == "clang" || compiler == "clang++") return "clang";
    return {};
}

uint64_t fnv1a(const std::vector<std::string> &values) {
    uint64_t hash = 1469598103934665603ULL;
    for (const auto &value : values) {
        for (const unsigned char c : value) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        hash ^= 0xff;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool source_argument(const std::string &argument) { return source_extension(argument); }

bool linker_argument(const std::string &argument) {
    const auto path = fs::path(argument);
    const auto name = path.filename().string();
    return argument.starts_with("-l") || argument.starts_with("-L") || argument.starts_with("-Wl,") || path.extension() == ".a" || path.extension() == ".so" || name.find(".so.") != std::string::npos;
}

int execute(const std::vector<std::string> &arguments, const fs::path &shell_nix, const fs::path &working_directory, bool capture, std::string *output = nullptr) {
    ProcessResult result;
    if (shell_nix.empty()) {
        result = Process::run(arguments, working_directory, {}, capture);
    } else {
        result = Process::run({"nix-shell", shell_nix.string(), "--run", shell_join(arguments)}, working_directory, {}, capture);
    }
    if (output) *output = std::move(result.output);
    return result.exit_code;
}

} // namespace

Builder::Builder(fs::path project_root, fs::path local_cpm_dir, fs::path global_cache_dir)
    : project_root_(std::move(project_root)), local_cpm_dir_(std::move(local_cpm_dir)), global_cache_dir_(std::move(global_cache_dir)) {}

std::string Builder::detect_compiler(const ProjectConfig &config) const {
    if (!config.compiler.empty()) {
        if (config.compiler.starts_with("gcc-")) {
            const auto host = "g++-" + config.compiler.substr(4);
            if (!NixEnv(local_cpm_dir_, global_cache_dir_).available() && Process::command_exists(host)) return host;
            return "g++";
        }
        if (config.compiler.starts_with("clang-")) {
            const auto host = "clang++-" + config.compiler.substr(6);
            if (!NixEnv(local_cpm_dir_, global_cache_dir_).available() && Process::command_exists(host)) return host;
            return "clang++";
        }
        if (config.compiler == "gcc") return "g++";
        if (config.compiler == "clang") return "clang++";
        return config.compiler;
    }
    return Config::get_compiler() == "clang" ? "clang++" : "g++";
}

fs::path Builder::get_output_path(const ProjectConfig &config) const { return project_root_ / (config.output.empty() ? config.name : config.output); }

std::set<std::string> Builder::collect_include_dirs(const ProjectConfig &config) const {
    std::set<std::string> directories;
    const auto dependency_include = local_cpm_dir_ / "include";
    if (fs::exists(dependency_include)) {
        directories.insert(fs::weakly_canonical(dependency_include).string());
        for (const auto &entry : fs::directory_iterator(dependency_include)) {
            if (entry.is_directory()) directories.insert(fs::weakly_canonical(entry.path()).string());
        }
    }
    const auto packages = local_cpm_dir_ / "packages";
    if (fs::is_directory(packages)) {
        for (const auto &package : fs::directory_iterator(packages)) {
            if (!package.is_directory() && !package.is_symlink()) continue;
            const auto root = fs::canonical(package.path());
            directories.insert(root.string());
            for (const auto &candidate : {root / "include", root / "single_include", root / "src"}) {
                if (fs::is_directory(candidate)) directories.insert(fs::weakly_canonical(candidate).string());
            }
        }
    }
    for (const auto &include : config.include_paths) {
        auto path = fs::path(include);
        if (path.is_relative()) path = project_root_ / path;
        if (!fs::is_directory(path)) throw std::runtime_error("include path not found: " + include);
        directories.insert(fs::weakly_canonical(path).string());
    }
    try {
        for (const auto &entry : fs::recursive_directory_iterator(project_root_, fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file() && header_extension(entry.path()) && !skipped(project_root_, entry.path())) {
                directories.insert(fs::weakly_canonical(entry.path().parent_path()).string());
            }
        }
    } catch (const fs::filesystem_error &) {
        // Permission-denied entries are already skipped; other inaccessible paths
        // do not make an otherwise valid project unbuildable.
    }
    return directories;
}

std::vector<fs::path> Builder::collect_source_files(const ProjectConfig &config, const std::string &entry_abs) const {
    std::set<std::string> sources;
    auto excluded = [&](const fs::path &path) {
        std::error_code error;
        const auto relative = fs::relative(path, project_root_, error).generic_string();
        if (error) return false;
        return std::ranges::any_of(config.exclude_sources, [&](const auto &prefix) {
            auto normalized = fs::path(prefix).lexically_normal().generic_string();
            while (normalized.starts_with("./")) normalized.erase(0, 2);
            return relative == normalized || relative.starts_with(normalized + "/");
        });
    };
    try {
        for (const auto &entry : fs::recursive_directory_iterator(project_root_, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file() || !source_extension(entry.path()) || skipped(project_root_, entry.path()) || excluded(entry.path())) continue;
            const auto canonical = fs::weakly_canonical(entry.path()).string();
            if (canonical != entry_abs) sources.insert(canonical);
        }
    } catch (const fs::filesystem_error &) {
    }
    for (const auto &source : config.extra_sources) {
        auto path = fs::path(source);
        if (path.is_relative()) path = project_root_ / path;
        if (!fs::is_regular_file(path)) throw std::runtime_error("source file not found: " + source);
        const auto canonical = fs::weakly_canonical(path).string();
        if (canonical != entry_abs) sources.insert(canonical);
    }
    std::vector<fs::path> result;
    result.reserve(sources.size());
    for (const auto &source : sources) result.emplace_back(source);
    return result;
}

std::vector<std::string> Builder::build_compile_arguments(const ProjectConfig &config, bool optimized) const {
    std::vector<std::string> arguments = {detect_compiler(config), "-std=c++" + config.cpp_standard};
    if (optimized) {
        arguments.emplace_back("-O3");
        arguments.emplace_back("-DNDEBUG");
    }
    for (const auto &define : config.defines) arguments.emplace_back("-D" + define);
    arguments.insert(arguments.end(), config.compile_options.begin(), config.compile_options.end());
    for (const auto &directory : collect_include_dirs(config)) {
        arguments.emplace_back("-I" + directory);
    }

    const auto entry_path = fs::weakly_canonical(project_root_ / config.entry);
    const bool header_entry = header_extension(entry_path);
    std::string entry_absolute = entry_path.string();
    const auto other_sources = collect_source_files(config, entry_absolute);
    if (header_entry) {
        if (other_sources.empty()) {
            const auto wrapper = local_cpm_dir_ / "_entry.cpp";
            fs::create_directories(local_cpm_dir_);
            const auto contents = "#include \"" + entry_path.string() + "\"\n";
            std::ifstream existing(wrapper);
            const std::string current((std::istreambuf_iterator<char>(existing)), {});
            if (current != contents) {
                std::ofstream output(wrapper, std::ios::trunc);
                output << contents;
            }
            entry_absolute = wrapper.string();
            arguments.push_back(entry_absolute);
        }
    } else {
        arguments.emplace_back(entry_absolute);
    }
    for (const auto &source : other_sources) arguments.emplace_back(source.string());

    append_package_flags(arguments, local_cpm_dir_);
    arguments.emplace_back("-o");
    arguments.emplace_back(get_output_path(config).string());

    std::set<std::string> linked;
    append_library_flags(arguments, local_cpm_dir_ / "lib", config.nix_libraries, config.link_libraries, linked);
    return arguments;
}

fs::path Builder::create_project_shell(const ProjectConfig &config) const {
    NixEnv nix(local_cpm_dir_, global_cache_dir_);
    const auto compiler = nix_compiler(config.compiler);
    const bool versioned_compiler = config.compiler.starts_with("gcc-") || config.compiler.starts_with("clang-");
    const bool has_user_config = !config.nix_config.empty() && fs::exists(project_root_ / config.nix_config);
    const bool required = !config.nix_libraries.empty() || !config.extra_nix_deps.empty() || versioned_compiler || has_user_config;
    if (!required) return {};
    if (!nix.available()) {
        const auto host_compiler = config.compiler.starts_with("gcc-") ? "g++-" + config.compiler.substr(4) : "clang++-" + config.compiler.substr(6);
        if (versioned_compiler && config.nix_libraries.empty() && config.extra_nix_deps.empty() && !has_user_config && Process::command_exists(host_compiler)) return {};
        throw std::runtime_error("this project requires Nix, but nix-shell and nix-build are not available");
    }
    if (compiler.empty() && versioned_compiler) throw std::runtime_error("cannot map compiler '" + config.compiler + "' to a Nix package");

    // Build the full list of CPM-managed packages (compiler + pkg-config + nix libs + extra deps).
    std::vector<std::string> cpm_deps;
    if (!compiler.empty()) cpm_deps.emplace_back(compiler);
    cpm_deps.emplace_back("pkg-config");
    for (const auto &library : config.nix_libraries) cpm_deps.emplace_back(library.nix_attr);
    for (const auto &dep : config.extra_nix_deps) cpm_deps.emplace_back(dep);

    // generate_shell_nix deduplicates and merges with user config if provided.
    const fs::path user_config = has_user_config ? project_root_ / config.nix_config : fs::path{};
    std::string expression = nix.generate_shell_nix(compiler, config.cpp_standard, cpm_deps, user_config);

    if (!config.nixpkgs.empty() && !has_user_config) {
        const auto import = expression.find("import <nixpkgs>");
        if (import != std::string::npos)
            expression.replace(import, std::string("import <nixpkgs>").size(), "import (builtins.fetchTarball { url = \"https://github.com/NixOS/nixpkgs/archive/" + config.nixpkgs + ".tar.gz\"; })");
    }

    const auto shell = local_cpm_dir_ / "project_shell.nix";
    std::ofstream output(shell, std::ios::trunc);
    output << expression;
    output.close();

    const auto check = Process::run({"nix-shell", shell.string(), "--run", "true"}, {}, {}, true);
    if (check.exit_code != 0) throw std::runtime_error("cannot create required project Nix shell:\n" + check.output);
    return shell;
}

int Builder::compile_incrementally(const ProjectConfig &config, const std::vector<std::string> &arguments, const fs::path &shell_nix, std::string &output) const {
    if (arguments.empty()) return 1;
    std::vector<std::string> sources;
    std::vector<std::string> compile_flags;
    std::vector<std::string> link_flags;
    std::string binary;
    for (size_t i = 1; i < arguments.size(); ++i) {
        if (arguments[i] == "-o" && i + 1 < arguments.size()) {
            binary = arguments[++i];
        } else if (source_argument(arguments[i])) {
            sources.emplace_back(arguments[i]);
        } else if (linker_argument(arguments[i])) {
            link_flags.emplace_back(arguments[i]);
        } else {
            compile_flags.emplace_back(arguments[i]);
            if (arguments[i] == "-pthread") link_flags.emplace_back(arguments[i]);
        }
    }
    link_flags.insert(link_flags.end(), config.link_options.begin(), config.link_options.end());
    if (sources.empty() || binary.empty()) return 1;

    auto environment_identity = compile_flags;
    const auto lock_path = project_root_ / "cpm.lock";
    if (fs::is_regular_file(lock_path)) {
        std::ifstream lock(lock_path);
        environment_identity.emplace_back(std::istreambuf_iterator<char>(lock), std::istreambuf_iterator<char>());
    }
    std::ostringstream hash_text;
    hash_text << std::hex << fnv1a(environment_identity);
    const auto object_directory = local_cpm_dir_ / "objects" / hash_text.str();
    fs::create_directories(object_directory);

    fs::file_time_type newest_header = fs::file_time_type::min();
    try {
        for (const auto &entry : fs::recursive_directory_iterator(project_root_, fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file() && header_extension(entry.path()) && !skipped(project_root_, entry.path())) {
                newest_header = std::max(newest_header, entry.last_write_time());
            }
        }
    } catch (const fs::filesystem_error &) {
    }

    std::vector<fs::path> objects(sources.size());
    std::vector<std::string> diagnostics(sources.size());
    std::vector<int> results(sources.size(), 0);
    std::vector<std::function<void()>> tasks;
    std::atomic<bool> compiled_any = false;
    for (size_t i = 0; i < sources.size(); ++i) {
        std::vector<std::string> identity = {sources[i]};
        std::ostringstream source_hash;
        source_hash << std::hex << fnv1a(identity);
        objects[i] = object_directory / (fs::path(sources[i]).stem().string() + "-" + source_hash.str() + ".o");
        std::error_code error;
        const bool current =
            fs::is_regular_file(objects[i], error) && fs::last_write_time(objects[i], error) >= fs::last_write_time(sources[i], error) && fs::last_write_time(objects[i], error) >= newest_header;
        if (current) continue;
        tasks.emplace_back([&, i] {
            std::vector<std::string> compile = {arguments.front()};
            compile.insert(compile.end(), compile_flags.begin(), compile_flags.end());
            compile.emplace_back("-MMD");
            compile.emplace_back("-MP");
            compile.emplace_back("-c");
            compile.emplace_back(sources[i]);
            compile.emplace_back("-o");
            compile.emplace_back(objects[i].string());
            results[i] = execute(compile, shell_nix, project_root_, true, &diagnostics[i]);
            compiled_any = true;
        });
    }
    const auto concurrency = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    parallel_execute(tasks, concurrency);
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i] != 0) {
            output += diagnostics[i];
            return results[i];
        }
    }

    bool link_needed = compiled_any || !fs::is_regular_file(binary);
    if (!link_needed) {
        const auto binary_time = fs::last_write_time(binary);
        for (const auto &flag : link_flags) {
            std::error_code error;
            if (fs::is_regular_file(flag, error) && fs::last_write_time(flag, error) > binary_time) {
                link_needed = true;
                break;
            }
        }
    }
    if (!link_needed) return 0;

    std::vector<std::string> link = {arguments.front()};
    for (const auto &object : objects) link.emplace_back(object.string());
    link.emplace_back("-o");
    link.emplace_back(binary);
    link.insert(link.end(), link_flags.begin(), link_flags.end());
    return execute(link, shell_nix, project_root_, true, &output);
}

void Builder::bundle_production(const ProjectConfig &config) const {
    const auto binary = get_output_path(config);
    const auto distribution = project_root_ / "dist";
    if (fs::exists(distribution)) fs::remove_all(distribution);
    fs::create_directories(distribution);
    const auto distributed_binary = distribution / binary.filename();
    fs::copy_file(binary, distributed_binary, fs::copy_options::overwrite_existing);
    if (Process::command_exists("strip")) Process::run({"strip", distributed_binary.string()}, {}, {}, true);

    if (Process::command_exists("ldd")) {
        const auto result = Process::run({"ldd", distributed_binary.string()}, {}, {{"LD_LIBRARY_PATH", build_runtime_library_path(local_cpm_dir_)}}, true);
        std::istringstream lines(result.output);
        std::string line;
        // Collect the real (canonical) paths of all needed shared libraries.
        std::set<fs::path> real_libs;
        while (std::getline(lines, line)) {
            const auto arrow = line.find("=>");
            if (arrow == std::string::npos) continue;
            std::istringstream fields(line.substr(arrow + 2));
            std::string dependency;
            fields >> dependency;
            // Bundle nix-store, .cpm/lib, and system runtime libraries (/usr/lib).
            // the entire glibc stack (libc, libm, libdl, libpthread, librt, etc.)
            const auto fname = fs::path(dependency).filename().string();
            if (fname.starts_with("ld-linux") || fname.starts_with("ld.so")) continue;
            const bool from_nix = dependency.starts_with("/nix/store/");
            const bool from_cpm = dependency.starts_with((local_cpm_dir_ / "lib").string());
            const bool from_sys = dependency.starts_with("/usr/lib") || dependency.starts_with("/lib");
            if (!from_nix && !from_cpm && !from_sys) continue;
            std::error_code error;
            const auto real = fs::canonical(dependency, error);
            if (!error && fs::is_regular_file(real)) real_libs.insert(real);
        }
        // Copy the real library file and then recreate every symlink in its
        // source directory that points to the same real file.  This ensures
        for (const auto &real : real_libs) {
            std::error_code error;
            fs::copy_file(real, distribution / real.filename(), fs::copy_options::overwrite_existing, error);
            // Walk siblings in the same directory and reproduce any symlinks
            for (const auto &sibling : fs::directory_iterator(real.parent_path(), error)) {
                if (!sibling.is_symlink()) continue;
                const auto sibling_real = fs::canonical(sibling.path(), error);
                if (error || sibling_real != real) continue;
                const auto link_target = distribution / sibling.path().filename();
                if (fs::exists(link_target) || fs::is_symlink(link_target)) continue;
                fs::create_symlink(real.filename(), link_target);
            }
        }
    }
    std::string interp_filename; // ld-linux filename if we manage to copy it
    if (Process::command_exists("patchelf")) {
        Process::run({"patchelf", "--set-rpath", "$ORIGIN", distributed_binary.string()}, {}, {}, true);

        const auto ri = Process::run({"patchelf", "--print-interpreter", distributed_binary.string()}, {}, {}, true);
        if (ri.exit_code == 0) {
            std::string interp = ri.output;
            while (!interp.empty() && (interp.back() == '\n' || interp.back() == '\r')) interp.pop_back();
            if (!interp.empty() && fs::is_regular_file(interp)) {
                interp_filename = fs::path(interp).filename().string();
                std::error_code ec;
                fs::copy_file(interp, distribution / interp_filename, fs::copy_options::overwrite_existing, ec);
                if (ec) interp_filename.clear(); // copy failed, fall back to direct exec
            }
        }
    }

    const auto launcher = distribution / "run.sh";
    std::ofstream script(launcher, std::ios::trunc);
    script << "#!/bin/sh\n"
           << "DIR=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n"
           << "export LD_LIBRARY_PATH=\"$DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n";
    if (!interp_filename.empty()) {
        script << "exec \"$DIR\"/" << shell_quote(interp_filename) << " --library-path \"$DIR\" "
               << "\"$DIR\"/" << shell_quote(binary.filename().string()) << " \"$@\"\n";
    } else {
        script << "exec \"$DIR\"/" << shell_quote(binary.filename().string()) << " \"$@\"\n";
    }
    script.close();
    fs::permissions(launcher, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec);
    std::cout << "[cpm] Built optimized bundle: " << distribution.string() << "\n";
}

int Builder::build(bool optimized) {
    const auto manifest = project_root_ / "cpm.toml";
    if (!fs::exists(manifest)) throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");
    const auto config = TomlParser::parse(manifest);
    if (!fs::is_regular_file(project_root_ / config.entry)) {
        throw std::runtime_error("entry file not found: " + config.entry);
    }
    fs::create_directories(get_output_path(config).parent_path());
    const auto arguments = build_compile_arguments(config, optimized);
    const auto shell = create_project_shell(config);
    BuildSpinner spinner(config.name, "c++" + config.cpp_standard + " - " + detect_compiler(config));
    std::string output;
    const int result = compile_incrementally(config, arguments, shell, output);
    spinner.finish(result == 0);
    if (result != 0) {
        std::cerr << "\n[cpm] Compiler output:\n" << output;
        return result;
    }
    if (optimized) bundle_production(config);
    return 0;
}

int Builder::run() {
    const auto manifest = project_root_ / "cpm.toml";
    if (!fs::exists(manifest)) throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");
    const auto config = TomlParser::parse(manifest);
    bool install_needed = !fs::is_directory(local_cpm_dir_ / "include");
    for (const auto &dependency : config.git_dependencies) {
        if (!fs::exists(local_cpm_dir_ / "packages" / dependency.name)) install_needed = true;
    }
    if (install_needed && (!config.git_dependencies.empty() || !config.system_dependencies.empty() || !config.nix_libraries.empty())) {
        Installer(project_root_, local_cpm_dir_, global_cache_dir_).install();
    }
    const int result = build();
    if (result != 0) return result;
    const auto binary = get_output_path(config);
    std::cout << "\n[cpm] Running " << binary.filename().string() << "...\n" << std::flush;
    return Process::run({binary.string()}, project_root_, {{"LD_LIBRARY_PATH", build_runtime_library_path(local_cpm_dir_)}}).exit_code;
}

int Builder::run_file(const std::string &file) {
    auto source = fs::path(file);
    if (source.is_relative()) source = project_root_ / source;
    if (!fs::is_regular_file(source)) {
        std::cerr << "[cpm] File not found: " << file << "\n";
        return 1;
    }
    const auto extension = source.extension().string();
    const bool cpp = extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".C";
    const bool c = extension == ".c";
    if (!cpp && !c) {
        std::cerr << "[cpm] Unsupported file type: " << extension << "\n";
        return 1;
    }

    ProjectConfig config;
    const auto manifest = project_root_ / "cpm.toml";
    if (fs::exists(manifest)) config = TomlParser::parse(manifest);
    const auto output = local_cpm_dir_ / "run" / source.stem();
    fs::create_directories(output.parent_path());
    std::vector<std::string> arguments = {
        c ? (config.compiler.starts_with("clang") ? "clang" : "gcc") : detect_compiler(config), c ? "-std=c17" : "-std=c++" + (config.cpp_standard.empty() ? "20" : config.cpp_standard)};
    if (fs::exists(manifest)) {
        for (const auto &define : config.defines) arguments.emplace_back("-D" + define);
        arguments.insert(arguments.end(), config.compile_options.begin(), config.compile_options.end());
        for (const auto &directory : collect_include_dirs(config)) arguments.emplace_back("-I" + directory);
        append_package_flags(arguments, local_cpm_dir_);
    }
    arguments.emplace_back(source.string());
    arguments.emplace_back("-o");
    arguments.emplace_back(output.string());

    std::set<std::string> linked;
    append_library_flags(arguments, local_cpm_dir_ / "lib", config.nix_libraries, config.link_libraries, linked);
    arguments.insert(arguments.end(), config.link_options.begin(), config.link_options.end());

    const auto shell = fs::exists(manifest) ? create_project_shell(config) : fs::path{};
    BuildSpinner spinner(source.stem().string(), c ? "c" : "c++");
    std::string compiler_output;
    const int compile = execute(arguments, shell, project_root_, true, &compiler_output);
    spinner.finish(compile == 0);
    if (compile != 0) {
        std::cerr << "\n[cpm] Compiler output:\n" << compiler_output;
        return compile;
    }
    const int result = Process::run({output.string()}, project_root_, {{"LD_LIBRARY_PATH", build_runtime_library_path(local_cpm_dir_)}}).exit_code;
    std::error_code error;
    fs::remove(output, error);
    return result;
}

int Builder::start() {
    const auto manifest = project_root_ / "cpm.toml";
    if (!fs::exists(manifest)) throw std::runtime_error("No cpm.toml found. Run 'cpm init <name>' first.");
    const auto config = TomlParser::parse(manifest);
    const auto binary = get_output_path(config);
    if (!fs::exists(binary)) {
        const int result = build();
        if (result != 0) return result;
    }
    const std::map<std::string, std::string> environment = {{"LD_LIBRARY_PATH", build_runtime_library_path(local_cpm_dir_)}};
    if (!config.start_script.empty()) {
        std::cout << "[cpm] Starting: " << config.start_script << "\n";
        return Process::shell(config.start_script, project_root_, environment).exit_code;
    }
    return Process::run({binary.string()}, project_root_, environment).exit_code;
}

} // namespace cpm

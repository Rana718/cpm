#include "cpm/package_manager.hpp"

#include "cpm/builder.hpp"
#include "cpm/config.hpp"
#include "cpm/environment.hpp"
#include "cpm/installer.hpp"
#include "cpm/resolver.hpp"
#include "cpm/toml_parser.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

namespace cpm {

namespace fs = std::filesystem;

PackageManager::PackageManager() : project_root_(fs::current_path()), local_cpm_dir_(project_root_ / ".cpm"), global_cache_dir_(Config::get_global_cache_dir()) {}

void PackageManager::init(const std::string &project_name) {
    std::cout << "[cpm] Initializing project '" << project_name << "'...\n"
              << "[cpm] Architecture: " << Config::get_architecture() << "\n"
              << "[cpm] OS:           " << Config::get_os() << "\n"
              << "[cpm] Compiler:     " << Config::get_compiler() << " " << Config::get_compiler_version() << "\n";

    auto toml_path = project_root_ / "cpm.toml";
    if (!fs::exists(toml_path)) {
        TomlParser::create_default(toml_path, project_name);
        std::cout << "[cpm] Created cpm.toml\n";
    } else {
        std::cout << "[cpm] cpm.toml already exists, skipping.\n";
    }

    Environment env(project_root_);
    env.create();
    fs::create_directories(global_cache_dir_);

    auto main_file = project_root_ / "main.cpp";
    if (!fs::exists(main_file)) {
        std::ofstream f(main_file);
        f << "#include <iostream>\n\n"
          << "int main() {\n"
          << "    std::cout << \"Hello from " << project_name << "!\" << std::endl;\n"
          << "    return 0;\n"
          << "}\n";
        std::cout << "[cpm] Created main.cpp\n";
    }

    generate_compile_commands();

    std::cout << "\n[cpm] Done! You can now:\n"
              << "  cpm run       # install + build + run\n"
              << "  cpm build     # compile the project\n"
              << "  cpm start     # run the built binary\n";
}

void PackageManager::install() {
    Installer(project_root_, local_cpm_dir_, global_cache_dir_).install();
    generate_compile_commands();
}

void PackageManager::install_package(const std::string &package_spec) {
    Installer(project_root_, local_cpm_dir_, global_cache_dir_).install_package(package_spec);
    generate_compile_commands();
}

void PackageManager::remove_package(const std::string &package_name) {
    Installer(project_root_, local_cpm_dir_, global_cache_dir_).remove_package(package_name);
    generate_compile_commands();
}

void PackageManager::update() {
    Installer(project_root_, local_cpm_dir_, global_cache_dir_).update();
    generate_compile_commands();
}

void PackageManager::list() const { Installer(project_root_, local_cpm_dir_, global_cache_dir_).list(); }

//Delegation — Builder 

int PackageManager::build(bool static_build) { return Builder(project_root_, local_cpm_dir_, global_cache_dir_).build(static_build); }

int PackageManager::run() { return Builder(project_root_, local_cpm_dir_, global_cache_dir_).run(); }

int PackageManager::run_file(const std::string &file) { return Builder(project_root_, local_cpm_dir_, global_cache_dir_).run_file(file); }

int PackageManager::start() { return Builder(project_root_, local_cpm_dir_, global_cache_dir_).start(); }

// Helpers 

void PackageManager::export_package_headers() const { Resolver(project_root_).export_headers(); }

void PackageManager::generate_compile_commands() const {
    auto toml_path = project_root_ / "cpm.toml";
    if (!fs::exists(toml_path)) return;

    auto config = TomlParser::parse(toml_path);
    auto include_dir = local_cpm_dir_ / "include";

    static const std::set<std::string> skip_dirs = {".cpm", ".git", "build", "_build", "_cpm_build", "dist", "node_modules"};

    // Determine the compiler binary
    std::string compiler = config.compiler.empty() ? (Config::get_compiler() == "clang" ? "clang++" : "g++") : config.compiler;

    // Build flags string
    std::string flags = " -std=c++" + config.cpp_standard;

    // Linux-only build
    flags += " -DSEED_PLATFORM_LINUX";

    // .cpm/include and subdirs
    if (fs::exists(include_dir)) {
        flags += " -I" + include_dir.string();
        for (const auto &entry : fs::directory_iterator(include_dir)) {
            if (entry.is_directory()) flags += " -I" + entry.path().string();
        }
    }

    // Extra include paths from cpm.toml
    for (const auto &inc : config.include_paths) {
        auto p = fs::path(inc);
        if (p.is_relative()) p = project_root_ / p;
        flags += " -I" + p.string();
    }

    // Auto-discover header directories in project tree
    std::set<std::string> seen_dirs;
    try {
        for (const auto &entry : fs::recursive_directory_iterator(project_root_, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".h" && ext != ".hpp" && ext != ".hh" && ext != ".hxx") continue;
            auto rel = fs::relative(entry.path(), project_root_);
            if (skip_dirs.count(rel.begin()->string())) continue;
            auto dir = fs::weakly_canonical(entry.path().parent_path()).string();
            if (seen_dirs.insert(dir).second) flags += " -I" + dir;
        }
    } catch (...) {
    }

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
            if (!line.empty() && line[0] == '-') flags += " " + line;
    }

    // Collect source files
    std::vector<fs::path> sources;
    try {
        for (const auto &entry : fs::recursive_directory_iterator(project_root_, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".cc" && ext != ".cxx") continue;
            auto rel = fs::relative(entry.path(), project_root_);
            if (skip_dirs.count(rel.begin()->string())) continue;
            sources.push_back(entry.path());
        }
    } catch (...) {
    }

    if (sources.empty() && !config.entry.empty()) sources.push_back(project_root_ / config.entry);

    // Write compile_commands.json
    std::ofstream cc(project_root_ / "compile_commands.json");
    cc << "[\n";
    for (size_t i = 0; i < sources.size(); ++i) {
        cc << "  {\n"
           << R"(    "directory": ")" << project_root_.string() << "\",\n"
           << R"(    "command": ")" << compiler << flags << " -c " << sources[i].string() << "\",\n"
           << R"(    "file": ")" << sources[i].string() << "\"\n"
           << "  }";
        if (i + 1 < sources.size()) cc << ",";
        cc << "\n";
    }
    cc << "]\n";
}

std::string PackageManager::get_include_flags() const { return "-I" + (local_cpm_dir_ / "include").string(); }

std::string PackageManager::get_library_flags() const {
    auto lib_dir = local_cpm_dir_ / "lib";
    return fs::exists(lib_dir) ? "-L" + lib_dir.string() : "";
}

} // namespace cpm

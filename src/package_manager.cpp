#include "cpm/package_manager.hpp"

#include "cpm/builder.hpp"
#include "cpm/config.hpp"
#include "cpm/environment.hpp"
#include "cpm/installer.hpp"
#include "cpm/toml_parser.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

namespace cpm {

namespace fs = std::filesystem;

namespace {
std::string json_string(const std::string &value) {
    std::ostringstream encoded;
    encoded << '"';
    for (const unsigned char c : value) {
        switch (c) {
        case '"':
            encoded << "\\\"";
            break;
        case '\\':
            encoded << "\\\\";
            break;
        case '\b':
            encoded << "\\b";
            break;
        case '\f':
            encoded << "\\f";
            break;
        case '\n':
            encoded << "\\n";
            break;
        case '\r':
            encoded << "\\r";
            break;
        case '\t':
            encoded << "\\t";
            break;
        default:
            if (c < 0x20) {
                encoded << "\\u00" << "0123456789abcdef"[(c >> 4) & 0xf] << "0123456789abcdef"[c & 0xf];
            } else
                encoded << static_cast<char>(c);
        }
    }
    encoded << '"';
    return encoded.str();
}

std::string compiler_for(const ProjectConfig &config) {
    if (config.compiler == "gcc" || config.compiler.starts_with("gcc-")) return "g++";
    if (config.compiler == "clang" || config.compiler.starts_with("clang-")) return "clang++";
    if (!config.compiler.empty()) return config.compiler;
    return Config::get_compiler() == "clang" ? "clang++" : "g++";
}
} // namespace

PackageManager::PackageManager() : project_root_(fs::current_path()), local_cpm_dir_(project_root_ / ".cpm"), global_cache_dir_(Config::get_global_cache_dir()) {}

void PackageManager::init(const std::string &project_name) {
    auto toml_path = project_root_ / "cpm.toml";
    std::string actual_name = project_name;
    if (!fs::exists(toml_path)) {
        TomlParser::create_default(toml_path, project_name);
        std::cout << "[cpm] Created cpm.toml\n";
    } else {
        actual_name = TomlParser::parse(toml_path).name;
        std::cout << "[cpm] cpm.toml already exists, skipping.\n";
    }
    std::cout << "[cpm] Initializing project '" << actual_name << "'...\n"
              << "[cpm] Architecture: " << Config::get_architecture() << "\n"
              << "[cpm] OS:           " << Config::get_os() << "\n"
              << "[cpm] Compiler:     " << Config::get_compiler() << " " << Config::get_compiler_version() << "\n";

    Environment env(project_root_);
    env.create();

    auto main_file = project_root_ / "main.cpp";
    if (!fs::exists(main_file)) {
        std::ofstream f(main_file);
        f << "#include <iostream>\n\n"
          << "int main() {\n"
          << "    std::cout << \"Hello from " << actual_name << "!\" << std::endl;\n"
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

void PackageManager::install_package(const std::string &package_spec, const std::string &kind) {
    Installer(project_root_, local_cpm_dir_, global_cache_dir_).install_package(package_spec, kind);
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

// Delegation — Builder

int PackageManager::build(bool static_build) {
    const int result = Builder(project_root_, local_cpm_dir_, global_cache_dir_).build(static_build);
    if (result == 0) generate_compile_commands();
    return result;
}

int PackageManager::run() {
    const int result = Builder(project_root_, local_cpm_dir_, global_cache_dir_).run();
    generate_compile_commands();
    return result;
}

int PackageManager::run_file(const std::string &file) { return Builder(project_root_, local_cpm_dir_, global_cache_dir_).run_file(file); }

int PackageManager::start() { return Builder(project_root_, local_cpm_dir_, global_cache_dir_).start(); }

// Helpers

void PackageManager::generate_compile_commands() const {
    auto toml_path = project_root_ / "cpm.toml";
    if (!fs::exists(toml_path)) return;

    auto config = TomlParser::parse(toml_path);
    auto include_dir = local_cpm_dir_ / "include";

    static const std::set<std::string> skip_dirs = {".cpm", ".git", "build", "_build", "_cpm_build", "dist", "node_modules"};

    std::vector<std::string> arguments = {compiler_for(config), "-std=c++" + config.cpp_standard};
    for (const auto &define : config.defines) arguments.push_back("-D" + define);
    arguments.insert(arguments.end(), config.compile_options.begin(), config.compile_options.end());

    // .cpm/include and subdirs
    if (fs::exists(include_dir)) {
        arguments.push_back("-I" + include_dir.string());
        for (const auto &entry : fs::directory_iterator(include_dir)) {
            if (entry.is_directory()) arguments.push_back("-I" + entry.path().string());
        }
    }
    const auto packages_dir = local_cpm_dir_ / "packages";
    if (fs::is_directory(packages_dir)) {
        for (const auto &package : fs::directory_iterator(packages_dir)) {
            if (!package.is_directory() && !package.is_symlink()) continue;
            const auto root = fs::canonical(package.path());
            arguments.push_back("-I" + root.string());
            for (const auto &candidate : {root / "include", root / "single_include", root / "src"}) {
                if (fs::is_directory(candidate)) arguments.push_back("-I" + candidate.string());
            }
        }
    }

    // Extra include paths from cpm.toml
    for (const auto &inc : config.include_paths) {
        auto p = fs::path(inc);
        if (p.is_relative()) p = project_root_ / p;
        arguments.push_back("-I" + p.string());
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
            if (seen_dirs.insert(dir).second) arguments.push_back("-I" + dir);
        }
    } catch (...) {
    }

    // defines.txt
    auto defines_file = local_cpm_dir_ / "flags.txt";
    if (!fs::exists(defines_file)) defines_file = local_cpm_dir_ / "defines.txt";
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
            if (!line.empty() && line[0] == '-') arguments.push_back(line);
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
        cc << "  {\n    \"directory\": " << json_string(project_root_.string()) << ",\n    \"arguments\": [";
        for (size_t arg = 0; arg < arguments.size(); ++arg) {
            if (arg != 0) cc << ", ";
            cc << json_string(arguments[arg]);
        }
        if (!arguments.empty()) cc << ", ";
        cc << json_string("-c") << ", " << json_string(sources[i].string()) << "],\n"
           << "    \"file\": " << json_string(sources[i].string()) << "\n  }";
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

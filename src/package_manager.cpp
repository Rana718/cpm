#include "cpm/package_manager.hpp"
#include "cpm/config.hpp"
#include "cpm/environment.hpp"
#include "cpm/resolver.hpp"
#include <iostream>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <array>
#include <memory>

namespace cpm {

PackageManager::PackageManager()
    : project_root_(std::filesystem::current_path())
    , local_cpm_dir_(project_root_ / ".cpm")
    , global_cache_dir_(Config::get_global_cache_dir())
{}

void PackageManager::init(const std::string& project_name) {
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

bool PackageManager::is_cached(const std::string& package_name, const std::string& version) const {
    return std::filesystem::exists(get_cache_path(package_name, version));
}

std::filesystem::path PackageManager::get_cache_path(const std::string& package_name, const std::string& version) const {
    return global_cache_dir_ / (package_name + "-" + version);
}

void PackageManager::link_from_cache(const std::string& package_name, const std::string& version) {
    auto cache_path = get_cache_path(package_name, version);
    auto local_path = local_cpm_dir_ / "packages" / package_name;

    if (std::filesystem::exists(local_path) || std::filesystem::is_symlink(local_path)) {
        std::filesystem::remove_all(local_path);
    }

    std::filesystem::create_directory_symlink(cache_path, local_path);
}

void PackageManager::clone_git_dependency(const GitDependency& dep) {
    std::string version = dep.version;

    // "*" means latest release tag
    if (version == "*") {
        // Get the latest tag from the repo
        std::string tag_cmd = "git ls-remote --tags --sort=-v:refname " + dep.github_url +
                              " 2>/dev/null | head -1 | sed 's/.*refs\\/tags\\///' | sed 's/\\^{}//'";
        std::array<char, 256> buffer;
        std::string latest_tag;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(tag_cmd.c_str(), "r"), pclose);
        if (pipe) {
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                latest_tag += buffer.data();
            }
        }
        // Trim
        while (!latest_tag.empty() && (latest_tag.back() == '\n' || latest_tag.back() == '\r')) {
            latest_tag.pop_back();
        }

        if (!latest_tag.empty()) {
            version = latest_tag;
            std::cout << "[cpm] " << dep.name << " → latest: " << version << "\n";
        } else {
            // No tags found, fall back to default branch
            version = "HEAD";
        }
    }

    if (is_cached(dep.name, version)) {
        std::cout << "[cpm] " << dep.name << " (cached)\n";
        link_from_cache(dep.name, version);
        return;
    }

    auto cache_path = get_cache_path(dep.name, version);
    std::filesystem::create_directories(cache_path);

    std::string clone_cmd = "git clone --depth 1 --quiet ";
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

void PackageManager::resolve_system_dependency(const SystemDependency& dep) {
    namespace fs = std::filesystem;

    std::string version = dep.version;

    // "*" means latest release tag
    if (version == "*") {
        std::string tag_cmd = "git ls-remote --tags --sort=-v:refname " + dep.github_url +
                              " 2>/dev/null | head -1 | sed 's/.*refs\\/tags\\///' | sed 's/\\^{}//'";
        std::array<char, 256> buffer;
        std::string latest_tag;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(tag_cmd.c_str(), "r"), pclose);
        if (pipe) {
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                latest_tag += buffer.data();
            }
        }
        while (!latest_tag.empty() && (latest_tag.back() == '\n' || latest_tag.back() == '\r')) {
            latest_tag.pop_back();
        }

        if (!latest_tag.empty()) {
            version = latest_tag;
            std::cout << "[cpm] " << dep.name << " → latest: " << version << "\n";
        } else {
            version = "HEAD";
        }
    }

    // Check if already built in global cache
    auto cache_key = dep.name + "-" + version + "-built";
    auto built_cache = global_cache_dir_ / cache_key;

    if (fs::exists(built_cache)) {
        std::cout << "[cpm] " << dep.name << " (cached, pre-built)\n";
        // Copy built artifacts to local .cpm/
        install_built_library(dep.name, built_cache);
        return;
    }

    // Download source
    std::cout << "[cpm] " << dep.name << " (downloading source...)\n";

    auto src_path = global_cache_dir_ / (dep.name + "-" + version + "-src");
    if (!fs::exists(src_path)) {
        fs::create_directories(src_path);

        std::string clone_cmd = "git clone --depth 1 --quiet ";
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

bool PackageManager::build_from_source(const std::string& name,
                                        const std::filesystem::path& src_path,
                                        const std::filesystem::path& install_prefix) {
    namespace fs = std::filesystem;

    auto build_dir = src_path / "_cpm_build";
    fs::create_directories(build_dir);

    int ret = -1;

    // Detect build system and build
    if (fs::exists(src_path / "CMakeLists.txt")) {
        // CMake project
        std::string cmake_cmd =
            "cd " + build_dir.string() +
            " && cmake " + src_path.string() +
            " -DCMAKE_INSTALL_PREFIX=" + install_prefix.string() +
            " -DCMAKE_BUILD_TYPE=Release"
            " -DCMAKE_POSITION_INDEPENDENT_CODE=ON"
            " -DBUILD_SHARED_LIBS=OFF"
            " -DBUILD_TESTING=OFF"
            " -DBUILD_TESTS=OFF"
            " -DBUILD_EXAMPLES=OFF"
            " > /dev/null 2>&1"
            " && cmake --build . --parallel > /dev/null 2>&1"
            " && cmake --install . > /dev/null 2>&1";

        ret = std::system(cmake_cmd.c_str());
    }
    else if (fs::exists(src_path / "Makefile") || fs::exists(src_path / "makefile")) {
        // Makefile project — try make install, if it fails just make and copy manually
        std::string make_cmd =
            "cd " + src_path.string() +
            " && make -j$(nproc) CFLAGS='-DLIBUS_NO_SSL -flto' > /dev/null 2>&1";

        ret = std::system(make_cmd.c_str());

        if (ret == 0) {
            // Try make install
            std::string install_cmd =
                "cd " + src_path.string() +
                " && make install PREFIX=" + install_prefix.string() + " > /dev/null 2>&1";
            int install_ret = std::system(install_cmd.c_str());

            if (install_ret != 0) {
                // No install target — manually copy artifacts
                fs::create_directories(install_prefix / "lib");
                fs::create_directories(install_prefix / "include");

                // Copy .a files
                for (const auto& entry : fs::directory_iterator(src_path)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".a") {
                        fs::copy(entry.path(), install_prefix / "lib" / entry.path().filename(),
                                 fs::copy_options::overwrite_existing);
                    }
                }

                // Copy headers from src/ or include/
                auto header_src = src_path / "include";
                if (!fs::exists(header_src)) header_src = src_path / "src";

                if (fs::exists(header_src)) {
                    for (const auto& entry : fs::recursive_directory_iterator(header_src)) {
                        if (!entry.is_regular_file()) continue;
                        auto ext = entry.path().extension().string();
                        if (ext == ".h" || ext == ".hpp") {
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
    else if (fs::exists(src_path / "meson.build")) {
        // Meson project
        std::string meson_cmd =
            "cd " + src_path.string() +
            " && meson setup " + build_dir.string() +
            " --prefix=" + install_prefix.string() +
            " --default-library=static"
            " > /dev/null 2>&1"
            " && meson compile -C " + build_dir.string() +
            " > /dev/null 2>&1"
            " && meson install -C " + build_dir.string() +
            " > /dev/null 2>&1";

        ret = std::system(meson_cmd.c_str());
    }
    else if (fs::exists(src_path / "configure")) {
        // Autotools
        std::string auto_cmd =
            "cd " + src_path.string() +
            " && ./configure --prefix=" + install_prefix.string() +
            " --enable-static --disable-shared"
            " > /dev/null 2>&1"
            " && make -j$(nproc) > /dev/null 2>&1"
            " && make install > /dev/null 2>&1";

        ret = std::system(auto_cmd.c_str());
    }
    else {
        std::cerr << "[cpm] ERROR: Cannot detect build system for " << name << "\n";
        std::cerr << "[cpm]        Looked for: CMakeLists.txt, Makefile, meson.build, configure\n";
        return false;
    }

    // Cleanup build dir
    if (fs::exists(build_dir)) {
        fs::remove_all(build_dir);
    }

    return (ret == 0);
}

void PackageManager::install_built_library(const std::string& name,
                                            const std::filesystem::path& built_path) {
    namespace fs = std::filesystem;

    // Copy/symlink headers from built_path/include/ → .cpm/include/
    auto src_include = built_path / "include";
    auto dst_include = local_cpm_dir_ / "include";

    if (fs::exists(src_include)) {
        for (const auto& entry : fs::directory_iterator(src_include)) {
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
        for (const auto& entry : fs::recursive_directory_iterator(src_lib)) {
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
        for (const auto& entry : fs::recursive_directory_iterator(src_lib64)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext == ".a" || ext == ".so" || ext.find(".so.") != std::string::npos) {
                auto target = dst_lib / entry.path().filename();
                if (fs::exists(target)) fs::remove(target);
                fs::copy(entry.path(), target);
            }
        }
    }
}

void PackageManager::export_package_headers() {
    Resolver resolver(project_root_);
    resolver.export_headers();
}

void PackageManager::generate_compile_commands() {
    auto toml_path = project_root_ / "cpm.toml";
    if (!std::filesystem::exists(toml_path)) return;

    auto config = TomlParser::parse(toml_path);
    auto include_dir = local_cpm_dir_ / "include";

    std::ofstream cc(project_root_ / "compile_commands.json");
    cc << "[\n";
    cc << "  {\n";
    cc << "    \"directory\": \"" << project_root_.string() << "\",\n";
    cc << "    \"command\": \"" << detect_compiler(config)
       << " -std=c++" << config.cpp_standard
       << " -I" << include_dir.string()
       << " -c " << config.entry << "\",\n";
    cc << "    \"file\": \"" << (project_root_ / config.entry).string() << "\"\n";
    cc << "  }\n";
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

    // Install header-only dependencies
    if (!config.git_dependencies.empty()) {
        std::cout << "[cpm] Resolving " << config.git_dependencies.size() << " header packages...\n";
        for (const auto& dep : config.git_dependencies) {
            clone_git_dependency(dep);
        }
    }

    // Build system dependencies from source (completely isolated in .cpm/)
    if (!config.system_dependencies.empty()) {
        std::cout << "[cpm] Building " << config.system_dependencies.size() << " system libraries...\n";
        for (const auto& dep : config.system_dependencies) {
            resolve_system_dependency(dep);
        }
    }

    export_package_headers();
    generate_compile_commands();
    std::cout << "[cpm] Packages ready.\n";
}

// ─── BUILD SYSTEM ────────────────────────────────────────────

std::string PackageManager::detect_compiler(const ProjectConfig& config) const {
    if (!config.compiler.empty()) {
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

std::filesystem::path PackageManager::get_output_path(const ProjectConfig& config) const {
    std::string out_name = config.output.empty() ? config.name : config.output;
    return project_root_ / out_name;
}

std::string PackageManager::build_compile_command(const ProjectConfig& config) const {
    std::ostringstream cmd;

    cmd << detect_compiler(config);
    cmd << " -std=c++" << config.cpp_standard;

    // Include .cpm/include — all headers (both header-only and compiled libs)
    auto include_dir = local_cpm_dir_ / "include";
    if (std::filesystem::exists(include_dir)) {
        cmd << " -I" << include_dir.string();
    }

    // Source file
    cmd << " " << config.entry;

    // Output binary
    cmd << " -o " << get_output_path(config).string();

    // Link against .cpm/lib/
    auto lib_dir = local_cpm_dir_ / "lib";
    if (std::filesystem::exists(lib_dir)) {
        cmd << " -L" << lib_dir.string();

        // Auto-link all .a files found in .cpm/lib/
        for (const auto& entry : std::filesystem::directory_iterator(lib_dir)) {
            if (!entry.is_regular_file()) continue;
            auto filename = entry.path().filename().string();
            auto ext = entry.path().extension().string();
            if (ext == ".a") {
                // libhiredis.a → -lhiredis
                if (filename.find("lib") == 0) {
                    auto lib_name = filename.substr(3); // remove "lib"
                    lib_name = lib_name.substr(0, lib_name.size() - 2); // remove ".a"
                    cmd << " -l" << lib_name;
                }
            }
        }
    }

    return cmd.str();
}

int PackageManager::build() {
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

    std::cout << "[cpm] Building " << config.name << "...\n";
    std::cout << "[cpm] " << compiler << " | c++" << config.cpp_standard
              << " | " << Config::get_architecture() << "\n";

    std::cout.flush();
    int ret = std::system(compile_cmd.c_str());
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
            for (const auto& dep : config.git_dependencies) {
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
    std::cout << "────────────────────────────────────────" << std::endl;

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
    std::cout << "────────────────────────────────────────" << std::endl;

    std::cout.flush();
    int ret = std::system(run_cmd.c_str());
    return WEXITSTATUS(ret);
}

// ─── PACKAGE OPERATIONS ──────────────────────────────────────

void PackageManager::install_package(const std::string& package_spec) {
    ensure_directories();

    GitDependency dep;
    std::string spec = package_spec;

    if (spec.find("github:") == 0) {
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

void PackageManager::remove_package(const std::string& package_name) {
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
        for (const auto& entry : std::filesystem::directory_iterator(packages_dir)) {
            if (entry.is_directory() || entry.is_symlink()) {
                std::cout << "  [header] " << entry.path().filename().string() << "\n";
                has_packages = true;
            }
        }
    }

    auto lib_dir = local_cpm_dir_ / "lib";
    if (std::filesystem::exists(lib_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(lib_dir)) {
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

std::string PackageManager::get_include_flags() const {
    return "-I" + (local_cpm_dir_ / "include").string();
}

std::string PackageManager::get_library_flags() const {
    std::string flags;
    auto lib_dir = local_cpm_dir_ / "lib";
    if (std::filesystem::exists(lib_dir)) {
        flags += "-L" + lib_dir.string();
    }
    return flags;
}

} // namespace cpm

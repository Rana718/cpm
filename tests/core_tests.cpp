#include "cpm/builder.hpp"
#include "cpm/environment.hpp"
#include "cpm/installer.hpp"
#include "cpm/process.hpp"
#include "cpm/resolver.hpp"
#include "cpm/toml_parser.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void write(const fs::path &path, const std::string &contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << contents;
}

void test_parser(const fs::path &root) {
    const auto manifest = root / "parser" / "cpm.toml";
    write(manifest, "[project]\n"
                    "name = \"sample\"\n"
                    "description = \"a # is data\" # comment\n"
                    "cpp_standard = \"23\"\n"
                    "entry = \"src/main.cpp\"\n\n"
                    "[dependencies]\n"
                    "one = \"https://example.test/acme/one.git@release/1\"\n"
                    "two = \"git@example.test:acme/two.git@deadbeef\"\n\n"
                    "[build]\n"
                    "include_paths = [\"include dir\", \"generated\"]\n"
                    "sources = [\"generated/a.cpp\"]\n"
                    "exclude_sources = [\"tests\"]\n"
                    "defines = [\"FEATURE=1\"]\n"
                    "compile_options = [\"-Wall\"]\n"
                    "link_libraries = [\"m\"]\n"
                    "future_option = true\n");
    const auto config = cpm::TomlParser::parse(manifest);
    require(config.description == "a # is data", "quoted comment was truncated");
    require(config.git_dependencies.size() == 2, "dependencies were not parsed");
    require(config.git_dependencies[0].github_url == "https://example.test/acme/one.git", "direct URL was rewritten");
    require(config.git_dependencies[0].version == "release/1", "slash ref was not parsed");
    require(config.git_dependencies[1].github_url == "git@example.test:acme/two.git", "SSH URL was rewritten");
    require(config.include_paths.size() == 2 && config.compile_options == std::vector<std::string>{"-Wall"}, "build arrays were not parsed");

    const auto dependency = cpm::TomlParser::parse_git_dependency("json", "github:nlohmann/json@v3.11.3");
    cpm::TomlParser::upsert_dependency(manifest, dependency);
    require(cpm::TomlParser::parse(manifest).git_dependencies.size() == 3, "dependency was not persisted");
    require(cpm::TomlParser::remove_dependency(manifest, "json"), "dependency was not removed");
    require(cpm::TomlParser::parse(manifest).git_dependencies.size() == 2, "removed dependency still parses");
    cpm::TomlParser::upsert_nix_library(manifest, {"ssl", "openssl"});
    require(cpm::TomlParser::parse(manifest).nix_libraries.size() == 1, "Nix library was not persisted");

    const auto invalid = root / "parser" / "invalid.toml";
    write(invalid, "[project]\nname=\"bad\"\nentry=\"../outside.cpp\"\n");
    bool rejected = false;
    try {
        (void)cpm::TomlParser::parse(invalid);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "entry path traversal was accepted");

    write(invalid, "[project]\nname=\"unterminated\n");
    rejected = false;
    try {
        (void)cpm::TomlParser::parse(invalid);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "unterminated string was accepted");
}

void test_process() {
    const std::string argument = "space ; $(not-a-command) ' quote";
    const auto result = cpm::Process::run({"printf", "%s", argument}, {}, {}, true);
    require(result.exit_code == 0 && result.output == argument, "process arguments were interpreted by a shell");
    require(cpm::Process::shell("exit 7").exit_code == 7, "shell exit status was not preserved");
    require(cpm::Process::command_exists("sh"), "PATH command lookup failed");
}

void test_environment_and_resolver(const fs::path &root) {
    const auto project = root / "resolver";
    cpm::Environment environment(project);
    environment.create();
    require(environment.exists(), "environment marker was not created");

    const auto package_a = root / "cache" / "a";
    const auto package_b = root / "cache" / "b";
    write(package_a / "include" / "shared" / "a.hpp", "#pragma once\n");
    write(package_b / "include" / "shared" / "b.hpp", "#pragma once\n");
    fs::create_directory_symlink(package_a, project / ".cpm" / "packages" / "a");
    fs::create_directory_symlink(package_b, project / ".cpm" / "packages" / "b");
    cpm::Resolver(project).export_headers();
    require(fs::is_symlink(project / ".cpm" / "include" / "a"), "package namespace a is missing");
    require(fs::is_symlink(project / ".cpm" / "include" / "b"), "package namespace b is missing");
    const auto shared = project / ".cpm" / "include" / "shared";
    require(fs::is_symlink(shared), "compatibility alias is missing");
    require(fs::exists(project / ".cpm" / "include" / "a" / "shared" / "a.hpp"), "namespaced header a is missing");
    require(fs::exists(project / ".cpm" / "include" / "b" / "shared" / "b.hpp"), "namespaced header b is missing");
    require(fs::exists(shared / "a.hpp") != fs::exists(shared / "b.hpp"), "colliding alias was replaced or merged");

    const auto special_project = root / "shell $' path";
    cpm::Environment(special_project).create();
    const auto activation = special_project / ".cpm" / "activate.sh";
    const auto shell = cpm::Process::run({"bash", "-c",
                                             "set -u; unset CPATH LIBRARY_PATH LD_LIBRARY_PATH PKG_CONFIG_PATH CMAKE_PREFIX_PATH CPM_ACTIVE CPM_ROOT; "
                                             "source \"$1\" >/dev/null; first=$CPATH; source \"$1\" >/dev/null; "
                                             "[ \"$first\" = \"$CPATH\" ]; cpm_deactivate; [ -z \"${CPM_ACTIVE:-}\" ]",
                                             "bash", activation.string()},
        {}, {}, true);
    require(shell.exit_code == 0, "activation script quoting or deactivation failed: " + shell.output);
}

void test_incremental_build(const fs::path &root) {
    const auto project = root / "project with spaces";
    fs::create_directories(project);
    write(project / "cpm.toml", "[project]\nname=\"incremental\"\nentry=\"main.cpp\"\noutput=\"app\"\ncpp_standard=\"20\"\n");
    write(project / "main.cpp", "int main() { return 0; }\n");
    cpm::Environment(project).create();
    cpm::Builder builder(project, project / ".cpm", root / "cache");
    require(builder.build() == 0, "initial project build failed");
    fs::path object;
    for (const auto &entry : fs::recursive_directory_iterator(project / ".cpm" / "objects")) {
        if (entry.path().extension() == ".o") {
            object = entry.path();
            break;
        }
    }
    require(!object.empty(), "incremental object was not created");
    const auto timestamp = fs::last_write_time(object);
    require(builder.build() == 0, "cached project build failed");
    require(fs::last_write_time(object) == timestamp, "unchanged object was recompiled");
}

void test_transaction_lock(const fs::path &root) {
    const auto project = root / "transaction";
    write(project / "cpm.toml", "[project]\nname=\"transaction\"\n");
    write(project / ".cpm.install.lock" / "pid", "99999999\n");
    cpm::Installer installer(project, project / ".cpm", root / "cache");
    installer.install();
    require(fs::exists(project / ".cpm" / ".cpm_env"), "transaction was not published");
    require(fs::exists(project / "cpm.lock"), "lockfile was not published");
    require(!fs::exists(project / ".cpm.install.lock"), "stale install lock was not cleaned");

    write(project / ".cpm.install.lock" / "pid", std::to_string(::getpid()) + "\n");
    bool rejected = false;
    try {
        installer.install();
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "live install lock was ignored");
}

} // namespace

int main() {
    const auto root = fs::temp_directory_path() / ("cpm-core-tests-" + std::to_string(::getpid()));
    try {
        fs::remove_all(root);
        fs::create_directories(root);
        test_parser(root);
        test_process();
        test_environment_and_resolver(root);
        test_incremental_build(root);
        test_transaction_lock(root);
        fs::remove_all(root);
        std::cout << "all core tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        fs::remove_all(root);
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}

#include "cpm/config.hpp"
#include "cpm/nix_env.hpp"
#include "cpm/package_manager.hpp"
#include "cpm/process.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

static void print_usage() {
    std::cout << R"(
  ██████╗██████╗ ███╗   ███╗
 ██╔════╝██╔══██╗████╗ ████║
 ██║     ██████╔╝██╔████╔██║
 ██║     ██╔═══╝ ██║╚██╔╝██║
 ╚██████╗██║     ██║ ╚═╝ ██║
  ╚═════╝╚═╝     ╚═╝     ╚═╝
  C/C++ Package Manager v)" CPM_VERSION R"(

USAGE:
    cpm <command> [args]

COMMANDS:
    init <name>         Initialize a new project with cpm.toml
    run                 Install + build + run (like uv run)
    run <file.cpp>      Compile and run a single file
    build               Compile the project
    build -s            Production build (optimized, stripped, portable bundle)
    start               Run the built binary
    install             Install all dependencies from cpm.toml
    add <package>       Add a header package
    add --system <pkg>  Add a compiled Git package
    add --lib <name=attr> Add a Nix library
    remove <name>       Remove a package
    update              Refresh moving refs and rebuild the environment
    list                List installed packages
    info                Show system and compiler info
    setup               Install nix backend (enables fast pre-built packages)

EXAMPLES:
    cpm init myproject
    cpm add github:fmtlib/fmt@10.1.1
    cpm run                 # resolves, builds, and runs
    cpm build               # just compile
    cpm start               # just run the binary
    cpm run hello.cpp       # one-shot single file
)";
}

static void print_info() {
    std::cout << "[cpm] System Information:\n"
              << "  Architecture: " << cpm::Config::get_architecture() << "\n"
              << "  OS:           " << cpm::Config::get_os() << "\n"
              << "  Compiler:     " << cpm::Config::get_compiler() << "\n"
              << "  Version:      " << cpm::Config::get_compiler_version() << "\n"
              << "  C++ Standard: " << cpm::Config::get_cpp_standard() << "\n"
              << "  Global Cache: " << cpm::Config::get_global_cache_dir().string() << "\n";
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    const std::string command = argv[1];

    try {
        std::unique_ptr<cpm::PackageManager> manager;
        auto pm = [&]() -> cpm::PackageManager & {
            if (!manager) manager = std::make_unique<cpm::PackageManager>();
            return *manager;
        };
        if (command == "init") {
            if (argc < 3) {
                std::cerr << "[cpm] ERROR: cpm init <name>\n";
                return 1;
            }
            pm().init(argv[2]);

        } else if (command == "run") {
            if (argc >= 3) {
                std::string arg = argv[2];
                const auto extension = std::filesystem::path(arg).extension().string();
                if (extension == ".c" || extension == ".C" || extension == ".cc" || extension == ".cpp" || extension == ".cxx") return pm().run_file(arg);
            }
            return pm().run();

        } else if (command == "build") {
            bool static_build = (argc >= 3 && std::string(argv[2]) == "-s");
            return pm().build(static_build);

        } else if (command == "start") {
            return pm().start();

        } else if (command == "install") {
            pm().install();

        } else if (command == "add") {
            if (argc < 3) {
                std::cerr << "[cpm] ERROR: cpm add github:user/repo@version\n";
                return 1;
            }
            std::string kind = "header";
            int package_index = 2;
            if (std::string(argv[2]) == "--system") {
                kind = "system";
                package_index = 3;
            } else if (std::string(argv[2]) == "--lib") {
                kind = "nix";
                package_index = 3;
            }
            if (argc <= package_index) {
                std::cerr << "[cpm] ERROR: missing package specification\n";
                return 1;
            }
            pm().install_package(argv[package_index], kind);

        } else if (command == "remove" || command == "rm") {
            if (argc < 3) {
                std::cerr << "[cpm] ERROR: cpm remove <name>\n";
                return 1;
            }
            pm().remove_package(argv[2]);

        } else if (command == "update") {
            pm().update();

        } else if (command == "list" || command == "ls") {
            pm().list();

        } else if (command == "info") {
            print_info();

        } else if (command == "setup") {
            const char *home = std::getenv("HOME");
            cpm::NixEnv nix(std::filesystem::current_path() / ".cpm", std::filesystem::path(home ? home : "/tmp") / ".cpm" / "cache");

            if (nix.available()) {
                std::cout << "[cpm] Nix is already installed and ready.\n";
            } else {
                const auto installer = std::filesystem::temp_directory_path() / "cpm-nix-installer.sh";
                const auto download = cpm::Process::run({"curl", "--proto", "=https", "--tlsv1.2", "-fsSL", "https://install.determinate.systems/nix", "-o", installer.string()});
                if (download.exit_code != 0) throw std::runtime_error("failed to download the Nix installer");
                const auto install = cpm::Process::run({"sh", installer.string(), "install", "--no-confirm"});
                std::error_code error;
                std::filesystem::remove(installer, error);
                if (install.exit_code != 0) throw std::runtime_error("Nix installation failed");
            }

        } else if (command == "--help" || command == "-h" || command == "help") {
            print_usage();

        } else if (command == "--version" || command == "-v" || command == "version") {
            std::cout << "cpm " << CPM_VERSION << "\n";

        } else {
            std::cerr << "[cpm] Unknown command: " << command << "\n"
                      << "[cpm] Run 'cpm help' for usage.\n";
            return 1;
        }

    } catch (const std::exception &e) {
        std::cerr << "[cpm] FATAL: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

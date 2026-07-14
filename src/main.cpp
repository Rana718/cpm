#include "cpm/config.hpp"
#include "cpm/nix_env.hpp"
#include "cpm/package_manager.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
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
    add <package>       Add a package (github:user/repo@version)
    remove <name>       Remove a package
    update              Re-download and rebuild all packages
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
    cpm::PackageManager pm;

    try {
        if (command == "init") {
            if (argc < 3) {
                std::cerr << "[cpm] ERROR: cpm init <name>\n";
                return 1;
            }
            pm.init(argv[2]);

        } else if (command == "run") {
            if (argc >= 3) {
                std::string arg = argv[2];
                // Single-file mode: any argument containing a .c extension
                if (arg.find(".c") != std::string::npos) return pm.run_file(arg);
            }
            return pm.run();

        } else if (command == "build") {
            bool static_build = (argc >= 3 && std::string(argv[2]) == "-s");
            return pm.build(static_build);

        } else if (command == "start") {
            return pm.start();

        } else if (command == "install") {
            pm.install();

        } else if (command == "add") {
            if (argc < 3) {
                std::cerr << "[cpm] ERROR: cpm add github:user/repo@version\n";
                return 1;
            }
            pm.install_package(argv[2]);

        } else if (command == "remove" || command == "rm") {
            if (argc < 3) {
                std::cerr << "[cpm] ERROR: cpm remove <name>\n";
                return 1;
            }
            pm.remove_package(argv[2]);

        } else if (command == "update") {
            pm.update();

        } else if (command == "list" || command == "ls") {
            pm.list();

        } else if (command == "info") {
            print_info();

        } else if (command == "setup") {
            const char *home = std::getenv("HOME");
            cpm::NixEnv nix(std::filesystem::current_path() / ".cpm", std::filesystem::path(home ? home : "/tmp") / ".cpm" / "cache");

            if (nix.available()) {
                std::cout << "[cpm] Nix is already installed and ready.\n";
            } else {
                std::system("curl --proto '=https' --tlsv1.2 -sSf -L "
                            "https://install.determinate.systems/nix"
                            " | sh -s -- install --no-confirm 2>&1");
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

#include "cpm/package_manager.hpp"
#include "cpm/config.hpp"
#include <iostream>
#include <string>
#include <vector>

void print_usage() {
    std::cout << R"(
  ██████╗██████╗ ███╗   ███╗
 ██╔════╝██╔══██╗████╗ ████║
 ██║     ██████╔╝██╔████╔██║
 ██║     ██╔═══╝ ██║╚██╔╝██║
 ╚██████╗██║     ██║ ╚═╝ ██║
  ╚═════╝╚═╝     ╚═╝     ╚═╝
  C/C++ Package Manager v0.1.0

USAGE:
    cpm <command> [args]

COMMANDS:
    init <name>         Initialize a new project with cpm.toml
    run                 Install + build + run (like uv run)
    build               Compile the project
    start               Run the built binary
    install             Install all dependencies from cpm.toml
    add <package>       Add a package (github:user/repo@version)
    remove <name>       Remove a package
    update              Update all packages
    list                List installed packages
    info                Show system and compiler info

EXAMPLES:
    cpm init myproject
    cpm add github:fmtlib/fmt@10.1.1
    cpm run                 # resolves, builds, and runs
    cpm build               # just compile
    cpm start               # just run the binary
)";
}

void print_info() {
    std::cout << "[cpm] System Information:\n";
    std::cout << "  Architecture: " << cpm::Config::get_architecture() << "\n";
    std::cout << "  OS:           " << cpm::Config::get_os() << "\n";
    std::cout << "  Compiler:     " << cpm::Config::get_compiler() << "\n";
    std::cout << "  Version:      " << cpm::Config::get_compiler_version() << "\n";
    std::cout << "  C++ Standard: " << cpm::Config::get_cpp_standard() << "\n";
    std::cout << "  Global Cache: " << cpm::Config::get_global_cache_dir().string() << "\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    std::string command = argv[1];
    cpm::PackageManager pm;

    try {
        if (command == "init") {
            if (argc < 3) {
                std::cerr << "[cpm] ERROR: cpm init <name>\n";
                return 1;
            }
            pm.init(argv[2]);
        }
        else if (command == "run") {
            return pm.run();
        }
        else if (command == "build") {
            return pm.build();
        }
        else if (command == "start") {
            return pm.start();
        }
        else if (command == "install") {
            pm.install();
        }
        else if (command == "add") {
            if (argc < 3) {
                std::cerr << "[cpm] ERROR: cpm add github:user/repo@version\n";
                return 1;
            }
            pm.install_package(argv[2]);
        }
        else if (command == "remove" || command == "rm") {
            if (argc < 3) {
                std::cerr << "[cpm] ERROR: cpm remove <name>\n";
                return 1;
            }
            pm.remove_package(argv[2]);
        }
        else if (command == "update") {
            pm.update();
        }
        else if (command == "list" || command == "ls") {
            pm.list();
        }
        else if (command == "info") {
            print_info();
        }
        else if (command == "--help" || command == "-h" || command == "help") {
            print_usage();
        }
        else {
            std::cerr << "[cpm] Unknown command: " << command << "\n";
            std::cerr << "[cpm] Run 'cpm help' for usage.\n";
            return 1;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[cpm] FATAL: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

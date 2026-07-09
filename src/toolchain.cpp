#include "cpm/toolchain.hpp"
#include <iostream>
#include <cstdlib>
#include <array>
#include <memory>

namespace cpm {

Toolchain::Toolchain(const std::filesystem::path& cpm_dir)
    : cpm_dir_(cpm_dir)
    , toolchain_dir_(cpm_dir / "toolchain")
{}

Toolchain::CompilerSpec Toolchain::parse_compiler(const std::string& spec) {
    CompilerSpec result;
    result.pinned = false;

    if (spec.empty()) {
        result.type = "gcc";
        result.version = "";
        return result;
    }

    // Support formats: "gcc-13", "gcc@13", "clang-17", "clang@17", "g++-13"
    std::string s = spec;

    // Normalize: gcc@13 → gcc-13
    auto at_pos = s.find('@');
    if (at_pos != std::string::npos) {
        s[at_pos] = '-';
    }

    auto dash_pos = s.rfind('-');
    if (dash_pos != std::string::npos && dash_pos > 0) {
        std::string maybe_version = s.substr(dash_pos + 1);
        // Check if it's a number
        bool is_num = !maybe_version.empty();
        for (char c : maybe_version) {
            if (!std::isdigit(c) && c != '.') { is_num = false; break; }
        }

        if (is_num) {
            result.type = s.substr(0, dash_pos);
            result.version = maybe_version;
            result.pinned = true;
            // Normalize type
            if (result.type == "g++") result.type = "gcc";
            if (result.type == "clang++") result.type = "clang";
            return result;
        }
    }

    // No version specified
    result.type = s;
    if (result.type == "g++") result.type = "gcc";
    if (result.type == "clang++") result.type = "clang";
    return result;
}

std::string Toolchain::get_cc(const CompilerSpec& spec) {
    if (spec.pinned) {
        if (spec.type == "clang") return "clang-" + spec.version;
        return "gcc-" + spec.version;
    }
    return (spec.type == "clang") ? "clang" : "gcc";
}

std::string Toolchain::get_cxx(const CompilerSpec& spec) {
    if (spec.pinned) {
        if (spec.type == "clang") return "clang++-" + spec.version;
        return "g++-" + spec.version;
    }
    return (spec.type == "clang") ? "clang++" : "g++";
}

std::string Toolchain::ensure_compiler(const CompilerSpec& spec) {
    std::string cxx = get_cxx(spec);

    // Check if the compiler exists on the system
    std::string check = "which " + cxx + " > /dev/null 2>&1";
    if (std::system(check.c_str()) == 0) {
        return cxx;
    }

    // Compiler not found — show helpful message
    std::cerr << "[cpm] ERROR: Compiler '" << cxx << "' not found on your system.\n";
    std::cerr << "[cpm]\n";
    std::cerr << "[cpm] Install it:\n";
    std::cerr << "[cpm]   Arch Linux:   sudo pacman -S " << spec.type << spec.version << "\n";
    std::cerr << "[cpm]   Ubuntu/Debian: sudo apt install ";
    if (spec.type == "gcc") {
        std::cerr << "g++-" << spec.version << "\n";
    } else {
        std::cerr << "clang-" << spec.version << "\n";
    }
    std::cerr << "[cpm]   Fedora:       sudo dnf install ";
    if (spec.type == "gcc") {
        std::cerr << "gcc-c++-" << spec.version << "\n";
    } else {
        std::cerr << "clang-" << spec.version << "\n";
    }
    std::cerr << "[cpm]\n";
    std::cerr << "[cpm] Or change 'compiler' in cpm.toml to use your system compiler.\n";
    std::cerr << "[cpm] Available compilers on this system:\n";

    // List what's available
    std::system("ls /usr/bin/g++* /usr/bin/clang++* 2>/dev/null | sed 's|/usr/bin/||' | sed 's/^/[cpm]   /'");

    // Fallback to system default
    std::cerr << "[cpm]\n";
    std::cerr << "[cpm] Falling back to system default compiler.\n";
    return (spec.type == "clang") ? "clang++" : "g++";
}

std::string Toolchain::get_path_prefix() {
    return ""; // No special path needed — using system compilers
}

// Not used — we use system compilers
std::string Toolchain::get_gcc_url(const std::string&) { return ""; }
std::string Toolchain::get_clang_url(const std::string&) { return ""; }
bool Toolchain::download_gcc(const std::string&) { return false; }
bool Toolchain::download_clang(const std::string&) { return false; }

} // namespace cpm

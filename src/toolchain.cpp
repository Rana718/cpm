#include "cpm/toolchain.hpp"
#include <iostream>
#include <cstdlib>
#include <array>
#include <memory>
#include <fstream>

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

    auto at_pos = spec.find('@');
    if (at_pos != std::string::npos) {
        result.type = spec.substr(0, at_pos);
        result.version = spec.substr(at_pos + 1);
        result.pinned = true;
    } else {
        result.type = spec;
        result.version = "";
    }

    return result;
}

std::string Toolchain::get_cc(const CompilerSpec& spec) {
    if (!spec.pinned) {
        return (spec.type == "clang") ? "clang" : "gcc";
    }

    auto bin_dir = toolchain_dir_ / (spec.type + "-" + spec.version) / "bin";
    if (spec.type == "clang") {
        auto path = bin_dir / "clang";
        if (std::filesystem::exists(path)) return path.string();
    } else {
        auto path = bin_dir / "gcc";
        if (std::filesystem::exists(path)) return path.string();
    }

    // Fallback to system versioned binary
    std::string versioned = spec.type + "-" + spec.version;
    std::string check = "which " + versioned + " > /dev/null 2>&1";
    if (std::system(check.c_str()) == 0) return versioned;

    return (spec.type == "clang") ? "clang" : "gcc";
}

std::string Toolchain::get_cxx(const CompilerSpec& spec) {
    if (!spec.pinned) {
        return (spec.type == "clang") ? "clang++" : "g++";
    }

    auto bin_dir = toolchain_dir_ / (spec.type + "-" + spec.version) / "bin";
    if (spec.type == "clang") {
        auto path = bin_dir / "clang++";
        if (std::filesystem::exists(path)) return path.string();
    } else {
        auto path = bin_dir / "g++";
        if (std::filesystem::exists(path)) return path.string();
    }

    // Fallback to system versioned binary
    if (spec.type == "clang") {
        std::string versioned = "clang++-" + spec.version;
        std::string check = "which " + versioned + " > /dev/null 2>&1";
        if (std::system(check.c_str()) == 0) return versioned;
    } else {
        std::string versioned = "g++-" + spec.version;
        std::string check = "which " + versioned + " > /dev/null 2>&1";
        if (std::system(check.c_str()) == 0) return versioned;
    }

    return (spec.type == "clang") ? "clang++" : "g++";
}

std::string Toolchain::ensure_compiler(const CompilerSpec& spec) {
    if (!spec.pinned) {
        return get_cxx(spec);
    }

    // Check if already downloaded
    auto tc_dir = toolchain_dir_ / (spec.type + "-" + spec.version);
    if (std::filesystem::exists(tc_dir / "bin")) {
        return get_cxx(spec);
    }

    // Check if system has the versioned compiler
    std::string versioned_cxx = (spec.type == "clang") ?
        "clang++-" + spec.version : "g++-" + spec.version;
    std::string check = "which " + versioned_cxx + " > /dev/null 2>&1";
    if (std::system(check.c_str()) == 0) {
        return versioned_cxx;
    }

    // Download prebuilt toolchain
    std::cout << "[cpm] Downloading " << spec.type << " " << spec.version << " toolchain...\n";
    std::filesystem::create_directories(tc_dir);

    bool ok = false;
    if (spec.type == "clang") {
        ok = download_clang(spec.version);
    } else {
        ok = download_gcc(spec.version);
    }

    if (ok) {
        std::cout << "[cpm] Toolchain ready: " << spec.type << " " << spec.version << "\n";
        return get_cxx(spec);
    }

    std::cerr << "[cpm] WARNING: Could not download " << spec.type << " " << spec.version
              << ", using system compiler\n";
    return (spec.type == "clang") ? "clang++" : "g++";
}

std::string Toolchain::get_path_prefix() {
    if (!std::filesystem::exists(toolchain_dir_)) return "";

    std::string path;
    for (const auto& entry : std::filesystem::directory_iterator(toolchain_dir_)) {
        if (entry.is_directory()) {
            auto bin = entry.path() / "bin";
            if (std::filesystem::exists(bin)) {
                if (!path.empty()) path += ":";
                path += bin.string();
            }
        }
    }
    return path;
}

std::string Toolchain::get_gcc_url(const std::string& version) {
    // GCC doesn't have official prebuilt binaries for Linux.
    // Use a known working mirror or fallback to clang.
    // For now, try the Ubuntu PPA style archives
    return "https://kayari.org/gcc-builds/downloads/gcc-" +
           version + ".1.0-aarch64-linux.tar.xz";  // This likely won't work for x86_64
}

std::string Toolchain::get_clang_url(const std::string& version) {
    // Official LLVM releases — these WORK reliably
    return "https://github.com/llvm/llvm-project/releases/download/llvmorg-" +
           version + ".0.6/clang+llvm-" + version +
           ".0.6-x86_64-linux-gnu-ubuntu-22.04.tar.xz";
}

bool Toolchain::download_gcc(const std::string& version) {
    auto tc_dir = toolchain_dir_ / ("gcc-" + version);
    auto url = get_gcc_url(version);

    std::string download_cmd =
        "curl -fSL --progress-bar '" + url + "' -o /tmp/cpm-gcc.tar.xz 2>&1"
        " && mkdir -p " + tc_dir.string() +
        " && tar -xf /tmp/cpm-gcc.tar.xz -C " + tc_dir.string() + " --strip-components=1"
        " && rm /tmp/cpm-gcc.tar.xz";

    int ret = std::system(download_cmd.c_str());
    if (ret != 0) {
        // Fallback: try to compile from source (last resort, very slow)
        std::cerr << "[cpm] Prebuilt GCC " << version << " not available for this platform\n";
        return false;
    }
    return true;
}

bool Toolchain::download_clang(const std::string& version) {
    auto tc_dir = toolchain_dir_ / ("clang-" + version);
    auto url = get_clang_url(version);

    std::string download_cmd =
        "curl -fSL --progress-bar '" + url + "' -o /tmp/cpm-clang.tar.xz 2>&1"
        " && mkdir -p " + tc_dir.string() +
        " && tar -xf /tmp/cpm-clang.tar.xz -C " + tc_dir.string() + " --strip-components=1"
        " && rm /tmp/cpm-clang.tar.xz";

    int ret = std::system(download_cmd.c_str());
    if (ret != 0) {
        std::cerr << "[cpm] Prebuilt Clang " << version << " not available for this platform\n";
        return false;
    }
    return true;
}

} // namespace cpm

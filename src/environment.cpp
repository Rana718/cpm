#include "cpm/environment.hpp"
#include <fstream>
#include <sstream>

namespace cpm {

Environment::Environment(const std::filesystem::path& project_root)
    : project_root_(project_root)
    , cpm_dir_(project_root / ".cpm")
{}

void Environment::create() {
    namespace fs = std::filesystem;

    fs::create_directories(cpm_dir_);
    fs::create_directories(get_include_dir());
    fs::create_directories(get_lib_dir());
    fs::create_directories(get_bin_dir());
    fs::create_directories(get_packages_dir());

    // Create marker file
    std::ofstream marker(cpm_dir_ / ".cpm_env");
    marker << "# CPM Environment - Do not delete\n";
    marker << "version=0.1.0\n";
    marker.close();

    // Create activation script
    std::ofstream activate(cpm_dir_ / "activate.sh");
    activate << generate_activation_script();
    activate.close();
}

bool Environment::exists() const {
    return std::filesystem::exists(cpm_dir_ / ".cpm_env");
}

std::map<std::string, std::string> Environment::get_env_vars() const {
    std::map<std::string, std::string> env;
    env["CPM_ROOT"] = cpm_dir_.string();
    env["CPATH"] = get_include_dir().string();
    env["LIBRARY_PATH"] = get_lib_dir().string();
    env["LD_LIBRARY_PATH"] = get_lib_dir().string();
    env["CMAKE_PREFIX_PATH"] = cpm_dir_.string();
    env["PKG_CONFIG_PATH"] = (get_lib_dir() / "pkgconfig").string();
    return env;
}

std::filesystem::path Environment::get_include_dir() const {
    return cpm_dir_ / "include";
}

std::filesystem::path Environment::get_lib_dir() const {
    return cpm_dir_ / "lib";
}

std::filesystem::path Environment::get_bin_dir() const {
    return cpm_dir_ / "bin";
}

std::filesystem::path Environment::get_packages_dir() const {
    return cpm_dir_ / "packages";
}

std::string Environment::generate_activation_script() const {
    std::ostringstream s;
    s << "#!/bin/bash\n";
    s << "# CPM Environment Activation Script\n";
    s << "# Usage: source .cpm/activate.sh\n\n";

    s << "export CPM_ACTIVE=1\n";
    s << "export CPM_ROOT=\"" << cpm_dir_.string() << "\"\n\n";

    s << "# Save old paths for deactivation\n";
    s << "export CPM_OLD_CPATH=\"$CPATH\"\n";
    s << "export CPM_OLD_LIBRARY_PATH=\"$LIBRARY_PATH\"\n";
    s << "export CPM_OLD_LD_LIBRARY_PATH=\"$LD_LIBRARY_PATH\"\n";
    s << "export CPM_OLD_PKG_CONFIG_PATH=\"$PKG_CONFIG_PATH\"\n";
    s << "export CPM_OLD_PATH=\"$PATH\"\n\n";

    s << "# Set CPM paths (override system)\n";
    s << "export CPATH=\"" << get_include_dir().string() << ":$CPATH\"\n";
    s << "export LIBRARY_PATH=\"" << get_lib_dir().string() << ":$LIBRARY_PATH\"\n";
    s << "export LD_LIBRARY_PATH=\"" << get_lib_dir().string() << ":$LD_LIBRARY_PATH\"\n";
    s << "export PKG_CONFIG_PATH=\"" << (get_lib_dir() / "pkgconfig").string() << ":$PKG_CONFIG_PATH\"\n";
    s << "export PATH=\"" << get_bin_dir().string() << ":$PATH\"\n\n";

    s << "echo \"[cpm] Environment activated.\"\n";
    s << "echo \"[cpm] Include: " << get_include_dir().string() << "\"\n";
    s << "echo \"[cpm] Lib:     " << get_lib_dir().string() << "\"\n";

    return s.str();
}

} // namespace cpm

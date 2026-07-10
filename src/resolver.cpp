#include "cpm/resolver.hpp"

#include <filesystem>
#include <string>

namespace cpm {

Resolver::Resolver(const std::filesystem::path &project_root) : project_root_(project_root), include_dir_(project_root / ".cpm" / "include"), packages_dir_(project_root / ".cpm" / "packages") {}

std::filesystem::path Resolver::find_include_root(const std::filesystem::path &package_dir) const {
    namespace fs = std::filesystem;

    // Priority 1: has "include/" with subdirectories
    auto include_dir = package_dir / "include";
    if (fs::exists(include_dir) && fs::is_directory(include_dir)) {
        return include_dir;
    }

    // Priority 2: "single_include/"
    auto single_include = package_dir / "single_include";
    if (fs::exists(single_include) && fs::is_directory(single_include)) {
        return single_include;
    }

    // Priority 3: "src/" with headers — treat as package-named directory
    // (i.e., src/App.h → include as <packagename/App.h>)
    auto src_dir = package_dir / "src";
    if (fs::exists(src_dir) && fs::is_directory(src_dir)) {
        for (const auto &entry : fs::directory_iterator(src_dir)) {
            if (entry.is_regular_file()) {
                auto ext = entry.path().extension().string();
                if (ext == ".h" || ext == ".hpp" || ext == ".hxx") {
                    // Return empty to signal "use package name as namespace"
                    return src_dir;
                }
            }
        }
    }

    return package_dir;
}

void Resolver::export_package(const std::string &package_name, const std::filesystem::path &package_dir) {
    namespace fs = std::filesystem;

    auto include_root = find_include_root(package_dir);

    // Check if include_root has subdirectories that match library naming
    // e.g., include/nlohmann/ → symlink nlohmann/
    // vs src/App.h → needs to be wrapped as packagename/App.h
    bool has_subdir_structure = false;
    for (const auto &entry : fs::directory_iterator(include_root)) {
        if (entry.is_directory()) {
            has_subdir_structure = true;
            break;
        }
    }

    if (has_subdir_structure && include_root.filename() != "src") {
        // Standard layout: include/nlohmann/json.hpp → symlink nlohmann/
        for (const auto &entry : fs::directory_iterator(include_root)) {
            auto target_name = entry.path().filename();
            auto link_path = include_dir_ / target_name;

            if (fs::exists(link_path) || fs::is_symlink(link_path)) {
                fs::remove_all(link_path);
            }

            fs::create_symlink(fs::absolute(entry.path()), link_path);
        }
    } else {
        // Headers directly in src/ or root → wrap under package name
        // src/App.h → .cpm/include/uWebSockets/App.h
        auto link_path = include_dir_ / package_name;

        if (fs::exists(link_path) || fs::is_symlink(link_path)) {
            fs::remove_all(link_path);
        }

        fs::create_symlink(fs::absolute(include_root), link_path);
    }
}

void Resolver::export_headers() {
    namespace fs = std::filesystem;

    if (!fs::exists(packages_dir_)) return;

    fs::create_directories(include_dir_);

    for (const auto &entry : fs::directory_iterator(packages_dir_)) {
        if (!entry.is_directory() && !entry.is_symlink()) continue;

        std::string package_name = entry.path().filename().string();

        auto actual_path = entry.path();
        if (fs::is_symlink(entry.path())) {
            actual_path = fs::read_symlink(entry.path());
        }

        export_package(package_name, actual_path);
    }
}

std::string Resolver::get_include_path() const { return include_dir_.string(); }

} // namespace cpm

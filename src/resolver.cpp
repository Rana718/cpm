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

    // Every package has a collision-free namespace. Build commands also add the
    // actual include root, which preserves upstream include conventions.
    const auto namespaced = include_dir_ / package_name;
    if (fs::is_symlink(namespaced)) fs::remove(namespaced);
    if (!fs::exists(namespaced)) {
        fs::create_directory_symlink(fs::absolute(include_root), namespaced);
    }

    // Compatibility aliases for conventional include/foo and single_include/foo
    // layouts. Never replace another package's export.
    if (include_root.filename() == "include" || include_root.filename() == "single_include") {
        for (const auto &entry : fs::directory_iterator(include_root)) {
            const auto alias = include_dir_ / entry.path().filename();
            if (fs::exists(alias) || fs::is_symlink(alias)) continue;
            if (entry.is_directory())
                fs::create_directory_symlink(fs::absolute(entry.path()), alias);
            else if (entry.is_regular_file())
                fs::create_symlink(fs::absolute(entry.path()), alias);
        }
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
            actual_path = fs::canonical(entry.path());
        }

        export_package(package_name, actual_path);
    }
}

std::string Resolver::get_include_path() const { return include_dir_.string(); }

} // namespace cpm

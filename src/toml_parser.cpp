#include "cpm/toml_parser.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace cpm {

std::string TomlParser::trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\r\n");
    auto end = str.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return str.substr(start, end - start + 1);
}

std::pair<std::string, std::string> TomlParser::parse_key_value(const std::string& line) {
    auto eq_pos = line.find('=');
    if (eq_pos == std::string::npos) return {"", ""};

    std::string key = trim(line.substr(0, eq_pos));
    std::string value = trim(line.substr(eq_pos + 1));

    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }

    return {key, value};
}

ProjectConfig TomlParser::parse(const std::filesystem::path& toml_path) {
    std::ifstream file(toml_path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open " + toml_path.string());
    }

    ProjectConfig config;
    std::string line;
    std::string current_section;

    while (std::getline(file, line)) {
        line = trim(line);

        if (line.empty() || line[0] == '#') continue;

        if (line[0] == '[') {
            auto end = line.find(']');
            if (end != std::string::npos) {
                current_section = line.substr(1, end - 1);
            }
            continue;
        }

        auto [key, value] = parse_key_value(line);
        if (key.empty()) continue;

        if (current_section == "project") {
            if (key == "name") config.name = value;
            else if (key == "version") config.version = value;
            else if (key == "description") config.description = value;
            else if (key == "cpp_standard") config.cpp_standard = value;
            else if (key == "compiler") config.compiler = value;
            else if (key == "entry") config.entry = value;
            else if (key == "output") config.output = value;
        }
        else if (current_section == "dependencies") {
            // Header-only packages: name = "github:user/repo@version"
            GitDependency dep;
            dep.name = key;

            std::string url = value;
            if (url.find("github:") == 0) {
                url = url.substr(7);
            }

            auto at_pos = url.find('@');
            if (at_pos != std::string::npos) {
                dep.github_url = "https://github.com/" + url.substr(0, at_pos);
                dep.version = url.substr(at_pos + 1);
            } else {
                dep.github_url = "https://github.com/" + url;
                dep.version = "*";
            }

            config.git_dependencies.push_back(dep);
        }
        else if (current_section == "system-dependencies") {
            // Compiled libraries: name = "github:user/repo@version"
            // These get built from source inside .cpm/
            SystemDependency dep;
            dep.name = key;

            std::string url = value;
            if (url.find("github:") == 0) {
                url = url.substr(7);
            }

            auto at_pos = url.find('@');
            if (at_pos != std::string::npos) {
                dep.github_url = "https://github.com/" + url.substr(0, at_pos);
                dep.version = url.substr(at_pos + 1);
            } else {
                dep.github_url = "https://github.com/" + url;
                dep.version = "*";
            }

            config.system_dependencies.push_back(dep);
        }
        else if (current_section == "scripts") {
            if (key == "start") config.start_script = value;
        }
    }

    if (config.cpp_standard.empty()) config.cpp_standard = "20";
    if (config.output.empty()) config.output = config.name;

    return config;
}

void TomlParser::create_default(const std::filesystem::path& toml_path, const std::string& project_name) {
    std::ofstream file(toml_path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot create " + toml_path.string());
    }

    file << "# CPM Package Configuration\n\n";

    file << "[project]\n";
    file << "name = \"" << project_name << "\"\n";
    file << "version = \"0.1.0\"\n";
    file << "description = \"\"\n";
    file << "cpp_standard = \"20\"\n";
    file << "# compiler = \"gcc\"  # auto-detected if not specified\n";
    file << "entry = \"main.cpp\"\n";
    file << "output = \"" << project_name << "\"\n\n";

    file << "[scripts]\n";
    file << "start = \"./" << project_name << "\"\n\n";

    file << "[dependencies]\n";
    file << "# Header-only libraries (just download, no build needed)\n";
    file << "# Format: name = \"github:user/repo@version\"\n";
    file << "#\n";
    file << "# json = \"github:nlohmann/json@v3.11.3\"\n";
    file << "# fmt = \"github:fmtlib/fmt@10.1.1\"\n\n";

    file << "[system-dependencies]\n";
    file << "# Compiled libraries (downloaded + built inside .cpm/)\n";
    file << "# Format: name = \"github:user/repo@version\"\n";
    file << "# These are built from source — completely isolated from system.\n";
    file << "#\n";
    file << "# hiredis = \"github:redis/hiredis@v1.2.0\"\n";
    file << "# zlib = \"github:madler/zlib@v1.3.1\"\n";

    file.close();
}

} // namespace cpm

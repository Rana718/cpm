#include "cpm/toml_parser.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace cpm {
namespace {

bool valid_package_name(const std::string &name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; });
}

bool valid_nix_attr(const std::string &name) { return valid_package_name(name); }

bool valid_nix_pin(const std::string &pin) {
    return std::all_of(pin.begin(), pin.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/'; });
}

bool safe_project_path(const std::string &value) {
    const std::filesystem::path path(value);
    if (path.empty() || path.is_absolute()) return false;
    return std::ranges::none_of(path, [](const auto &component) { return component == ".."; });
}

std::string strip_comment(const std::string &line) {
    bool quoted = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            escaped = false;
        } else if (c == '\\' && quoted) {
            escaped = true;
        } else if (c == '"') {
            quoted = !quoted;
        } else if (c == '#' && !quoted) {
            return line.substr(0, i);
        }
    }
    return line;
}

std::string decode_string(const std::string &raw) {
    if (raw.empty() || raw.front() != '"') return raw;
    if (raw.size() < 2 || raw.back() != '"') throw std::runtime_error("unterminated quoted string");
    std::string value;
    value.reserve(raw.size() - 2);
    bool escaped = false;
    for (size_t i = 1; i + 1 < raw.size(); ++i) {
        const char c = raw[i];
        if (!escaped && c == '\\') {
            escaped = true;
            continue;
        }
        if (escaped) {
            switch (c) {
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case '"':
                value.push_back('"');
                break;
            case '\\':
                value.push_back('\\');
                break;
            default:
                throw std::runtime_error(std::string("unsupported escape sequence: \\") + c);
            }
            escaped = false;
        } else {
            value.push_back(c);
        }
    }
    if (escaped) throw std::runtime_error("unterminated escape sequence");
    return value;
}

std::vector<std::string> parse_string_array(const std::string &raw) {
    if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']') {
        throw std::runtime_error("expected an array of strings");
    }
    std::vector<std::string> values;
    size_t i = 1;
    while (i + 1 < raw.size()) {
        while (i + 1 < raw.size() && (std::isspace(static_cast<unsigned char>(raw[i])) || raw[i] == ',')) ++i;
        if (i + 1 >= raw.size()) break;
        if (raw[i] != '"') throw std::runtime_error("array values must be quoted strings");
        const size_t start = i++;
        bool escaped = false;
        for (; i < raw.size(); ++i) {
            if (!escaped && raw[i] == '"') break;
            if (!escaped && raw[i] == '\\')
                escaped = true;
            else
                escaped = false;
        }
        if (i >= raw.size()) throw std::runtime_error("unterminated string in array");
        values.push_back(decode_string(raw.substr(start, i - start + 1)));
        ++i;
        while (i + 1 < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        if (i + 1 < raw.size() && raw[i] != ',') throw std::runtime_error("expected ',' between array values");
    }
    return values;
}

std::string encode_string(const std::string &value) {
    std::string encoded = "\"";
    for (const char c : value) {
        if (c == '\\' || c == '"') encoded.push_back('\\');
        encoded.push_back(c);
    }
    encoded.push_back('"');
    return encoded;
}

std::string dependency_spec(const GitDependency &dep) {
    std::string url = dep.github_url;
    constexpr const char *github = "https://github.com/";
    if (url.starts_with(github)) url = "github:" + url.substr(std::char_traits<char>::length(github));
    if (!dep.version.empty() && dep.version != "*") url += "@" + dep.version;
    return url;
}

void upsert_section_value(const std::filesystem::path &path, const std::string &section_name, const std::string &key, const std::string &value) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    const std::string heading = "[" + section_name + "]";
    size_t section = lines.size();
    size_t section_end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        auto clean = strip_comment(lines[i]);
        const auto begin = clean.find_first_not_of(" \t\r\n");
        const auto end = clean.find_last_not_of(" \t\r\n");
        clean = begin == std::string::npos ? std::string{} : clean.substr(begin, end - begin + 1);
        if (clean == heading) {
            section = i;
            continue;
        }
        if (section != lines.size() && i > section && clean.starts_with("[")) {
            section_end = i;
            break;
        }
    }
    const auto replacement = key + " = " + encode_string(value);
    if (section == lines.size()) {
        lines.push_back({});
        lines.push_back(heading);
        lines.push_back(replacement);
    } else {
        bool replaced = false;
        for (size_t i = section + 1; i < section_end; ++i) {
            const auto clean = strip_comment(lines[i]);
            const auto equals = clean.find('=');
            if (equals == std::string::npos) continue;
            auto candidate = clean.substr(0, equals);
            const auto begin = candidate.find_first_not_of(" \t\r\n");
            const auto end = candidate.find_last_not_of(" \t\r\n");
            candidate = begin == std::string::npos ? std::string{} : candidate.substr(begin, end - begin + 1);
            if (candidate == key) {
                lines[i] = replacement;
                replaced = true;
                break;
            }
        }
        if (!replaced) lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(section_end), replacement);
    }
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write " + temporary);
    for (const auto &output_line : lines) output << output_line << '\n';
    output.close();
    std::filesystem::rename(temporary, path);
}

} // namespace

std::string TomlParser::trim(const std::string &str) {
    const auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

std::pair<std::string, std::string> TomlParser::parse_key_value(const std::string &line) {
    bool quoted = false;
    bool escaped = false;
    size_t eq = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escaped)
            escaped = false;
        else if (c == '\\' && quoted)
            escaped = true;
        else if (c == '"')
            quoted = !quoted;
        else if (c == '=' && !quoted) {
            eq = i;
            break;
        }
    }
    if (eq == std::string::npos) return {};
    auto key = trim(line.substr(0, eq));
    auto value = trim(line.substr(eq + 1));
    if (!value.empty() && value.front() == '"') value = decode_string(value);
    return {key, value};
}

GitDependency TomlParser::parse_git_dependency(const std::string &name, const std::string &raw_spec) {
    if (!valid_package_name(name)) throw std::runtime_error("invalid package name '" + name + "'");
    std::string spec = trim(raw_spec);
    if (spec.empty()) throw std::runtime_error("empty dependency specification for '" + name + "'");

    GitDependency dep{name, {}, "*"};
    const bool shorthand = spec.starts_with("github:");
    if (shorthand) spec.erase(0, 7);

    // A trailing @ref is supported for shorthands and URLs. The @ in git@host
    // is not a version delimiter because it appears before the repository path.
    size_t at = std::string::npos;
    if (shorthand || (spec.find("://") == std::string::npos && !spec.starts_with("git@"))) {
        at = spec.find('@');
    } else if (const auto scheme = spec.find("://"); scheme != std::string::npos) {
        const auto path_start = spec.find('/', scheme + 3);
        if (path_start != std::string::npos) at = spec.find('@', path_start);
    } else if (spec.starts_with("git@")) {
        const auto repository_start = spec.find(':');
        if (repository_start != std::string::npos) at = spec.find('@', repository_start);
    }
    if (at != std::string::npos && at + 1 < spec.size()) {
        dep.version = spec.substr(at + 1);
        spec.erase(at);
    }
    if (spec.empty()) throw std::runtime_error("missing repository for '" + name + "'");

    if (shorthand || (spec.find("://") == std::string::npos && !spec.starts_with("git@") && spec.find('/') != std::string::npos)) {
        dep.github_url = "https://github.com/" + spec;
    } else {
        dep.github_url = spec;
    }
    return dep;
}

ProjectConfig TomlParser::parse(const std::filesystem::path &toml_path) {
    std::ifstream file(toml_path);
    if (!file) throw std::runtime_error("cannot open " + toml_path.string());

    ProjectConfig config;
    std::string line;
    std::string section;
    size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        line = trim(strip_comment(line));
        if (line.empty()) continue;
        try {
            if (line.front() == '[') {
                if (line.back() != ']' || line.size() < 3) throw std::runtime_error("invalid section header");
                section = trim(line.substr(1, line.size() - 2));
                continue;
            }
            auto [key, value] = parse_key_value(line);
            if (key.empty()) throw std::runtime_error("expected key = value");

            if (section == "project") {
                if (key == "name")
                    config.name = value;
                else if (key == "version")
                    config.version = value;
                else if (key == "description")
                    config.description = value;
                else if (key == "cpp_standard")
                    config.cpp_standard = value;
                else if (key == "compiler")
                    config.compiler = value;
                else if (key == "nixpkgs")
                    config.nixpkgs = value;
                else if (key == "entry")
                    config.entry = value;
                else if (key == "output")
                    config.output = value;
            } else if (section == "dependencies" || section == "system-dependencies") {
                auto git = parse_git_dependency(key, value);
                if (section == "dependencies")
                    config.git_dependencies.push_back(std::move(git));
                else
                    config.system_dependencies.push_back({git.name, git.github_url, git.version});
            } else if (section == "libs") {
                if (!valid_package_name(key)) throw std::runtime_error("invalid library name '" + key + "'");
                const auto attribute = value.empty() ? key : value;
                if (!valid_nix_attr(attribute)) throw std::runtime_error("invalid Nix attribute '" + attribute + "'");
                config.nix_libraries.push_back({key, attribute});
            } else if (section == "scripts" && key == "start") {
                config.start_script = value;
            } else if (section == "build") {
                if (key == "include_paths" || key == "sources" || key == "exclude_sources" || key == "compile_options" || key == "link_options" || key == "link_libraries" || key == "defines" ||
                    key == "nix_deps") {
                    const auto values = parse_string_array(value);
                    if (key == "include_paths")
                        config.include_paths = values;
                    else if (key == "sources")
                        config.extra_sources = values;
                    else if (key == "exclude_sources")
                        config.exclude_sources = values;
                    else if (key == "compile_options")
                        config.compile_options = values;
                    else if (key == "link_options")
                        config.link_options = values;
                    else if (key == "link_libraries")
                        config.link_libraries = values;
                    else if (key == "defines")
                        config.defines = values;
                    else if (key == "nix_deps")
                        config.extra_nix_deps = values;
                }
            }
        } catch (const std::exception &e) {
            throw std::runtime_error(toml_path.string() + ":" + std::to_string(line_number) + ": " + e.what());
        }
    }

    if (config.name.empty()) throw std::runtime_error(toml_path.string() + ": [project].name is required");
    if (!valid_package_name(config.name)) throw std::runtime_error(toml_path.string() + ": invalid project name '" + config.name + "'");
    if (config.cpp_standard.empty()) config.cpp_standard = "20";
    if (config.output.empty()) config.output = config.name;
    if (config.entry.empty()) config.entry = "main.cpp";
    static const std::vector<std::string> standards = {"11", "14", "17", "20", "23", "26"};
    if (std::find(standards.begin(), standards.end(), config.cpp_standard) == standards.end()) {
        throw std::runtime_error(toml_path.string() + ": unsupported cpp_standard '" + config.cpp_standard + "'");
    }
    if (!safe_project_path(config.output)) {
        throw std::runtime_error(toml_path.string() + ": output must stay inside the project");
    }
    if (!safe_project_path(config.entry)) {
        throw std::runtime_error(toml_path.string() + ": entry must stay inside the project");
    }
    if (!config.nixpkgs.empty() && !valid_nix_pin(config.nixpkgs)) {
        throw std::runtime_error(toml_path.string() + ": invalid nixpkgs pin");
    }
    for (const auto &attribute : config.extra_nix_deps) {
        if (!valid_nix_attr(attribute)) throw std::runtime_error(toml_path.string() + ": invalid Nix attribute '" + attribute + "'");
    }
    std::set<std::string> dependency_names;
    auto unique_name = [&](const std::string &name) {
        if (!dependency_names.insert(name).second) throw std::runtime_error(toml_path.string() + ": duplicate dependency name '" + name + "'");
    };
    for (const auto &dependency : config.git_dependencies) unique_name(dependency.name);
    for (const auto &dependency : config.system_dependencies) unique_name(dependency.name);
    for (const auto &library : config.nix_libraries) unique_name(library.name);
    return config;
}

void TomlParser::create_default(const std::filesystem::path &toml_path, const std::string &project_name) {
    if (!valid_package_name(project_name)) throw std::runtime_error("invalid project name '" + project_name + "'");
    std::ofstream file(toml_path);
    if (!file) throw std::runtime_error("cannot create " + toml_path.string());
    file << "# CPM Package Configuration\n\n"
         << "[project]\nname = " << encode_string(project_name) << "\nversion = \"0.1.0\"\ndescription = \"\"\ncpp_standard = \"20\"\n"
         << "# compiler = \"gcc\"\nentry = \"main.cpp\"\noutput = " << encode_string(project_name) << "\n\n[scripts]\nstart = \"./" << project_name
         << "\"\n\n[dependencies]\n# json = \"github:nlohmann/json@v3.11.3\"\n"
         << "\n[system-dependencies]\n# hiredis = \"github:redis/hiredis@v1.2.0\"\n"
         << "\n[libs]\n# zlib = \"zlib\"\n"
         << "\n[build]\n# include_paths = [\"include\"]\n# sources = [\"generated/source.cpp\"]\n"
         << "# exclude_sources = [\"tests\"]\n# defines = [\"FEATURE=1\"]\n"
         << "# compile_options = [\"-Wall\"]\n# link_libraries = [\"m\"]\n# link_options = []\n";
}

void TomlParser::upsert_dependency(const std::filesystem::path &toml_path, const GitDependency &dep, bool compiled) {
    if (!valid_package_name(dep.name)) throw std::runtime_error("invalid package name '" + dep.name + "'");
    upsert_section_value(toml_path, compiled ? "system-dependencies" : "dependencies", dep.name, dependency_spec(dep));
}

void TomlParser::upsert_nix_library(const std::filesystem::path &toml_path, const NixLibrary &library) {
    if (!valid_package_name(library.name)) throw std::runtime_error("invalid library name '" + library.name + "'");
    if (!valid_nix_attr(library.nix_attr)) throw std::runtime_error("invalid Nix attribute '" + library.nix_attr + "'");
    upsert_section_value(toml_path, "libs", library.name, library.nix_attr);
}

bool TomlParser::remove_dependency(const std::filesystem::path &toml_path, const std::string &name) {
    std::ifstream input(toml_path);
    if (!input) throw std::runtime_error("cannot open " + toml_path.string());
    std::vector<std::string> lines;
    std::string line;
    std::string section;
    bool removed = false;
    while (std::getline(input, line)) {
        const auto clean = trim(strip_comment(line));
        if (clean.starts_with("[") && clean.ends_with("]")) section = clean.substr(1, clean.size() - 2);
        auto [key, unused] = parse_key_value(clean);
        if ((section == "dependencies" || section == "system-dependencies" || section == "libs") && key == name) {
            removed = true;
            continue;
        }
        lines.push_back(line);
    }
    if (!removed) return false;
    const auto temp = toml_path.string() + ".tmp";
    std::ofstream output(temp, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write " + temp);
    for (const auto &out : lines) output << out << '\n';
    output.close();
    std::filesystem::rename(temp, toml_path);
    return true;
}

} // namespace cpm

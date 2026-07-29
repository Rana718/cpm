#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cpm {

struct ProcessResult {
    int exit_code = -1;
    std::string output;
};

class Process {
  public:
    static ProcessResult run(const std::vector<std::string> &arguments, const std::filesystem::path &working_directory = {}, const std::map<std::string, std::string> &environment = {},
        bool capture_output = false);
    static ProcessResult shell(const std::string &script, const std::filesystem::path &working_directory = {}, const std::map<std::string, std::string> &environment = {}, bool capture_output = false);
    static bool command_exists(const std::string &command);
};

} // namespace cpm

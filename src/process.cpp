#include "cpm/process.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace cpm {
namespace {

int decoded_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

std::vector<std::string> merged_environment(const std::map<std::string, std::string> &overrides) {
    std::map<std::string, std::string> variables;
    for (char **item = environ; item && *item; ++item) {
        std::string entry(*item);
        const auto equals = entry.find('=');
        if (equals != std::string::npos) variables[entry.substr(0, equals)] = entry.substr(equals + 1);
    }
    for (const auto &[name, value] : overrides) variables[name] = value;
    std::vector<std::string> result;
    result.reserve(variables.size());
    for (const auto &[name, value] : variables) result.push_back(name + "=" + value);
    return result;
}

} // namespace

ProcessResult Process::run(const std::vector<std::string> &arguments, const std::filesystem::path &working_directory, const std::map<std::string, std::string> &environment, bool capture_output) {
    if (arguments.empty() || arguments.front().empty()) throw std::invalid_argument("process command is empty");

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    auto environment_storage = merged_environment(environment);
    std::vector<char *> envp;
    envp.reserve(environment_storage.size() + 1);
    for (auto &entry : environment_storage) envp.push_back(entry.data());
    envp.push_back(nullptr);

    std::array<int, 2> output_pipe{-1, -1};
    if (capture_output && pipe(output_pipe.data()) != 0) {
        throw std::runtime_error("cannot create process pipe: " + std::string(std::strerror(errno)));
    }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error != 0) {
        if (capture_output) {
            close(output_pipe[0]);
            close(output_pipe[1]);
        }
        throw std::runtime_error("cannot initialize process: " + std::string(std::strerror(error)));
    }
    if (!working_directory.empty()) {
        error = posix_spawn_file_actions_addchdir_np(&actions, working_directory.c_str());
    }
    if (error == 0 && capture_output) error = posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    if (error == 0 && capture_output) error = posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
    if (error == 0 && capture_output) error = posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
    if (error == 0 && capture_output) error = posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
    if (error != 0) {
        posix_spawn_file_actions_destroy(&actions);
        if (capture_output) {
            close(output_pipe[0]);
            close(output_pipe[1]);
        }
        throw std::runtime_error("cannot configure process: " + std::string(std::strerror(error)));
    }

    pid_t pid = -1;
    error = posix_spawnp(&pid, argv.front(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) {
        if (capture_output) {
            close(output_pipe[0]);
            close(output_pipe[1]);
        }
        return {error == ENOENT ? 127 : 126, std::strerror(error)};
    }

    std::string output;
    if (capture_output) {
        close(output_pipe[1]);
        std::array<char, 4096> buffer{};
        while (true) {
            const auto count = read(output_pipe[0], buffer.data(), buffer.size());
            if (count > 0)
                output.append(buffer.data(), static_cast<size_t>(count));
            else if (count == 0)
                break;
            else if (errno != EINTR)
                break;
        }
        close(output_pipe[0]);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("cannot wait for process: " + std::string(std::strerror(errno)));
    }
    return {decoded_status(status), std::move(output)};
}

ProcessResult Process::shell(const std::string &script, const std::filesystem::path &working_directory, const std::map<std::string, std::string> &environment, bool capture_output) {
    return run({"/bin/sh", "-c", script}, working_directory, environment, capture_output);
}

bool Process::command_exists(const std::string &command) {
    if (command.empty() || command.find('/') != std::string::npos) {
        return !command.empty() && std::filesystem::is_regular_file(command) && access(command.c_str(), X_OK) == 0;
    }
    const char *path = std::getenv("PATH");
    if (!path) return false;
    std::string paths(path);
    size_t begin = 0;
    while (begin <= paths.size()) {
        const auto end = paths.find(':', begin);
        const auto directory = paths.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        const auto candidate = std::filesystem::path(directory.empty() ? "." : directory) / command;
        if (std::filesystem::is_regular_file(candidate) && access(candidate.c_str(), X_OK) == 0) return true;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

} // namespace cpm

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace cpm {

class NixEnv {
  public:
    NixEnv(std::filesystem::path cpm_dir, std::filesystem::path global_cache);

    [[nodiscard]] bool available() const;
    [[nodiscard]] std::string generate_shell_nix(const std::string &compiler, const std::string &cpp_standard,
                                                  const std::vector<std::string> &extra_deps = {},
                                                  const std::filesystem::path &user_nix_config = {}) const;
    [[nodiscard]] std::vector<std::string> detect_nix_deps(const std::filesystem::path &source) const;
    [[nodiscard]] std::string build_package(const std::string &attribute, const std::string &nixpkgs_pin = {}) const;

  private:
    std::filesystem::path cpm_dir_;
    std::filesystem::path global_cache_;
};

} // namespace cpm

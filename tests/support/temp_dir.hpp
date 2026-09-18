// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_TEST_TEMP_DIR_HPP
#define NAF_TEST_TEMP_DIR_HPP

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace naftest {

/// Creates a unique scratch directory and removes it when destroyed.
class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    static std::atomic<std::uint64_t> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("naf-" + tag + "-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_{};
};

}  // namespace naftest

#endif  // NAF_TEST_TEMP_DIR_HPP

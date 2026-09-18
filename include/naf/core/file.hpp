// Network Admission Fabric - crash-safe file primitives.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_CORE_FILE_HPP
#define NAF_CORE_FILE_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>

#include "naf/core/status.hpp"

namespace naf {

/// Thin, explicit wrapper over a C stdio handle with real durability
/// primitives. No buffering is left implicit: flush() pushes to the OS and
/// sync() pushes the OS cache to stable storage.
class FileHandle {
 public:
  FileHandle() = default;
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;
  ~FileHandle();

  /// Opens for read and/or write. When append is true, writes always land at
  /// the end of the file.
  static Expected<FileHandle> open(const std::filesystem::path& path, bool read, bool write, bool append,
                                   bool create);

  [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }
  [[nodiscard]] std::uint64_t size() const;

  Status write_all(std::span<const std::byte> data);
  /// Reads up to size bytes at the current position. Returns the number read.
  Expected<std::size_t> read_some(std::span<std::byte> out);

  Status flush();
  /// Pushes buffered data to stable storage. A successful return means the bytes
  /// survive process death; only then may a caller acknowledge durable work.
  Status sync();
  Status seek(std::uint64_t offset);
  Status truncate(std::uint64_t size);
  Status close();

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::FILE* raw() const noexcept { return handle_; }

 private:
  std::FILE* handle_ = nullptr;
  std::filesystem::path path_{};
};

/// Durably replaces destination with source. On success the destination holds
/// either the old or the new contents, never a blend.
Status atomic_replace_file(const std::filesystem::path& source, const std::filesystem::path& destination);

/// Flushes the directory entry describing path so a rename survives a crash.
Status sync_parent_directory(const std::filesystem::path& path);

Status ensure_directory(const std::filesystem::path& path);

/// Reads a whole file with a hard size bound.
Expected<std::string> read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes);

Status write_file_atomic(const std::filesystem::path& path, std::string_view contents);

}  // namespace naf

#endif  // NAF_CORE_FILE_HPP

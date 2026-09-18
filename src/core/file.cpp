// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/core/file.hpp"

#include "naf/core/bytes.hpp"

#include <cerrno>
#include <cstring>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace naf {
namespace {

Status last_error_status(StatusCode code, const std::filesystem::path& path, const char* what) {
  std::string message(what);
  message += " failed for ";
  message += path.string();
  message += ": errno=";
  message += std::to_string(errno);
  return Status::error(code, std::move(message));
}

}  // namespace

FileHandle::FileHandle(FileHandle&& other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)) {
  other.handle_ = nullptr;
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    if (handle_ != nullptr) {
      std::fclose(handle_);
    }
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    other.handle_ = nullptr;
  }
  return *this;
}

FileHandle::~FileHandle() {
  if (handle_ != nullptr) {
    std::fclose(handle_);
    handle_ = nullptr;
  }
}

Expected<FileHandle> FileHandle::open(const std::filesystem::path& path, bool read, bool write,
                                      bool append, bool create) {
  const char* mode = nullptr;
  if (append && write) {
    mode = read ? "a+b" : "ab";
  } else if (read && write) {
    mode = create ? "w+b" : "r+b";
  } else if (write) {
    mode = "wb";
  } else {
    mode = "rb";
  }
  if (append && create && !read) mode = "ab";

  FileHandle handle;
  handle.path_ = path;
  errno = 0;
#if defined(_WIN32)
  if (fopen_s(&handle.handle_, path.string().c_str(), mode) != 0 || handle.handle_ == nullptr) {
    handle.handle_ = nullptr;
    Status failure = last_error_status(StatusCode::DurableFailure, path, "fopen");
    return Status::error(failure.code(), failure.message() + " mode=" + std::string(mode));
  }
#else
  handle.handle_ = std::fopen(path.string().c_str(), mode);
  if (handle.handle_ == nullptr) {
    return last_error_status(StatusCode::DurableFailure, path, "fopen");
  }
#endif
  return handle;
}

std::uint64_t FileHandle::size() const {
  if (handle_ == nullptr) return 0;
  const long here = std::ftell(handle_);
#if defined(_WIN32)
  if (_fseeki64(handle_, 0, SEEK_END) != 0) return 0;
  const __int64 total = _ftelli64(handle_);
  if (here >= 0) (void)_fseeki64(handle_, here, SEEK_SET);
  return total < 0 ? 0 : static_cast<std::uint64_t>(total);
#else
  if (fseeko(handle_, 0, SEEK_END) != 0) return 0;
  const off_t total = ftello(handle_);
  if (here >= 0) (void)fseeko(handle_, here, SEEK_SET);
  return total < 0 ? 0 : static_cast<std::uint64_t>(total);
#endif
}

Status FileHandle::write_all(std::span<const std::byte> data) {
  if (handle_ == nullptr) return Status::error(StatusCode::InvalidArgument, "file is not open");
  if (data.empty()) return Status::ok();
  const std::size_t written =
      std::fwrite(data.data(), 1, data.size(), handle_);
  if (written != data.size()) {
    return last_error_status(StatusCode::DurableFailure, path_, "fwrite");
  }
  return Status::ok();
}

Expected<std::size_t> FileHandle::read_some(std::span<std::byte> out) {
  if (handle_ == nullptr) return Status::error(StatusCode::InvalidArgument, "file is not open");
  if (out.empty()) return std::size_t{0};
  const std::size_t got = std::fread(out.data(), 1, out.size(), handle_);
  if (got < out.size() && std::ferror(handle_) != 0) {
    return last_error_status(StatusCode::DurableFailure, path_, "fread");
  }
  return got;
}

Status FileHandle::flush() {
  if (handle_ == nullptr) return Status::error(StatusCode::InvalidArgument, "file is not open");
  if (std::fflush(handle_) != 0) {
    return last_error_status(StatusCode::DurableFailure, path_, "fflush");
  }
  return Status::ok();
}

Status FileHandle::sync() {
  if (handle_ == nullptr) return Status::error(StatusCode::InvalidArgument, "file is not open");
  if (std::fflush(handle_) != 0) {
    return last_error_status(StatusCode::DurableFailure, path_, "fflush");
  }
#if defined(_WIN32)
  if (_commit(_fileno(handle_)) != 0) {
    return last_error_status(StatusCode::DurableFailure, path_, "_commit");
  }
#else
  if (fsync(fileno(handle_)) != 0) {
    return last_error_status(StatusCode::DurableFailure, path_, "fsync");
  }
#endif
  return Status::ok();
}

Status FileHandle::seek(std::uint64_t offset) {
  if (handle_ == nullptr) return Status::error(StatusCode::InvalidArgument, "file is not open");
#if defined(_WIN32)
  if (_fseeki64(handle_, static_cast<__int64>(offset), SEEK_SET) != 0) {
    return last_error_status(StatusCode::DurableFailure, path_, "_fseeki64");
  }
#else
  if (fseeko(handle_, static_cast<off_t>(offset), SEEK_SET) != 0) {
    return last_error_status(StatusCode::DurableFailure, path_, "fseeko");
  }
#endif
  return Status::ok();
}

Status FileHandle::truncate(std::uint64_t size) {
  if (handle_ == nullptr) return Status::error(StatusCode::InvalidArgument, "file is not open");
  if (std::fflush(handle_) != 0) {
    return last_error_status(StatusCode::DurableFailure, path_, "fflush");
  }
#if defined(_WIN32)
  if (_chsize_s(_fileno(handle_), static_cast<__int64>(size)) != 0) {
    return last_error_status(StatusCode::DurableFailure, path_, "_chsize_s");
  }
#else
  if (ftruncate(fileno(handle_), static_cast<off_t>(size)) != 0) {
    return last_error_status(StatusCode::DurableFailure, path_, "ftruncate");
  }
#endif
  return seek(size);
}

Status FileHandle::close() {
  if (handle_ == nullptr) return Status::ok();
  std::FILE* handle = handle_;
  handle_ = nullptr;
  if (std::fclose(handle) != 0) {
    return last_error_status(StatusCode::DurableFailure, path_, "fclose");
  }
  return Status::ok();
}

Status atomic_replace_file(const std::filesystem::path& source, const std::filesystem::path& destination) {
#if defined(_WIN32)
  if (MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Status::error(StatusCode::DurableFailure,
                         "MoveFileEx failed for " + destination.string() +
                             " (error=" + std::to_string(GetLastError()) + ")");
  }
  return Status::ok();
#else
  if (::rename(source.string().c_str(), destination.string().c_str()) != 0) {
    return last_error_status(StatusCode::DurableFailure, destination, "rename");
  }
  return sync_parent_directory(destination);
#endif
}

Status sync_parent_directory(const std::filesystem::path& path) {
#if defined(_WIN32)
  (void)path;
  return Status::ok();
#else
  std::filesystem::path parent = path.parent_path();
  if (parent.empty()) parent = ".";
  const int fd = ::open(parent.string().c_str(), O_RDONLY);
  if (fd < 0) {
    return last_error_status(StatusCode::DurableFailure, parent, "open");
  }
  const int rc = fsync(fd);
  ::close(fd);
  if (rc != 0) {
    return last_error_status(StatusCode::DurableFailure, parent, "fsync");
  }
  return Status::ok();
#endif
}

Status ensure_directory(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec && !std::filesystem::is_directory(path)) {
    return Status::error(StatusCode::DurableFailure,
                         "cannot create directory " + path.string() + ": " + ec.message());
  }
  return Status::ok();
}

Expected<std::string> read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes) {
  auto opened = FileHandle::open(path, true, false, false, false);
  if (!opened) return opened.status();
  FileHandle handle = std::move(opened).value();
  const std::uint64_t total = handle.size();
  if (total > max_bytes) {
    return Status::error(StatusCode::OversizedInput, "file " + path.string() + " is larger than the bound");
  }
  std::string out;
  out.resize(static_cast<std::size_t>(total));
  if (total == 0) return out;
  std::size_t offset = 0;
  while (offset < out.size()) {
    auto got = handle.read_some(std::span<std::byte>(reinterpret_cast<std::byte*>(out.data() + offset),
                                                     out.size() - offset));
    if (!got) return got.status();
    if (got.value() == 0) break;
    offset += got.value();
  }
  out.resize(offset);
  return out;
}

Status write_file_atomic(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    auto opened = FileHandle::open(temp, false, true, false, true);
    if (!opened) return opened.status();
    FileHandle handle = std::move(opened).value();
    Status write = handle.write_all(as_bytes(contents));
    if (!write.is_ok()) return write;
    Status sync = handle.sync();
    if (!sync.is_ok()) return sync;
    Status closed = handle.close();
    if (!closed.is_ok()) return closed;
  }
  return atomic_replace_file(temp, path);
}

}  // namespace naf

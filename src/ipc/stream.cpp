// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/ipc/stream.hpp"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace naf::ipc {

Status MemoryStream::feed(std::span<const std::byte> data) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (incoming_.size() + data.size() > capacity_) {
    return Status::error(StatusCode::OversizedInput, "memory stream buffer bound exceeded");
  }
  for (const std::byte b : data) incoming_.push_back(b);
  return Status::ok();
}

Status MemoryStream::feed(std::string_view text) { return feed(as_bytes(text)); }

Expected<std::size_t> MemoryStream::read_some(std::span<std::byte> out) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!open_) return Status::error(StatusCode::TransportFailure, "stream is closed");
  if (incoming_.empty()) {
    if (!peer_open_) return std::size_t{0};
    return std::size_t{0};
  }
  const std::size_t count = std::min(out.size(), incoming_.size());
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = incoming_.front();
    incoming_.pop_front();
  }
  return count;
}

Status MemoryStream::write_all(std::span<const std::byte> data) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!open_) return Status::error(StatusCode::TransportFailure, "stream is closed");
  if (outgoing_.size() + data.size() > capacity_) {
    return Status::error(StatusCode::OversizedInput, "memory stream write bound exceeded");
  }
  outgoing_.insert(outgoing_.end(), data.begin(), data.end());
  return Status::ok();
}

Status MemoryStream::close() {
  std::lock_guard<std::mutex> guard(mutex_);
  open_ = false;
  peer_open_ = false;
  return Status::ok();
}

bool MemoryStream::is_open() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return open_;
}

const ByteBuffer& MemoryStream::written() const { return outgoing_; }

std::size_t MemoryStream::pending() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return incoming_.size();
}

void MemoryStream::close_peer() {
  std::lock_guard<std::mutex> guard(mutex_);
  peer_open_ = false;
}

FileStream::FileStream(std::FILE* handle, bool owns) : handle_(handle), owns_(owns) {}

FileStream::~FileStream() {
  if (owns_ && handle_ != nullptr) {
    std::fclose(handle_);
    handle_ = nullptr;
  }
}

Expected<std::size_t> FileStream::read_some(std::span<std::byte> out) {
  if (handle_ == nullptr) return Status::error(StatusCode::TransportFailure, "stream is closed");
  if (out.empty()) return std::size_t{0};
  const std::size_t got = std::fread(out.data(), 1, out.size(), handle_);
  if (got == 0 && std::ferror(handle_) != 0) {
    return Status::error(StatusCode::TransportFailure, "read failed");
  }
  return got;
}

Status FileStream::write_all(std::span<const std::byte> data) {
  if (handle_ == nullptr) return Status::error(StatusCode::TransportFailure, "stream is closed");
  if (data.empty()) return Status::ok();
  const std::size_t wrote = std::fwrite(data.data(), 1, data.size(), handle_);
  if (wrote != data.size()) return Status::error(StatusCode::TransportFailure, "write failed");
  if (std::fflush(handle_) != 0) return Status::error(StatusCode::TransportFailure, "flush failed");
  return Status::ok();
}

Status FileStream::close() {
  if (handle_ == nullptr) return Status::ok();
  if (owns_) {
    std::FILE* handle = handle_;
    handle_ = nullptr;
    if (std::fclose(handle) != 0) {
      return Status::error(StatusCode::TransportFailure, "close failed");
    }
    return Status::ok();
  }
  if (std::fflush(handle_) != 0) return Status::error(StatusCode::TransportFailure, "flush failed");
  handle_ = nullptr;
  return Status::ok();
}

bool FileStream::is_open() const { return handle_ != nullptr; }

#if defined(_WIN32)
namespace {
struct BinaryMode {
  BinaryMode() {
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
  }
};
}  // namespace
#endif

StdioStream::StdioStream() {
#if defined(_WIN32)
  static const BinaryMode binary_mode{};
  (void)binary_mode;
#endif
}

StdioStream::~StdioStream() { (void)close(); }

Expected<std::size_t> StdioStream::read_some(std::span<std::byte> out) {
  if (!open_) return Status::error(StatusCode::TransportFailure, "stdio stream is closed");
  if (out.empty()) return std::size_t{0};
  const std::size_t got = std::fread(out.data(), 1, out.size(), stdin);
  if (got == 0) {
    if (std::ferror(stdin) != 0) {
      return Status::error(StatusCode::TransportFailure, "stdin read failed");
    }
    return std::size_t{0};
  }
  return got;
}

Status StdioStream::write_all(std::span<const std::byte> data) {
  if (!open_) return Status::error(StatusCode::TransportFailure, "stdio stream is closed");
  if (data.empty()) return Status::ok();
  const std::size_t wrote = std::fwrite(data.data(), 1, data.size(), stdout);
  if (wrote != data.size()) {
    return Status::error(StatusCode::TransportFailure, "stdout write failed");
  }
  if (std::fflush(stdout) != 0) {
    return Status::error(StatusCode::TransportFailure, "stdout flush failed");
  }
  return Status::ok();
}

Status StdioStream::close() {
  if (!open_) return Status::ok();
  open_ = false;
  (void)std::fflush(stdout);
  return Status::ok();
}

bool StdioStream::is_open() const { return open_; }

}  // namespace naf::ipc

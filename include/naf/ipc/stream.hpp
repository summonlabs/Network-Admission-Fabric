// Network Admission Fabric - byte stream abstraction used by framed transport.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_IPC_STREAM_HPP
#define NAF_IPC_STREAM_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

#include "naf/core/bytes.hpp"
#include "naf/core/status.hpp"

namespace naf::ipc {

/// A duplex byte stream. Implementations must be safe to use from one reader
/// thread and one writer thread at a time. close() is idempotent and unblocks a
/// concurrent read so that shutdown never waits on a peer that has gone quiet.
class Stream {
 public:
  Stream() = default;
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;
  virtual ~Stream() = default;

  [[nodiscard]] virtual Expected<std::size_t> read_some(std::span<std::byte> out) = 0;
  virtual Status write_all(std::span<const std::byte> data) = 0;
  virtual Status close() = 0;
  [[nodiscard]] virtual bool is_open() const = 0;
};

/// Bounded in-memory duplex stream. Used by framing tests and by the
/// protocol self-checks; it enforces the same bounds as a real transport.
class MemoryStream final : public Stream {
 public:
  MemoryStream() = default;
  explicit MemoryStream(std::size_t capacity) : capacity_(capacity) {}

  /// Queues bytes as if they had arrived from the peer.
  Status feed(std::span<const std::byte> data);
  /// Convenience overload for text fixtures.
  Status feed(std::string_view text);

  [[nodiscard]] Expected<std::size_t> read_some(std::span<std::byte> out) override;
  Status write_all(std::span<const std::byte> data) override;
  Status close() override;
  [[nodiscard]] bool is_open() const override;

  [[nodiscard]] const ByteBuffer& written() const;
  [[nodiscard]] std::size_t pending() const;
  /// Simulates a peer that closed its end without sending a complete frame.
  void close_peer();

 private:
  mutable std::mutex mutex_{};
  std::deque<std::byte> incoming_{};
  ByteBuffer outgoing_{};
  std::size_t capacity_ = 1u << 20;
  bool open_ = true;
  bool peer_open_ = true;
};

/// Stream over a C stdio handle (used for stdio mode and for file fixtures).
class FileStream final : public Stream {
 public:
  explicit FileStream(std::FILE* handle, bool owns);
  ~FileStream() override;

  [[nodiscard]] Expected<std::size_t> read_some(std::span<std::byte> out) override;
  Status write_all(std::span<const std::byte> data) override;
  Status close() override;
  [[nodiscard]] bool is_open() const override;

 private:
  std::FILE* handle_ = nullptr;
  bool owns_ = false;
};

/// Duplex stream over the process standard input and output handles. Used for
/// the coordinator stdio mode, where a parent process drives the server over
/// real anonymous pipes.
class StdioStream final : public Stream {
 public:
  StdioStream();
  ~StdioStream() override;

  [[nodiscard]] Expected<std::size_t> read_some(std::span<std::byte> out) override;
  Status write_all(std::span<const std::byte> data) override;
  Status close() override;
  [[nodiscard]] bool is_open() const override;

 private:
  bool open_ = true;
};

}  // namespace naf::ipc

#endif  // NAF_IPC_STREAM_HPP

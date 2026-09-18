// Network Admission Fabric - bounded little-endian byte codec.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every byte that crosses a process, file or trust boundary is written and read
// through this codec. Reads are bounds-checked and never reinterpret untrusted
// memory; host endianness is irrelevant.
#ifndef NAF_CORE_BYTES_HPP
#define NAF_CORE_BYTES_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "naf/core/limits.hpp"
#include "naf/core/status.hpp"

namespace naf {

using ByteBuffer = std::vector<std::byte>;

inline std::span<const std::byte> as_bytes(std::string_view s) noexcept {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

class ByteWriter {
 public:
  explicit ByteWriter(std::size_t reserve = 64) { buffer_.reserve(reserve); }

  void u8(std::uint8_t v) { buffer_.push_back(static_cast<std::byte>(v)); }

  void u16(std::uint16_t v) {
    for (int i = 0; i < 2; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }

  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }

  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }

  void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
  void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
  void boolean(bool v) { u8(v ? 1u : 0u); }

  /// Writes a length-prefixed string. Refuses to write beyond max_string_bytes so
  /// oversized payloads fail at the producer rather than at the consumer.
  bool text(std::string_view v) {
    if (v.size() > limits::max_string_bytes) return false;
    u16(static_cast<std::uint16_t>(v.size()));
    for (const char c : v) u8(static_cast<std::uint8_t>(c));
    return true;
  }

  bool blob(std::span<const std::byte> v, std::size_t max_bytes) {
    if (v.size() > max_bytes) return false;
    u32(static_cast<std::uint32_t>(v.size()));
    buffer_.insert(buffer_.end(), v.begin(), v.end());
    return true;
  }

  void raw(std::span<const std::byte> v) { buffer_.insert(buffer_.end(), v.begin(), v.end()); }

  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] const ByteBuffer& buffer() const noexcept { return buffer_; }
  [[nodiscard]] ByteBuffer take() && { return std::move(buffer_); }

 private:
  ByteBuffer buffer_;
};

class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool exhausted() const noexcept { return offset_ == data_.size(); }

  bool u8(std::uint8_t& out) {
    if (remaining() < 1) return false;
    out = static_cast<std::uint8_t>(data_[offset_]);
    offset_ += 1;
    return true;
  }

  bool u16(std::uint16_t& out) {
    if (remaining() < 2) return false;
    out = 0;
    for (int i = 0; i < 2; ++i) out |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(data_[offset_ + static_cast<std::size_t>(i)])) << (8 * i);
    offset_ += 2;
    return true;
  }

  bool u32(std::uint32_t& out) {
    if (remaining() < 4) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) out |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[offset_ + static_cast<std::size_t>(i)])) << (8 * i);
    offset_ += 4;
    return true;
  }

  bool u64(std::uint64_t& out) {
    if (remaining() < 8) return false;
    out = 0;
    for (int i = 0; i < 8; ++i) out |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data_[offset_ + static_cast<std::size_t>(i)])) << (8 * i);
    offset_ += 8;
    return true;
  }

  bool i64(std::int64_t& out) {
    std::uint64_t v = 0;
    if (!u64(v)) return false;
    out = static_cast<std::int64_t>(v);
    return true;
  }

  bool boolean(bool& out) {
    std::uint8_t v = 0;
    if (!u8(v)) return false;
    if (v > 1) return false;
    out = (v == 1);
    return true;
  }

  bool text(std::string& out) {
    std::uint16_t len = 0;
    if (!u16(len)) return false;
    if (len > limits::max_string_bytes) return false;
    if (remaining() < len) return false;
    out.assign(reinterpret_cast<const char*>(data_.data() + offset_), len);
    offset_ += len;
    return true;
  }

  bool blob(ByteBuffer& out, std::size_t max_bytes) {
    std::uint32_t len = 0;
    if (!u32(len)) return false;
    if (len > max_bytes) return false;
    if (remaining() < len) return false;
    out.assign(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
               data_.begin() + static_cast<std::ptrdiff_t>(offset_ + len));
    offset_ += len;
    return true;
  }

  bool skip(std::size_t n) {
    if (remaining() < n) return false;
    offset_ += n;
    return true;
  }

 private:
  std::span<const std::byte> data_;
  std::size_t offset_ = 0;
};

}  // namespace naf

#endif  // NAF_CORE_BYTES_HPP

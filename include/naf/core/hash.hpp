// Network Admission Fabric - deterministic content fingerprinting.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_CORE_HASH_HPP
#define NAF_CORE_HASH_HPP

#include <cstdint>
#include <string_view>

namespace naf {

/// 64-bit FNV-1a. Used only for identity fingerprints and canonical digests,
/// never for secrecy. The digest must be stable across platforms, compilers and
/// process restarts, so the algorithm is fixed here rather than delegated to a
/// standard library implementation whose output is unspecified.
class Fingerprint {
 public:
  Fingerprint() = default;

  void u8(std::uint8_t v) noexcept { byte(v); }

  void byte(std::uint8_t v) noexcept {
    value_ ^= static_cast<std::uint64_t>(v);
    value_ *= 0x100000001B3ull;
  }

  void u16(std::uint16_t v) noexcept {
    byte(static_cast<std::uint8_t>(v & 0xFFu));
    byte(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
  }

  void u32(std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) byte(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }

  void u64(std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) byte(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }

  void i64(std::int64_t v) noexcept { u64(static_cast<std::uint64_t>(v)); }

  void boolean(bool v) noexcept { byte(v ? 1u : 0u); }

  void text(std::string_view v) noexcept {
    u64(static_cast<std::uint64_t>(v.size()));
    for (const char c : v) byte(static_cast<std::uint8_t>(c));
  }

  void raw(const void* data, std::size_t size) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) byte(p[i]);
  }

  void separator(std::uint8_t tag) noexcept {
    // Domain separation byte keeps concatenated field streams distinguishable.
    byte(tag);
    byte(0x5Au);
  }

  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

 private:
  std::uint64_t value_ = 0xCBF29CE484222325ull;
};

inline std::uint64_t fingerprint_u64(std::uint64_t v) noexcept {
  Fingerprint f;
  f.u64(v);
  return f.value();
}

}  // namespace naf

#endif  // NAF_CORE_HASH_HPP

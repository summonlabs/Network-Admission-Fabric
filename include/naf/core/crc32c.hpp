// Network Admission Fabric - CRC-32C (Castagnoli) integrity.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_CORE_CRC32C_HPP
#define NAF_CORE_CRC32C_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace naf {

namespace detail {

inline constexpr std::size_t crc32c_table_size = 256;

struct Crc32cTable {
  std::uint32_t entries[crc32c_table_size] = {};

  constexpr Crc32cTable() noexcept {
    for (std::size_t i = 0; i < crc32c_table_size; ++i) {
      std::uint32_t crc = static_cast<std::uint32_t>(i);
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) ? ((crc >> 1) ^ 0x82F63B78u) : (crc >> 1);
      }
      entries[i] = crc;
    }
  }
};

inline constexpr Crc32cTable crc32c_table{};

}  // namespace detail

/// Software CRC-32C. Deterministic across platforms and compilers; used for
/// journal frame integrity and framed transport integrity.
/// Indexes the lookup table with an explicitly masked, explicitly sized value so
/// the bound is provable to a static analyser as well as to a reader.
inline std::uint32_t crc32c_step(std::uint32_t crc, std::uint8_t byte) noexcept {
  const std::uint32_t mixed = (crc ^ static_cast<std::uint32_t>(byte)) & 0xFFu;
  const std::size_t index = static_cast<std::size_t>(mixed) % detail::crc32c_table_size;
  return detail::crc32c_table.entries[index] ^ (crc >> 8);
}

inline std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const std::byte b : data) {
    crc = crc32c_step(crc, static_cast<std::uint8_t>(b));
  }
  return crc ^ 0xFFFFFFFFu;
}

inline std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::byte> data) noexcept {
  std::uint32_t crc = seed ^ 0xFFFFFFFFu;
  for (const std::byte b : data) {
    crc = crc32c_step(crc, static_cast<std::uint8_t>(b));
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace naf

#endif  // NAF_CORE_CRC32C_HPP

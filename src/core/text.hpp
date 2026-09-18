// Network Admission Fabric - internal decimal rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Not installed. Shared by every diagnostic renderer so the digit loop is
// written once and is obviously bounded.
#ifndef NAF_SRC_CORE_TEXT_HPP
#define NAF_SRC_CORE_TEXT_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace naf::text {

/// Appends the decimal rendering of value. The digit buffer is sized for the
/// widest possible uint64 (20 digits) and the loop is bounded by that size, so
/// there is no path on which an index can fall outside it.
inline void append_u64(std::string& out, std::uint64_t value) {
  char digits[20];
  std::size_t count = 0;
  do {
    digits[count] = static_cast<char>('0' + static_cast<int>(value % 10u));
    ++count;
    value /= 10u;
  } while (value != 0 && count < sizeof(digits));
  while (count != 0) {
    --count;
    out.push_back(digits[count]);
  }
}

}  // namespace naf::text

#endif  // NAF_SRC_CORE_TEXT_HPP

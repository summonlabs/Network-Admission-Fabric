// Network Admission Fabric - checked integer arithmetic.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// All capacity accounting flows through these helpers. Nothing in the runtime
// performs raw 64-bit addition, subtraction, multiplication or division on
// externally influenced quantities.
#ifndef NAF_CORE_CHECKED_HPP
#define NAF_CORE_CHECKED_HPP

#include <cstdint>
#include <limits>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace naf {

/// Portable 128-bit unsigned value used for intermediate products.
struct U128 {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
};

constexpr U128 mul_u64_portable(std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t a0 = a & 0xFFFFFFFFu;
  const std::uint64_t a1 = a >> 32;
  const std::uint64_t b0 = b & 0xFFFFFFFFu;
  const std::uint64_t b1 = b >> 32;

  const std::uint64_t p00 = a0 * b0;
  const std::uint64_t p01 = a0 * b1;
  const std::uint64_t p10 = a1 * b0;
  const std::uint64_t p11 = a1 * b1;

  const std::uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);

  U128 r;
  r.lo = (p00 & 0xFFFFFFFFu) | (mid << 32);
  r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
  return r;
}

/// Multiplies two 64-bit values into a 128-bit result without loss.
inline U128 mul_u64(std::uint64_t a, std::uint64_t b) noexcept {
#if defined(_MSC_VER) && defined(_M_X64)
  U128 r;
  r.lo = _umul128(a, b, &r.hi);
  return r;
#elif defined(__SIZEOF_INT128__)
  const unsigned __int128 p = static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
  U128 r;
  r.lo = static_cast<std::uint64_t>(p);
  r.hi = static_cast<std::uint64_t>(p >> 64);
  return r;
#else
  return mul_u64_portable(a, b);
#endif
}

/// Portable 128-by-64 division. Returns false when d == 0 or when the quotient
/// does not fit in 64 bits.
inline bool divmod_u128_portable(U128 v, std::uint64_t d, std::uint64_t& q, std::uint64_t& r) noexcept {
  if (d == 0) return false;
  if (v.hi >= d) return false;
  std::uint64_t rem = v.hi;
  std::uint64_t quot = 0;
  for (int i = 63; i >= 0; --i) {
    const std::uint64_t bit = (v.lo >> i) & 1u;
    const std::uint64_t carry = rem >> 63;
    rem = (rem << 1) | bit;
    if (carry != 0 || rem >= d) {
      rem -= d;
      quot |= (std::uint64_t{1} << i);
    }
  }
  q = quot;
  r = rem;
  return true;
}

/// Divides (hi:lo) by d. Returns false on zero divisor or 64-bit quotient overflow.
inline bool divmod_u128(U128 v, std::uint64_t d, std::uint64_t& q, std::uint64_t& r) noexcept {
  if (d == 0) return false;
  if (v.hi >= d) return false;
#if defined(_MSC_VER) && defined(_M_X64)
  std::uint64_t remainder = 0;
  const std::uint64_t quotient = _udiv128(v.hi, v.lo, d, &remainder);
  q = quotient;
  r = remainder;
  return true;
#elif defined(__SIZEOF_INT128__)
  const unsigned __int128 n = (static_cast<unsigned __int128>(v.hi) << 64) | v.lo;
  q = static_cast<std::uint64_t>(n / d);
  r = static_cast<std::uint64_t>(n % d);
  return true;
#else
  return divmod_u128_portable(v, d, q, r);
#endif
}

/// Computes floor(a * num / den). Returns false when den == 0 or the result
/// does not fit in 64 bits (the caller must treat that as a refusal, not a wrap).
inline bool mul_div_floor(std::uint64_t a, std::uint64_t num, std::uint64_t den, std::uint64_t& out) noexcept {
  if (den == 0) return false;
  if (a == 0 || num == 0) {
    out = 0;
    return true;
  }
  const U128 p = mul_u64(a, num);
  std::uint64_t q = 0;
  std::uint64_t r = 0;
  if (!divmod_u128(p, den, q, r)) return false;
  out = q;
  return true;
}

/// Computes ceil(a * num / den). Returns false on the same conditions.
inline bool mul_div_ceil(std::uint64_t a, std::uint64_t num, std::uint64_t den, std::uint64_t& out) noexcept {
  if (den == 0) return false;
  if (a == 0 || num == 0) {
    out = 0;
    return true;
  }
  const U128 p = mul_u64(a, num);
  std::uint64_t q = 0;
  std::uint64_t r = 0;
  if (!divmod_u128(p, den, q, r)) return false;
  if (r != 0) {
    if (q == std::numeric_limits<std::uint64_t>::max()) return false;
    ++q;
  }
  out = q;
  return true;
}

inline bool add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) return false;
  out = a + b;
  return true;
}

inline bool sub_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (b > a) return false;
  out = a - b;
  return true;
}

inline std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) noexcept {
  std::uint64_t out = 0;
  if (!add_u64(a, b, out)) return std::numeric_limits<std::uint64_t>::max();
  return out;
}

/// Saturating subtraction that floors at zero. Never produces a negative value.
inline std::uint64_t sat_sub(std::uint64_t a, std::uint64_t b) noexcept {
  return b > a ? std::uint64_t{0} : a - b;
}

inline std::uint64_t sat_mul_div_floor(std::uint64_t a, std::uint64_t num, std::uint64_t den) noexcept {
  std::uint64_t out = 0;
  if (!mul_div_floor(a, num, den, out)) return std::numeric_limits<std::uint64_t>::max();
  return out;
}

/// Running sum with an explicit overflow latch. Accounting code uses this so an
/// overflow can never silently wrap into a plausible-looking total.
class SumLatch {
 public:
  void add(std::uint64_t v) noexcept {
    if (overflow_) return;
    if (!add_u64(total_, v, total_)) overflow_ = true;
  }
  [[nodiscard]] bool overflowed() const noexcept { return overflow_; }
  [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
  [[nodiscard]] std::uint64_t saturated_total() const noexcept {
    return overflow_ ? std::numeric_limits<std::uint64_t>::max() : total_;
  }

 private:
  std::uint64_t total_ = 0;
  bool overflow_ = false;
};

}  // namespace naf

#endif  // NAF_CORE_CHECKED_HPP

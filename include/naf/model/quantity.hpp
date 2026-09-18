// Network Admission Fabric - strongly typed quantities.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_MODEL_QUANTITY_HPP
#define NAF_MODEL_QUANTITY_HPP

#include <compare>
#include <cstdint>
#include <string>

#include "naf/core/checked.hpp"

namespace naf {

/// Strongly typed non-negative quantity. The tag prevents adding a latency to a
/// rate, or comparing a headroom against a demand minimum, by accident.
template <class Tag>
class Quantity {
 public:
  using tag_type = Tag;
  using value_type = std::uint64_t;

  constexpr Quantity() noexcept = default;
  constexpr explicit Quantity(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Quantity from_value(std::uint64_t value) noexcept { return Quantity{value}; }
  [[nodiscard]] static constexpr Quantity zero() noexcept { return Quantity{}; }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  [[nodiscard]] constexpr Quantity saturating_add(Quantity other) const noexcept {
    return Quantity{sat_add(value_, other.value_)};
  }
  [[nodiscard]] constexpr Quantity saturating_sub(Quantity other) const noexcept {
    return Quantity{sat_sub(value_, other.value_)};
  }
  /// Floored permille share of this quantity. parts must be 0..1000.
  [[nodiscard]] Quantity permille(std::uint32_t parts) const noexcept {
    return Quantity{sat_mul_div_floor(value_, parts, 1000u)};
  }
  [[nodiscard]] Quantity clamped_to(Quantity upper) const noexcept {
    return Quantity{value_ < upper.value_ ? value_ : upper.value_};
  }
  [[nodiscard]] Quantity floored_at(Quantity lower) const noexcept {
    return Quantity{value_ > lower.value_ ? value_ : lower.value_};
  }

  friend constexpr bool operator==(Quantity, Quantity) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(Quantity, Quantity) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

struct RateTag {};
struct LatencyTag {};
struct DurationTag {};

/// Line rate in bits per second. The only bandwidth unit in the runtime.
using Rate = Quantity<RateTag>;
/// Latency budget in microseconds.
using Latency = Quantity<LatencyTag>;
/// Logical duration in nanoseconds.
using Duration = Quantity<DurationTag>;

[[nodiscard]] std::string render_rate(Rate rate);
[[nodiscard]] std::string render_latency(Latency latency);

/// Inclusive rate window. minimum is the contractual floor below which the
/// demand is useless; maximum is the ceiling the claimant will never exceed.
struct RateBounds {
  Rate minimum{};
  Rate desired{};
  Rate maximum{};

  friend constexpr bool operator==(const RateBounds&, const RateBounds&) noexcept = default;

  /// Coherent when minimum <= desired <= maximum and the ceiling is non-zero.
  [[nodiscard]] bool is_coherent() const noexcept {
    return maximum.value() != 0 && minimum <= desired && desired <= maximum;
  }
  [[nodiscard]] bool is_degenerate() const noexcept { return maximum.value() == 0; }
};

}  // namespace naf

#endif  // NAF_MODEL_QUANTITY_HPP

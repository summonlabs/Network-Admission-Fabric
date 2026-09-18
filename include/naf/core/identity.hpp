// Network Admission Fabric - strongly typed identities, generations, epochs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Cross-assigning one identity kind to another is a compile error: every id is
// a distinct type produced from a distinct tag. Value 0 is reserved everywhere
// and means UNKNOWN/NONE.
#ifndef NAF_CORE_IDENTITY_HPP
#define NAF_CORE_IDENTITY_HPP

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "naf/core/limits.hpp"

namespace naf {

/// Tagged 64-bit identity. Never implicitly convertible to or from an integer.
template <class Tag>
class StrongId {
 public:
  using tag_type = Tag;
  using value_type = std::uint64_t;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr StrongId from_value(std::uint64_t value) noexcept { return StrongId{value}; }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_unknown() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool is_known() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(StrongId, StrongId) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

struct AdmissionRequestIdTag {};
struct AdmissionDecisionIdTag {};
struct DemandIdTag {};
struct ResourceIdTag {};
struct PathIdTag {};
struct CapacitySnapshotIdTag {};
struct ReservationSnapshotIdTag {};
struct ReservationIdTag {};
struct QoSClassIdTag {};
struct PriorityClassIdTag {};
struct PolicyIdTag {};
struct PublisherIdTag {};
struct BootIdTag {};
struct SessionNonceTag {};
struct AttemptIdTag {};
struct FairnessGroupIdTag {};
struct AuditSequenceTag {};
struct TenantIdTag {};
struct TraceIdTag {};

using AdmissionRequestId = StrongId<AdmissionRequestIdTag>;
using AdmissionDecisionId = StrongId<AdmissionDecisionIdTag>;
using DemandId = StrongId<DemandIdTag>;
using ResourceId = StrongId<ResourceIdTag>;
using PathId = StrongId<PathIdTag>;
using CapacitySnapshotId = StrongId<CapacitySnapshotIdTag>;
using ReservationSnapshotId = StrongId<ReservationSnapshotIdTag>;
using ReservationId = StrongId<ReservationIdTag>;
using QoSClassId = StrongId<QoSClassIdTag>;
using PriorityClassId = StrongId<PriorityClassIdTag>;
using PolicyId = StrongId<PolicyIdTag>;
using PublisherId = StrongId<PublisherIdTag>;
using BootId = StrongId<BootIdTag>;
using SessionNonce = StrongId<SessionNonceTag>;
using AttemptId = StrongId<AttemptIdTag>;
using FairnessGroupId = StrongId<FairnessGroupIdTag>;
using AuditSequence = StrongId<AuditSequenceTag>;
using TenantId = StrongId<TenantIdTag>;
using TraceId = StrongId<TraceIdTag>;

/// Monotonic authority revision. Generation 0 means UNKNOWN; a generation is
/// only ever compared for exact equality (bind) or strict ordering (freshness).
class Generation {
 public:
  constexpr Generation() noexcept = default;
  constexpr explicit Generation(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Generation unknown() noexcept { return Generation{}; }
  [[nodiscard]] static constexpr Generation first() noexcept { return Generation{1}; }
  [[nodiscard]] static constexpr Generation from_value(std::uint64_t value) noexcept { return Generation{value}; }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_unknown() const noexcept { return value_ == limits::unknown_generation; }
  [[nodiscard]] constexpr bool is_max() const noexcept { return value_ == limits::max_generation; }
  [[nodiscard]] constexpr bool is_well_formed() const noexcept {
    return value_ != limits::unknown_generation && value_ != limits::max_generation;
  }

  /// Advances the generation. Returns false at the maximum value instead of wrapping.
  [[nodiscard]] bool advance() noexcept {
    if (value_ == limits::max_generation) return false;
    ++value_;
    return true;
  }

  [[nodiscard]] constexpr Generation next() const noexcept {
    return value_ == limits::max_generation ? Generation{limits::max_generation} : Generation{value_ + 1};
  }

  friend constexpr bool operator==(Generation, Generation) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(Generation, Generation) noexcept = default;

 private:
  std::uint64_t value_ = limits::unknown_generation;
};

/// Fabric-wide fencing epoch. Advances on every coordinator boot; work stamped
/// with an older epoch is refused rather than reinterpreted.
class FabricEpoch {
 public:
  constexpr FabricEpoch() noexcept = default;
  constexpr explicit FabricEpoch(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr FabricEpoch none() noexcept { return FabricEpoch{}; }
  [[nodiscard]] static constexpr FabricEpoch first() noexcept { return FabricEpoch{limits::first_epoch}; }
  [[nodiscard]] static constexpr FabricEpoch from_value(std::uint64_t value) noexcept { return FabricEpoch{value}; }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_none() const noexcept { return value_ == limits::unknown_epoch; }
  [[nodiscard]] constexpr bool is_well_formed() const noexcept { return value_ != limits::unknown_epoch; }

  [[nodiscard]] bool advance() noexcept {
    if (value_ == limits::max_generation) return false;
    ++value_;
    return true;
  }

  friend constexpr bool operator==(FabricEpoch, FabricEpoch) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(FabricEpoch, FabricEpoch) noexcept = default;

 private:
  std::uint64_t value_ = limits::unknown_epoch;
};

/// Identity of a single coordinator process incarnation. Never persisted as live
/// authority; it exists to detect that a claimant is talking to a restarted peer.
class CoordinatorIncarnation {
 public:
  constexpr CoordinatorIncarnation() noexcept = default;
  constexpr explicit CoordinatorIncarnation(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr CoordinatorIncarnation from_value(std::uint64_t value) noexcept {
    return CoordinatorIncarnation{value};
  }
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_unknown() const noexcept { return value_ == 0; }

  friend constexpr bool operator==(CoordinatorIncarnation, CoordinatorIncarnation) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(CoordinatorIncarnation, CoordinatorIncarnation) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

/// Where a request originated. In-process traffic is not exempt from authority
/// binding, but it is exempt from transport-level fencing.
enum class OriginKind : std::uint8_t {
  InProcess = 0,
  Claimant = 1,
  Operator = 2,
  Recovery = 3,
  Replay = 4,
};

[[nodiscard]] std::string_view to_string(OriginKind kind) noexcept;

/// Full provenance stamp carried by every externally supplied input.
struct Provenance {
  PublisherId publisher{};
  BootId boot{};
  CoordinatorIncarnation incarnation{};
  FabricEpoch epoch{};
  AttemptId attempt{};
  TraceId trace{};
  std::uint64_t sequence = 0;
  OriginKind origin = OriginKind::InProcess;

  friend constexpr bool operator==(const Provenance&, const Provenance&) noexcept = default;
};

/// Compact printable rendering used by tools and diagnostics.
[[nodiscard]] std::string render_provenance(const Provenance& provenance);

}  // namespace naf

#endif  // NAF_CORE_IDENTITY_HPP

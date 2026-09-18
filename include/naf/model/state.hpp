// Network Admission Fabric - authoritative fabric state inputs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every structure here is an *input fact* supplied by an adjacent runtime that
// owns that truth. Admission never invents, repairs or extrapolates any of it.
// UNKNOWN is a first-class, non-authorizing value.
#ifndef NAF_MODEL_STATE_HPP
#define NAF_MODEL_STATE_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "naf/core/identity.hpp"
#include "naf/core/status.hpp"
#include "naf/model/quantity.hpp"

namespace naf {

/// Whether an authority has actually published an observation.
enum class EvidenceState : std::uint8_t {
  Unknown = 0,
  Known = 1,
};

enum class PathState : std::uint8_t {
  Unknown = 0,
  Up = 1,
  Down = 2,
};

[[nodiscard]] std::string_view to_string(EvidenceState state) noexcept;
[[nodiscard]] std::string_view to_string(PathState state) noexcept;

/// Usable capacity of one governed resource, as published by the capacity
/// authority. The usable amount already excludes capacity the resource keeps
/// for its own purposes; mandatory headroom is headroom the capacity authority
/// itself requires admission to hold back.
struct ResourceCapacity {
  ResourceId resource{};
  Generation generation{};
  EvidenceState evidence = EvidenceState::Unknown;
  Rate usable{};
  Rate mandatory_headroom{};

  friend bool operator==(const ResourceCapacity&, const ResourceCapacity&) = default;
};

/// One immutable capacity observation for a whole resource set.
struct CapacitySnapshot {
  CapacitySnapshotId snapshot{};
  Generation generation{};
  FabricEpoch epoch{};
  std::uint64_t observed_tick = 0;
  /// Ordered by ResourceId, strictly increasing, no duplicates.
  std::vector<ResourceCapacity> resources{};

  friend bool operator==(const CapacitySnapshot&, const CapacitySnapshot&) = default;
};

/// Capacity held by a reservation that admission must preserve. Admission never
/// creates, extends, shrinks or releases one of these; it only respects it.
struct ProtectedObligation {
  ReservationId reservation{};
  Generation reservation_generation{};
  ResourceId resource{};
  Rate reserved{};
  PriorityClassId priority{};
  /// Logical tick at which the reservation authority will release this hold.
  /// 0 means there is no scheduled release.
  std::uint64_t release_tick = 0;
  /// True when the reservation authority declares the hold may never be
  /// consumed. An inviolable hold can never be preempted by admission.
  bool inviolable = true;

  friend bool operator==(const ProtectedObligation&, const ProtectedObligation&) = default;
};

/// One immutable obligation observation.
struct ReservationSnapshot {
  ReservationSnapshotId snapshot{};
  Generation generation{};
  FabricEpoch epoch{};
  std::uint64_t observed_tick = 0;
  /// Ordered by (resource, reservation), strictly increasing, no duplicates.
  std::vector<ProtectedObligation> obligations{};

  friend bool operator==(const ReservationSnapshot&, const ReservationSnapshot&) = default;
};

/// Admission-relevant facts about a path. The path authority owns legality,
/// computation and lifecycle; admission consumes only these attributes.
struct PathAdmissionFact {
  PathId path{};
  Generation path_authority_generation{};
  PathState state = PathState::Unknown;
  Rate path_usable_capacity{};
  Latency path_latency{};
  /// Authoritative traversal set. Ordered by ResourceId, strictly increasing.
  std::vector<ResourceId> resources{};

  friend bool operator==(const PathAdmissionFact&, const PathAdmissionFact&) = default;
};

struct PathCatalog {
  Generation path_authority_generation{};
  FabricEpoch epoch{};
  std::uint64_t observed_tick = 0;
  /// Ordered by PathId, strictly increasing.
  std::vector<PathAdmissionFact> paths{};

  friend bool operator==(const PathCatalog&, const PathCatalog&) = default;
};

struct QoSClassFact {
  QoSClassId qos{};
  Generation generation{};
  Rate minimum_rate{};
  Rate maximum_rate{};
  Latency maximum_latency{};
  bool degradation_allowed = true;

  friend bool operator==(const QoSClassFact&, const QoSClassFact&) = default;
};

struct QoSClassCatalog {
  Generation generation{};
  FabricEpoch epoch{};
  std::uint64_t observed_tick = 0;
  std::vector<QoSClassFact> classes{};

  friend bool operator==(const QoSClassCatalog&, const QoSClassCatalog&) = default;
};

struct PriorityClassFact {
  PriorityClassId priority{};
  Generation generation{};
  /// Higher rank means a stronger obligation. Ranks must be unique.
  std::uint32_t rank = 0;
  /// True when traffic in this class may be displaced by stronger classes.
  bool preemptible = false;

  friend bool operator==(const PriorityClassFact&, const PriorityClassFact&) = default;
};

struct PriorityClassCatalog {
  Generation generation{};
  FabricEpoch epoch{};
  std::uint64_t observed_tick = 0;
  std::vector<PriorityClassFact> classes{};

  friend bool operator==(const PriorityClassCatalog&, const PriorityClassCatalog&) = default;
};

// ---------------------------------------------------------------------------
// Validation. Each snapshot is fully validated before it can become current
// authority. Invalid input is refused, never repaired.
// ---------------------------------------------------------------------------

[[nodiscard]] Status validate(const CapacitySnapshot& snapshot);
[[nodiscard]] Status validate(const ReservationSnapshot& snapshot);
[[nodiscard]] Status validate(const PathCatalog& catalog);
[[nodiscard]] Status validate(const QoSClassCatalog& catalog);
[[nodiscard]] Status validate(const PriorityClassCatalog& catalog);

/// Per-resource totals used by accounting and by the explanation surface.
struct ResourceTotals {
  Rate usable{};
  Rate headroom{};
  Rate obligations{};
  Rate admitted{};
};

}  // namespace naf

#endif  // NAF_MODEL_STATE_HPP

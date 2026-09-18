// Network Admission Fabric - admission policy.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Policy is owned and published by the policy authority. Admission evaluates it
// deterministically; it never mutates or derives policy.
#ifndef NAF_MODEL_POLICY_HPP
#define NAF_MODEL_POLICY_HPP

#include <cstdint>
#include <vector>

#include "naf/core/identity.hpp"
#include "naf/core/limits.hpp"
#include "naf/core/status.hpp"
#include "naf/model/quantity.hpp"

namespace naf {

enum class ContentionAction : std::uint8_t {
  Defer = 0,
  Reject = 1,
};

[[nodiscard]] std::string_view to_string(ContentionAction action) noexcept;

struct AdmissionPolicy {
  PolicyId id{};
  Generation generation{};
  FabricEpoch epoch{};
  std::uint64_t observed_tick = 0;

  /// Additional headroom admission must hold back, expressed in permille of the
  /// resource usable capacity, floored.
  std::uint32_t headroom_permille = 0;
  /// Absolute headroom floor. The effective headroom is the larger of the
  /// capacity authority mandatory headroom, the percentage and this floor.
  Rate headroom_floor{};

  /// When false, a demand that cannot be served at its desired rate is refused
  /// rather than served at its minimum.
  bool allow_degraded = true;
  /// When false, a demand declaring itself preemptible is refused.
  bool allow_preemptible_admission = true;
  /// When true, every request must name at least one authorized path candidate
  /// and admission must select one.
  bool require_path_binding = false;
  /// When true, every request must reference at least one existing reservation.
  bool require_reservation_reference = false;
  /// A policy may widen or narrow the path substitution right of the claimant.
  bool allow_path_substitution = true;

  ContentionAction on_contention = ContentionAction::Reject;

  /// Empty means every class known to the catalog is allowed.
  std::vector<QoSClassId> allowed_qos{};
  /// Empty means every class known to the catalog is allowed.
  std::vector<PriorityClassId> allowed_priorities{};

  /// Bounds on the explanation surface so a hostile request cannot force an
  /// unbounded decision payload.
  std::uint32_t max_explanation_constraints = 16;
  std::uint32_t max_explanation_bytes = 4096;
  /// Bounds on the rendered effective-capacity summary.
  std::uint32_t max_effective_resources = 16;

  /// Ticks a deferred demand may wait before it is refused instead.
  std::uint64_t defer_horizon_ticks = 0;

  friend bool operator==(const AdmissionPolicy&, const AdmissionPolicy&) = default;
};

[[nodiscard]] Status validate(const AdmissionPolicy& policy);

}  // namespace naf

#endif  // NAF_MODEL_POLICY_HPP

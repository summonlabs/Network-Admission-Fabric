// Network Admission Fabric - the admission request contract.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_MODEL_REQUEST_HPP
#define NAF_MODEL_REQUEST_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "naf/core/identity.hpp"
#include "naf/core/status.hpp"
#include "naf/model/quantity.hpp"
#include "naf/model/state.hpp"

namespace naf {

enum class ContentionPreference : std::uint8_t {
  InheritPolicy = 0,
  Defer = 1,
  Reject = 2,
};

enum class DegradePreference : std::uint8_t {
  InheritPolicy = 0,
  Never = 1,
  DownToMinimum = 2,
};

[[nodiscard]] std::string_view to_string(ContentionPreference preference) noexcept;
[[nodiscard]] std::string_view to_string(DegradePreference preference) noexcept;

/// A resource the demand needs capacity on, together with the generation of the
/// capacity observation the claimant is relying on.
struct ResourceBinding {
  ResourceId resource{};
  Generation generation{};

  friend bool operator==(const ResourceBinding&, const ResourceBinding&) = default;
};

/// An authorized path the claimant is permitted to use, in descending
/// preference order. Admission selects among these; it never computes or
/// invents a path.
struct PathCandidate {
  PathId path{};
  Generation path_authority_generation{};

  friend bool operator==(const PathCandidate&, const PathCandidate&) = default;
};

/// The exact authority revisions the claimant observed before asking. A mismatch
/// against current authority is a stale-input refusal, not a silent refresh.
struct AuthorityExpectation {
  CapacitySnapshotId capacity_snapshot{};
  Generation capacity_generation{};
  ReservationSnapshotId reservation_snapshot{};
  Generation reservation_generation{};
  Generation path_authority_generation{};
  Generation policy_generation{};
  Generation qos_catalog_generation{};
  Generation priority_catalog_generation{};

  friend bool operator==(const AuthorityExpectation&, const AuthorityExpectation&) = default;
};

/// Reference to an existing reservation owned by the reservation runtime.
struct ReservationRef {
  ReservationId reservation{};
  Generation generation{};

  friend bool operator==(const ReservationRef&, const ReservationRef&) = default;
};

/// A fully specified admission request. All identities are explicit so that a
/// decision can be bound to exactly the evidence that justified it.
struct AdmissionRequest {
  AdmissionRequestId request{};
  DemandId demand{};
  Generation demand_generation{};
  AttemptId attempt{};

  RateBounds rate{};
  QoSClassId qos{};
  Generation qos_generation{};
  PriorityClassId priority{};
  Generation priority_generation{};
  Latency maximum_latency{};

  /// Ordered by ResourceId, strictly increasing.
  std::vector<ResourceBinding> resource_bindings{};
  /// Declared preference order. Must contain distinct paths.
  std::vector<PathCandidate> path_candidates{};
  /// When set, the claimant requires exactly this path and no substitution.
  PathId required_path{};
  /// Ordered by reservation id, strictly increasing.
  std::vector<ReservationRef> reservation_refs{};

  FairnessGroupId fairness_group{};
  TenantId tenant{};

  bool preemptible = false;
  /// A claimant asking admission to preempt protected obligations is asking for
  /// something admission does not own; such a request is refused.
  bool requests_obligation_preemption = false;
  bool allow_path_substitution = true;

  ContentionPreference contention = ContentionPreference::InheritPolicy;
  DegradePreference degradation = DegradePreference::InheritPolicy;

  /// How long the claimant is willing to wait for capacity, in logical ticks.
  /// Zero means the claimant cannot wait at all.
  std::uint64_t deadline_ticks = 0;
  /// Logical tick at which the claimant observed the authority vector.
  std::uint64_t effective_tick = 0;
  /// Intended holding interval; reported back so callers know when to
  /// revalidate. Admission stores it but does not schedule on it.
  Duration effective_interval{};

  /// The exact authority revisions the claimant observed. A mismatch against
  /// current authority is a stale-input refusal, not a silent refresh.
  AuthorityExpectation expected{};

  Provenance provenance{};

  friend bool operator==(const AdmissionRequest&, const AdmissionRequest&) = default;
};

/// Structural validation. Returns Status::ok() when the request is well formed.
/// Every failure is a contract violation, never a capacity statement.
[[nodiscard]] Status validate(const AdmissionRequest& request);

/// Stable content fingerprint over every decision-relevant field. Two requests
/// with the same fingerprint are interchangeable; a reused attempt identity with
/// a different fingerprint is a conflict.
[[nodiscard]] std::uint64_t fingerprint(const AdmissionRequest& request);

/// One-line structural summary used by diagnostics and tests.
[[nodiscard]] std::string render_request(const AdmissionRequest& request);

}  // namespace naf

#endif  // NAF_MODEL_REQUEST_HPP

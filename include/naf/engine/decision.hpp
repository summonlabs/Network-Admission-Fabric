// Network Admission Fabric - admission decisions and explanations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_ENGINE_DECISION_HPP
#define NAF_ENGINE_DECISION_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "naf/authority/authority.hpp"
#include "naf/core/identity.hpp"
#include "naf/core/limits.hpp"
#include "naf/model/quantity.hpp"

namespace naf {

/// The complete admission outcome space. Nothing outside this set is produced.
enum class AdmissionOutcome : std::uint8_t {
  Admit = 0,
  AdmitDegraded = 1,
  Defer = 2,
  RejectCapacity = 3,
  RejectPolicy = 4,
  RejectObligation = 5,
  RejectPath = 6,
  RejectQoS = 7,
  StaleInput = 8,
  ConflictingInput = 9,
  FencedClaimant = 10,
};

[[nodiscard]] std::string_view to_string(AdmissionOutcome outcome) noexcept;
/// True for outcomes that consume capacity and therefore mutate the ledger.
[[nodiscard]] constexpr bool is_admitting(AdmissionOutcome outcome) noexcept {
  return outcome == AdmissionOutcome::Admit || outcome == AdmissionOutcome::AdmitDegraded;
}

enum class BindingConstraintKind : std::uint8_t {
  None = 0,
  RequestContract,
  Fence,
  IdentityConflict,
  UnknownEvidence,
  StaleGeneration,
  Policy,
  QoSClass,
  PriorityClass,
  Path,
  Capacity,
  Headroom,
  Obligation,
  AdmittedLoad,
  Degradation,
  Durability,
};

[[nodiscard]] std::string_view to_string(BindingConstraintKind kind) noexcept;

/// One concrete reason a decision came out the way it did, with the numbers and
/// authority revisions that produced it.
struct BindingConstraint {
  BindingConstraintKind kind = BindingConstraintKind::None;
  std::string subject{};
  std::uint64_t subject_id = 0;
  Rate required{};
  Rate available{};
  Generation observed{};
  Generation authoritative{};
  std::string detail{};

  friend bool operator==(const BindingConstraint&, const BindingConstraint&) = default;
};

/// Per-resource arithmetic exposed with the decision so that accounting closure
/// can be checked by the caller rather than trusted.
struct EffectiveResource {
  ResourceId resource{};
  Generation generation{};
  Rate usable{};
  Rate headroom{};
  Rate obligations{};
  Rate admitted{};
  Rate available_for_new{};

  friend bool operator==(const EffectiveResource&, const EffectiveResource&) = default;
};

/// Bounded explanation surface. Adding a constraint beyond either bound records
/// the omission instead of growing without limit.
class Explanation {
 public:
  void configure(std::size_t max_constraints, std::size_t max_bytes) noexcept;
  /// Returns false when the constraint was dropped because a bound was reached.
  bool add(BindingConstraint constraint);

  [[nodiscard]] const std::vector<BindingConstraint>& constraints() const noexcept { return constraints_; }
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }
  [[nodiscard]] std::uint32_t omitted() const noexcept { return omitted_; }
  [[nodiscard]] std::size_t byte_size() const noexcept { return bytes_; }
  [[nodiscard]] std::size_t max_bytes() const noexcept { return max_bytes_; }
  [[nodiscard]] std::size_t max_constraints() const noexcept { return max_constraints_; }

  [[nodiscard]] std::string render() const;
  [[nodiscard]] const BindingConstraint* primary() const noexcept {
    return constraints_.empty() ? nullptr : &constraints_.front();
  }

 private:
  std::vector<BindingConstraint> constraints_{};
  std::size_t max_constraints_ = limits::max_explanation_constraints;
  std::size_t max_bytes_ = limits::max_explanation_bytes;
  std::size_t bytes_ = 0;
  std::uint32_t omitted_ = 0;
  bool truncated_ = false;
};

/// Why a previously admitted demand stopped being valid.
enum class RevocationReason : std::uint8_t {
  NotRevoked = 0,
  OperatorRequest,
  GenerationAdvance,
  CapacityReduction,
  PathRevoked,
  PolicyChange,
  ClaimantDeath,
  CoordinatorRestart,
  ObligationChange,
  Expired,
};

[[nodiscard]] std::string_view to_string(RevocationReason reason) noexcept;

/// A complete, self-describing admission result.
struct AdmissionDecision {
  AdmissionDecisionId decision{};
  /// Monotonic per-coordinator decision sequence; also the durable audit index.
  AuditSequence audit{};
  AdmissionOutcome outcome = AdmissionOutcome::RejectPolicy;

  AdmissionRequestId request{};
  DemandId demand{};
  Generation demand_generation{};
  AttemptId attempt{};
  TraceId trace{};

  FabricEpoch epoch{};
  CoordinatorIncarnation incarnation{};
  std::uint64_t decided_tick = 0;

  PathId selected_path{};
  Generation selected_path_generation{};
  Rate granted{};
  Rate minimum_guaranteed{};
  Rate desired{};
  Rate maximum{};
  Duration holding_interval{};

  /// Capacity generation the grant is bound to. When current authority advances
  /// past this the decision must be revalidated before it can be trusted.
  Generation bound_capacity_generation{};
  bool revalidation_required = false;

  AuthorityVector authority{};
  std::vector<EffectiveResource> effective{};
  bool effective_truncated = false;

  Explanation explanation{};
  /// True when this result is a replay of an earlier identical attempt.
  bool idempotent_replay = false;
  std::uint64_t request_fingerprint = 0;
};

/// State of a decision when re-evaluated against current authority.
enum class RevalidationState : std::uint8_t {
  Fresh = 0,
  Valid = 1,
  Invalidated = 2,
  Revoked = 3,
  Expired = 4,
  Ambiguous = 5,
  Unknown = 6,
};

[[nodiscard]] std::string_view to_string(RevalidationState state) noexcept;

struct RevalidationResult {
  AdmissionDecisionId decision{};
  AdmissionOutcome original_outcome = AdmissionOutcome::RejectPolicy;
  RevalidationState state = RevalidationState::Unknown;
  RevocationReason reason = RevocationReason::NotRevoked;
  Rate granted{};
  std::uint64_t tick = 0;
  bool ledger_released = false;
  AuthorityVector authority{};
  Explanation explanation{};
};

}  // namespace naf

#endif  // NAF_ENGINE_DECISION_HPP

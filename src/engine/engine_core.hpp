// Network Admission Fabric - internal engine core.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Not installed. Declares the shared implementation state and the evaluation
// steps that admission.cpp and engine.cpp both use.
#ifndef NAF_SRC_ENGINE_CORE_HPP
#define NAF_SRC_ENGINE_CORE_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "naf/core/identity.hpp"
#include "naf/core/status.hpp"
#include "naf/durable/journal.hpp"
#include "naf/durable/records.hpp"
#include "naf/engine/decision.hpp"
#include "naf/engine/engine.hpp"
#include "naf/engine/history.hpp"
#include "naf/engine/ledger.hpp"
#include "naf/model/policy.hpp"
#include "naf/model/request.hpp"
#include "naf/model/state.hpp"

namespace naf::detail {

struct SessionRecord {
  SessionNonce nonce{};
  PublisherId publisher{};
  BootId boot{};
};

struct Counters {
  std::uint64_t decisions = 0;
  std::uint64_t admissions = 0;
  std::uint64_t refusals = 0;
  std::uint64_t deferrals = 0;
  std::uint64_t idempotent_replays = 0;
  std::uint64_t conflicts = 0;
  std::uint64_t fences = 0;
  std::uint64_t stale_refusals = 0;
};

/// Per-resource arithmetic performed for one request. Exposed with the decision
/// so accounting closure can be checked by the caller rather than trusted.
struct ResourceView {
  ResourceId resource{};
  Generation generation{};
  Rate usable{};
  Rate headroom{};
  Rate obligations{};
  Rate admitted{};
  Rate available_after_obligations{};
  Rate available_for_new{};
};

/// Everything the engine owns. A single mutex guards all of it. No caller code,
/// no event emission and no unbounded allocation ever happens while it is held,
/// so lock re-entry is impossible by construction.
struct EngineCore {
  mutable std::mutex mutex_{};
  EngineConfig config{};
  FabricEpoch epoch = FabricEpoch::none();
  CoordinatorIncarnation incarnation{};
  std::uint64_t tick = 0;

  std::optional<AdmissionPolicy> policy{};
  std::optional<CapacitySnapshot> capacity{};
  std::optional<ReservationSnapshot> reservations{};
  std::optional<PathCatalog> paths{};
  std::optional<QoSClassCatalog> qos{};
  std::optional<PriorityClassCatalog> priorities{};

  AdmittedLoadLedger ledger{};
  DecisionHistory history{};
  AttemptRegistry attempts{};
  std::map<std::uint64_t, AdmissionDecision> decision_cache{};
  std::map<std::uint64_t, SessionRecord> sessions{};

  std::uint64_t next_session_nonce = 1;
  std::uint64_t next_decision_sequence = 1;
  AuditSequence audit{};
  Counters counters{};

  // Derived from the current reservation snapshot so per-admission accounting
  // does not rescan the whole obligation set. Rebuilt whenever the snapshot is
  // replaced or recovered.
  std::map<std::uint64_t, Rate> obligation_totals_{};
  std::map<std::uint64_t, std::vector<std::size_t>> obligation_indices_{};

  std::unique_ptr<Journal> journal{};
  bool journal_ready = false;
  bool pending_reconciliation = false;
  FaultInjection faults{};
  ReconciliationReport reconciliation{};

  // -- lookups --------------------------------------------------------------
  [[nodiscard]] const ResourceCapacity* find_capacity(ResourceId resource) const;
  [[nodiscard]] const PathAdmissionFact* find_path(PathId path) const;
  [[nodiscard]] const QoSClassFact* find_qos(QoSClassId id) const;
  [[nodiscard]] const PriorityClassFact* find_priority(PriorityClassId id) const;
  void rebuild_obligation_index();
  [[nodiscard]] Rate obligations_for(ResourceId resource) const;
  [[nodiscard]] bool obligation_releases_within(ResourceId resource, std::uint64_t horizon) const;
  [[nodiscard]] Rate headroom_for(const ResourceCapacity& entry) const;
  [[nodiscard]] Rate admitted_on_path(PathId path) const;
  [[nodiscard]] EngineReadiness readiness() const;

  // -- decision plumbing ----------------------------------------------------
  /// Fabric-global authority only: epoch, incarnation, snapshots, policy and
  /// catalogs. Per-resource and per-path references are added once the decision
  /// actually depends on them, so the vector stays bounded by what justified it.
  void fill_authority(AuthorityVector& vector) const;
  /// Adds the authority references for the resources and path this decision
  /// actually evaluated.
  void fill_target_authority(AuthorityVector& vector, const std::vector<ResourceView>& views,
                             PathId path, Generation path_generation) const;
  void configure_explanation(Explanation& explanation) const;
  [[nodiscard]] AdmissionDecision begin_decision(const AdmissionRequest& request,
                                                 const ClaimContext& claim,
                                                 AdmissionOutcome outcome);
  [[nodiscard]] DecisionRecord to_record(const AdmissionDecision& decision) const;
  void remember(const AdmissionDecision& decision);
  void count(const AdmissionDecision& decision);
  void fill_effective(const std::vector<ResourceView>& views, AdmissionDecision& decision) const;

  [[nodiscard]] AdmissionDecision refusal(const AdmissionRequest& request,
                                          const ClaimContext& claim,
                                          AdmissionOutcome outcome,
                                          BindingConstraint constraint);

  Status finish_refusal(AdmissionDecision& decision, const AdmissionRequest& request,
                        bool record_attempt);
  Status finish_admission(AdmissionDecision& decision, const AdmissionRequest& request,
                          const std::vector<std::pair<ResourceId, Rate>>& amounts);

  // -- evaluation steps -----------------------------------------------------
  [[nodiscard]] bool check_fence(const ClaimContext& claim, AdmissionOutcome& outcome,
                                 BindingConstraint& constraint) const;
  [[nodiscard]] bool check_readiness(AdmissionOutcome& outcome, BindingConstraint& constraint) const;
  [[nodiscard]] bool check_expectations(const AdmissionRequest& request, AdmissionOutcome& outcome,
                                        BindingConstraint& constraint) const;
  [[nodiscard]] bool check_policy(const AdmissionRequest& request, AdmissionOutcome& outcome,
                                  BindingConstraint& constraint) const;
  /// Selects an authorized path. When no path can be used, distinguishes a path
  /// refusal from a capacity refusal via capacity_blocked.
  [[nodiscard]] AdmissionOutcome select_path(const AdmissionRequest& request, PathId& selected,
                                             Generation& selected_generation,
                                             std::vector<ResourceId>& traversal,
                                             bool& path_selected, bool& capacity_blocked,
                                             BindingConstraint& constraint) const;
  [[nodiscard]] AdmissionOutcome evaluate_capacity(const AdmissionRequest& request,
                                                   const std::vector<ResourceId>& targets,
                                                   const PathAdmissionFact* selected_path,
                                                   std::vector<ResourceView>& views, Rate& ceiling,
                                                   BindingConstraint& constraint) const;

  // -- durable writers ------------------------------------------------------
  Status write_record(JournalRecordKind kind, const ByteBuffer& payload);
  Status write_boot();
  Status write_policy_record(const AdmissionPolicy& policy);
  Status write_prepare(const PrepareRecord& record);
  Status write_commit(const DecisionRecord& record);
  Status write_refusal(const DecisionRecord& record);
  Status write_release(const ReleaseRecord& record);
  Status write_revocation(const RevocationRecord& record);
  Status write_audit_mark();

  // -- operations -----------------------------------------------------------
  /// Revokes owned admissions, newest first, until admitted load plus protected
  /// obligations plus headroom fits inside authoritative usable capacity.
  Status enforce_accounting_locked(EnforcementTrigger trigger);
  /// Retires history beyond the configured capacity, never dropping a record
  /// whose grant is still live.
  void trim_history();
  /// Rewrites the journal with only the state that must survive a restart.
  Status compact_locked();
  /// Compacts when the journal has grown past its configured frame threshold.
  Status compact_if_needed_locked();
  [[nodiscard]] Expected<AdmissionDecision> admit_locked(const AdmissionRequest& request,
                                                         const ClaimContext& claim);
  [[nodiscard]] Expected<RevalidationResult> revalidate_locked(AdmissionDecisionId decision,
                                                               const ClaimContext& claim);
  [[nodiscard]] Expected<RevalidationResult> revoke_locked(AdmissionDecisionId decision,
                                                           RevocationReason reason,
                                                           const ClaimContext& claim);
  Status release_decision_locked(AdmissionDecisionId decision, RevocationReason reason,
                                 RevalidationState state, Explanation& explanation);
  [[nodiscard]] FabricStatus status_locked() const;
};

/// Shared factory used by tests and tools to build a boot record.
[[nodiscard]] BootRecord make_boot_record(FabricEpoch epoch, CoordinatorIncarnation incarnation);

// A generation identifies an immutable observation. Republishing the same
// generation with different decision-relevant content is identity reuse and is
// refused. The fencing epoch and the observation tick are metadata that this
// coordinator re-stamps, so they are excluded from the comparison.
[[nodiscard]] bool content_equal(const AdmissionPolicy& a, const AdmissionPolicy& b) noexcept;
[[nodiscard]] bool content_equal(const CapacitySnapshot& a, const CapacitySnapshot& b) noexcept;
[[nodiscard]] bool content_equal(const ReservationSnapshot& a, const ReservationSnapshot& b) noexcept;
[[nodiscard]] bool content_equal(const PathCatalog& a, const PathCatalog& b) noexcept;
[[nodiscard]] bool content_equal(const QoSClassCatalog& a, const QoSClassCatalog& b) noexcept;
[[nodiscard]] bool content_equal(const PriorityClassCatalog& a, const PriorityClassCatalog& b) noexcept;

}  // namespace naf::detail

#endif  // NAF_SRC_ENGINE_CORE_HPP

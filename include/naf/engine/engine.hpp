// Network Admission Fabric - the admission engine.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Boundary: this engine owns the yes/defer/no decision for new traffic entering
// governed fabric capacity. It does not own topology truth, path legality or
// computation, route lifecycle, global TE optimisation, reservation lifecycle,
// instantaneous bandwidth arbitration, flow scheduling, path placement, rate
// enforcement, QoS or priority definition, packet scheduling, congestion
// control, telemetry or device programming. Everything outside that boundary
// arrives as an authoritative fact carrying its own generation.
#ifndef NAF_ENGINE_ENGINE_HPP
#define NAF_ENGINE_ENGINE_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "naf/authority/authority.hpp"
#include "naf/core/identity.hpp"
#include "naf/core/limits.hpp"
#include "naf/core/status.hpp"
#include "naf/durable/journal.hpp"
#include "naf/durable/recovery.hpp"
#include "naf/engine/decision.hpp"
#include "naf/engine/history.hpp"
#include "naf/engine/ledger.hpp"
#include "naf/model/policy.hpp"
#include "naf/model/request.hpp"
#include "naf/model/state.hpp"

namespace naf {

namespace detail {
struct EngineCore;
}  // namespace detail

struct EngineConfig {
  FabricEpoch epoch = FabricEpoch::first();
  CoordinatorIncarnation incarnation{};
  std::uint64_t initial_tick = 0;
  std::size_t history_capacity = limits::max_decision_records;
  std::size_t attempt_capacity = limits::max_attempt_records;
  std::size_t ledger_capacity = limits::max_ledger_entries;
  std::size_t session_capacity = limits::max_session_records;
  /// When true, requests carrying OriginKind::InProcess skip session fencing.
  /// They are still bound to authority generations.
  bool accept_in_process_claims = true;
};

/// How a caller proved it is allowed to talk to this coordinator incarnation.
struct ClaimContext {
  OriginKind origin = OriginKind::InProcess;
  PublisherId publisher{};
  BootId boot{};
  SessionNonce session{};
  CoordinatorIncarnation incarnation{};
  FabricEpoch epoch{};
  AttemptId attempt{};
  TraceId trace{};
  std::uint64_t sequence = 0;

  [[nodiscard]] static ClaimContext in_process() noexcept { return ClaimContext{}; }
  friend bool operator==(const ClaimContext&, const ClaimContext&) = default;
};

struct SessionGrant {
  SessionNonce session{};
  CoordinatorIncarnation incarnation{};
  FabricEpoch epoch{};
  PublisherId publisher{};
  BootId boot{};
};

/// Readiness gate. Admission is refused, never guessed, until every authority
/// the decision needs has actually published.
enum class EngineReadiness : std::uint8_t {
  AwaitingPolicy = 0,
  AwaitingCapacity = 1,
  AwaitingReservations = 2,
  AwaitingCatalogs = 3,
  AwaitingPaths = 4,
  WaitingForReconciliation = 5,
  Ready = 6,
};

[[nodiscard]] std::string_view to_string(EngineReadiness readiness) noexcept;

struct FabricStatus {
  EngineReadiness readiness = EngineReadiness::AwaitingPolicy;
  FabricEpoch epoch{};
  CoordinatorIncarnation incarnation{};
  std::uint64_t tick = 0;

  bool has_policy = false;
  bool has_capacity = false;
  bool has_reservations = false;
  bool has_paths = false;
  bool has_qos = false;
  bool has_priority = false;

  Generation policy_generation{};
  Generation capacity_generation{};
  Generation reservation_generation{};
  Generation path_generation{};
  Generation qos_generation{};
  Generation priority_generation{};
  CapacitySnapshotId capacity_snapshot{};
  ReservationSnapshotId reservation_snapshot{};

  AuditSequence audit_sequence{};
  std::uint64_t decisions = 0;
  std::uint64_t admissions = 0;
  std::uint64_t refusals = 0;
  std::uint64_t deferrals = 0;
  std::uint64_t idempotent_replays = 0;
  std::uint64_t conflicts = 0;
  std::uint64_t fences = 0;
  std::uint64_t stale_refusals = 0;

  std::size_t ledger_entries = 0;
  std::size_t live_decisions = 0;
  std::size_t live_sessions = 0;
  Rate total_admitted{};
  std::size_t infeasible_resources = 0;
  bool durable = false;
  bool reconciled = false;
};

/// Result of an accounting-enforcement pass. The pass runs whenever authority
/// that admission's own load depends on changes, and whenever durable load is
/// recovered: admitted load plus protected obligations plus headroom must never
/// exceed authoritative usable capacity, so load that no longer fits is revoked
/// newest first.
struct ReconciliationReport {
  bool performed = false;
  EnforcementTrigger trigger = EnforcementTrigger::None;
  std::size_t revoked = 0;
  std::size_t retained = 0;
  Rate released{};
  /// Resources where protected obligations plus headroom alone exceed usable
  /// capacity. That is a contradiction in the authoritative inputs; admission
  /// holds nothing there and refuses everything targeting it.
  std::size_t infeasible_resources = 0;
  std::vector<AdmissionDecisionId> revoked_decisions{};
  std::vector<BindingConstraint> constraints{};
};

/// Deterministic, explicit fault injection for failure-path tests. Plain data
/// only: no callbacks are ever invoked while engine state is locked.
struct FaultInjection {
  bool fail_next_journal_append = false;
  bool fail_next_commit = false;
  bool drop_prepare_record = false;
  bool drop_commit_record = false;
  bool corrupt_next_commit = false;
  bool fail_reconciliation = false;

  void clear() noexcept { *this = FaultInjection{}; }
};

/// The admission engine. Every public method is safe to call concurrently.
///
/// Locking contract: a single internal mutex guards all authoritative state.
/// The engine never invokes caller-supplied code, never emits events and never
/// allocates unbounded memory while holding it, so re-entry is impossible by
/// construction. Nested engine calls from engine code do not exist.
class AdmissionEngine {
 public:
  explicit AdmissionEngine(EngineConfig config = {});
  ~AdmissionEngine();

  AdmissionEngine(const AdmissionEngine&) = delete;
  AdmissionEngine& operator=(const AdmissionEngine&) = delete;

  // ------------------------------------------------------------------
  // Authority publication. Publishing an identical snapshot again is a no-op
  // (idempotent). Publishing an older generation is refused as stale.
  // ------------------------------------------------------------------
  Status publish_policy(AdmissionPolicy policy);
  Status publish_capacity(const CapacitySnapshot& snapshot);
  Status publish_reservations(const ReservationSnapshot& snapshot);
  Status publish_paths(const PathCatalog& catalog);
  Status publish_qos_catalog(const QoSClassCatalog& catalog);
  Status publish_priority_catalog(const PriorityClassCatalog& catalog);

  /// Drops capacity authority, returning the engine to an UNKNOWN capacity
  /// state. Used to model a capacity authority that has gone away.
  Status invalidate_capacity();
  Status invalidate_paths();

  // ------------------------------------------------------------------
  // Durability. The journal must be attached before open_durable().
  // ------------------------------------------------------------------
  Status attach_journal(std::unique_ptr<Journal> journal);
  /// Replays durable state into the engine. Must be called before serving.
  Expected<RecoveryReport> open_durable();
  Status compact_journal();
  void set_fault_injection(FaultInjection faults);
  [[nodiscard]] FaultInjection fault_injection() const;

  // ------------------------------------------------------------------
  // Claimant sessions. Sessions live only in this process; they are never
  // persisted, so a restart fences every previous claimant by construction.
  // ------------------------------------------------------------------
  Expected<SessionGrant> register_session(PublisherId publisher, BootId boot);
  Status retire_session(SessionNonce session);
  std::size_t live_sessions() const;
  /// Advances the fencing epoch and drops every live session. Any claim stamped
  /// with the previous epoch is thereafter refused as fenced.
  Status begin_new_incarnation(CoordinatorIncarnation incarnation);

  // ------------------------------------------------------------------
  // Logical time
  // ------------------------------------------------------------------
  void advance_tick(std::uint64_t ticks = 1);
  std::uint64_t tick() const;

  // ------------------------------------------------------------------
  // Decisions
  // ------------------------------------------------------------------
  Expected<AdmissionDecision> admit(const AdmissionRequest& request, const ClaimContext& claim);
  Expected<AdmissionDecision> admit_local(const AdmissionRequest& request);

  Expected<RevalidationResult> revalidate(AdmissionDecisionId decision, const ClaimContext& claim);
  Expected<RevalidationResult> revoke(AdmissionDecisionId decision, RevocationReason reason,
                                      const ClaimContext& claim);

  // ------------------------------------------------------------------
  // Introspection. Everything is returned by value; no internal reference
  // escapes and no lock is held after the call.
  // ------------------------------------------------------------------
  [[nodiscard]] FabricStatus status() const;
  [[nodiscard]] Rate admitted_load(ResourceId resource) const;
  [[nodiscard]] std::optional<AdmissionPolicy> policy() const;
  [[nodiscard]] std::optional<DecisionRecord> decision_record(AdmissionDecisionId decision) const;
  [[nodiscard]] std::vector<DecisionRecord> decision_history() const;
  [[nodiscard]] std::vector<LedgerEntry> ledger_entries() const;
  [[nodiscard]] FabricEpoch epoch() const;
  [[nodiscard]] CoordinatorIncarnation incarnation() const;
  [[nodiscard]] ReconciliationReport last_reconciliation() const;

 private:
  std::unique_ptr<detail::EngineCore> core_;
};

}  // namespace naf

#endif  // NAF_ENGINE_ENGINE_HPP

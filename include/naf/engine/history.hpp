// Network Admission Fabric - decision history and attempt identity registry.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Both stores are bounded. When a bound is reached the oldest record is retired
// and the retirement is counted, so growth is never unbounded and never silent.
#ifndef NAF_ENGINE_HISTORY_HPP
#define NAF_ENGINE_HISTORY_HPP

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "naf/core/identity.hpp"
#include "naf/core/limits.hpp"
#include "naf/core/status.hpp"
#include "naf/engine/decision.hpp"
#include "naf/engine/ledger.hpp"

namespace naf {

/// Compact record of a decision, sufficient to revalidate or revoke it.
struct DecisionRecord {
  AdmissionDecisionId decision{};
  AuditSequence audit{};
  AdmissionOutcome outcome = AdmissionOutcome::RejectPolicy;
  AdmissionRequestId request{};
  DemandId demand{};
  Generation demand_generation{};
  AttemptId attempt{};
  PathId path{};
  Rate granted{};
  FabricEpoch epoch{};
  CoordinatorIncarnation incarnation{};
  std::uint64_t decided_tick = 0;
  std::uint64_t fingerprint = 0;
  Generation bound_capacity_generation{};
  Generation bound_policy_generation{};
  Generation bound_qos_generation{};
  Generation bound_priority_generation{};
  Generation bound_path_generation{};
  Generation bound_reservation_generation{};
  RevalidationState state = RevalidationState::Fresh;
  RevocationReason reason = RevocationReason::NotRevoked;
  bool ledger_live = false;
  bool restored_from_disk = false;
};

/// Bounded decision history keyed by decision id, retained in audit order.
class DecisionHistory {
 public:
  explicit DecisionHistory(std::size_t capacity = limits::max_decision_records)
      : capacity_(capacity) {}

  void insert(DecisionRecord record);
  /// Retires the oldest unprotected records once the capacity is exceeded. A
  /// record whose decision still holds admitted load is never retired: it must
  /// remain recoverable from durable history. A monotonic cursor keeps the
  /// amortised cost of retirement constant instead of rescanning a window that
  /// is full of protected records on every insertion.
  void trim(const AdmittedLoadLedger& ledger);
  /// Signals that a grant was released, so previously protected records may now
  /// be retired and the retirement cursor must be reconsidered.
  void note_release() noexcept { cursor_audit_ = 0; }
  [[nodiscard]] bool needs_trim() const noexcept { return records_.size() > capacity_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] const DecisionRecord* find(AdmissionDecisionId decision) const;
  bool update(AdmissionDecisionId decision, const DecisionRecord& record);
  [[nodiscard]] std::vector<DecisionRecord> in_audit_order() const;
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] std::uint64_t retired() const noexcept { return retired_; }

 private:
  std::size_t capacity_;
  std::map<std::uint64_t, DecisionRecord> records_{};
  std::map<std::uint64_t, std::uint64_t> audit_index_{};
  /// Audit sequence at which the retirement scan resumes. Stored as a value
  /// rather than an iterator so the history stays safely copyable.
  std::uint64_t cursor_audit_ = 0;
  std::uint64_t retired_ = 0;
};

enum class AttemptDisposition : std::uint8_t {
  New = 0,
  Replay = 1,
  Conflict = 2,
};

struct AttemptRecord {
  AdmissionRequestId request{};
  DemandId demand{};
  Generation demand_generation{};
  AttemptId attempt{};
  std::uint64_t fingerprint = 0;
  AdmissionDecisionId decision{};
  AdmissionOutcome outcome = AdmissionOutcome::RejectPolicy;
  std::uint64_t recorded_tick = 0;
};

/// Idempotency and identity-reuse guard.
///
/// The same (demand, attempt, fingerprint) triple is a replay and returns the
/// original decision. The same (demand, attempt) with a different fingerprint is
/// identity reuse and is refused. The same request id with a different
/// fingerprint is likewise refused.
class AttemptRegistry {
 public:
  explicit AttemptRegistry(std::size_t capacity = limits::max_attempt_records) : capacity_(capacity) {}

  struct Classification {
    AttemptDisposition disposition = AttemptDisposition::New;
    AttemptRecord existing{};
    bool request_id_conflict = false;
  };

  [[nodiscard]] Classification classify(const AttemptRecord& candidate) const;
  void record(const AttemptRecord& candidate);

  [[nodiscard]] std::size_t size() const noexcept { return attempts_.size(); }
  [[nodiscard]] std::uint64_t retired() const noexcept { return retired_; }
  void clear();

 private:
  std::size_t capacity_;
  std::map<std::pair<std::uint64_t, std::uint64_t>, AttemptRecord> attempts_{};
  std::map<std::uint64_t, std::uint64_t> request_fingerprints_{};
  std::uint64_t retired_ = 0;
};

}  // namespace naf

#endif  // NAF_ENGINE_HISTORY_HPP

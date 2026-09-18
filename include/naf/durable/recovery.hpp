// Network Admission Fabric - recovery classification.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Recovery explicitly separates durable configuration, committed authoritative
// state, unfinished attempts, ambiguous outcomes, stale live authority and
// evidence that must be revalidated. Nothing recovered is ever treated as live
// process authority.
#ifndef NAF_DURABLE_RECOVERY_HPP
#define NAF_DURABLE_RECOVERY_HPP

#include <cstdint>
#include <optional>
#include <vector>

#include "naf/core/identity.hpp"
#include "naf/core/status.hpp"
#include "naf/durable/records.hpp"
#include "naf/engine/history.hpp"
#include "naf/engine/ledger.hpp"
#include "naf/model/policy.hpp"

namespace naf {

/// A prepare record whose commit was never observed. The outcome is genuinely
/// ambiguous: the admission is NOT restored into the ledger, and any claimant
/// relying on it must revalidate.
struct AmbiguousAttempt {
  PrepareRecord prepare{};
  std::uint64_t prepare_sequence = 0;
};

/// What caused an accounting-enforcement pass over the admitted load.
enum class EnforcementTrigger : std::uint8_t {
  None = 0,
  CapacityPublication = 1,
  ReservationPublication = 2,
  PolicyPublication = 3,
  RestartRecovery = 4,
};

[[nodiscard]] std::string_view to_string(EnforcementTrigger trigger) noexcept;

enum class RecoveryOutcome : std::uint8_t {
  FreshStart = 0,
  Recovered = 1,
  RecoveredWithAmbiguity = 2,
  RepairedTornTail = 3,
  Refused = 4,
};

[[nodiscard]] std::string_view to_string(RecoveryOutcome outcome) noexcept;

struct RecoveryReport {
  RecoveryOutcome outcome = RecoveryOutcome::FreshStart;
  Status status{};

  bool journal_present = false;
  std::uint64_t frames_scanned = 0;
  std::uint64_t frames_replayed = 0;
  std::uint64_t trailing_bytes_discarded = 0;
  bool corrupt = false;

  FabricEpoch recovered_epoch{};
  bool epoch_advanced = false;

  AuditSequence audit_sequence{};
  std::uint64_t next_decision_sequence = 1;

  std::optional<AdmissionPolicy> policy{};
  std::vector<DecisionRecord> history{};
  std::vector<LedgerEntry> live_ledger{};
  std::vector<AmbiguousAttempt> ambiguous{};

  /// False until the owning capacities, reservations and paths have been
  /// republished. While false the engine refuses to admit.
  bool awaiting_authority_republish = false;
  EnforcementTrigger trigger = EnforcementTrigger::None;

  [[nodiscard]] std::string render() const;
};

}  // namespace naf

#endif  // NAF_DURABLE_RECOVERY_HPP

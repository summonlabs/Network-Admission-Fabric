// Network Admission Fabric - admitted load accounting.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The ledger is the only place admitted load lives. It is owned entirely by the
// admission runtime; no adjacent system writes it. Every mutation is checked:
// totals can never overflow and can never go negative.
#ifndef NAF_ENGINE_LEDGER_HPP
#define NAF_ENGINE_LEDGER_HPP

#include <cstdint>
#include <map>
#include <vector>

#include "naf/core/identity.hpp"
#include "naf/core/limits.hpp"
#include "naf/core/status.hpp"
#include "naf/model/quantity.hpp"

namespace naf {

struct LedgerEntry {
  AdmissionDecisionId decision{};
  DemandId demand{};
  ResourceId resource{};
  Rate amount{};
  PathId path{};
  FabricEpoch epoch{};
  Generation bound_capacity_generation{};
  bool requires_revalidation = false;

  friend bool operator==(const LedgerEntry&, const LedgerEntry&) = default;
};

/// Per-resource admitted load with per-decision provenance so a grant can be
/// released exactly once and exactly in the amount that was reserved.
class AdmittedLoadLedger {
 public:
  AdmittedLoadLedger() = default;

  /// Adds every entry of one decision atomically: either all reservations are
  /// applied or none are.
  Status reserve(AdmissionDecisionId decision, DemandId demand, FabricEpoch epoch,
                 Generation bound_capacity_generation, PathId path,
                 const std::vector<std::pair<ResourceId, Rate>>& amounts);

  /// Removes all load held by a decision. Returns NotFound when nothing is held.
  Status release(AdmissionDecisionId decision, std::vector<LedgerEntry>& released);

  [[nodiscard]] bool holds(AdmissionDecisionId decision) const;
  [[nodiscard]] Rate admitted(ResourceId resource) const;
  [[nodiscard]] Rate admitted_on_path(PathId path) const;
  [[nodiscard]] std::size_t entry_count() const noexcept { return entry_count_; }
  [[nodiscard]] std::size_t live_decisions() const noexcept { return by_decision_.size(); }

  /// Resource ids with non-zero admitted load, in ascending order.
  [[nodiscard]] std::vector<ResourceId> resources() const;
  /// Decision ids that currently hold load. Used to protect durable history
  /// records from retirement while their grant is still live.
  [[nodiscard]] std::vector<std::uint64_t> live_decision_keys() const;
  [[nodiscard]] std::vector<LedgerEntry> entries() const;

  /// Marks every restored entry as requiring revalidation. Used after a restart
  /// so that recovered load is never treated as freshly authorized.
  void mark_all_requiring_revalidation();
  /// Clears the revalidation requirement once recovered load has been
  /// reconciled against republished authority.
  void clear_revalidation_flags();

  void clear();

 private:
  Status remove(AdmissionDecisionId decision, std::vector<LedgerEntry>& released);

  std::map<std::uint64_t, Rate> totals_{};
  std::map<std::uint64_t, Rate> path_totals_{};
  std::map<std::uint64_t, std::vector<LedgerEntry>> by_decision_{};
  std::size_t entry_count_ = 0;
};

}  // namespace naf

#endif  // NAF_ENGINE_LEDGER_HPP

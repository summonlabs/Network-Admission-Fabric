// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/engine/ledger.hpp"

#include <algorithm>

#include "naf/core/checked.hpp"

namespace naf {

Status AdmittedLoadLedger::reserve(AdmissionDecisionId decision, DemandId demand, FabricEpoch epoch,
                                   Generation bound_capacity_generation, PathId path,
                                   const std::vector<std::pair<ResourceId, Rate>>& amounts) {
  if (decision.is_unknown()) {
    return Status::error(StatusCode::InvalidArgument, "cannot reserve load for an unknown decision id");
  }
  if (by_decision_.find(decision.value()) != by_decision_.end()) {
    return Status::error(StatusCode::AlreadyExists, "decision already holds admitted load");
  }
  if (amounts.empty()) {
    return Status::error(StatusCode::InvalidArgument, "refusing to reserve an empty resource set");
  }
  if (entry_count_ + amounts.size() > limits::max_ledger_entries) {
    return Status::error(StatusCode::OversizedInput, "admitted load ledger is full");
  }

  // Validate every addition before mutating anything so the reservation is
  // all-or-nothing across resources.
  for (const auto& [resource, amount] : amounts) {
    if (resource.is_unknown()) {
      return Status::error(StatusCode::InvalidArgument, "reservation names an unknown resource");
    }
    const auto it = totals_.find(resource.value());
    const std::uint64_t current = it == totals_.end() ? 0 : it->second.value();
    std::uint64_t updated = 0;
    if (!add_u64(current, amount.value(), updated)) {
      return Status::error(StatusCode::ArithmeticOverflow,
                           "admitted load would overflow for resource " + std::to_string(resource.value()));
    }
  }

  std::vector<LedgerEntry> entries;
  entries.reserve(amounts.size());
  for (const auto& [resource, amount] : amounts) {
    std::uint64_t current = 0;
    const auto it = totals_.find(resource.value());
    if (it != totals_.end()) current = it->second.value();
    totals_[resource.value()] = Rate::from_value(current + amount.value());
    LedgerEntry entry;
    entry.decision = decision;
    entry.demand = demand;
    entry.resource = resource;
    entry.amount = amount;
    entry.path = path;
    entry.epoch = epoch;
    entry.bound_capacity_generation = bound_capacity_generation;
    entry.requires_revalidation = false;
    entries.push_back(entry);
  }
  if (path.is_known()) {
    const auto it = path_totals_.find(path.value());
    const std::uint64_t current = it == path_totals_.end() ? 0 : it->second.value();
    std::uint64_t updated = 0;
    if (!add_u64(current, amounts.front().second.value(), updated)) {
      // Roll the resource totals back before reporting the failure.
      for (const auto& [resource, amount] : amounts) {
        auto total = totals_.find(resource.value());
        const std::uint64_t restored = total->second.value() - amount.value();
        if (restored == 0) {
          totals_.erase(total);
        } else {
          total->second = Rate::from_value(restored);
        }
      }
      return Status::error(StatusCode::ArithmeticOverflow, "admitted load on the path would overflow");
    }
    path_totals_[path.value()] = Rate::from_value(updated);
  }
  entry_count_ += entries.size();
  by_decision_.emplace(decision.value(), std::move(entries));
  return Status::ok();
}

Status AdmittedLoadLedger::remove(AdmissionDecisionId decision, std::vector<LedgerEntry>& released) {
  const auto it = by_decision_.find(decision.value());
  if (it == by_decision_.end()) {
    return Status::error(StatusCode::NotFound, "decision holds no admitted load");
  }
  for (const auto& entry : it->second) {
    const auto total = totals_.find(entry.resource.value());
    if (total == totals_.end() || total->second < entry.amount) {
      return Status::error(StatusCode::ArithmeticUnderflow,
                           "admitted load accounting would go negative for resource " +
                               std::to_string(entry.resource.value()));
    }
  }
  released = it->second;
  for (const auto& entry : it->second) {
    auto total = totals_.find(entry.resource.value());
    const std::uint64_t updated = total->second.value() - entry.amount.value();
    if (updated == 0) {
      totals_.erase(total);
    } else {
      total->second = Rate::from_value(updated);
    }
  }
  if (!it->second.empty() && it->second.front().path.is_known()) {
    const naf::PathId path = it->second.front().path;
    SumLatch path_total;
    for (const auto& entry : it->second) path_total.add(entry.amount.value());
    auto total = path_totals_.find(path.value());
    if (total != path_totals_.end()) {
      const std::uint64_t updated = total->second.value() - path_total.saturated_total();
      if (updated == 0) {
        path_totals_.erase(total);
      } else {
        total->second = Rate::from_value(updated);
      }
    }
  }
  entry_count_ -= it->second.size();
  by_decision_.erase(it);
  return Status::ok();
}

Status AdmittedLoadLedger::release(AdmissionDecisionId decision, std::vector<LedgerEntry>& released) {
  if (decision.is_unknown()) {
    return Status::error(StatusCode::InvalidArgument, "cannot release an unknown decision id");
  }
  return remove(decision, released);
}

bool AdmittedLoadLedger::holds(AdmissionDecisionId decision) const {
  return by_decision_.find(decision.value()) != by_decision_.end();
}

Rate AdmittedLoadLedger::admitted(ResourceId resource) const {
  const auto it = totals_.find(resource.value());
  return it == totals_.end() ? Rate{} : it->second;
}

Rate AdmittedLoadLedger::admitted_on_path(PathId path) const {
  if (path.is_unknown()) return Rate{};
  const auto it = path_totals_.find(path.value());
  return it == path_totals_.end() ? Rate{} : it->second;
}

std::vector<ResourceId> AdmittedLoadLedger::resources() const {
  std::vector<ResourceId> out;
  out.reserve(totals_.size());
  for (const auto& [key, value] : totals_) {
    (void)value;
    out.push_back(ResourceId::from_value(key));
  }
  return out;
}

std::vector<std::uint64_t> AdmittedLoadLedger::live_decision_keys() const {
  std::vector<std::uint64_t> out;
  out.reserve(by_decision_.size());
  for (const auto& [key, entries] : by_decision_) {
    (void)entries;
    out.push_back(key);
  }
  return out;
}

std::vector<LedgerEntry> AdmittedLoadLedger::entries() const {
  std::vector<LedgerEntry> out;
  out.reserve(entry_count_);
  for (const auto& [key, entries] : by_decision_) {
    (void)key;
    for (const auto& entry : entries) out.push_back(entry);
  }
  std::sort(out.begin(), out.end(), [](const LedgerEntry& a, const LedgerEntry& b) {
    if (a.resource != b.resource) return a.resource < b.resource;
    return a.decision < b.decision;
  });
  return out;
}

void AdmittedLoadLedger::mark_all_requiring_revalidation() {
  for (auto& [key, entries] : by_decision_) {
    (void)key;
    for (auto& entry : entries) entry.requires_revalidation = true;
  }
}

void AdmittedLoadLedger::clear_revalidation_flags() {
  for (auto& [key, entries] : by_decision_) {
    (void)key;
    for (auto& entry : entries) entry.requires_revalidation = false;
  }
}

void AdmittedLoadLedger::clear() {
  totals_.clear();
  path_totals_.clear();
  by_decision_.clear();
  entry_count_ = 0;
}

}  // namespace naf

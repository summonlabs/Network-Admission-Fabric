// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/engine/history.hpp"

#include <algorithm>

namespace naf {

void DecisionHistory::insert(DecisionRecord record) {
  if (record.decision.is_unknown()) return;
  const auto existing = records_.find(record.decision.value());
  if (existing != records_.end()) {
    audit_index_.erase(existing->second.audit.value());
    existing->second = std::move(record);
    audit_index_[existing->second.audit.value()] = existing->first;
    return;
  }
  const std::uint64_t key = record.decision.value();
  audit_index_[record.audit.value()] = key;
  records_[key] = std::move(record);
}

void DecisionHistory::trim(const AdmittedLoadLedger& ledger) {
  if (records_.size() <= capacity_) {
    cursor_audit_ = 0;
    return;
  }
  auto cursor = audit_index_.lower_bound(cursor_audit_);
  while (records_.size() > capacity_ && cursor != audit_index_.end()) {
    const AdmissionDecisionId decision = AdmissionDecisionId::from_value(cursor->second);
    if (ledger.holds(decision)) {
      // Protected: a live grant's record must remain recoverable.
      ++cursor;
      continue;
    }
    records_.erase(cursor->second);
    cursor = audit_index_.erase(cursor);
    ++retired_;
  }
  // Resume just past the last record examined. Audits are assigned in
  // increasing order, so a new record always has a higher audit than the cursor
  // and is still reached. A later release resets the cursor to the front.
  cursor_audit_ = (cursor == audit_index_.end())
                      ? (audit_index_.empty() ? 0 : audit_index_.rbegin()->first + 1)
                      : cursor->first;
}

const DecisionRecord* DecisionHistory::find(AdmissionDecisionId decision) const {
  const auto it = records_.find(decision.value());
  return it == records_.end() ? nullptr : &it->second;
}

bool DecisionHistory::update(AdmissionDecisionId decision, const DecisionRecord& record) {
  const auto it = records_.find(decision.value());
  if (it == records_.end()) return false;
  const AuditSequence audit = it->second.audit;
  it->second = record;
  it->second.audit = audit;
  return true;
}

std::vector<DecisionRecord> DecisionHistory::in_audit_order() const {
  std::vector<DecisionRecord> out;
  out.reserve(records_.size());
  for (const auto& [audit, key] : audit_index_) {
    const auto it = records_.find(key);
    if (it != records_.end()) out.push_back(it->second);
  }
  (void)0;
  return out;
}

AttemptRegistry::Classification AttemptRegistry::classify(const AttemptRecord& candidate) const {
  Classification result;
  const auto key = std::make_pair(candidate.demand.value(), candidate.attempt.value());
  const auto it = attempts_.find(key);
  if (it != attempts_.end()) {
    if (it->second.fingerprint == candidate.fingerprint) {
      result.disposition = AttemptDisposition::Replay;
      result.existing = it->second;
      return result;
    }
    result.disposition = AttemptDisposition::Conflict;
    result.existing = it->second;
    return result;
  }
  // The request identity itself must never be reused for different content.
  if (candidate.request.is_known()) {
    const auto known = request_fingerprints_.find(candidate.request.value());
    if (known != request_fingerprints_.end() && known->second != candidate.fingerprint) {
      result.disposition = AttemptDisposition::Conflict;
      result.request_id_conflict = true;
      return result;
    }
  }
  result.disposition = AttemptDisposition::New;
  return result;
}

void AttemptRegistry::record(const AttemptRecord& candidate) {
  const auto key = std::make_pair(candidate.demand.value(), candidate.attempt.value());
  if (attempts_.find(key) == attempts_.end()) {
    while (attempts_.size() >= capacity_ && !attempts_.empty()) {
      auto oldest = attempts_.begin();
      attempts_.erase(oldest);
      ++retired_;
    }
  }
  attempts_[key] = candidate;
  if (candidate.request.is_known()) {
    request_fingerprints_.emplace(candidate.request.value(), candidate.fingerprint);
  }
}

void AttemptRegistry::clear() {
  attempts_.clear();
  request_fingerprints_.clear();
  retired_ = 0;
}

}  // namespace naf

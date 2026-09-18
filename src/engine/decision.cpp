// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/engine/decision.hpp"

#include <string>

#include "core/text.hpp"

namespace naf {
namespace {

std::string bounded(std::string value, std::size_t limit) {
  if (value.size() <= limit) return value;
  value.resize(limit);
  return value;
}

}  // namespace

std::string_view to_string(AdmissionOutcome outcome) noexcept {
  switch (outcome) {
    case AdmissionOutcome::Admit: return "ADMIT";
    case AdmissionOutcome::AdmitDegraded: return "ADMIT_DEGRADED";
    case AdmissionOutcome::Defer: return "DEFER";
    case AdmissionOutcome::RejectCapacity: return "REJECT_CAPACITY";
    case AdmissionOutcome::RejectPolicy: return "REJECT_POLICY";
    case AdmissionOutcome::RejectObligation: return "REJECT_OBLIGATION";
    case AdmissionOutcome::RejectPath: return "REJECT_PATH";
    case AdmissionOutcome::RejectQoS: return "REJECT_QOS";
    case AdmissionOutcome::StaleInput: return "STALE_INPUT";
    case AdmissionOutcome::ConflictingInput: return "CONFLICTING_INPUT";
    case AdmissionOutcome::FencedClaimant: return "FENCED_CLAIMANT";
  }
  return "UNKNOWN";
}

std::string_view to_string(BindingConstraintKind kind) noexcept {
  switch (kind) {
    case BindingConstraintKind::None: return "none";
    case BindingConstraintKind::RequestContract: return "request_contract";
    case BindingConstraintKind::Fence: return "fence";
    case BindingConstraintKind::IdentityConflict: return "identity_conflict";
    case BindingConstraintKind::UnknownEvidence: return "unknown_evidence";
    case BindingConstraintKind::StaleGeneration: return "stale_generation";
    case BindingConstraintKind::Policy: return "policy";
    case BindingConstraintKind::QoSClass: return "qos_class";
    case BindingConstraintKind::PriorityClass: return "priority_class";
    case BindingConstraintKind::Path: return "path";
    case BindingConstraintKind::Capacity: return "capacity";
    case BindingConstraintKind::Headroom: return "headroom";
    case BindingConstraintKind::Obligation: return "obligation";
    case BindingConstraintKind::AdmittedLoad: return "admitted_load";
    case BindingConstraintKind::Degradation: return "degradation";
    case BindingConstraintKind::Durability: return "durability";
  }
  return "unknown";
}

std::string_view to_string(RevocationReason reason) noexcept {
  switch (reason) {
    case RevocationReason::NotRevoked: return "not_revoked";
    case RevocationReason::OperatorRequest: return "operator_request";
    case RevocationReason::GenerationAdvance: return "generation_advance";
    case RevocationReason::CapacityReduction: return "capacity_reduction";
    case RevocationReason::PathRevoked: return "path_revoked";
    case RevocationReason::PolicyChange: return "policy_change";
    case RevocationReason::ClaimantDeath: return "claimant_death";
    case RevocationReason::CoordinatorRestart: return "coordinator_restart";
    case RevocationReason::ObligationChange: return "obligation_change";
    case RevocationReason::Expired: return "expired";
  }
  return "unknown";
}

std::string_view to_string(RevalidationState state) noexcept {
  switch (state) {
    case RevalidationState::Fresh: return "fresh";
    case RevalidationState::Valid: return "valid";
    case RevalidationState::Invalidated: return "invalidated";
    case RevalidationState::Revoked: return "revoked";
    case RevalidationState::Expired: return "expired";
    case RevalidationState::Ambiguous: return "ambiguous";
    case RevalidationState::Unknown: return "unknown";
  }
  return "unknown";
}

void Explanation::configure(std::size_t max_constraints, std::size_t max_bytes) noexcept {
  if (max_constraints == 0) max_constraints = 1;
  if (max_constraints > limits::max_explanation_constraints) {
    max_constraints = limits::max_explanation_constraints;
  }
  if (max_bytes < 64) max_bytes = 64;
  if (max_bytes > limits::max_explanation_bytes) max_bytes = limits::max_explanation_bytes;
  max_constraints_ = max_constraints;
  max_bytes_ = max_bytes;
}

bool Explanation::add(BindingConstraint constraint) {
  constraint.subject = bounded(std::move(constraint.subject), limits::max_string_bytes);
  constraint.detail = bounded(std::move(constraint.detail), limits::max_detail_bytes);
  const std::size_t cost = constraint.subject.size() + constraint.detail.size() + 96;
  if (constraints_.size() >= max_constraints_ || bytes_ + cost > max_bytes_) {
    truncated_ = true;
    if (omitted_ != 0xFFFFFFFFu) ++omitted_;
    return false;
  }
  bytes_ += cost;
  constraints_.push_back(std::move(constraint));
  return true;
}

std::string Explanation::render() const {
  std::string out;
  out.reserve(bytes_ + 32);
  bool first = true;
  for (const auto& constraint : constraints_) {
    if (!first) out.push_back('\n');
    first = false;
    out += to_string(constraint.kind);
    if (!constraint.subject.empty()) {
      out += " subject=";
      out += constraint.subject;
    }
    if (constraint.subject_id != 0) {
      out += " id=";
      naf::text::append_u64(out, constraint.subject_id);
    }
    if (constraint.required.value() != 0 || constraint.available.value() != 0) {
      out += " required=";
      naf::text::append_u64(out, constraint.required.value());
      out += " available=";
      naf::text::append_u64(out, constraint.available.value());
    }
    if (!constraint.observed.is_unknown() || !constraint.authoritative.is_unknown()) {
      out += " observed=";
      naf::text::append_u64(out, constraint.observed.value());
      out += " authoritative=";
      naf::text::append_u64(out, constraint.authoritative.value());
    }
    if (!constraint.detail.empty()) {
      out += " detail=";
      out += constraint.detail;
    }
  }
  if (truncated_) {
    out += "\n(truncated; omitted=";
    naf::text::append_u64(out, omitted_);
    out.push_back(')');
  }
  if (out.size() > max_bytes_ + 64) out.resize(max_bytes_ + 64);
  return out;
}

}  // namespace naf

// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/durable/records.hpp"

#include <string>

#include "naf/core/limits.hpp"

namespace naf {
namespace {

Status malformed(std::string message) { return Status::error(StatusCode::MalformedInput, std::move(message)); }
Status oversized(std::string message) { return Status::error(StatusCode::OversizedInput, std::move(message)); }

Status finish(ByteReader& reader) {
  if (!reader.exhausted()) return malformed("durable record has trailing bytes");
  return Status::ok();
}

}  // namespace

ByteBuffer encode(const BootRecord& record) {
  ByteWriter w(48);
  w.u16(record.format_revision);
  (void)w.text(record.product);
  w.u64(record.epoch.value());
  w.u64(record.boot_nonce);
  w.u64(record.incarnation.value());
  return std::move(w).take();
}

Expected<BootRecord> decode_boot(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  BootRecord out;
  if (!r.u16(out.format_revision)) return malformed("boot record is truncated");
  if (!r.text(out.product)) return malformed("boot record product is invalid");
  std::uint64_t epoch = 0;
  if (!r.u64(epoch)) return malformed("boot record is truncated");
  out.epoch = FabricEpoch::from_value(epoch);
  if (!r.u64(out.boot_nonce)) return malformed("boot record is truncated");
  std::uint64_t incarnation = 0;
  if (!r.u64(incarnation)) return malformed("boot record is truncated");
  out.incarnation = CoordinatorIncarnation::from_value(incarnation);
  Status s = finish(r);
  if (!s.is_ok()) return s;
  return out;
}

ByteBuffer encode(const AdmissionPolicy& policy) {
  ByteWriter w(256);
  w.u64(policy.id.value());
  w.u64(policy.generation.value());
  w.u64(policy.epoch.value());
  w.u64(policy.observed_tick);
  w.u32(policy.headroom_permille);
  w.u64(policy.headroom_floor.value());
  w.boolean(policy.allow_degraded);
  w.boolean(policy.allow_preemptible_admission);
  w.boolean(policy.require_path_binding);
  w.boolean(policy.require_reservation_reference);
  w.boolean(policy.allow_path_substitution);
  w.u8(static_cast<std::uint8_t>(policy.on_contention));
  w.u32(static_cast<std::uint32_t>(policy.allowed_qos.size()));
  for (const auto& id : policy.allowed_qos) w.u64(id.value());
  w.u32(static_cast<std::uint32_t>(policy.allowed_priorities.size()));
  for (const auto& id : policy.allowed_priorities) w.u64(id.value());
  w.u32(policy.max_explanation_constraints);
  w.u32(policy.max_explanation_bytes);
  w.u32(policy.max_effective_resources);
  w.u64(policy.defer_horizon_ticks);
  return std::move(w).take();
}

Expected<AdmissionPolicy> decode_policy(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  AdmissionPolicy out;
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("policy record is truncated");
  out.id = PolicyId::from_value(value);
  if (!r.u64(value)) return malformed("policy record is truncated");
  out.generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("policy record is truncated");
  out.epoch = FabricEpoch::from_value(value);
  if (!r.u64(out.observed_tick)) return malformed("policy record is truncated");
  if (!r.u32(out.headroom_permille)) return malformed("policy record is truncated");
  if (!r.u64(value)) return malformed("policy record is truncated");
  out.headroom_floor = Rate::from_value(value);
  if (!r.boolean(out.allow_degraded)) return malformed("policy record is truncated");
  if (!r.boolean(out.allow_preemptible_admission)) return malformed("policy record is truncated");
  if (!r.boolean(out.require_path_binding)) return malformed("policy record is truncated");
  if (!r.boolean(out.require_reservation_reference)) return malformed("policy record is truncated");
  if (!r.boolean(out.allow_path_substitution)) return malformed("policy record is truncated");
  std::uint8_t contention = 0;
  if (!r.u8(contention)) return malformed("policy record is truncated");
  if (contention > static_cast<std::uint8_t>(ContentionAction::Reject)) {
    return malformed("policy contention action is not recognised");
  }
  out.on_contention = static_cast<ContentionAction>(contention);

  std::uint32_t count = 0;
  if (!r.u32(count)) return malformed("policy record is truncated");
  if (count > limits::max_allowed_classes) return oversized("policy allowed qos list is too large");
  out.allowed_qos.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!r.u64(value)) return malformed("policy record is truncated");
    out.allowed_qos.push_back(QoSClassId::from_value(value));
  }
  if (!r.u32(count)) return malformed("policy record is truncated");
  if (count > limits::max_allowed_classes) return oversized("policy allowed priority list is too large");
  out.allowed_priorities.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!r.u64(value)) return malformed("policy record is truncated");
    out.allowed_priorities.push_back(PriorityClassId::from_value(value));
  }
  if (!r.u32(out.max_explanation_constraints)) return malformed("policy record is truncated");
  if (!r.u32(out.max_explanation_bytes)) return malformed("policy record is truncated");
  if (!r.u32(out.max_effective_resources)) return malformed("policy record is truncated");
  if (!r.u64(out.defer_horizon_ticks)) return malformed("policy record is truncated");
  Status s = finish(r);
  if (!s.is_ok()) return s;
  return out;
}

ByteBuffer encode(const PrepareRecord& record) {
  ByteWriter w(64 + record.amounts.size() * 16);
  w.u64(record.decision.value());
  w.u64(record.demand.value());
  w.u64(record.attempt.value());
  w.u64(record.fingerprint);
  w.u64(record.epoch.value());
  w.u64(record.bound_capacity_generation.value());
  w.u64(record.path.value());
  w.u32(static_cast<std::uint32_t>(record.amounts.size()));
  for (const auto& [resource, amount] : record.amounts) {
    w.u64(resource.value());
    w.u64(amount.value());
  }
  return std::move(w).take();
}

Expected<PrepareRecord> decode_prepare(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  PrepareRecord out;
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("prepare record is truncated");
  out.decision = AdmissionDecisionId::from_value(value);
  if (!r.u64(value)) return malformed("prepare record is truncated");
  out.demand = DemandId::from_value(value);
  if (!r.u64(value)) return malformed("prepare record is truncated");
  out.attempt = AttemptId::from_value(value);
  if (!r.u64(out.fingerprint)) return malformed("prepare record is truncated");
  if (!r.u64(value)) return malformed("prepare record is truncated");
  out.epoch = FabricEpoch::from_value(value);
  if (!r.u64(value)) return malformed("prepare record is truncated");
  out.bound_capacity_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("prepare record is truncated");
  out.path = PathId::from_value(value);
  std::uint32_t count = 0;
  if (!r.u32(count)) return malformed("prepare record is truncated");
  if (count > limits::max_resource_bindings) return oversized("prepare record has too many amounts");
  out.amounts.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint64_t resource = 0;
    std::uint64_t amount = 0;
    if (!r.u64(resource) || !r.u64(amount)) return malformed("prepare record is truncated");
    out.amounts.emplace_back(ResourceId::from_value(resource), Rate::from_value(amount));
  }
  Status s = finish(r);
  if (!s.is_ok()) return s;
  return out;
}

ByteBuffer encode(const DecisionRecord& record) {
  ByteWriter w(256);
  w.u64(record.decision.value());
  w.u64(record.audit.value());
  w.u8(static_cast<std::uint8_t>(record.outcome));
  w.u64(record.request.value());
  w.u64(record.demand.value());
  w.u64(record.demand_generation.value());
  w.u64(record.attempt.value());
  w.u64(record.path.value());
  w.u64(record.granted.value());
  w.u64(record.epoch.value());
  w.u64(record.incarnation.value());
  w.u64(record.decided_tick);
  w.u64(record.fingerprint);
  w.u64(record.bound_capacity_generation.value());
  w.u64(record.bound_policy_generation.value());
  w.u64(record.bound_qos_generation.value());
  w.u64(record.bound_priority_generation.value());
  w.u64(record.bound_path_generation.value());
  w.u64(record.bound_reservation_generation.value());
  w.u8(static_cast<std::uint8_t>(record.state));
  w.u8(static_cast<std::uint8_t>(record.reason));
  w.boolean(record.ledger_live);
  w.boolean(record.restored_from_disk);
  return std::move(w).take();
}

Expected<DecisionRecord> decode_decision_record(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  DecisionRecord out;
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.decision = AdmissionDecisionId::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.audit = AuditSequence::from_value(value);
  std::uint8_t outcome = 0;
  if (!r.u8(outcome)) return malformed("decision record is truncated");
  if (outcome > static_cast<std::uint8_t>(AdmissionOutcome::FencedClaimant)) {
    return malformed("decision record outcome is not recognised");
  }
  out.outcome = static_cast<AdmissionOutcome>(outcome);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.request = AdmissionRequestId::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.demand = DemandId::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.demand_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.attempt = AttemptId::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.path = PathId::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.granted = Rate::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.epoch = FabricEpoch::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.incarnation = CoordinatorIncarnation::from_value(value);
  if (!r.u64(out.decided_tick)) return malformed("decision record is truncated");
  if (!r.u64(out.fingerprint)) return malformed("decision record is truncated");
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.bound_capacity_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.bound_policy_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.bound_qos_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.bound_priority_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.bound_path_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("decision record is truncated");
  out.bound_reservation_generation = Generation::from_value(value);
  std::uint8_t state = 0;
  if (!r.u8(state)) return malformed("decision record is truncated");
  if (state > static_cast<std::uint8_t>(RevalidationState::Unknown)) {
    return malformed("decision record revalidation state is not recognised");
  }
  out.state = static_cast<RevalidationState>(state);
  std::uint8_t reason = 0;
  if (!r.u8(reason)) return malformed("decision record is truncated");
  if (reason > static_cast<std::uint8_t>(RevocationReason::Expired)) {
    return malformed("decision record revocation reason is not recognised");
  }
  out.reason = static_cast<RevocationReason>(reason);
  if (!r.boolean(out.ledger_live)) return malformed("decision record is truncated");
  if (!r.boolean(out.restored_from_disk)) return malformed("decision record is truncated");
  Status s = finish(r);
  if (!s.is_ok()) return s;
  return out;
}

ByteBuffer encode(const ReleaseRecord& record) {
  ByteWriter w(64 + record.released.size() * 56);
  w.u64(record.decision.value());
  w.u64(record.epoch.value());
  w.u8(static_cast<std::uint8_t>(record.reason));
  w.u32(static_cast<std::uint32_t>(record.released.size()));
  for (const auto& entry : record.released) {
    w.u64(entry.decision.value());
    w.u64(entry.demand.value());
    w.u64(entry.resource.value());
    w.u64(entry.amount.value());
    w.u64(entry.path.value());
    w.u64(entry.epoch.value());
    w.u64(entry.bound_capacity_generation.value());
    w.boolean(entry.requires_revalidation);
  }
  return std::move(w).take();
}

Expected<ReleaseRecord> decode_release(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  ReleaseRecord out;
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("release record is truncated");
  out.decision = AdmissionDecisionId::from_value(value);
  if (!r.u64(value)) return malformed("release record is truncated");
  out.epoch = FabricEpoch::from_value(value);
  std::uint8_t reason = 0;
  if (!r.u8(reason)) return malformed("release record is truncated");
  if (reason > static_cast<std::uint8_t>(RevocationReason::Expired)) {
    return malformed("release record reason is not recognised");
  }
  out.reason = static_cast<RevocationReason>(reason);
  std::uint32_t count = 0;
  if (!r.u32(count)) return malformed("release record is truncated");
  if (count > limits::max_resource_bindings) return oversized("release record has too many entries");
  out.released.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    LedgerEntry entry;
    if (!r.u64(value)) return malformed("release record is truncated");
    entry.decision = AdmissionDecisionId::from_value(value);
    if (!r.u64(value)) return malformed("release record is truncated");
    entry.demand = DemandId::from_value(value);
    if (!r.u64(value)) return malformed("release record is truncated");
    entry.resource = ResourceId::from_value(value);
    if (!r.u64(value)) return malformed("release record is truncated");
    entry.amount = Rate::from_value(value);
    if (!r.u64(value)) return malformed("release record is truncated");
    entry.path = PathId::from_value(value);
    if (!r.u64(value)) return malformed("release record is truncated");
    entry.epoch = FabricEpoch::from_value(value);
    if (!r.u64(value)) return malformed("release record is truncated");
    entry.bound_capacity_generation = Generation::from_value(value);
    if (!r.boolean(entry.requires_revalidation)) return malformed("release record is truncated");
    out.released.push_back(entry);
  }
  Status s = finish(r);
  if (!s.is_ok()) return s;
  return out;
}

ByteBuffer encode(const RevocationRecord& record) {
  ByteWriter w(48);
  w.u64(record.decision.value());
  w.u64(record.epoch.value());
  w.u64(record.audit.value());
  w.u8(static_cast<std::uint8_t>(record.reason));
  w.u8(static_cast<std::uint8_t>(record.state));
  return std::move(w).take();
}

Expected<RevocationRecord> decode_revocation(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  RevocationRecord out;
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("revocation record is truncated");
  out.decision = AdmissionDecisionId::from_value(value);
  if (!r.u64(value)) return malformed("revocation record is truncated");
  out.epoch = FabricEpoch::from_value(value);
  if (!r.u64(value)) return malformed("revocation record is truncated");
  out.audit = AuditSequence::from_value(value);
  std::uint8_t reason = 0;
  if (!r.u8(reason)) return malformed("revocation record is truncated");
  if (reason > static_cast<std::uint8_t>(RevocationReason::Expired)) {
    return malformed("revocation record reason is not recognised");
  }
  out.reason = static_cast<RevocationReason>(reason);
  std::uint8_t state = 0;
  if (!r.u8(state)) return malformed("revocation record is truncated");
  if (state > static_cast<std::uint8_t>(RevalidationState::Unknown)) {
    return malformed("revocation record state is not recognised");
  }
  out.state = static_cast<RevalidationState>(state);
  Status s = finish(r);
  if (!s.is_ok()) return s;
  return out;
}

ByteBuffer encode(const AuditMarkRecord& record) {
  ByteWriter w(24);
  w.u64(record.sequence.value());
  w.u64(record.epoch.value());
  w.u64(record.frames);
  return std::move(w).take();
}

Expected<AuditMarkRecord> decode_audit_mark(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  AuditMarkRecord out;
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("audit mark is truncated");
  out.sequence = AuditSequence::from_value(value);
  if (!r.u64(value)) return malformed("audit mark is truncated");
  out.epoch = FabricEpoch::from_value(value);
  if (!r.u64(out.frames)) return malformed("audit mark is truncated");
  Status s = finish(r);
  if (!s.is_ok()) return s;
  return out;
}

}  // namespace naf

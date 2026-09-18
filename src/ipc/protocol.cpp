// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/ipc/protocol.hpp"

#include <string>

#include "naf/core/limits.hpp"

namespace naf::ipc {
namespace {

Status malformed(std::string message) { return Status::error(StatusCode::MalformedInput, std::move(message)); }
Status oversized(std::string message) { return Status::error(StatusCode::OversizedInput, std::move(message)); }

Status finish(ByteReader& reader) {
  if (!reader.exhausted()) return malformed("protocol payload has trailing bytes");
  return Status::ok();
}

Status write_claim(ByteWriter& w, const ClaimContext& claim) {
  w.u8(static_cast<std::uint8_t>(claim.origin));
  w.u64(claim.publisher.value());
  w.u64(claim.boot.value());
  w.u64(claim.session.value());
  w.u64(claim.incarnation.value());
  w.u64(claim.epoch.value());
  w.u64(claim.attempt.value());
  w.u64(claim.trace.value());
  w.u64(claim.sequence);
  return Status::ok();
}

Status read_claim(ByteReader& r, ClaimContext& claim) {
  std::uint8_t origin = 0;
  if (!r.u8(origin)) return malformed("claim is truncated");
  if (origin > static_cast<std::uint8_t>(OriginKind::Replay)) return malformed("claim origin is invalid");
  claim.origin = static_cast<OriginKind>(origin);
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("claim is truncated");
  claim.publisher = PublisherId::from_value(value);
  if (!r.u64(value)) return malformed("claim is truncated");
  claim.boot = BootId::from_value(value);
  if (!r.u64(value)) return malformed("claim is truncated");
  claim.session = SessionNonce::from_value(value);
  if (!r.u64(value)) return malformed("claim is truncated");
  claim.incarnation = CoordinatorIncarnation::from_value(value);
  if (!r.u64(value)) return malformed("claim is truncated");
  claim.epoch = FabricEpoch::from_value(value);
  if (!r.u64(value)) return malformed("claim is truncated");
  claim.attempt = AttemptId::from_value(value);
  if (!r.u64(value)) return malformed("claim is truncated");
  claim.trace = TraceId::from_value(value);
  if (!r.u64(claim.sequence)) return malformed("claim is truncated");
  return Status::ok();
}

Status write_explanation(ByteWriter& w, const Explanation& explanation) {
  w.u32(static_cast<std::uint32_t>(explanation.constraints().size()));
  for (const auto& constraint : explanation.constraints()) {
    w.u8(static_cast<std::uint8_t>(constraint.kind));
    if (!w.text(constraint.subject)) return oversized("explanation subject is too long");
    w.u64(constraint.subject_id);
    w.u64(constraint.required.value());
    w.u64(constraint.available.value());
    w.u64(constraint.observed.value());
    w.u64(constraint.authoritative.value());
    if (!w.text(constraint.detail)) return oversized("explanation detail is too long");
  }
  w.boolean(explanation.truncated());
  w.u32(explanation.omitted());
  return Status::ok();
}

Status read_explanation(ByteReader& r, Explanation& explanation) {
  std::uint32_t count = 0;
  if (!r.u32(count)) return malformed("explanation is truncated");
  if (count > limits::max_explanation_constraints) return oversized("explanation has too many constraints");
  for (std::uint32_t i = 0; i < count; ++i) {
    BindingConstraint constraint;
    std::uint8_t kind = 0;
    if (!r.u8(kind)) return malformed("explanation is truncated");
    if (kind > static_cast<std::uint8_t>(BindingConstraintKind::Durability)) {
      return malformed("explanation constraint kind is invalid");
    }
    constraint.kind = static_cast<BindingConstraintKind>(kind);
    if (!r.text(constraint.subject)) return malformed("explanation subject is invalid");
    if (!r.u64(constraint.subject_id)) return malformed("explanation is truncated");
    std::uint64_t value = 0;
    if (!r.u64(value)) return malformed("explanation is truncated");
    constraint.required = Rate::from_value(value);
    if (!r.u64(value)) return malformed("explanation is truncated");
    constraint.available = Rate::from_value(value);
    if (!r.u64(value)) return malformed("explanation is truncated");
    constraint.observed = Generation::from_value(value);
    if (!r.u64(value)) return malformed("explanation is truncated");
    constraint.authoritative = Generation::from_value(value);
    if (!r.text(constraint.detail)) return malformed("explanation detail is invalid");
    explanation.add(std::move(constraint));
  }
  bool truncated = false;
  if (!r.boolean(truncated)) return malformed("explanation is truncated");
  std::uint32_t omitted = 0;
  if (!r.u32(omitted)) return malformed("explanation is truncated");
  if (truncated) explanation.add(BindingConstraint{});
  (void)omitted;
  return Status::ok();
}

Status write_authority(ByteWriter& w, const AuthorityVector& vector) {
  w.u32(static_cast<std::uint32_t>(vector.refs().size()));
  for (const auto& ref : vector.refs()) {
    w.u8(static_cast<std::uint8_t>(ref.kind));
    w.u64(ref.subject);
    w.u64(ref.generation.value());
    w.u64(ref.epoch.value());
  }
  w.boolean(vector.truncated());
  return Status::ok();
}

Status read_authority(ByteReader& r, AuthorityVector& vector) {
  std::uint32_t count = 0;
  if (!r.u32(count)) return malformed("authority vector is truncated");
  if (count > limits::max_authority_refs) return oversized("authority vector is too large");
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint8_t kind = 0;
    if (!r.u8(kind)) return malformed("authority vector is truncated");
    if (kind > static_cast<std::uint8_t>(AuthorityKind::PriorityClass)) {
      return malformed("authority kind is invalid");
    }
    std::uint64_t subject = 0;
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0;
    if (!r.u64(subject) || !r.u64(generation) || !r.u64(epoch)) {
      return malformed("authority vector is truncated");
    }
    vector.add(static_cast<AuthorityKind>(kind), subject, Generation::from_value(generation),
               FabricEpoch::from_value(epoch));
  }
  bool truncated = false;
  if (!r.boolean(truncated)) return malformed("authority vector is truncated");
  return Status::ok();
}

}  // namespace

ByteBuffer encode(const HelloMessage& message) {
  ByteWriter w(64);
  w.u16(message.protocol_version);
  w.u64(message.publisher.value());
  w.u64(message.boot.value());
  w.u32(message.max_frame);
  w.u64(message.client_nonce);
  return std::move(w).take();
}

Expected<HelloMessage> decode_hello(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  HelloMessage out;
  if (!r.u16(out.protocol_version)) return malformed("hello is truncated");
  if (out.protocol_version != protocol_version) {
    return Status::error(StatusCode::Unsupported, "protocol version mismatch");
  }
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("hello is truncated");
  out.publisher = PublisherId::from_value(value);
  if (!r.u64(value)) return malformed("hello is truncated");
  out.boot = BootId::from_value(value);
  if (!r.u32(out.max_frame)) return malformed("hello is truncated");
  if (out.max_frame > limits::max_frame_bytes) return oversized("hello frame bound is too large");
  if (!r.u64(out.client_nonce)) return malformed("hello is truncated");
  Status s = finish(r);
  if (!s.is_ok()) return s;
  return out;
}

ByteBuffer encode(const WelcomeMessage& message) {
  ByteWriter w(64);
  w.boolean(message.accepted);
  w.u64(message.epoch.value());
  w.u64(message.incarnation.value());
  w.u64(message.session.value());
  (void)w.text(message.reason);
  return std::move(w).take();
}

Expected<WelcomeMessage> decode_welcome(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  WelcomeMessage out;
  if (!r.boolean(out.accepted)) return malformed("welcome is truncated");
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("welcome is truncated");
  out.epoch = FabricEpoch::from_value(value);
  if (!r.u64(value)) return malformed("welcome is truncated");
  out.incarnation = CoordinatorIncarnation::from_value(value);
  if (!r.u64(value)) return malformed("welcome is truncated");
  out.session = SessionNonce::from_value(value);
  if (!r.text(out.reason)) return malformed("welcome reason is invalid");
  Status s = finish(r);
  if (!s.is_ok()) return s;
  return out;
}

ByteBuffer encode(const AdmitMessage& message) {
  ByteWriter w(512);
  (void)write_claim(w, message.claim);
  const AdmissionRequest& request = message.request;
  w.u64(request.request.value());
  w.u64(request.demand.value());
  w.u64(request.demand_generation.value());
  w.u64(request.attempt.value());
  w.u64(request.rate.minimum.value());
  w.u64(request.rate.desired.value());
  w.u64(request.rate.maximum.value());
  w.u64(request.qos.value());
  w.u64(request.qos_generation.value());
  w.u64(request.priority.value());
  w.u64(request.priority_generation.value());
  w.u64(request.maximum_latency.value());
  w.u32(static_cast<std::uint32_t>(request.resource_bindings.size()));
  for (const auto& binding : request.resource_bindings) {
    w.u64(binding.resource.value());
    w.u64(binding.generation.value());
  }
  w.u32(static_cast<std::uint32_t>(request.path_candidates.size()));
  for (const auto& candidate : request.path_candidates) {
    w.u64(candidate.path.value());
    w.u64(candidate.path_authority_generation.value());
  }
  w.u64(request.required_path.value());
  w.u32(static_cast<std::uint32_t>(request.reservation_refs.size()));
  for (const auto& ref : request.reservation_refs) {
    w.u64(ref.reservation.value());
    w.u64(ref.generation.value());
  }
  w.u64(request.fairness_group.value());
  w.u64(request.tenant.value());
  w.boolean(request.preemptible);
  w.boolean(request.requests_obligation_preemption);
  w.boolean(request.allow_path_substitution);
  w.u8(static_cast<std::uint8_t>(request.contention));
  w.u8(static_cast<std::uint8_t>(request.degradation));
  w.u64(request.deadline_ticks);
  w.u64(request.effective_tick);
  w.u64(request.effective_interval.value());
  w.u64(request.expected.capacity_snapshot.value());
  w.u64(request.expected.capacity_generation.value());
  w.u64(request.expected.reservation_snapshot.value());
  w.u64(request.expected.reservation_generation.value());
  w.u64(request.expected.path_authority_generation.value());
  w.u64(request.expected.policy_generation.value());
  w.u64(request.expected.qos_catalog_generation.value());
  w.u64(request.expected.priority_catalog_generation.value());
  w.u8(static_cast<std::uint8_t>(request.provenance.origin));
  w.u64(request.provenance.publisher.value());
  w.u64(request.provenance.boot.value());
  w.u64(request.provenance.incarnation.value());
  w.u64(request.provenance.epoch.value());
  w.u64(request.provenance.attempt.value());
  w.u64(request.provenance.trace.value());
  w.u64(request.provenance.sequence);
  return std::move(w).take();
}

Expected<AdmitMessage> decode_admit(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  AdmitMessage out;
  Status s = read_claim(r, out.claim);
  if (!s.is_ok()) return s;
  AdmissionRequest& request = out.request;
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("admit is truncated");
  request.request = AdmissionRequestId::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.demand = DemandId::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.demand_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.attempt = AttemptId::from_value(value);
  std::uint64_t minimum = 0;
  std::uint64_t desired = 0;
  std::uint64_t maximum = 0;
  if (!r.u64(minimum) || !r.u64(desired) || !r.u64(maximum)) return malformed("admit is truncated");
  request.rate.minimum = Rate::from_value(minimum);
  request.rate.desired = Rate::from_value(desired);
  request.rate.maximum = Rate::from_value(maximum);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.qos = QoSClassId::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.qos_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.priority = PriorityClassId::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.priority_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.maximum_latency = Latency::from_value(value);

  std::uint32_t count = 0;
  if (!r.u32(count)) return malformed("admit is truncated");
  if (count > limits::max_resource_bindings) return oversized("too many resource bindings");
  request.resource_bindings.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    ResourceBinding binding;
    std::uint64_t resource = 0;
    std::uint64_t generation = 0;
    if (!r.u64(resource) || !r.u64(generation)) return malformed("admit is truncated");
    binding.resource = ResourceId::from_value(resource);
    binding.generation = Generation::from_value(generation);
    request.resource_bindings.push_back(binding);
  }
  if (!r.u32(count)) return malformed("admit is truncated");
  if (count > limits::max_path_candidates) return oversized("too many path candidates");
  request.path_candidates.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    PathCandidate candidate;
    std::uint64_t path = 0;
    std::uint64_t generation = 0;
    if (!r.u64(path) || !r.u64(generation)) return malformed("admit is truncated");
    candidate.path = PathId::from_value(path);
    candidate.path_authority_generation = Generation::from_value(generation);
    request.path_candidates.push_back(candidate);
  }
  if (!r.u64(value)) return malformed("admit is truncated");
  request.required_path = PathId::from_value(value);
  if (!r.u32(count)) return malformed("admit is truncated");
  if (count > limits::max_reservation_refs) return oversized("too many reservation references");
  request.reservation_refs.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    ReservationRef ref;
    std::uint64_t reservation = 0;
    std::uint64_t generation = 0;
    if (!r.u64(reservation) || !r.u64(generation)) return malformed("admit is truncated");
    ref.reservation = ReservationId::from_value(reservation);
    ref.generation = Generation::from_value(generation);
    request.reservation_refs.push_back(ref);
  }
  if (!r.u64(value)) return malformed("admit is truncated");
  request.fairness_group = FairnessGroupId::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.tenant = TenantId::from_value(value);
  if (!r.boolean(request.preemptible)) return malformed("admit is truncated");
  if (!r.boolean(request.requests_obligation_preemption)) return malformed("admit is truncated");
  if (!r.boolean(request.allow_path_substitution)) return malformed("admit is truncated");
  std::uint8_t contention = 0;
  std::uint8_t degradation = 0;
  if (!r.u8(contention) || !r.u8(degradation)) return malformed("admit is truncated");
  if (contention > static_cast<std::uint8_t>(ContentionPreference::Reject)) {
    return malformed("contention preference is invalid");
  }
  if (degradation > static_cast<std::uint8_t>(DegradePreference::DownToMinimum)) {
    return malformed("degradation preference is invalid");
  }
  request.contention = static_cast<ContentionPreference>(contention);
  request.degradation = static_cast<DegradePreference>(degradation);
  if (!r.u64(request.deadline_ticks)) return malformed("admit is truncated");
  if (!r.u64(request.effective_tick)) return malformed("admit is truncated");
  if (!r.u64(value)) return malformed("admit is truncated");
  request.effective_interval = Duration::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.expected.capacity_snapshot = CapacitySnapshotId::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.expected.capacity_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.expected.reservation_snapshot = ReservationSnapshotId::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.expected.reservation_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.expected.path_authority_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.expected.policy_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.expected.qos_catalog_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.expected.priority_catalog_generation = Generation::from_value(value);
  std::uint8_t origin = 0;
  if (!r.u8(origin)) return malformed("admit is truncated");
  if (origin > static_cast<std::uint8_t>(OriginKind::Replay)) return malformed("origin is invalid");
  request.provenance.origin = static_cast<OriginKind>(origin);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.provenance.publisher = PublisherId::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.provenance.boot = BootId::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.provenance.incarnation = CoordinatorIncarnation::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.provenance.epoch = FabricEpoch::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.provenance.attempt = AttemptId::from_value(value);
  if (!r.u64(value)) return malformed("admit is truncated");
  request.provenance.trace = TraceId::from_value(value);
  if (!r.u64(request.provenance.sequence)) return malformed("admit is truncated");

  Status s2 = finish(r);
  if (!s2.is_ok()) return s2;
  return out;
}

ByteBuffer encode(const RevalidateMessage& message) {
  ByteWriter w(96);
  (void)write_claim(w, message.claim);
  w.u64(message.decision.value());
  return std::move(w).take();
}

Expected<RevalidateMessage> decode_revalidate(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  RevalidateMessage out;
  Status s = read_claim(r, out.claim);
  if (!s.is_ok()) return s;
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("revalidate is truncated");
  out.decision = AdmissionDecisionId::from_value(value);
  Status s2 = finish(r);
  if (!s2.is_ok()) return s2;
  return out;
}

ByteBuffer encode(const RevokeMessage& message) {
  ByteWriter w(96);
  (void)write_claim(w, message.claim);
  w.u64(message.decision.value());
  w.u8(static_cast<std::uint8_t>(message.reason));
  return std::move(w).take();
}

Expected<RevokeMessage> decode_revoke(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  RevokeMessage out;
  Status s = read_claim(r, out.claim);
  if (!s.is_ok()) return s;
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("revoke is truncated");
  out.decision = AdmissionDecisionId::from_value(value);
  std::uint8_t reason = 0;
  if (!r.u8(reason)) return malformed("revoke is truncated");
  if (reason > static_cast<std::uint8_t>(RevocationReason::Expired)) {
    return malformed("revocation reason is invalid");
  }
  out.reason = static_cast<RevocationReason>(reason);
  Status s2 = finish(r);
  if (!s2.is_ok()) return s2;
  return out;
}

ByteBuffer encode(const ErrorMessage& message) {
  ByteWriter w(96);
  w.u16(message.code);
  (void)w.text(message.text);
  return std::move(w).take();
}

Expected<ErrorMessage> decode_error(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  ErrorMessage out;
  if (!r.u16(out.code)) return malformed("error is truncated");
  if (!r.text(out.text)) return malformed("error text is invalid");
  Status s = finish(r);
  if (!s.is_ok()) return s;
  return out;
}

ByteBuffer encode(const AdmissionDecision& decision) {
  ByteWriter w(512);
  w.u64(decision.decision.value());
  w.u64(decision.audit.value());
  w.u8(static_cast<std::uint8_t>(decision.outcome));
  w.u64(decision.request.value());
  w.u64(decision.demand.value());
  w.u64(decision.demand_generation.value());
  w.u64(decision.attempt.value());
  w.u64(decision.trace.value());
  w.u64(decision.epoch.value());
  w.u64(decision.incarnation.value());
  w.u64(decision.decided_tick);
  w.u64(decision.selected_path.value());
  w.u64(decision.selected_path_generation.value());
  w.u64(decision.granted.value());
  w.u64(decision.minimum_guaranteed.value());
  w.u64(decision.desired.value());
  w.u64(decision.maximum.value());
  w.u64(decision.holding_interval.value());
  w.u64(decision.bound_capacity_generation.value());
  w.boolean(decision.revalidation_required);
  w.boolean(decision.idempotent_replay);
  w.u64(decision.request_fingerprint);
  w.u32(static_cast<std::uint32_t>(decision.effective.size()));
  for (const auto& entry : decision.effective) {
    w.u64(entry.resource.value());
    w.u64(entry.generation.value());
    w.u64(entry.usable.value());
    w.u64(entry.headroom.value());
    w.u64(entry.obligations.value());
    w.u64(entry.admitted.value());
    w.u64(entry.available_for_new.value());
  }
  w.boolean(decision.effective_truncated);
  (void)write_explanation(w, decision.explanation);
  (void)write_authority(w, decision.authority);
  return std::move(w).take();
}

Expected<AdmissionDecision> decode_decision(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  AdmissionDecision out;
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("decision is truncated");
  out.decision = AdmissionDecisionId::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.audit = AuditSequence::from_value(value);
  std::uint8_t outcome = 0;
  if (!r.u8(outcome)) return malformed("decision is truncated");
  if (outcome > static_cast<std::uint8_t>(AdmissionOutcome::FencedClaimant)) {
    return malformed("decision outcome is invalid");
  }
  out.outcome = static_cast<AdmissionOutcome>(outcome);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.request = AdmissionRequestId::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.demand = DemandId::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.demand_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.attempt = AttemptId::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.trace = TraceId::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.epoch = FabricEpoch::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.incarnation = CoordinatorIncarnation::from_value(value);
  if (!r.u64(out.decided_tick)) return malformed("decision is truncated");
  if (!r.u64(value)) return malformed("decision is truncated");
  out.selected_path = PathId::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.selected_path_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.granted = Rate::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.minimum_guaranteed = Rate::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.desired = Rate::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.maximum = Rate::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.holding_interval = Duration::from_value(value);
  if (!r.u64(value)) return malformed("decision is truncated");
  out.bound_capacity_generation = Generation::from_value(value);
  if (!r.boolean(out.revalidation_required)) return malformed("decision is truncated");
  if (!r.boolean(out.idempotent_replay)) return malformed("decision is truncated");
  if (!r.u64(out.request_fingerprint)) return malformed("decision is truncated");
  std::uint32_t count = 0;
  if (!r.u32(count)) return malformed("decision is truncated");
  if (count > limits::max_effective_resources) return oversized("decision effective list is too large");
  out.effective.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    EffectiveResource entry;
    std::uint64_t resource = 0;
    std::uint64_t generation = 0;
    if (!r.u64(resource) || !r.u64(generation)) return malformed("decision is truncated");
    entry.resource = ResourceId::from_value(resource);
    entry.generation = Generation::from_value(generation);
    std::uint64_t usable = 0;
    std::uint64_t headroom = 0;
    std::uint64_t obligations = 0;
    std::uint64_t admitted = 0;
    std::uint64_t available = 0;
    if (!r.u64(usable) || !r.u64(headroom) || !r.u64(obligations) || !r.u64(admitted) ||
        !r.u64(available)) {
      return malformed("decision is truncated");
    }
    entry.usable = Rate::from_value(usable);
    entry.headroom = Rate::from_value(headroom);
    entry.obligations = Rate::from_value(obligations);
    entry.admitted = Rate::from_value(admitted);
    entry.available_for_new = Rate::from_value(available);
    out.effective.push_back(entry);
  }
  if (!r.boolean(out.effective_truncated)) return malformed("decision is truncated");
  out.explanation.configure(limits::max_explanation_constraints, limits::max_explanation_bytes);
  Status s = read_explanation(r, out.explanation);
  if (!s.is_ok()) return s;
  Status s2 = read_authority(r, out.authority);
  if (!s2.is_ok()) return s2;
  Status s3 = finish(r);
  if (!s3.is_ok()) return s3;
  return out;
}

ByteBuffer encode(const RevalidationResult& result) {
  ByteWriter w(384);
  w.u64(result.decision.value());
  w.u8(static_cast<std::uint8_t>(result.original_outcome));
  w.u8(static_cast<std::uint8_t>(result.state));
  w.u8(static_cast<std::uint8_t>(result.reason));
  w.u64(result.granted.value());
  w.u64(result.tick);
  w.boolean(result.ledger_released);
  (void)write_explanation(w, result.explanation);
  (void)write_authority(w, result.authority);
  return std::move(w).take();
}

Expected<RevalidationResult> decode_revalidation(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  RevalidationResult out;
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("revalidation is truncated");
  out.decision = AdmissionDecisionId::from_value(value);
  std::uint8_t outcome = 0;
  std::uint8_t state = 0;
  std::uint8_t reason = 0;
  if (!r.u8(outcome) || !r.u8(state) || !r.u8(reason)) return malformed("revalidation is truncated");
  if (outcome > static_cast<std::uint8_t>(AdmissionOutcome::FencedClaimant)) {
    return malformed("revalidation outcome is invalid");
  }
  if (state > static_cast<std::uint8_t>(RevalidationState::Unknown)) {
    return malformed("revalidation state is invalid");
  }
  if (reason > static_cast<std::uint8_t>(RevocationReason::Expired)) {
    return malformed("revalidation reason is invalid");
  }
  out.original_outcome = static_cast<AdmissionOutcome>(outcome);
  out.state = static_cast<RevalidationState>(state);
  out.reason = static_cast<RevocationReason>(reason);
  if (!r.u64(value)) return malformed("revalidation is truncated");
  out.granted = Rate::from_value(value);
  if (!r.u64(out.tick)) return malformed("revalidation is truncated");
  if (!r.boolean(out.ledger_released)) return malformed("revalidation is truncated");
  out.explanation.configure(limits::max_explanation_constraints, limits::max_explanation_bytes);
  Status s = read_explanation(r, out.explanation);
  if (!s.is_ok()) return s;
  Status s2 = read_authority(r, out.authority);
  if (!s2.is_ok()) return s2;
  Status s3 = finish(r);
  if (!s3.is_ok()) return s3;
  return out;
}

ByteBuffer encode(const FabricStatus& status) {
  ByteWriter w(256);
  w.u8(static_cast<std::uint8_t>(status.readiness));
  w.u64(status.epoch.value());
  w.u64(status.incarnation.value());
  w.u64(status.tick);
  w.boolean(status.has_policy);
  w.boolean(status.has_capacity);
  w.boolean(status.has_reservations);
  w.boolean(status.has_paths);
  w.boolean(status.has_qos);
  w.boolean(status.has_priority);
  w.u64(status.policy_generation.value());
  w.u64(status.capacity_generation.value());
  w.u64(status.reservation_generation.value());
  w.u64(status.path_generation.value());
  w.u64(status.qos_generation.value());
  w.u64(status.priority_generation.value());
  w.u64(status.capacity_snapshot.value());
  w.u64(status.reservation_snapshot.value());
  w.u64(status.audit_sequence.value());
  w.u64(status.decisions);
  w.u64(status.admissions);
  w.u64(status.refusals);
  w.u64(status.deferrals);
  w.u64(status.idempotent_replays);
  w.u64(status.conflicts);
  w.u64(status.fences);
  w.u64(status.stale_refusals);
  w.u32(static_cast<std::uint32_t>(status.ledger_entries));
  w.u32(static_cast<std::uint32_t>(status.live_decisions));
  w.u32(static_cast<std::uint32_t>(status.live_sessions));
  w.u64(status.total_admitted.value());
  w.boolean(status.durable);
  w.boolean(status.reconciled);
  return std::move(w).take();
}

Expected<FabricStatus> decode_status(std::span<const std::byte> bytes) {
  ByteReader r(bytes);
  FabricStatus out;
  std::uint8_t readiness = 0;
  if (!r.u8(readiness)) return malformed("status is truncated");
  if (readiness > static_cast<std::uint8_t>(EngineReadiness::Ready)) {
    return malformed("status readiness is invalid");
  }
  out.readiness = static_cast<EngineReadiness>(readiness);
  std::uint64_t value = 0;
  if (!r.u64(value)) return malformed("status is truncated");
  out.epoch = FabricEpoch::from_value(value);
  if (!r.u64(value)) return malformed("status is truncated");
  out.incarnation = CoordinatorIncarnation::from_value(value);
  if (!r.u64(out.tick)) return malformed("status is truncated");
  if (!r.boolean(out.has_policy)) return malformed("status is truncated");
  if (!r.boolean(out.has_capacity)) return malformed("status is truncated");
  if (!r.boolean(out.has_reservations)) return malformed("status is truncated");
  if (!r.boolean(out.has_paths)) return malformed("status is truncated");
  if (!r.boolean(out.has_qos)) return malformed("status is truncated");
  if (!r.boolean(out.has_priority)) return malformed("status is truncated");
  if (!r.u64(value)) return malformed("status is truncated");
  out.policy_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("status is truncated");
  out.capacity_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("status is truncated");
  out.reservation_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("status is truncated");
  out.path_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("status is truncated");
  out.qos_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("status is truncated");
  out.priority_generation = Generation::from_value(value);
  if (!r.u64(value)) return malformed("status is truncated");
  out.capacity_snapshot = CapacitySnapshotId::from_value(value);
  if (!r.u64(value)) return malformed("status is truncated");
  out.reservation_snapshot = ReservationSnapshotId::from_value(value);
  if (!r.u64(value)) return malformed("status is truncated");
  out.audit_sequence = AuditSequence::from_value(value);
  if (!r.u64(out.decisions)) return malformed("status is truncated");
  if (!r.u64(out.admissions)) return malformed("status is truncated");
  if (!r.u64(out.refusals)) return malformed("status is truncated");
  if (!r.u64(out.deferrals)) return malformed("status is truncated");
  if (!r.u64(out.idempotent_replays)) return malformed("status is truncated");
  if (!r.u64(out.conflicts)) return malformed("status is truncated");
  if (!r.u64(out.fences)) return malformed("status is truncated");
  if (!r.u64(out.stale_refusals)) return malformed("status is truncated");
  std::uint32_t small = 0;
  if (!r.u32(small)) return malformed("status is truncated");
  out.ledger_entries = small;
  if (!r.u32(small)) return malformed("status is truncated");
  out.live_decisions = small;
  if (!r.u32(small)) return malformed("status is truncated");
  out.live_sessions = small;
  if (!r.u64(value)) return malformed("status is truncated");
  out.total_admitted = Rate::from_value(value);
  if (!r.boolean(out.durable)) return malformed("status is truncated");
  if (!r.boolean(out.reconciled)) return malformed("status is truncated");
  Status s = finish(r);
  if (!s.is_ok()) return s;
  return out;
}

}  // namespace naf::ipc

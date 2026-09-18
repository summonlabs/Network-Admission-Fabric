// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Adversarial input: malformed, contradictory, oversized, truncated and
// corrupt. Nothing here may crash, wrap, silently default or partially apply.
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "support/test_framework.hpp"

using namespace naftest;

namespace {

bool rejected(const naf::Status& status, naf::StatusCode code) {
  return !status.is_ok() && status.code() == code;
}

naf::AdmissionRequest minimal_request() {
  naf::AdmissionRequest request;
  request.request = naf::AdmissionRequestId::from_value(1);
  request.demand = naf::DemandId::from_value(1);
  request.demand_generation = naf::Generation::first();
  request.attempt = naf::AttemptId::from_value(1);
  request.rate.minimum = naf::Rate::from_value(10);
  request.rate.desired = naf::Rate::from_value(20);
  request.rate.maximum = naf::Rate::from_value(30);
  request.qos = naf::QoSClassId::from_value(1);
  request.qos_generation = naf::Generation::first();
  request.priority = naf::PriorityClassId::from_value(1);
  request.priority_generation = naf::Generation::first();
  naf::ResourceBinding binding;
  binding.resource = naf::ResourceId::from_value(1);
  binding.generation = naf::Generation::first();
  request.resource_bindings.push_back(binding);
  return request;
}

}  // namespace

NAF_TEST(adversarial, capacity_snapshots_are_validated) {
  naf::CapacitySnapshot snapshot;
  snapshot.snapshot = naf::CapacitySnapshotId::from_value(1);
  snapshot.generation = naf::Generation::first();
  snapshot.epoch = naf::FabricEpoch::first();
  NAF_CHECK(naf::validate(snapshot).is_ok());

  naf::CapacitySnapshot no_id = snapshot;
  no_id.snapshot = naf::CapacitySnapshotId{};
  NAF_CHECK(rejected(naf::validate(no_id), naf::StatusCode::MalformedInput));

  naf::CapacitySnapshot unknown_generation = snapshot;
  unknown_generation.generation = naf::Generation::unknown();
  NAF_CHECK(rejected(naf::validate(unknown_generation), naf::StatusCode::MalformedInput));

  naf::CapacitySnapshot bad_epoch = snapshot;
  bad_epoch.epoch = naf::FabricEpoch::none();
  NAF_CHECK(rejected(naf::validate(bad_epoch), naf::StatusCode::MalformedInput));

  naf::CapacitySnapshot duplicated = snapshot;
  for (int i = 0; i < 2; ++i) {
    naf::ResourceCapacity entry;
    entry.resource = naf::ResourceId::from_value(1);
    entry.generation = naf::Generation::first();
    entry.evidence = naf::EvidenceState::Known;
    entry.usable = naf::Rate::from_value(100);
    duplicated.resources.push_back(entry);
  }
  NAF_CHECK(rejected(naf::validate(duplicated), naf::StatusCode::MalformedInput));

  naf::CapacitySnapshot unsorted = snapshot;
  for (const std::uint64_t id : {2ull, 1ull}) {
    naf::ResourceCapacity entry;
    entry.resource = naf::ResourceId::from_value(id);
    entry.generation = naf::Generation::first();
    entry.evidence = naf::EvidenceState::Known;
    entry.usable = naf::Rate::from_value(100);
    unsorted.resources.push_back(entry);
  }
  NAF_CHECK(rejected(naf::validate(unsorted), naf::StatusCode::MalformedInput));

  // UNKNOWN evidence carrying numbers would let a placeholder become authority.
  naf::CapacitySnapshot lying = snapshot;
  naf::ResourceCapacity entry;
  entry.resource = naf::ResourceId::from_value(1);
  entry.generation = naf::Generation::first();
  entry.evidence = naf::EvidenceState::Unknown;
  entry.usable = naf::Rate::from_value(5);
  lying.resources.push_back(entry);
  NAF_CHECK(rejected(naf::validate(lying), naf::StatusCode::MalformedInput));

  naf::CapacitySnapshot contradictory = snapshot;
  entry.evidence = naf::EvidenceState::Known;
  entry.usable = naf::Rate::from_value(10);
  entry.mandatory_headroom = naf::Rate::from_value(11);
  contradictory.resources.push_back(entry);
  NAF_CHECK(rejected(naf::validate(contradictory), naf::StatusCode::CapacityViolation));

  naf::CapacitySnapshot oversized = snapshot;
  for (std::uint64_t i = 0; i < naf::limits::max_resources + 1; ++i) {
    naf::ResourceCapacity item;
    item.resource = naf::ResourceId::from_value(i + 1);
    item.generation = naf::Generation::first();
    item.evidence = naf::EvidenceState::Known;
    oversized.resources.push_back(item);
  }
  NAF_CHECK(rejected(naf::validate(oversized), naf::StatusCode::OversizedInput));
}

NAF_TEST(adversarial, reservation_snapshots_are_validated) {
  naf::ReservationSnapshot snapshot;
  snapshot.snapshot = naf::ReservationSnapshotId::from_value(1);
  snapshot.generation = naf::Generation::first();
  snapshot.epoch = naf::FabricEpoch::first();
  NAF_CHECK(naf::validate(snapshot).is_ok());

  auto obligation = [](std::uint64_t reservation, std::uint64_t resource, std::uint64_t amount) {
    naf::ProtectedObligation item;
    item.reservation = naf::ReservationId::from_value(reservation);
    item.reservation_generation = naf::Generation::first();
    item.resource = naf::ResourceId::from_value(resource);
    item.reserved = naf::Rate::from_value(amount);
    return item;
  };

  naf::ReservationSnapshot duplicates = snapshot;
  duplicates.obligations.push_back(obligation(1, 1, 10));
  duplicates.obligations.push_back(obligation(1, 1, 10));
  NAF_CHECK(rejected(naf::validate(duplicates), naf::StatusCode::MalformedInput));

  naf::ReservationSnapshot overflow = snapshot;
  overflow.obligations.push_back(obligation(1, 1, ~std::uint64_t{0}));
  overflow.obligations.push_back(obligation(2, 1, 1));
  NAF_CHECK(rejected(naf::validate(overflow), naf::StatusCode::ArithmeticOverflow));

  naf::ReservationSnapshot unknown_reservation = snapshot;
  naf::ProtectedObligation bad = obligation(1, 1, 10);
  bad.reservation = naf::ReservationId{};
  unknown_reservation.obligations.push_back(bad);
  NAF_CHECK(rejected(naf::validate(unknown_reservation), naf::StatusCode::MalformedInput));
}

NAF_TEST(adversarial, catalogs_are_validated) {
  naf::QoSClassCatalog qos;
  qos.generation = naf::Generation::first();
  qos.epoch = naf::FabricEpoch::first();
  naf::QoSClassFact fact;
  fact.qos = naf::QoSClassId::from_value(1);
  fact.generation = naf::Generation::first();
  fact.minimum_rate = naf::Rate::from_value(100);
  fact.maximum_rate = naf::Rate::from_value(50);
  qos.classes.push_back(fact);
  NAF_CHECK(rejected(naf::validate(qos), naf::StatusCode::MalformedInput));

  qos.classes[0].minimum_rate = naf::Rate::from_value(10);
  qos.classes[0].maximum_rate = naf::Rate{};
  NAF_CHECK(rejected(naf::validate(qos), naf::StatusCode::MalformedInput));

  naf::PriorityClassCatalog priorities;
  priorities.generation = naf::Generation::first();
  priorities.epoch = naf::FabricEpoch::first();
  for (std::uint64_t i = 0; i < 2; ++i) {
    naf::PriorityClassFact item;
    item.priority = naf::PriorityClassId::from_value(i + 1);
    item.generation = naf::Generation::first();
    item.rank = 7;
    priorities.classes.push_back(item);
  }
  NAF_CHECK(rejected(naf::validate(priorities), naf::StatusCode::MalformedInput));

  naf::PathCatalog paths;
  paths.path_authority_generation = naf::Generation::first();
  paths.epoch = naf::FabricEpoch::first();
  naf::PathAdmissionFact path;
  path.path = naf::PathId::from_value(1);
  path.path_authority_generation = naf::Generation::first();
  path.resources.push_back(naf::ResourceId::from_value(2));
  path.resources.push_back(naf::ResourceId::from_value(1));
  paths.paths.push_back(path);
  NAF_CHECK(rejected(naf::validate(paths), naf::StatusCode::MalformedInput));
}

NAF_TEST(adversarial, contradictory_rate_bounds_are_refused) {
  Fabric fabric;
  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 200, 300);
  request.rate.minimum = naf::Rate::from_value(400);
  request.rate.desired = naf::Rate::from_value(200);
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectPolicy);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::RequestContract);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);

  naf::AdmissionRequest degenerate = fabric.make_request(2, 2, 0, 0, 0);
  NAF_CHECK(fabric.admit(degenerate) == naf::AdmissionOutcome::RejectPolicy);
}

NAF_TEST(adversarial, structurally_invalid_requests_are_refused) {
  Fabric fabric;
  const auto expect_contract = [&](const naf::AdmissionRequest& request, const char* what) {
    auto decision = fabric.engine().admit_local(request);
    if (!decision.has_value()) {
      naftest::Registry::instance().fail(__FILE__, __LINE__,
                                         std::string(what) + " returned an error status");
      return;
    }
    if (decision.value().outcome != naf::AdmissionOutcome::RejectPolicy) {
      naftest::Registry::instance().fail(
          __FILE__, __LINE__,
          std::string(what) + " produced " + std::string(naf::to_string(decision.value().outcome)));
      return;
    }
    naftest::Registry::instance().count_check();
  };

  {
    naf::AdmissionRequest request = minimal_request();
    request.request = naf::AdmissionRequestId{};
    expect_contract(request, "unknown request id");
  }
  {
    naf::AdmissionRequest request = minimal_request();
    request.demand = naf::DemandId{};
    expect_contract(request, "unknown demand id");
  }
  {
    naf::AdmissionRequest request = minimal_request();
    request.attempt = naf::AttemptId{};
    expect_contract(request, "unknown attempt id");
  }
  {
    naf::AdmissionRequest request = minimal_request();
    request.demand_generation = naf::Generation::unknown();
    expect_contract(request, "unknown demand generation");
  }
  {
    naf::AdmissionRequest request = minimal_request();
    request.demand_generation = naf::Generation::from_value(naf::limits::max_generation);
    expect_contract(request, "malformed demand generation");
  }
  {
    naf::AdmissionRequest request = minimal_request();
    request.resource_bindings[0].generation = naf::Generation::unknown();
    expect_contract(request, "unknown resource generation");
  }
  {
    naf::AdmissionRequest request = minimal_request();
    naf::ResourceBinding binding;
    binding.resource = naf::ResourceId::from_value(1);
    binding.generation = naf::Generation::first();
    request.resource_bindings.push_back(binding);
    expect_contract(request, "duplicate resource binding");
  }
  {
    naf::AdmissionRequest request = minimal_request();
    request.resource_bindings.clear();
    expect_contract(request, "no bindings at all");
  }
  {
    naf::AdmissionRequest request = minimal_request();
    request.path_candidates.push_back(naf::PathCandidate{naf::PathId::from_value(1), naf::Generation::first()});
    request.path_candidates.push_back(naf::PathCandidate{naf::PathId::from_value(1), naf::Generation::first()});
    expect_contract(request, "duplicate path candidate");
  }
  {
    naf::AdmissionRequest request = minimal_request();
    request.required_path = naf::PathId::from_value(9);
    request.path_candidates.push_back(naf::PathCandidate{naf::PathId::from_value(1), naf::Generation::first()});
    expect_contract(request, "required path is not a candidate");
  }
  {
    naf::AdmissionRequest request = minimal_request();
    request.provenance.attempt = naf::AttemptId::from_value(77);
    expect_contract(request, "provenance attempt mismatch");
  }
  {
    naf::AdmissionRequest request = minimal_request();
    request.expected.policy_generation = naf::Generation::from_value(naf::limits::max_generation);
    expect_contract(request, "malformed expectation generation");
  }
  {
    naf::AdmissionRequest request = minimal_request();
    request.qos = naf::QoSClassId{};
    expect_contract(request, "unknown qos class");
  }
}

NAF_TEST(adversarial, oversized_collections_are_refused) {
  Fabric fabric;
  naf::AdmissionRequest request = fabric.make_request(1, 1, 10, 20, 30);
  request.resource_bindings.clear();
  for (std::uint64_t i = 0; i < naf::limits::max_resource_bindings + 1; ++i) {
    naf::ResourceBinding binding;
    binding.resource = naf::ResourceId::from_value(i + 1);
    binding.generation = naf::Generation::first();
    request.resource_bindings.push_back(binding);
  }
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectPolicy);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->detail.find("too many") != std::string::npos);
}

NAF_TEST(adversarial, explanations_are_bounded) {
  naf::Explanation explanation;
  explanation.configure(3, 256);
  for (int i = 0; i < 40; ++i) {
    naf::BindingConstraint constraint;
    constraint.kind = naf::BindingConstraintKind::Capacity;
    constraint.subject = "resource-" + std::to_string(i);
    constraint.detail = std::string(200, 'x');
    explanation.add(std::move(constraint));
  }
  NAF_CHECK(explanation.constraints().size() <= 3);
  NAF_CHECK(explanation.truncated());
  NAF_CHECK(explanation.omitted() > 0);
  NAF_CHECK(explanation.render().size() <= 256 + 64);

  // Bounds beyond the hard limits are clamped rather than trusted.
  naf::Explanation wide;
  wide.configure(1u << 20, 1u << 30);
  NAF_CHECK_U64(wide.max_constraints(), naf::limits::max_explanation_constraints);
  NAF_CHECK_U64(wide.max_bytes(), naf::limits::max_explanation_bytes);
  wide.configure(0, 0);
  NAF_CHECK_U64(wide.max_constraints(), 1);
  NAF_CHECK(wide.max_bytes() >= 64);
}

NAF_TEST(adversarial, a_flood_of_bindings_cannot_inflate_a_decision) {
  FabricOptions options;
  options.resources = 64;
  options.usable = 1'000'000;
  options.max_explanation_constraints = 4;
  options.max_explanation_bytes = 256;
  options.max_effective_resources = 4;
  Fabric fabric(options);

  naf::AdmissionRequest request = fabric.make_request(1, 1, 1, 2, 3);
  request.resource_bindings.clear();
  for (std::uint64_t i = 1; i <= 64; ++i) {
    naf::ResourceBinding binding;
    binding.resource = naf::ResourceId::from_value(i);
    binding.generation = naf::Generation::first();
    request.resource_bindings.push_back(binding);
  }
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK(decision.effective.size() <= 4);
  NAF_CHECK(decision.effective_truncated);
  NAF_CHECK(decision.explanation.constraints().size() <= 4);
  NAF_CHECK(decision.explanation.render().size() <= 256 + 64);
}

NAF_TEST(adversarial, publication_conflicts_are_refused) {
  Fabric fabric;

  naf::AdmissionPolicy same_generation = fabric.policy();
  same_generation.allow_degraded = !same_generation.allow_degraded;
  NAF_CHECK(rejected(fabric.engine().publish_policy(same_generation), naf::StatusCode::ConflictIdentity));
  NAF_CHECK(fabric.engine().publish_policy(fabric.policy()).is_ok());

  naf::AdmissionPolicy backwards = fabric.policy();
  backwards.generation = naf::Generation::unknown();
  NAF_CHECK(!fabric.engine().publish_policy(backwards).is_ok());

  naf::CapacitySnapshot wrong_epoch = fabric.capacity();
  wrong_epoch.snapshot = naf::CapacitySnapshotId::from_value(9);
  wrong_epoch.generation = naf::Generation::from_value(9);
  wrong_epoch.epoch = naf::FabricEpoch::from_value(99);
  NAF_CHECK(rejected(fabric.engine().publish_capacity(wrong_epoch), naf::StatusCode::EpochMismatch));

  naf::CapacitySnapshot stale = fabric.capacity();
  stale.snapshot = naf::CapacitySnapshotId::from_value(9);
  stale.generation = naf::Generation::unknown();
  NAF_CHECK(!fabric.engine().publish_capacity(stale).is_ok());

  naf::CapacitySnapshot conflicting = fabric.capacity();
  conflicting.resources[0].usable = naf::Rate::from_value(1);
  NAF_CHECK(rejected(fabric.engine().publish_capacity(conflicting), naf::StatusCode::ConflictIdentity));

  naf::PathCatalog stale_paths = fabric.paths();
  stale_paths.path_authority_generation = naf::Generation::unknown();
  NAF_CHECK(rejected(fabric.engine().publish_paths(stale_paths), naf::StatusCode::MalformedInput));
}

NAF_TEST(adversarial, message_decoding_survives_truncation_at_every_offset) {
  naf::ipc::AdmitMessage message;
  message.claim.origin = naf::OriginKind::Claimant;
  message.claim.publisher = naf::PublisherId::from_value(1);
  message.claim.boot = naf::BootId::from_value(2);
  message.claim.session = naf::SessionNonce::from_value(3);
  message.claim.incarnation = naf::CoordinatorIncarnation::from_value(4);
  message.claim.epoch = naf::FabricEpoch::from_value(5);
  message.claim.attempt = naf::AttemptId::from_value(6);
  message.request = minimal_request();
  message.request.attempt = naf::AttemptId::from_value(6);
  const naf::ByteBuffer encoded = naf::ipc::encode(message);
  NAF_CHECK(encoded.size() > 32);

  auto full = naf::ipc::decode_admit(encoded);
  NAF_REQUIRE(full.has_value());
  NAF_CHECK_U64(full.value().request.rate.desired.value(), 20);

  std::uint64_t accepted = 0;
  for (std::size_t length = 0; length < encoded.size(); ++length) {
    auto decoded = naf::ipc::decode_admit(std::span<const std::byte>(encoded.data(), length));
    if (decoded.has_value()) ++accepted;
  }
  NAF_CHECK_U64(accepted, 0);

  // Trailing bytes are a structural error, not something to ignore.
  naf::ByteBuffer extended = encoded;
  extended.push_back(std::byte{0});
  NAF_CHECK(!naf::ipc::decode_admit(extended).has_value());
}

NAF_TEST(adversarial, message_decoding_survives_byte_corruption) {
  naf::ipc::HelloMessage hello;
  hello.protocol_version = naf::ipc::protocol_version;
  hello.publisher = naf::PublisherId::from_value(11);
  hello.boot = naf::BootId::from_value(22);
  hello.max_frame = 4096;
  const naf::ByteBuffer encoded = naf::ipc::encode(hello);
  NAF_REQUIRE(naf::ipc::decode_hello(encoded).has_value());

  std::uint64_t crashes = 0;
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const std::uint8_t masks[] = {0x01u, 0x80u, 0xFFu};
    for (const std::uint8_t mask : masks) {
      naf::ByteBuffer corrupt = encoded;
      corrupt[index] = static_cast<std::byte>(static_cast<std::uint8_t>(corrupt[index]) ^ mask);
      auto decoded = naf::ipc::decode_hello(corrupt);
      if (decoded.has_value()) {
        // If it decodes it must still be structurally consistent.
        if (decoded.value().max_frame > naf::limits::max_frame_bytes) ++crashes;
      }
    }
  }
  NAF_CHECK_U64(crashes, 0);
}

NAF_TEST(adversarial, decision_payloads_survive_a_wire_roundtrip) {
  Fabric fabric;
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 400, 500));
  const naf::ByteBuffer encoded = naf::ipc::encode(decision);
  auto decoded = naf::ipc::decode_decision(encoded);
  NAF_REQUIRE(decoded.has_value());
  NAF_CHECK(decoded.value().outcome == decision.outcome);
  NAF_CHECK_U64(decoded.value().decision.value(), decision.decision.value());
  NAF_CHECK_U64(decoded.value().granted.value(), decision.granted.value());
  NAF_CHECK_U64(decoded.value().authority.digest(), decision.authority.digest());
  NAF_CHECK(decoded.value().explanation.render() == decision.explanation.render());

  // Decoding refuses a decision payload whose collection counts are absurd.
  naf::ByteWriter writer;
  writer.u64(1);
  writer.u64(1);
  writer.u8(static_cast<std::uint8_t>(naf::AdmissionOutcome::Admit));
  for (int i = 0; i < 12; ++i) writer.u64(0);
  writer.u64(0);
  writer.boolean(false);
  writer.boolean(false);
  writer.u64(0);
  writer.u32(0xFFFFFFFFu);  // effective list claims 4 billion entries
  NAF_CHECK(!naf::ipc::decode_decision(writer.buffer()).has_value());
}

NAF_TEST(adversarial, durable_records_survive_truncation) {
  naf::PrepareRecord prepare;
  prepare.decision = naf::AdmissionDecisionId::from_value(1);
  prepare.demand = naf::DemandId::from_value(2);
  prepare.attempt = naf::AttemptId::from_value(3);
  prepare.epoch = naf::FabricEpoch::first();
  prepare.amounts.emplace_back(naf::ResourceId::from_value(1), naf::Rate::from_value(500));
  const naf::ByteBuffer encoded = naf::encode(prepare);
  NAF_REQUIRE(naf::decode_prepare(encoded).has_value());
  for (std::size_t length = 0; length < encoded.size(); ++length) {
    NAF_CHECK(!naf::decode_prepare(std::span<const std::byte>(encoded.data(), length)).has_value());
  }

  naf::DecisionRecord record;
  record.decision = naf::AdmissionDecisionId::from_value(1);
  record.outcome = naf::AdmissionOutcome::Admit;
  const naf::ByteBuffer record_bytes = naf::encode(record);
  NAF_REQUIRE(naf::decode_decision_record(record_bytes).has_value());
  for (std::size_t length = 0; length < record_bytes.size(); ++length) {
    NAF_CHECK(!naf::decode_decision_record(std::span<const std::byte>(record_bytes.data(), length))
                   .has_value());
  }
  naf::ByteBuffer bad_outcome = record_bytes;
  bad_outcome[16] = std::byte{0x7F};
  NAF_CHECK(!naf::decode_decision_record(bad_outcome).has_value());
}

NAF_TEST(adversarial, unknown_evidence_never_becomes_authority) {
  FabricOptions options;
  options.resources = 2;
  options.usable = 1000;
  Fabric fabric(options);

  naf::CapacitySnapshot snapshot = fabric.capacity();
  snapshot.snapshot = naf::CapacitySnapshotId::from_value(2);
  snapshot.generation = naf::Generation::from_value(2);
  snapshot.resources[1].evidence = naf::EvidenceState::Unknown;
  snapshot.resources[1].usable = naf::Rate{};
  snapshot.resources[1].mandatory_headroom = naf::Rate{};
  fabric.republish_capacity(snapshot);

  // The unknown resource is not the one named, so this still admits.
  const naf::AdmissionDecision first = fabric.admit_full(fabric.make_request(1, 1, 100, 200, 300, 1));
  NAF_CHECK(first.outcome == naf::AdmissionOutcome::Admit);

  // Naming the unknown resource must refuse rather than assume capacity.
  naf::AdmissionRequest second = fabric.make_request(2, 2, 100, 200, 300, 2);
  second.resource_bindings[0].generation = naf::Generation::from_value(2);
  const naf::AdmissionDecision decision = fabric.admit_full(second);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::StaleInput);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::UnknownEvidence);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(2)).value(), 0);
}

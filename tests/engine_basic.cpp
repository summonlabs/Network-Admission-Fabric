// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <limits>
#include <string>

#include "fixture.hpp"
#include "support/test_framework.hpp"

using namespace naftest;

NAF_TEST(engine_admit, admits_within_capacity) {
  Fabric fabric;
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 200, 300));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK_U64(decision.granted.value(), 300);
  NAF_CHECK_U64(decision.selected_path.value(), 0);
  NAF_CHECK_U64(decision.bound_capacity_generation.value(), fabric.capacity_generation().value());
  NAF_CHECK(decision.revalidation_required);
  NAF_CHECK(!decision.authority.empty());
  NAF_CHECK(!decision.explanation.constraints().empty());
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 300);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(2)).value(), 0);
}

NAF_TEST(engine_admit, admits_degraded_up_to_the_ceiling) {
  Fabric fabric;
  NAF_CHECK(fabric.admit(fabric.make_request(1, 1, 100, 500, 500)) == naf::AdmissionOutcome::Admit);
  const naf::AdmissionDecision decision =
      fabric.admit_full(fabric.make_request(2, 2, 400, 900, 1000));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::AdmitDegraded);
  NAF_CHECK_U64(decision.granted.value(), 500);
  NAF_CHECK_U64(decision.minimum_guaranteed.value(), 400);
  NAF_CHECK(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::Degradation);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 1000);
}

NAF_TEST(engine_admit, refuses_contention_when_degradation_is_forbidden) {
  FabricOptions options;
  options.allow_degraded = false;
  Fabric fabric(options);
  NAF_CHECK(fabric.admit(fabric.make_request(1, 1, 100, 500, 500)) == naf::AdmissionOutcome::Admit);
  const naf::AdmissionDecision decision =
      fabric.admit_full(fabric.make_request(2, 2, 200, 900, 1000));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectCapacity);
  NAF_CHECK(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::Degradation);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 500);
}

NAF_TEST(engine_admit, rejects_when_raw_capacity_is_below_the_minimum) {
  Fabric fabric;
  const naf::AdmissionDecision decision =
      fabric.admit_full(fabric.make_request(1, 1, 2000, 2500, 3000));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectCapacity);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::Capacity);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);
}

NAF_TEST(engine_admit, protects_obligations_and_reports_them) {
  Fabric fabric;
  naf::ReservationSnapshot reservations;
  reservations.snapshot = naf::ReservationSnapshotId::from_value(2);
  reservations.generation = naf::Generation::from_value(2);
  reservations.epoch = fabric.epoch();
  naf::ProtectedObligation obligation;
  obligation.reservation = naf::ReservationId::from_value(10);
  obligation.reservation_generation = naf::Generation::from_value(1);
  obligation.resource = naf::ResourceId::from_value(1);
  obligation.reserved = naf::Rate::from_value(900);
  obligation.inviolable = true;
  reservations.obligations.push_back(obligation);
  fabric.republish_reservations(reservations);

  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 200, 400, 500));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectObligation);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::Obligation);
  NAF_CHECK_U64(decision.explanation.primary()->available.value(), 100);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);
}

NAF_TEST(engine_admit, defers_when_a_protected_obligation_is_about_to_release) {
  FabricOptions options;
  options.contention = naf::ContentionAction::Defer;
  options.defer_horizon = 100;
  Fabric fabric(options);

  naf::ReservationSnapshot reservations;
  reservations.snapshot = naf::ReservationSnapshotId::from_value(2);
  reservations.generation = naf::Generation::from_value(2);
  reservations.epoch = fabric.epoch();
  naf::ProtectedObligation obligation;
  obligation.reservation = naf::ReservationId::from_value(10);
  obligation.reservation_generation = naf::Generation::from_value(1);
  obligation.resource = naf::ResourceId::from_value(1);
  obligation.reserved = naf::Rate::from_value(900);
  obligation.inviolable = false;
  obligation.release_tick = 50;
  reservations.obligations.push_back(obligation);
  fabric.republish_reservations(reservations);

  naf::AdmissionRequest request = fabric.make_request(1, 1, 200, 400, 500);
  request.deadline_ticks = 600;
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::Defer);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);

  // An inviolable obligation never releases, so the same shape is refused.
  naf::ReservationSnapshot inviolable = reservations;
  inviolable.snapshot = naf::ReservationSnapshotId::from_value(3);
  inviolable.generation = naf::Generation::from_value(3);
  inviolable.obligations[0].inviolable = true;
  fabric.republish_reservations(inviolable);
  naf::AdmissionRequest second = fabric.make_request(2, 2, 200, 400, 500);
  second.deadline_ticks = 600;
  NAF_CHECK(fabric.admit(second) == naf::AdmissionOutcome::RejectObligation);
}

NAF_TEST(engine_admit, rejects_when_the_demand_asks_to_preempt_obligations) {
  Fabric fabric;
  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 200, 300);
  request.requests_obligation_preemption = true;
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectObligation);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::Obligation);
}

NAF_TEST(engine_admit, enforces_protected_headroom) {
  FabricOptions options;
  options.usable = 1000;
  options.mandatory_headroom = 800;
  Fabric fabric(options);
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 300, 400, 500));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectCapacity);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::Headroom);
  NAF_CHECK_U64(decision.explanation.primary()->available.value(), 200);
}

NAF_TEST(engine_admit, honours_policy_headroom_permille) {
  FabricOptions options;
  options.usable = 1000;
  options.policy_headroom_permille = 200;
  Fabric fabric(options);
  // 200 permille of 1000 leaves 800 usable.
  NAF_CHECK(fabric.admit(fabric.make_request(1, 1, 100, 700, 700)) == naf::AdmissionOutcome::Admit);
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(2, 2, 200, 400, 500));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectCapacity);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::AdmittedLoad);
  NAF_CHECK_U64(decision.explanation.primary()->available.value(), 100);
}

NAF_TEST(engine_admit, exact_capacity_boundary_is_admitted) {
  FabricOptions options;
  options.usable = 1000;
  Fabric fabric(options);
  NAF_CHECK(fabric.admit(fabric.make_request(1, 1, 1000, 1000, 1000)) == naf::AdmissionOutcome::Admit);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 1000);
  // One bit more must not fit, and must not wrap.
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(2, 2, 1, 1, 1));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectCapacity);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 1000);
}

NAF_TEST(engine_admit, huge_values_do_not_overflow) {
  FabricOptions options;
  options.usable = ~std::uint64_t{0};
  Fabric fabric(options);
  const naf::AdmissionDecision decision =
      fabric.admit_full(fabric.make_request(1, 1, ~std::uint64_t{0} - 10, ~std::uint64_t{0} - 5, ~std::uint64_t{0}));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK_U64(decision.granted.value(), ~std::uint64_t{0});
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), ~std::uint64_t{0});
  const naf::AdmissionDecision second = fabric.admit_full(fabric.make_request(2, 2, 1, 1, 1));
  NAF_CHECK(second.outcome == naf::AdmissionOutcome::RejectCapacity);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), ~std::uint64_t{0});
}

NAF_TEST(engine_admit, selects_only_declared_authorized_paths) {
  FabricOptions options;
  options.resources = 2;
  options.publish_paths = true;
  Fabric fabric(options);

  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500, 1);
  request.resource_bindings.clear();
  naf::PathCandidate candidate;
  candidate.path = naf::PathId::from_value(2);
  candidate.path_authority_generation = fabric.path_generation();
  request.path_candidates.push_back(candidate);
  request.required_path = candidate.path;
  request.allow_path_substitution = false;
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK_U64(decision.selected_path.value(), 2);
  // The traversed resource carries the load, not the resource the claimant named.
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(2)).value(), 500);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);
}

NAF_TEST(engine_admit, refuses_when_every_authorized_path_is_down) {
  Fabric fabric;
  naf::PathCatalog catalog = fabric.paths();
  catalog.path_authority_generation = naf::Generation::from_value(2);
  for (auto& fact : catalog.paths) fact.state = naf::PathState::Down;
  fabric.republish_paths(catalog);

  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500);
  naf::PathCandidate candidate;
  candidate.path = naf::PathId::from_value(1);
  candidate.path_authority_generation = naf::Generation::first();
  request.path_candidates.push_back(candidate);
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectPath);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::Path);
}

NAF_TEST(engine_admit, refuses_an_unknown_path_state) {
  Fabric fabric;
  naf::PathCatalog catalog = fabric.paths();
  catalog.path_authority_generation = naf::Generation::from_value(2);
  catalog.paths[0].state = naf::PathState::Unknown;
  fabric.republish_paths(catalog);

  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500);
  request.resource_bindings.clear();
  naf::PathCandidate candidate;
  candidate.path = naf::PathId::from_value(1);
  candidate.path_authority_generation = naf::Generation::first();
  request.path_candidates.push_back(candidate);
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::StaleInput);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::UnknownEvidence);
}

NAF_TEST(engine_admit, refuses_a_path_that_cannot_meet_the_latency_budget) {
  Fabric fabric;
  naf::PathCatalog catalog = fabric.paths();
  catalog.path_authority_generation = naf::Generation::from_value(2);
  catalog.paths[0].path_latency = naf::Latency::from_value(500);
  fabric.republish_paths(catalog);

  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 200, 300);
  naf::PathCandidate candidate;
  candidate.path = naf::PathId::from_value(1);
  candidate.path_authority_generation = naf::Generation::first();
  request.path_candidates.push_back(candidate);
  request.maximum_latency = naf::Latency::from_value(100);
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectPath);
}

NAF_TEST(engine_policy, refuses_a_qos_class_the_policy_does_not_allow) {
  Fabric fabric;
  naf::AdmissionPolicy policy = fabric.policy();
  policy.generation = naf::Generation::from_value(2);
  policy.allowed_qos.push_back(naf::QoSClassId::from_value(9));
  fabric.republish_policy(policy);

  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 200, 300));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectQoS);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::QoSClass);
}

NAF_TEST(engine_policy, refuses_a_priority_class_the_policy_does_not_allow) {
  Fabric fabric;
  naf::AdmissionPolicy policy = fabric.policy();
  policy.generation = naf::Generation::from_value(2);
  policy.allowed_priorities.push_back(naf::PriorityClassId::from_value(9));
  fabric.republish_policy(policy);

  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 200, 300));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectPolicy);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::PriorityClass);
}

NAF_TEST(engine_policy, refuses_a_demand_below_the_qos_class_floor) {
  FabricOptions options;
  options.qos_min_rate = 500;
  Fabric fabric(options);
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 600, 700));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectQoS);
}

NAF_TEST(engine_policy, refuses_a_demand_above_the_qos_class_ceiling) {
  FabricOptions options;
  options.qos_max_rate = 250;
  Fabric fabric(options);
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 200, 300));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectQoS);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK_U64(decision.explanation.primary()->available.value(), 250);
}

NAF_TEST(engine_policy, refuses_a_latency_budget_beyond_the_class) {
  FabricOptions options;
  options.qos_max_latency = 50;
  Fabric fabric(options);
  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 200, 300);
  request.maximum_latency = naf::Latency::from_value(51);
  NAF_CHECK(fabric.admit(request) == naf::AdmissionOutcome::RejectQoS);
}

NAF_TEST(engine_policy, requires_a_reservation_reference_when_the_policy_says_so) {
  Fabric fabric;
  naf::AdmissionPolicy policy = fabric.policy();
  policy.generation = naf::Generation::from_value(2);
  policy.require_reservation_reference = true;
  fabric.republish_policy(policy);
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 200, 300));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectPolicy);
}

NAF_TEST(engine_policy, requires_a_path_binding_when_the_policy_says_so) {
  FabricOptions options;
  options.require_path_binding = true;
  Fabric fabric(options);
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 200, 300));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::RejectPolicy);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::Policy);
}

NAF_TEST(engine_stale, refuses_a_mismatched_capacity_generation) {
  Fabric fabric;
  naf::CapacitySnapshot snapshot = fabric.capacity();
  snapshot.snapshot = naf::CapacitySnapshotId::from_value(2);
  snapshot.generation = naf::Generation::from_value(2);
  fabric.republish_capacity(snapshot);

  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 200, 300);
  request.expected.capacity_generation = naf::Generation::first();
  request.expected.capacity_snapshot = naf::CapacitySnapshotId::from_value(1);
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::StaleInput);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::StaleGeneration);
  NAF_CHECK_U64(decision.explanation.primary()->observed.value(), 1);
  NAF_CHECK_U64(decision.explanation.primary()->authoritative.value(), 2);
}

NAF_TEST(engine_stale, refuses_an_unstated_authority_generation) {
  Fabric fabric;
  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 200, 300);
  request.expected.policy_generation = naf::Generation::unknown();
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::StaleInput);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::UnknownEvidence);
}

NAF_TEST(engine_stale, refuses_a_mismatched_resource_generation) {
  Fabric fabric;
  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 200, 300);
  request.resource_bindings[0].generation = naf::Generation::from_value(9);
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::StaleInput);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::StaleGeneration);
}

NAF_TEST(engine_stale, refuses_a_superseded_qos_class_generation) {
  Fabric fabric;
  naf::QoSClassCatalog catalog;
  catalog.generation = naf::Generation::from_value(2);
  catalog.epoch = fabric.epoch();
  naf::QoSClassFact fact;
  fact.qos = naf::QoSClassId::from_value(1);
  fact.generation = naf::Generation::from_value(5);
  fact.maximum_rate = naf::Rate::from_value(100000);
  fact.maximum_latency = naf::Latency::from_value(100000);
  catalog.classes.push_back(fact);
  fabric.republish_qos(catalog);

  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 200, 300);
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::StaleInput);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK_U64(decision.explanation.primary()->authoritative.value(), 5);
}

NAF_TEST(engine_stale, unknown_capacity_cannot_authorize) {
  Fabric fabric;
  naf::Status invalidated = fabric.engine().invalidate_capacity();
  NAF_CHECK(invalidated.is_ok());
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 200, 300));
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::StaleInput);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::UnknownEvidence);
}

NAF_TEST(engine_stale, unknown_obligations_cannot_authorize) {
  naf::EngineConfig config;
  config.epoch = naf::FabricEpoch::first();
  config.incarnation = naf::CoordinatorIncarnation::from_value(1);
  naf::AdmissionEngine engine(config);

  naf::AdmissionPolicy policy;
  policy.id = naf::PolicyId::from_value(1);
  policy.generation = naf::Generation::first();
  policy.epoch = config.epoch;
  NAF_REQUIRE(engine.publish_policy(policy).is_ok());
  naf::CapacitySnapshot capacity;
  capacity.snapshot = naf::CapacitySnapshotId::from_value(1);
  capacity.generation = naf::Generation::first();
  capacity.epoch = config.epoch;
  naf::ResourceCapacity entry;
  entry.resource = naf::ResourceId::from_value(1);
  entry.generation = naf::Generation::first();
  entry.evidence = naf::EvidenceState::Known;
  entry.usable = naf::Rate::from_value(1000);
  capacity.resources.push_back(entry);
  NAF_REQUIRE(engine.publish_capacity(capacity).is_ok());
  naf::QoSClassCatalog qos;
  qos.generation = naf::Generation::first();
  qos.epoch = config.epoch;
  naf::QoSClassFact qos_fact;
  qos_fact.qos = naf::QoSClassId::from_value(1);
  qos_fact.generation = naf::Generation::first();
  qos_fact.maximum_rate = naf::Rate::from_value(100000);
  qos_fact.maximum_latency = naf::Latency::from_value(100000);
  qos.classes.push_back(qos_fact);
  NAF_REQUIRE(engine.publish_qos_catalog(qos).is_ok());
  naf::PriorityClassCatalog priorities;
  priorities.generation = naf::Generation::first();
  priorities.epoch = config.epoch;
  naf::PriorityClassFact priority_fact;
  priority_fact.priority = naf::PriorityClassId::from_value(1);
  priority_fact.generation = naf::Generation::first();
  priority_fact.rank = 1;
  priorities.classes.push_back(priority_fact);
  NAF_REQUIRE(engine.publish_priority_catalog(priorities).is_ok());

  NAF_CHECK(engine.status().readiness == naf::EngineReadiness::AwaitingReservations);

  naf::AdmissionRequest request;
  request.request = naf::AdmissionRequestId::from_value(1);
  request.demand = naf::DemandId::from_value(1);
  request.demand_generation = naf::Generation::first();
  request.attempt = naf::AttemptId::from_value(1);
  request.rate.minimum = naf::Rate::from_value(100);
  request.rate.desired = naf::Rate::from_value(200);
  request.rate.maximum = naf::Rate::from_value(300);
  request.qos = naf::QoSClassId::from_value(1);
  request.qos_generation = naf::Generation::first();
  request.priority = naf::PriorityClassId::from_value(1);
  request.priority_generation = naf::Generation::first();
  naf::ResourceBinding binding;
  binding.resource = naf::ResourceId::from_value(1);
  binding.generation = naf::Generation::first();
  request.resource_bindings.push_back(binding);
  request.expected.capacity_snapshot = naf::CapacitySnapshotId::from_value(1);
  request.expected.capacity_generation = naf::Generation::first();
  request.expected.reservation_snapshot = naf::ReservationSnapshotId::from_value(1);
  request.expected.reservation_generation = naf::Generation::first();
  request.expected.policy_generation = naf::Generation::first();
  request.expected.qos_catalog_generation = naf::Generation::first();
  request.expected.priority_catalog_generation = naf::Generation::first();

  auto decision = engine.admit_local(request);
  NAF_REQUIRE(decision.has_value());
  NAF_CHECK(decision.value().outcome == naf::AdmissionOutcome::StaleInput);
}

NAF_TEST(engine_stale, generation_advance_invalidates_a_live_decision) {
  Fabric fabric;
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 400, 500));
  NAF_REQUIRE(decision.outcome == naf::AdmissionOutcome::Admit);

  auto fresh = fabric.engine().revalidate(decision.decision, naf::ClaimContext::in_process());
  NAF_REQUIRE(fresh.has_value());
  NAF_CHECK(fresh.value().state == naf::RevalidationState::Valid);

  naf::CapacitySnapshot snapshot = fabric.capacity();
  snapshot.snapshot = naf::CapacitySnapshotId::from_value(2);
  snapshot.generation = naf::Generation::from_value(2);
  fabric.republish_capacity(snapshot);

  auto invalidated = fabric.engine().revalidate(decision.decision, naf::ClaimContext::in_process());
  NAF_REQUIRE(invalidated.has_value());
  NAF_CHECK(invalidated.value().state == naf::RevalidationState::Invalidated);
  NAF_CHECK(invalidated.value().reason == naf::RevocationReason::GenerationAdvance);
  NAF_CHECK(!invalidated.value().ledger_released);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 500);
}

NAF_TEST(engine_revoke, capacity_reduction_revokes_and_releases) {
  Fabric fabric;
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 900, 950));
  NAF_REQUIRE(decision.outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 950);

  naf::CapacitySnapshot snapshot = fabric.capacity();
  snapshot.snapshot = naf::CapacitySnapshotId::from_value(2);
  snapshot.generation = naf::Generation::from_value(2);
  for (auto& entry : snapshot.resources) entry.usable = naf::Rate::from_value(100);
  fabric.republish_capacity(snapshot);

  // Publishing capacity that can no longer cover owned load revokes it at once,
  // newest first, so the invariant is restored rather than merely reported.
  const naf::ReconciliationReport enforcement = fabric.engine().last_reconciliation();
  NAF_CHECK(enforcement.performed);
  NAF_CHECK(enforcement.trigger == naf::EnforcementTrigger::CapacityPublication);
  NAF_CHECK_U64(enforcement.revoked, 1);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);
  NAF_CHECK_U64(fabric.engine().status().live_decisions, 0);
  const std::optional<naf::DecisionRecord> record = fabric.engine().decision_record(decision.decision);
  NAF_REQUIRE(record.has_value());
  NAF_CHECK(record->state == naf::RevalidationState::Revoked);
  NAF_CHECK(record->reason == naf::RevocationReason::CapacityReduction);
  NAF_CHECK(!record->ledger_live);

  auto revalidated = fabric.engine().revalidate(decision.decision, naf::ClaimContext::in_process());
  NAF_REQUIRE(revalidated.has_value());
  NAF_CHECK(revalidated.value().state == naf::RevalidationState::Revoked);
  NAF_CHECK(revalidated.value().reason == naf::RevocationReason::CapacityReduction);
  NAF_CHECK(!revalidated.value().ledger_released);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);

  // Releasing twice must not double-count.
  auto again = fabric.engine().revalidate(decision.decision, naf::ClaimContext::in_process());
  NAF_REQUIRE(again.has_value());
  NAF_CHECK(again.value().state == naf::RevalidationState::Revoked);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);
}

NAF_TEST(engine_revoke, unknown_capacity_revokes_on_revalidation) {
  Fabric fabric;
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 900, 950));
  NAF_REQUIRE(decision.outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 950);

  NAF_CHECK(fabric.engine().invalidate_capacity().is_ok());
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 950);

  auto revalidated = fabric.engine().revalidate(decision.decision, naf::ClaimContext::in_process());
  NAF_REQUIRE(revalidated.has_value());
  NAF_CHECK(revalidated.value().state == naf::RevalidationState::Revoked);
  NAF_CHECK(revalidated.value().ledger_released);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);
}

NAF_TEST(engine_revoke, path_revocation_revokes_a_path_bound_grant) {
  Fabric fabric;
  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500);
  naf::PathCandidate candidate;
  candidate.path = naf::PathId::from_value(1);
  candidate.path_authority_generation = naf::Generation::first();
  request.path_candidates.push_back(candidate);
  const naf::AdmissionDecision decision = fabric.admit_full(request);
  NAF_REQUIRE(decision.outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK_U64(decision.selected_path.value(), 1);

  naf::PathCatalog catalog = fabric.paths();
  catalog.path_authority_generation = naf::Generation::from_value(2);
  catalog.paths[0].state = naf::PathState::Down;
  fabric.republish_paths(catalog);

  auto revalidated = fabric.engine().revalidate(decision.decision, naf::ClaimContext::in_process());
  NAF_REQUIRE(revalidated.has_value());
  NAF_CHECK(revalidated.value().state == naf::RevalidationState::Revoked);
  NAF_CHECK(revalidated.value().reason == naf::RevocationReason::PathRevoked);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);
}

NAF_TEST(engine_revoke, explicit_revocation_is_idempotent) {
  Fabric fabric;
  const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(1, 1, 100, 400, 500));
  NAF_REQUIRE(decision.outcome == naf::AdmissionOutcome::Admit);

  auto first = fabric.engine().revoke(decision.decision, naf::RevocationReason::OperatorRequest,
                                      naf::ClaimContext::in_process());
  NAF_REQUIRE(first.has_value());
  NAF_CHECK(first.value().state == naf::RevalidationState::Revoked);
  NAF_CHECK(first.value().ledger_released);

  auto second = fabric.engine().revoke(decision.decision, naf::RevocationReason::OperatorRequest,
                                       naf::ClaimContext::in_process());
  NAF_REQUIRE(second.has_value());
  NAF_CHECK(second.value().state == naf::RevalidationState::Revoked);
  NAF_CHECK(!second.value().ledger_released);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);
}

NAF_TEST(engine_accounting, obligations_and_load_never_exceed_capacity) {
  FabricOptions options;
  options.resources = 3;
  options.usable = 1000;
  Fabric fabric(options);

  naf::ReservationSnapshot reservations;
  reservations.snapshot = naf::ReservationSnapshotId::from_value(2);
  reservations.generation = naf::Generation::from_value(2);
  reservations.epoch = fabric.epoch();
  for (std::uint64_t resource = 1; resource <= 3; ++resource) {
    naf::ProtectedObligation obligation;
    obligation.reservation = naf::ReservationId::from_value(100 + resource);
    obligation.reservation_generation = naf::Generation::first();
    obligation.resource = naf::ResourceId::from_value(resource);
    obligation.reserved = naf::Rate::from_value(300);
    reservations.obligations.push_back(obligation);
  }
  fabric.republish_reservations(reservations);

  std::uint64_t admitted_total = 0;
  for (std::uint64_t i = 1; i <= 40; ++i) {
    naf::AdmissionRequest request = fabric.make_request(i, i, 100, 100, 150, 1 + (i % 3));
    const naf::AdmissionDecision decision = fabric.admit_full(request);
    if (decision.outcome == naf::AdmissionOutcome::Admit) {
      admitted_total += decision.granted.value();
    }
    for (std::uint64_t resource = 1; resource <= 3; ++resource) {
      const std::uint64_t admitted =
          fabric.engine().admitted_load(naf::ResourceId::from_value(resource)).value();
      NAF_CHECK(admitted <= 700);
    }
  }
  NAF_CHECK(admitted_total > 0);
  const naf::FabricStatus status = fabric.engine().status();
  NAF_CHECK_U64(status.total_admitted.value(), admitted_total);
}

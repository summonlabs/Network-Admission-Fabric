// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <limits>
#include <string>

#include "fixture.hpp"
#include "support/test_framework.hpp"

using namespace naftest;

NAF_TEST(engine_identity, identical_attempt_is_idempotent) {
  Fabric fabric;
  const naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500);
  const naf::AdmissionDecision first = fabric.admit_full(request);
  NAF_REQUIRE(first.outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK(!first.idempotent_replay);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 500);

  const naf::AdmissionDecision second = fabric.admit_full(request);
  NAF_CHECK(second.outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK(second.idempotent_replay);
  NAF_CHECK_U64(second.decision.value(), first.decision.value());
  NAF_CHECK_U64(second.audit.value(), first.audit.value());
  NAF_CHECK_U64(second.granted.value(), first.granted.value());
  NAF_CHECK(second.explanation.render() == first.explanation.render());
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 500);

  for (int i = 0; i < 20; ++i) {
    const naf::AdmissionDecision repeated = fabric.admit_full(request);
    NAF_CHECK(repeated.idempotent_replay);
    NAF_CHECK_U64(repeated.decision.value(), first.decision.value());
  }
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 500);
  NAF_CHECK_U64(fabric.engine().status().decisions, 1);
}

NAF_TEST(engine_identity, a_retry_from_a_restarted_claimant_is_still_idempotent) {
  Fabric fabric;
  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500);
  const naf::AdmissionDecision first = fabric.admit_full(request);
  NAF_REQUIRE(first.outcome == naf::AdmissionOutcome::Admit);

  // The claimant restarted: new boot identity and a replay origin, same attempt.
  request.provenance.boot = naf::BootId::from_value(77);
  request.provenance.origin = naf::OriginKind::Replay;
  request.provenance.sequence = 42;
  const naf::AdmissionDecision second = fabric.admit_full(request);
  NAF_CHECK(second.idempotent_replay);
  NAF_CHECK_U64(second.decision.value(), first.decision.value());
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 500);
}

NAF_TEST(engine_identity, reusing_an_attempt_with_different_content_is_refused) {
  Fabric fabric;
  const naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500);
  NAF_REQUIRE(fabric.admit_full(request).outcome == naf::AdmissionOutcome::Admit);

  naf::AdmissionRequest reused = request;
  reused.rate.desired = naf::Rate::from_value(450);
  reused.rate.maximum = naf::Rate::from_value(450);
  const naf::AdmissionDecision decision = fabric.admit_full(reused);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::ConflictingInput);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->kind == naf::BindingConstraintKind::IdentityConflict);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 500);
  NAF_CHECK_U64(fabric.engine().status().conflicts, 1);
}

NAF_TEST(engine_identity, reusing_a_request_identity_for_other_content_is_refused) {
  Fabric fabric;
  const naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500);
  NAF_REQUIRE(fabric.admit_full(request).outcome == naf::AdmissionOutcome::Admit);

  naf::AdmissionRequest other = request;
  other.demand = naf::DemandId::from_value(2);
  other.attempt = naf::AttemptId::from_value(2);
  other.provenance.attempt = other.attempt;
  const naf::AdmissionDecision decision = fabric.admit_full(other);
  NAF_CHECK(decision.outcome == naf::AdmissionOutcome::ConflictingInput);
  NAF_REQUIRE(decision.explanation.primary() != nullptr);
  NAF_CHECK(decision.explanation.primary()->subject == "attempt");
  NAF_CHECK(decision.explanation.primary()->detail.find("request identity") != std::string::npos);
}

NAF_TEST(engine_identity, a_refusal_is_replayed_not_re_evaluated) {
  FabricOptions options;
  options.allow_degraded = false;
  Fabric fabric(options);
  const naf::AdmissionRequest request = fabric.make_request(1, 1, 1200, 1300, 1400);
  const naf::AdmissionDecision first = fabric.admit_full(request);
  NAF_REQUIRE(first.outcome == naf::AdmissionOutcome::RejectCapacity);

  // Capacity grows, but an identical retry must still see the original answer.
  naf::CapacitySnapshot snapshot = fabric.capacity();
  snapshot.snapshot = naf::CapacitySnapshotId::from_value(2);
  snapshot.generation = naf::Generation::from_value(2);
  for (auto& entry : snapshot.resources) entry.usable = naf::Rate::from_value(100000);
  fabric.republish_capacity(snapshot);

  const naf::AdmissionDecision second = fabric.admit_full(request);
  NAF_CHECK(second.outcome == naf::AdmissionOutcome::RejectCapacity);
  NAF_CHECK(second.idempotent_replay);
  NAF_CHECK_U64(second.decision.value(), first.decision.value());
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);
}

NAF_TEST(engine_identity, replay_survives_a_capacity_advance) {
  Fabric fabric;
  const naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500);
  const naf::AdmissionDecision first = fabric.admit_full(request);
  NAF_REQUIRE(first.outcome == naf::AdmissionOutcome::Admit);

  naf::CapacitySnapshot snapshot = fabric.capacity();
  snapshot.snapshot = naf::CapacitySnapshotId::from_value(2);
  snapshot.generation = naf::Generation::from_value(2);
  fabric.republish_capacity(snapshot);

  const naf::AdmissionDecision second = fabric.admit_full(request);
  NAF_CHECK(second.idempotent_replay);
  NAF_CHECK(second.outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK_U64(second.decision.value(), first.decision.value());
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 500);
}

NAF_TEST(engine_identity, a_lost_commit_leaves_no_load_and_the_retry_admits_once) {
  Fabric fabric;
  const naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500);

  naf::FaultInjection faults;
  faults.fail_next_commit = true;
  fabric.engine().set_fault_injection(faults);

  auto ambiguous = fabric.engine().admit_local(request);
  NAF_CHECK(!ambiguous.has_value());
  NAF_CHECK(ambiguous.status().code() == naf::StatusCode::DurableFailure);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);
  NAF_CHECK_U64(fabric.engine().status().live_decisions, 0);

  fabric.engine().set_fault_injection(naf::FaultInjection{});
  const naf::AdmissionDecision retry = fabric.admit_full(request);
  NAF_CHECK(retry.outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK(!retry.idempotent_replay);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 500);

  const naf::AdmissionDecision again = fabric.admit_full(request);
  NAF_CHECK(again.idempotent_replay);
  NAF_CHECK_U64(again.decision.value(), retry.decision.value());
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 500);
}

NAF_TEST(engine_fence, a_registered_session_can_admit) {
  Fabric fabric;
  auto grant = fabric.engine().register_session(naf::PublisherId::from_value(5), naf::BootId::from_value(6));
  NAF_REQUIRE(grant.has_value());

  naf::ClaimContext claim;
  claim.origin = naf::OriginKind::Claimant;
  claim.publisher = naf::PublisherId::from_value(5);
  claim.boot = naf::BootId::from_value(6);
  claim.session = grant.value().session;
  claim.incarnation = grant.value().incarnation;
  claim.epoch = grant.value().epoch;

  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500);
  request.provenance.origin = naf::OriginKind::Claimant;
  auto decision = fabric.engine().admit(request, claim);
  NAF_REQUIRE(decision.has_value());
  NAF_CHECK(decision.value().outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK_EQ(fabric.engine().live_sessions(), std::size_t{1});
  NAF_CHECK(fabric.engine().retire_session(grant.value().session).is_ok());
  NAF_CHECK_EQ(fabric.engine().live_sessions(), std::size_t{0});
}

NAF_TEST(engine_fence, stale_epoch_incarnation_and_session_are_refused) {
  Fabric fabric;
  auto grant = fabric.engine().register_session(naf::PublisherId::from_value(5), naf::BootId::from_value(6));
  NAF_REQUIRE(grant.has_value());

  naf::ClaimContext claim;
  claim.origin = naf::OriginKind::Claimant;
  claim.publisher = naf::PublisherId::from_value(5);
  claim.boot = naf::BootId::from_value(6);
  claim.session = grant.value().session;
  claim.incarnation = grant.value().incarnation;
  claim.epoch = grant.value().epoch;

  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500);
  request.provenance.origin = naf::OriginKind::Claimant;

  const auto expect_fenced = [&](const naf::ClaimContext& candidate, const char* what,
                                 naf::AdmissionOutcome expected) {
    auto decision = fabric.engine().admit(request, candidate);
    NAF_REQUIRE(decision.has_value());
    if (decision.value().outcome != expected) {
      naftest::Registry::instance().fail(__FILE__, __LINE__,
                                       std::string(what) + " produced " +
                                           std::string(naf::to_string(decision.value().outcome)));
      return;
    }
    naftest::Registry::instance().count_check();
  };

  naf::ClaimContext stale = claim;
  stale.epoch = naf::FabricEpoch::from_value(claim.epoch.value() - 1);
  expect_fenced(stale, "stale epoch", naf::AdmissionOutcome::FencedClaimant);

  naf::ClaimContext future = claim;
  future.epoch = naf::FabricEpoch::from_value(claim.epoch.value() + 1);
  expect_fenced(future, "future epoch", naf::AdmissionOutcome::ConflictingInput);

  naf::ClaimContext wrong_incarnation = claim;
  wrong_incarnation.incarnation = naf::CoordinatorIncarnation::from_value(99);
  expect_fenced(wrong_incarnation, "wrong incarnation", naf::AdmissionOutcome::FencedClaimant);

  naf::ClaimContext unknown_session = claim;
  unknown_session.session = naf::SessionNonce::from_value(1234);
  expect_fenced(unknown_session, "unknown session", naf::AdmissionOutcome::FencedClaimant);

  naf::ClaimContext no_session = claim;
  no_session.session = naf::SessionNonce{};
  expect_fenced(no_session, "missing session", naf::AdmissionOutcome::FencedClaimant);

  naf::ClaimContext wrong_boot = claim;
  wrong_boot.boot = naf::BootId::from_value(7);
  expect_fenced(wrong_boot, "wrong boot identity", naf::AdmissionOutcome::FencedClaimant);

  naf::ClaimContext no_epoch = claim;
  no_epoch.epoch = naf::FabricEpoch::none();
  expect_fenced(no_epoch, "missing epoch", naf::AdmissionOutcome::FencedClaimant);

  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);
  const naf::FabricStatus status = fabric.engine().status();
  NAF_CHECK_U64(status.fences, 6);
  NAF_CHECK_U64(status.conflicts, 1);
  NAF_CHECK_U64(status.admissions, 0);
}

NAF_TEST(engine_fence, a_new_incarnation_fences_every_previous_claimant) {
  Fabric fabric;
  auto grant = fabric.engine().register_session(naf::PublisherId::from_value(5), naf::BootId::from_value(6));
  NAF_REQUIRE(grant.has_value());

  naf::ClaimContext claim;
  claim.origin = naf::OriginKind::Claimant;
  claim.publisher = naf::PublisherId::from_value(5);
  claim.boot = naf::BootId::from_value(6);
  claim.session = grant.value().session;
  claim.incarnation = grant.value().incarnation;
  claim.epoch = grant.value().epoch;

  naf::AdmissionRequest request = fabric.make_request(1, 1, 100, 400, 500);
  request.provenance.origin = naf::OriginKind::Claimant;
  auto before = fabric.engine().admit(request, claim);
  NAF_REQUIRE(before.has_value());
  NAF_CHECK(before.value().outcome == naf::AdmissionOutcome::Admit);

  NAF_REQUIRE(fabric.engine()
                  .begin_new_incarnation(naf::CoordinatorIncarnation::from_value(2))
                  .is_ok());
  NAF_CHECK_EQ(fabric.engine().live_sessions(), std::size_t{0});

  naf::AdmissionRequest second = fabric.make_request(2, 2, 100, 400, 500);
  second.provenance.origin = naf::OriginKind::Claimant;
  auto after = fabric.engine().admit(second, claim);
  NAF_REQUIRE(after.has_value());
  NAF_CHECK(after.value().outcome == naf::AdmissionOutcome::FencedClaimant);
}

NAF_TEST(engine_fence, admission_never_creates_reservations_or_paths) {
  Fabric fabric;
  const std::uint64_t reservations_before = fabric.engine().status().reservation_generation.value();
  const std::uint64_t paths_before = fabric.engine().status().path_generation.value();

  for (std::uint64_t i = 1; i <= 10; ++i) {
    naf::AdmissionRequest request = fabric.make_request(i, i, 10, 20, 30);
    (void)fabric.admit_full(request);
  }
  const naf::FabricStatus status = fabric.engine().status();
  NAF_CHECK_U64(status.reservation_generation.value(), reservations_before);
  NAF_CHECK_U64(status.path_generation.value(), paths_before);
  for (const auto& entry : fabric.engine().ledger_entries()) {
    NAF_CHECK(entry.resource.is_known());
  }
}

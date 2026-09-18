// Network Admission Fabric - shared test fixture.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Builds a small, fully explicit authoritative state so that every test states
// exactly which facts it relies on. Nothing here is a network model.
#ifndef NAF_TEST_FIXTURE_HPP
#define NAF_TEST_FIXTURE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "naf/naf.hpp"
#include "test_framework.hpp"

namespace naftest {

struct FabricOptions {
  std::size_t resources = 2;
  std::uint64_t usable = 1000;
  std::uint64_t mandatory_headroom = 0;
  std::uint32_t policy_headroom_permille = 0;
  bool allow_degraded = true;
  bool allow_preemptible = true;
  bool require_path_binding = false;
  bool publish_paths = true;
  naf::ContentionAction contention = naf::ContentionAction::Reject;
  std::uint64_t defer_horizon = 0;
  std::uint64_t qos_min_rate = 0;
  std::uint64_t qos_max_rate = ~std::uint64_t{0};
  std::uint64_t qos_max_latency = ~std::uint64_t{0};
  bool qos_degradation_allowed = true;
  std::uint32_t max_explanation_constraints = 8;
  std::uint32_t max_explanation_bytes = 2048;
  std::uint32_t max_effective_resources = 8;
  std::uint64_t path_capacity = 0;  // 0 means "same as usable"
};

/// One fully published authoritative fabric state.
class Fabric {
 public:
  explicit Fabric(FabricOptions options = {}) : options_(options) {
    naf::EngineConfig config;
    config.epoch = naf::FabricEpoch::first();
    config.incarnation = naf::CoordinatorIncarnation::from_value(1);
    engine_ = std::make_unique<naf::AdmissionEngine>(config);
    publish_all();
  }

  [[nodiscard]] naf::AdmissionEngine& engine() { return *engine_; }
  [[nodiscard]] const FabricOptions& options() const { return options_; }
  [[nodiscard]] naf::FabricEpoch epoch() const { return engine_->epoch(); }

  [[nodiscard]] naf::Generation policy_generation() const { return policy_generation_; }
  [[nodiscard]] naf::Generation capacity_generation() const { return capacity_generation_; }
  [[nodiscard]] naf::Generation reservation_generation() const { return reservation_generation_; }
  [[nodiscard]] naf::Generation path_generation() const { return path_generation_; }
  [[nodiscard]] naf::Generation qos_generation() const { return qos_generation_; }
  [[nodiscard]] naf::Generation priority_generation() const { return priority_generation_; }
  [[nodiscard]] naf::CapacitySnapshotId capacity_snapshot() const { return capacity_snapshot_; }
  [[nodiscard]] naf::ReservationSnapshotId reservation_snapshot() const { return reservation_snapshot_; }

  void republish_policy(const naf::AdmissionPolicy& policy) {
    policy_generation_ = policy.generation;
    const naf::Status status = engine_->publish_policy(policy);
    NAF_REQUIRE(status.is_ok());
  }

  void republish_capacity(const naf::CapacitySnapshot& snapshot) {
    capacity_generation_ = snapshot.generation;
    capacity_snapshot_ = snapshot.snapshot;
    const naf::Status status = engine_->publish_capacity(snapshot);
    NAF_REQUIRE(status.is_ok());
  }

  void republish_reservations(const naf::ReservationSnapshot& snapshot) {
    reservation_generation_ = snapshot.generation;
    reservation_snapshot_ = snapshot.snapshot;
    const naf::Status status = engine_->publish_reservations(snapshot);
    NAF_REQUIRE(status.is_ok());
  }

  void republish_paths(const naf::PathCatalog& catalog) {
    path_generation_ = catalog.path_authority_generation;
    const naf::Status status = engine_->publish_paths(catalog);
    NAF_REQUIRE(status.is_ok());
  }

  void republish_qos(const naf::QoSClassCatalog& catalog) {
    qos_generation_ = catalog.generation;
    const naf::Status status = engine_->publish_qos_catalog(catalog);
    NAF_REQUIRE(status.is_ok());
  }

  void republish_priorities(const naf::PriorityClassCatalog& catalog) {
    priority_generation_ = catalog.generation;
    const naf::Status status = engine_->publish_priority_catalog(catalog);
    NAF_REQUIRE(status.is_ok());
  }

  [[nodiscard]] naf::AdmissionPolicy policy() const {
    naf::AdmissionPolicy policy;
    policy.id = naf::PolicyId::from_value(1);
    policy.generation = policy_generation_;
    policy.epoch = epoch();
    policy.headroom_permille = options_.policy_headroom_permille;
    policy.headroom_floor = naf::Rate{};
    policy.allow_degraded = options_.allow_degraded;
    policy.allow_preemptible_admission = options_.allow_preemptible;
    policy.require_path_binding = options_.require_path_binding;
    policy.on_contention = options_.contention;
    policy.defer_horizon_ticks = options_.defer_horizon;
    policy.max_explanation_constraints = options_.max_explanation_constraints;
    policy.max_explanation_bytes = options_.max_explanation_bytes;
    policy.max_effective_resources = options_.max_effective_resources;
    return policy;
  }

  [[nodiscard]] naf::CapacitySnapshot capacity() const {
    naf::CapacitySnapshot snapshot;
    snapshot.snapshot = capacity_snapshot_;
    snapshot.generation = capacity_generation_;
    snapshot.epoch = epoch();
    for (std::size_t i = 1; i <= options_.resources; ++i) {
      naf::ResourceCapacity entry;
      entry.resource = naf::ResourceId::from_value(i);
      entry.generation = naf::Generation::first();
      entry.evidence = naf::EvidenceState::Known;
      entry.usable = naf::Rate::from_value(options_.usable);
      entry.mandatory_headroom = naf::Rate::from_value(options_.mandatory_headroom);
      snapshot.resources.push_back(entry);
    }
    return snapshot;
  }

  [[nodiscard]] naf::PathCatalog paths() const {
    naf::PathCatalog catalog;
    catalog.path_authority_generation = path_generation_;
    catalog.epoch = epoch();
    for (std::size_t i = 1; i <= options_.resources; ++i) {
      naf::PathAdmissionFact fact;
      fact.path = naf::PathId::from_value(i);
      fact.path_authority_generation = naf::Generation::first();
      fact.state = naf::PathState::Up;
      fact.path_usable_capacity = naf::Rate::from_value(
          options_.path_capacity == 0 ? options_.usable : options_.path_capacity);
      fact.resources.push_back(naf::ResourceId::from_value(i));
      catalog.paths.push_back(fact);
    }
    return catalog;
  }

  void publish_all() {
    policy_generation_ = naf::Generation::first();
    capacity_generation_ = naf::Generation::first();
    reservation_generation_ = naf::Generation::first();
    path_generation_ = naf::Generation::first();
    qos_generation_ = naf::Generation::first();
    priority_generation_ = naf::Generation::first();
    capacity_snapshot_ = naf::CapacitySnapshotId::from_value(1);
    reservation_snapshot_ = naf::ReservationSnapshotId::from_value(1);

    NAF_REQUIRE(engine_->publish_policy(policy()).is_ok());
    NAF_REQUIRE(engine_->publish_capacity(capacity()).is_ok());

    naf::ReservationSnapshot reservations;
    reservations.snapshot = reservation_snapshot_;
    reservations.generation = reservation_generation_;
    reservations.epoch = epoch();
    NAF_REQUIRE(engine_->publish_reservations(reservations).is_ok());

    if (options_.publish_paths) {
      NAF_REQUIRE(engine_->publish_paths(paths()).is_ok());
    }

    naf::QoSClassCatalog qos;
    qos.generation = qos_generation_;
    qos.epoch = epoch();
    naf::QoSClassFact qos_fact;
    qos_fact.qos = naf::QoSClassId::from_value(1);
    qos_fact.generation = naf::Generation::first();
    qos_fact.minimum_rate = naf::Rate::from_value(options_.qos_min_rate);
    qos_fact.maximum_rate = naf::Rate::from_value(options_.qos_max_rate);
    qos_fact.maximum_latency = naf::Latency::from_value(options_.qos_max_latency);
    qos_fact.degradation_allowed = options_.qos_degradation_allowed;
    qos.classes.push_back(qos_fact);
    NAF_REQUIRE(engine_->publish_qos_catalog(qos).is_ok());

    naf::PriorityClassCatalog priorities;
    priorities.generation = priority_generation_;
    priorities.epoch = epoch();
    naf::PriorityClassFact priority_fact;
    priority_fact.priority = naf::PriorityClassId::from_value(1);
    priority_fact.generation = naf::Generation::first();
    priority_fact.rank = 1;
    priority_fact.preemptible = true;
    priorities.classes.push_back(priority_fact);
    NAF_REQUIRE(engine_->publish_priority_catalog(priorities).is_ok());
  }

  /// Fills the authority expectation from the fixture's own bookkeeping, so a
  /// test only has to change what it is deliberately testing.
  void fill_expected(naf::AdmissionRequest& request, bool with_capacity = true,
                     bool with_reservations = true, bool with_paths = true) const {
    if (with_capacity) {
      request.expected.capacity_snapshot = capacity_snapshot_;
      request.expected.capacity_generation = capacity_generation_;
    }
    if (with_reservations) {
      request.expected.reservation_snapshot = reservation_snapshot_;
      request.expected.reservation_generation = reservation_generation_;
    }
    if (with_paths) request.expected.path_authority_generation = path_generation_;
    request.expected.policy_generation = policy_generation_;
    request.expected.qos_catalog_generation = qos_generation_;
    request.expected.priority_catalog_generation = priority_generation_;
  }

  [[nodiscard]] naf::AdmissionRequest make_request(std::uint64_t request_id, std::uint64_t demand,
                                                   std::uint64_t minimum, std::uint64_t desired,
                                                   std::uint64_t maximum,
                                                   std::uint64_t resource = 1) const {
    naf::AdmissionRequest request;
    request.request = naf::AdmissionRequestId::from_value(request_id);
    request.demand = naf::DemandId::from_value(demand);
    request.demand_generation = naf::Generation::first();
    request.attempt = naf::AttemptId::from_value(request_id);
    request.rate.minimum = naf::Rate::from_value(minimum);
    request.rate.desired = naf::Rate::from_value(desired);
    request.rate.maximum = naf::Rate::from_value(maximum);
    request.qos = naf::QoSClassId::from_value(1);
    request.qos_generation = naf::Generation::first();
    request.priority = naf::PriorityClassId::from_value(1);
    request.priority_generation = naf::Generation::first();
    request.maximum_latency = naf::Latency{};
    naf::ResourceBinding binding;
    binding.resource = naf::ResourceId::from_value(resource);
    binding.generation = naf::Generation::first();
    request.resource_bindings.push_back(binding);
    request.provenance.publisher = naf::PublisherId::from_value(1);
    request.provenance.boot = naf::BootId::from_value(1);
    request.provenance.attempt = request.attempt;
    request.provenance.origin = naf::OriginKind::InProcess;
    fill_expected(request);
    return request;
  }

  [[nodiscard]] naf::AdmissionOutcome admit(const naf::AdmissionRequest& request) {
    return admit_full(request).outcome;
  }

  [[nodiscard]] naf::AdmissionDecision admit_full(const naf::AdmissionRequest& request) {
    auto decision = engine_->admit_local(request);
    naftest::Registry::instance().count_check();
    if (!decision.has_value()) {
      naftest::Registry::instance().fail(__FILE__, __LINE__,
                                         "admit returned an error: " + decision.status().to_string());
      last_decision_ = naf::AdmissionDecision{};
      return last_decision_;
    }
    last_decision_ = decision.value();
    return last_decision_;
  }

  [[nodiscard]] const naf::AdmissionDecision& last_decision() const { return last_decision_; }

 private:
  FabricOptions options_{};
  std::unique_ptr<naf::AdmissionEngine> engine_{};
  naf::Generation policy_generation_{};
  naf::Generation capacity_generation_{};
  naf::Generation reservation_generation_{};
  naf::Generation path_generation_{};
  naf::Generation qos_generation_{};
  naf::Generation priority_generation_{};
  naf::CapacitySnapshotId capacity_snapshot_{};
  naf::ReservationSnapshotId reservation_snapshot_{};
  naf::AdmissionDecision last_decision_{};
};

}  // namespace naftest

#endif  // NAF_TEST_FIXTURE_HPP

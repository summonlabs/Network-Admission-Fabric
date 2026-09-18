// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Seeded randomized property tests. Every run is reproducible from its seed and
// the whole transcript is compared between two independent engines, so
// determinism is checked rather than assumed.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "naf/naf.hpp"
#include "support/test_framework.hpp"

using namespace naftest;

namespace {

/// Deterministic splitmix64. Reproducible across platforms and runs.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }
  bool chance(std::uint32_t percent) { return below(100) < percent; }

 private:
  std::uint64_t state_;
};

/// Drives one engine through a randomized script and records every observable
/// outcome so two runs can be compared byte for byte.
class Driver {
 public:
  Driver(std::uint64_t seed, std::size_t resources, std::uint64_t usable)
      : rng_(seed), resources_(resources), usable_(usable) {
    naf::EngineConfig config;
    config.epoch = naf::FabricEpoch::first();
    config.incarnation = naf::CoordinatorIncarnation::from_value(1);
    engine_ = std::make_unique<naf::AdmissionEngine>(config);
    publish_all();
  }

  [[nodiscard]] naf::AdmissionEngine& engine() { return *engine_; }

  std::string run(std::size_t steps) {
    std::string transcript;
    for (std::size_t step = 0; step < steps; ++step) {
      switch (rng_.below(10)) {
        case 0:
        case 1:
          capacity_generation_ = capacity_generation_.next();
          capacity_snapshot_ = naf::CapacitySnapshotId::from_value(capacity_snapshot_.value() + 1);
          publish_capacity();
          transcript += "capacity " + std::to_string(capacity_generation_.value()) + " ";
          break;
        case 2:
          reservation_generation_ = reservation_generation_.next();
          reservation_snapshot_ = naf::ReservationSnapshotId::from_value(reservation_snapshot_.value() + 1);
          publish_reservations();
          transcript += "reservations " + std::to_string(reservation_generation_.value()) + " ";
          break;
        case 3:
          policy_generation_ = policy_generation_.next();
          publish_policy();
          transcript += "policy " + std::to_string(policy_generation_.value()) + " ";
          break;
        case 4:
          engine_->advance_tick(1 + rng_.below(50));
          transcript += "tick " + std::to_string(engine_->tick()) + " ";
          break;
        case 5:
          revoke_something(transcript);
          break;
        default:
          admit_something(transcript);
          break;
      }
      transcript += verify_invariants();
      transcript.push_back('\n');
    }
    return transcript;
  }

 private:
  void publish_all() {
    policy_generation_ = naf::Generation::first();
    capacity_generation_ = naf::Generation::first();
    reservation_generation_ = naf::Generation::first();
    qos_generation_ = naf::Generation::first();
    priority_generation_ = naf::Generation::first();
    capacity_snapshot_ = naf::CapacitySnapshotId::from_value(1);
    reservation_snapshot_ = naf::ReservationSnapshotId::from_value(1);
    publish_policy();
    publish_capacity();
    publish_reservations();
    naf::QoSClassCatalog qos;
    qos.generation = qos_generation_;
    qos.epoch = engine_->epoch();
    naf::QoSClassFact fact;
    fact.qos = naf::QoSClassId::from_value(1);
    fact.generation = naf::Generation::first();
    fact.maximum_rate = naf::Rate::from_value(~std::uint64_t{0});
    fact.maximum_latency = naf::Latency::from_value(~std::uint64_t{0});
    fact.degradation_allowed = true;
    qos.classes.push_back(fact);
    NAF_CHECK(engine_->publish_qos_catalog(qos).is_ok());
    naf::PriorityClassCatalog priorities;
    priorities.generation = priority_generation_;
    priorities.epoch = engine_->epoch();
    naf::PriorityClassFact priority;
    priority.priority = naf::PriorityClassId::from_value(1);
    priority.generation = naf::Generation::first();
    priority.rank = 1;
    priorities.classes.push_back(priority);
    NAF_CHECK(engine_->publish_priority_catalog(priorities).is_ok());
  }

  void publish_policy() {
    naf::AdmissionPolicy policy;
    policy.id = naf::PolicyId::from_value(1);
    policy.generation = policy_generation_;
    policy.epoch = engine_->epoch();
    policy.headroom_permille = headroom_permille_;
    policy.allow_degraded = allow_degraded_;
    policy.on_contention = naf::ContentionAction::Defer;
    policy.defer_horizon_ticks = 200;
    policy.max_explanation_constraints = 8;
    policy.max_explanation_bytes = 2048;
    policy.max_effective_resources = 8;
    const naf::Status status = engine_->publish_policy(policy);
    NAF_CHECK(status.is_ok());
  }

  void publish_capacity() {
    naf::CapacitySnapshot snapshot;
    snapshot.snapshot = capacity_snapshot_;
    snapshot.generation = capacity_generation_;
    snapshot.epoch = engine_->epoch();
    snapshot.observed_tick = engine_->tick();
    for (std::size_t i = 1; i <= resources_; ++i) {
      naf::ResourceCapacity entry;
      entry.resource = naf::ResourceId::from_value(i);
      entry.generation = naf::Generation::first();
      entry.evidence = evidence_known_ ? naf::EvidenceState::Known : naf::EvidenceState::Unknown;
      entry.usable = entry.evidence == naf::EvidenceState::Known ? naf::Rate::from_value(usable_)
                                                                 : naf::Rate{};
      entry.mandatory_headroom = naf::Rate::from_value(mandatory_headroom_);
      snapshot.resources.push_back(entry);
    }
    const naf::Status status = engine_->publish_capacity(snapshot);
    NAF_CHECK(status.is_ok());
  }

  void publish_reservations() {
    naf::ReservationSnapshot snapshot;
    snapshot.snapshot = reservation_snapshot_;
    snapshot.generation = reservation_generation_;
    snapshot.epoch = engine_->epoch();
    snapshot.observed_tick = engine_->tick();
    const std::uint64_t count = rng_.below(4);
    std::uint64_t reservation_id = 100;
    for (std::uint64_t i = 0; i < count; ++i) {
      naf::ProtectedObligation obligation;
      obligation.reservation = naf::ReservationId::from_value(reservation_id++);
      obligation.reservation_generation = naf::Generation::first();
      obligation.resource = naf::ResourceId::from_value(1 + rng_.below(resources_));
      obligation.reserved = naf::Rate::from_value(rng_.below(usable_ / 2 + 1));
      obligation.inviolable = rng_.chance(50);
      obligation.release_tick = rng_.chance(50) ? engine_->tick() + 1 + rng_.below(300) : 0;
      snapshot.obligations.push_back(obligation);
    }
    std::sort(snapshot.obligations.begin(), snapshot.obligations.end(),
              [](const naf::ProtectedObligation& a, const naf::ProtectedObligation& b) {
                if (a.resource != b.resource) return a.resource < b.resource;
                return a.reservation < b.reservation;
              });
    const naf::Status status = engine_->publish_reservations(snapshot);
    NAF_CHECK(status.is_ok());
    // Only an accepted publication becomes the driver's view of the obligations.
    if (!status.is_ok()) return;
    published_obligations_.clear();
    for (const auto& obligation : snapshot.obligations) {
      published_obligations_.emplace_back(obligation.resource.value(), obligation.reserved.value());
    }
  }

  [[nodiscard]] naf::AdmissionRequest build_request(std::uint64_t sequence) {
    naf::AdmissionRequest request;
    request.request = naf::AdmissionRequestId::from_value(sequence);
    request.demand = naf::DemandId::from_value(sequence);
    request.demand_generation = naf::Generation::first();
    request.attempt = naf::AttemptId::from_value(rng_.below(4) == 0 ? 1 : sequence);
    const std::uint64_t ceiling = usable_ == 0 ? 1 : usable_;
    // Minimum is at least one bit per second so the window is never degenerate;
    // a zero ceiling is a contract violation, which is tested elsewhere.
    const std::uint64_t minimum = 1 + rng_.below(ceiling);
    const std::uint64_t desired = minimum + rng_.below(ceiling - minimum + 1);
    const std::uint64_t maximum = desired + rng_.below(ceiling - desired + 1);
    request.rate.minimum = naf::Rate::from_value(minimum);
    request.rate.desired = naf::Rate::from_value(desired);
    request.rate.maximum = naf::Rate::from_value(maximum);
    request.qos = naf::QoSClassId::from_value(1);
    request.qos_generation = naf::Generation::first();
    request.priority = naf::PriorityClassId::from_value(1);
    request.priority_generation = naf::Generation::first();
    const std::uint64_t resource = 1 + rng_.below(resources_);
    naf::ResourceBinding binding;
    binding.resource = naf::ResourceId::from_value(resource);
    binding.generation = naf::Generation::first();
    request.resource_bindings.push_back(binding);
    request.expected.capacity_snapshot = capacity_snapshot_;
    request.expected.capacity_generation = capacity_generation_;
    request.expected.reservation_snapshot = reservation_snapshot_;
    request.expected.reservation_generation = reservation_generation_;
    request.expected.policy_generation = policy_generation_;
    request.expected.qos_catalog_generation = qos_generation_;
    request.expected.priority_catalog_generation = priority_generation_;
    request.deadline_ticks = rng_.below(400);
    request.provenance.publisher = naf::PublisherId::from_value(1);
    request.provenance.boot = naf::BootId::from_value(1);
    request.provenance.attempt = request.attempt;
    return request;
  }

  void admit_something(std::string& transcript) {
    naf::AdmissionRequest request = build_request(next_sequence_++);
    auto decision = engine_->admit_local(request);
    if (!decision.has_value()) {
      transcript += "error ";
      return;
    }
    transcript += std::string(naf::to_string(decision.value().outcome));
    transcript += " grant=" + std::to_string(decision.value().granted.value());
    transcript += " decision=" + std::to_string(decision.value().decision.value());
    if (decision.value().outcome == naf::AdmissionOutcome::Admit ||
        decision.value().outcome == naf::AdmissionOutcome::AdmitDegraded) {
      live_.push_back(decision.value().decision);
    }
    // A repeated attempt must be idempotent.
    if (rng_.chance(20)) {
      auto replay = engine_->admit_local(request);
      NAF_CHECK(replay.has_value());
      NAF_CHECK(replay.value().idempotent_replay);
      NAF_CHECK_U64(replay.value().decision.value(), decision.value().decision.value());
      NAF_CHECK(replay.value().outcome == decision.value().outcome);
      transcript += " replay_same=1";
    }
    transcript.push_back(' ');
  }

  void revoke_something(std::string& transcript) {
    if (live_.empty()) {
      transcript += "revoke_none ";
      return;
    }
    const std::size_t index = static_cast<std::size_t>(rng_.below(live_.size()));
    const naf::AdmissionDecisionId decision = live_[index];
    live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(index));
    auto result = engine_->revoke(decision, naf::RevocationReason::OperatorRequest,
                                  naf::ClaimContext::in_process());
    NAF_CHECK(result.has_value());
    transcript += "revoked=" + std::to_string(decision.value());
    transcript.push_back(' ');
  }

  [[nodiscard]] std::string verify_invariants() {
    const naf::FabricStatus status = engine_->status();
    std::string out = "|";
    std::uint64_t sum = 0;
    for (std::size_t i = 1; i <= resources_; ++i) {
      const naf::ResourceId resource = naf::ResourceId::from_value(i);
      const std::uint64_t admitted = engine_->admitted_load(resource).value();
      const std::uint64_t obligations = obligations_for(resource);
      const std::uint64_t headroom = headroom_for();
      check(admitted, obligations, headroom, "resource " + std::to_string(i));
      // Saturating, exactly like the engine's own accounting: the invariant is
      // that nothing wraps, not that a sum of maxima is representable.
      sum = naf::sat_add(sum, admitted);
      out += std::to_string(admitted) + "," + std::to_string(obligations) + "," +
             std::to_string(headroom) + ";";
    }
    NAF_CHECK_U64(status.total_admitted.value(), sum);
    out += std::to_string(status.decisions) + "/" + std::to_string(status.admissions) + "/" +
           std::to_string(status.refusals);
    return out;
  }

  [[nodiscard]] std::uint64_t obligations_for(naf::ResourceId resource) const {
    // The driver tracks the published obligation set so the invariant can be
    // checked independently of the engine's own bookkeeping.
    std::uint64_t total = 0;
    for (const auto& pair : published_obligations_) {
      if (pair.first == resource.value()) total += pair.second;
    }
    return total;
  }

  [[nodiscard]] std::uint64_t headroom_for() const {
    if (!evidence_known_) return usable_;
    const std::uint64_t policy_headroom =
        static_cast<std::uint64_t>((static_cast<unsigned long long>(usable_) *
                                    static_cast<unsigned long long>(headroom_permille_)) /
                                   1000ull);
    return std::max(mandatory_headroom_, policy_headroom) > usable_
               ? usable_
               : std::max(mandatory_headroom_, policy_headroom);
  }

  void check(std::uint64_t admitted, std::uint64_t obligations, std::uint64_t headroom,
             const std::string& what) {
    const std::uint64_t committed = admitted + obligations + headroom;
    if (committed > usable_) {
      // If the authoritative inputs are themselves contradictory -- protected
      // obligations plus headroom already exceed usable capacity -- admission
      // must hold nothing on that resource.
      if (obligations + headroom > usable_) {
        if (admitted != 0) {
          naftest::Registry::instance().fail(
              __FILE__, __LINE__,
              "infeasible " + what + " still holds " + std::to_string(admitted) + " of admitted load");
          return;
        }
        naftest::Registry::instance().count_check();
        return;
      }
      naftest::Registry::instance().fail(__FILE__, __LINE__,
                                         "invariant violated on " + what + ": " +
                                             std::to_string(committed) + " > " +
                                             std::to_string(usable_));
      return;
    }
    naftest::Registry::instance().count_check();
  }

  Rng rng_;
  std::size_t resources_;
  std::uint64_t usable_;
  std::uint32_t headroom_permille_ = 0;
  std::uint64_t mandatory_headroom_ = 0;
  bool evidence_known_ = true;
  bool allow_degraded_ = true;
  std::unique_ptr<naf::AdmissionEngine> engine_{};
  naf::Generation policy_generation_{};
  naf::Generation capacity_generation_{};
  naf::Generation reservation_generation_{};
  naf::Generation qos_generation_{};
  naf::Generation priority_generation_{};
  naf::CapacitySnapshotId capacity_snapshot_{};
  naf::ReservationSnapshotId reservation_snapshot_{};
  std::vector<naf::AdmissionDecisionId> live_{};
  std::vector<std::pair<std::uint64_t, std::uint64_t>> published_obligations_{};
  std::uint64_t next_sequence_ = 1;
};

}  // namespace

NAF_TEST(property, accounting_closure_holds_over_random_scripts) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    Driver driver(seed, 2 + (seed % 3), 1000);
    const std::string transcript = driver.run(120);
    NAF_CHECK(!transcript.empty());
    NAF_CHECK_U64(seed, seed);
  }
}

NAF_TEST(property, identical_scripts_produce_identical_decisions) {
  for (std::uint64_t seed = 100; seed <= 106; ++seed) {
    Driver first(seed, 2, 1000);
    Driver second(seed, 2, 1000);
    const std::string a = first.run(150);
    const std::string b = second.run(150);
    if (a != b) {
      naftest::Registry::instance().fail(__FILE__, __LINE__,
                                         "seeded scripts diverged for seed " + std::to_string(seed));
      continue;
    }
    naftest::Registry::instance().count_check();
  }
}

NAF_TEST(property, huge_capacities_never_overflow) {
  for (std::uint64_t seed = 900; seed <= 906; ++seed) {
    Driver driver(seed, 2, ~std::uint64_t{0});
    const std::string transcript = driver.run(80);
    NAF_CHECK(!transcript.empty());
  }
}

NAF_TEST(property, tiny_capacities_still_preserve_the_invariant) {
  for (std::uint64_t seed = 500; seed <= 508; ++seed) {
    Driver driver(seed, 3, 3);
    const std::string transcript = driver.run(120);
    NAF_CHECK(!transcript.empty());
  }
}

NAF_TEST(property, a_generation_perturbation_is_always_refused) {
  Driver driver(7, 2, 1000);
  (void)driver.run(10);
  // Baseline: the warm-up script may legitimately have admitted something.
  const std::uint64_t baseline_admissions = driver.engine().status().admissions;
  const std::uint64_t baseline_load = driver.engine().admitted_load(naf::ResourceId::from_value(1)).value();
  std::size_t refusals = 0;
  std::uint64_t sequence = 1000;
  for (std::uint64_t i = 0; i < 200; ++i) {
    naf::AdmissionRequest request;
    request.request = naf::AdmissionRequestId::from_value(sequence);
    request.demand = naf::DemandId::from_value(sequence);
    request.demand_generation = naf::Generation::first();
    request.attempt = naf::AttemptId::from_value(sequence);
    request.rate.minimum = naf::Rate::from_value(1);
    request.rate.desired = naf::Rate::from_value(2);
    request.rate.maximum = naf::Rate::from_value(3);
    request.qos = naf::QoSClassId::from_value(1);
    request.qos_generation = naf::Generation::first();
    request.priority = naf::PriorityClassId::from_value(1);
    request.priority_generation = naf::Generation::first();
    naf::ResourceBinding binding;
    binding.resource = naf::ResourceId::from_value(1);
    binding.generation = naf::Generation::first();
    request.resource_bindings.push_back(binding);
    // Deliberately wrong expectations: either unstated or a bogus revision.
    request.expected.capacity_snapshot = naf::CapacitySnapshotId::from_value(1 + i);
    request.expected.capacity_generation = naf::Generation::from_value(1000 + i);
    request.expected.policy_generation = naf::Generation::unknown();
    request.expected.qos_catalog_generation = naf::Generation::from_value(1000 + i);
    request.expected.priority_catalog_generation = naf::Generation::from_value(1000 + i);
    auto decision = driver.engine().admit_local(request);
    NAF_CHECK(decision.has_value());
    if (decision.value().outcome == naf::AdmissionOutcome::StaleInput) ++refusals;
    ++sequence;
  }
  NAF_CHECK_U64(refusals, 200);
  // A stale or unstated generation never authorizes and never consumes capacity.
  NAF_CHECK_U64(driver.engine().status().admissions, baseline_admissions);
  NAF_CHECK_U64(driver.engine().admitted_load(naf::ResourceId::from_value(1)).value(), baseline_load);
}

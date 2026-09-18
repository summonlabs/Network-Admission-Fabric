// Network Admission Fabric - synthetic admission benchmark.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// HONESTY STATEMENT
// -----------------
// Every population measured here is SYNTHETIC: demand populations, resource
// populations and obligation densities are generated numbers. Nothing is
// measured on, or extrapolated to, any physical network, switch, NIC, DPU or
// fabric. The benchmark reports COMPLETED admission decisions, not enqueue
// latency or queue depth.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "naf/naf.hpp"

namespace {

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

 private:
  std::uint64_t state_;
};

struct Scenario {
  std::string name;
  std::uint64_t resources = 1;
  std::uint64_t usable_per_resource = 1'000'000'000;
  std::uint64_t obligations_per_resource = 0;
  std::uint64_t obligation_size = 0;
  std::uint32_t demand_below_capacity_percent = 90;
  std::size_t pool = 4096;
  std::size_t iterations = 200000;
  bool durable = false;
  std::uint32_t headroom_permille = 50;
};

struct Result {
  std::string name;
  std::uint64_t decisions = 0;
  std::uint64_t admissions = 0;
  std::uint64_t refusals = 0;
  double seconds = 0.0;
  double p50_ns = 0.0;
  double p99_ns = 0.0;
  double max_ns = 0.0;
  bool durable = false;
  std::map<std::string, std::uint64_t> outcomes{};
};

std::string label(const Result& result) {
  return result.durable ? "SYNTHETIC+DURABLE" : "SYNTHETIC";
}

/// Publishes a synthetic authority set built entirely from the scenario numbers.
naf::Status publish_synthetic_authority(naf::AdmissionEngine& engine, const Scenario& scenario,
                                        Rng& rng) {
  const naf::FabricEpoch epoch = engine.epoch();

  naf::AdmissionPolicy policy;
  policy.id = naf::PolicyId::from_value(1);
  policy.generation = naf::Generation::first();
  policy.epoch = epoch;
  policy.headroom_permille = scenario.headroom_permille;
  policy.allow_degraded = true;
  policy.on_contention = naf::ContentionAction::Reject;
  policy.max_explanation_constraints = 8;
  policy.max_explanation_bytes = 1024;
  policy.max_effective_resources = 8;
  if (auto status = engine.publish_policy(policy); !status.is_ok()) return status;

  naf::CapacitySnapshot capacity;
  capacity.snapshot = naf::CapacitySnapshotId::from_value(1);
  capacity.generation = naf::Generation::first();
  capacity.epoch = epoch;
  for (std::uint64_t i = 1; i <= scenario.resources; ++i) {
    naf::ResourceCapacity entry;
    entry.resource = naf::ResourceId::from_value(i);
    entry.generation = naf::Generation::first();
    entry.evidence = naf::EvidenceState::Known;
    entry.usable = naf::Rate::from_value(scenario.usable_per_resource);
    capacity.resources.push_back(entry);
  }
  if (auto status = engine.publish_capacity(capacity); !status.is_ok()) return status;

  naf::ReservationSnapshot reservations;
  reservations.snapshot = naf::ReservationSnapshotId::from_value(1);
  reservations.generation = naf::Generation::first();
  reservations.epoch = epoch;
  std::uint64_t reservation_id = 1;
  for (std::uint64_t resource = 1; resource <= scenario.resources; ++resource) {
    for (std::uint64_t k = 0; k < scenario.obligations_per_resource; ++k) {
      naf::ProtectedObligation obligation;
      obligation.reservation = naf::ReservationId::from_value(reservation_id++);
      obligation.reservation_generation = naf::Generation::first();
      obligation.resource = naf::ResourceId::from_value(resource);
      obligation.reserved = naf::Rate::from_value(scenario.obligation_size);
      obligation.inviolable = true;
      reservations.obligations.push_back(obligation);
    }
  }
  (void)rng;
  if (auto status = engine.publish_reservations(reservations); !status.is_ok()) return status;

  naf::PathCatalog paths;
  paths.path_authority_generation = naf::Generation::first();
  paths.epoch = epoch;
  if (auto status = engine.publish_paths(paths); !status.is_ok()) return status;

  naf::QoSClassCatalog qos;
  qos.generation = naf::Generation::first();
  qos.epoch = epoch;
  naf::QoSClassFact qos_fact;
  qos_fact.qos = naf::QoSClassId::from_value(1);
  qos_fact.generation = naf::Generation::first();
  qos_fact.maximum_rate = naf::Rate::from_value(~std::uint64_t{0});
  qos_fact.maximum_latency = naf::Latency::from_value(~std::uint64_t{0});
  qos.classes.push_back(qos_fact);
  if (auto status = engine.publish_qos_catalog(qos); !status.is_ok()) return status;

  naf::PriorityClassCatalog priorities;
  priorities.generation = naf::Generation::first();
  priorities.epoch = epoch;
  naf::PriorityClassFact priority;
  priority.priority = naf::PriorityClassId::from_value(1);
  priority.generation = naf::Generation::first();
  priority.rank = 1;
  priorities.classes.push_back(priority);
  return engine.publish_priority_catalog(priorities);
}

Result run_scenario(const Scenario& scenario) {
  Result result;
  result.name = scenario.name;
  result.durable = scenario.durable;

  naf::EngineConfig config;
  config.epoch = scenario.durable ? naf::FabricEpoch::none() : naf::FabricEpoch::first();
  config.incarnation = naf::CoordinatorIncarnation::from_value(1);
  naf::AdmissionEngine engine(config);

  // A durable scenario writes a real journal, in a scratch directory that is
  // removed afterwards: the benchmark never leaves artifacts behind.
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() / "naf-benchmark-scratch";
  std::unique_ptr<naf::Journal> journal;
  if (scenario.durable) {
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);
    journal = std::make_unique<naf::Journal>(scratch / "admission.journal", naf::JournalOptions{});
  }
  if (journal != nullptr) {
    (void)engine.attach_journal(std::move(journal));
    auto opened = engine.open_durable();
    if (!opened.has_value() || !opened.value().status.is_ok()) {
      std::fprintf(stderr, "benchmark: durable engine failed to open: %s\n",
                   opened.has_value() ? opened.value().status.to_string().c_str()
                                      : opened.status().to_string().c_str());
      return result;
    }
  }

  Rng rng(0x5EED1234u + scenario.resources);
  if (auto status = publish_synthetic_authority(engine, scenario, rng); !status.is_ok()) {
    std::fprintf(stderr, "benchmark: authority publication failed: %s\n", status.to_string().c_str());
    return result;
  }

  const naf::FabricStatus fabric = engine.status();
  const std::uint64_t per_demand_cap =
      scenario.usable_per_resource / std::max<std::uint64_t>(scenario.demand_below_capacity_percent, 1);
  const std::uint64_t demand_max = std::max<std::uint64_t>(
      per_demand_cap * scenario.demand_below_capacity_percent / 100, 1);

  // Build the synthetic demand pool outside the timed region.
  std::vector<naf::AdmissionRequest> pool;
  pool.reserve(scenario.pool);
  std::uint64_t sequence = 1;
  for (std::size_t i = 0; i < scenario.pool; ++i) {
    naf::AdmissionRequest request;
    request.request = naf::AdmissionRequestId::from_value(sequence);
    request.demand = naf::DemandId::from_value(sequence);
    request.demand_generation = naf::Generation::first();
    request.attempt = naf::AttemptId::from_value(sequence);
    const std::uint64_t minimum = 1 + rng.below(demand_max);
    const std::uint64_t desired = minimum + rng.below(demand_max - minimum + 1);
    const std::uint64_t maximum = desired;
    request.rate.minimum = naf::Rate::from_value(minimum);
    request.rate.desired = naf::Rate::from_value(desired);
    request.rate.maximum = naf::Rate::from_value(maximum);
    request.qos = naf::QoSClassId::from_value(1);
    request.qos_generation = naf::Generation::first();
    request.priority = naf::PriorityClassId::from_value(1);
    request.priority_generation = naf::Generation::first();
    naf::ResourceBinding binding;
    binding.resource = naf::ResourceId::from_value(1 + rng.below(scenario.resources));
    binding.generation = naf::Generation::first();
    request.resource_bindings.push_back(binding);
    request.expected.capacity_snapshot = fabric.capacity_snapshot;
    request.expected.capacity_generation = fabric.capacity_generation;
    request.expected.reservation_snapshot = fabric.reservation_snapshot;
    request.expected.reservation_generation = fabric.reservation_generation;
    request.expected.policy_generation = fabric.policy_generation;
    request.expected.qos_catalog_generation = fabric.qos_generation;
    request.expected.priority_catalog_generation = fabric.priority_generation;
    pool.push_back(request);
    ++sequence;
  }

  std::vector<std::uint64_t> latencies;
  latencies.reserve(scenario.iterations);

  // Warm-up runs on a throwaway engine with the identical synthetic authority
  // set, so allocator growth and first-touch faults do not distort the measured
  // engine's accounting or its first round.
  {
    naf::EngineConfig warm_config = config;
    naf::AdmissionEngine warm_engine(warm_config);
    std::unique_ptr<naf::Journal> warm_journal;
    if (scenario.durable) {
      warm_journal = std::make_unique<naf::Journal>(scratch / "warmup.journal", naf::JournalOptions{});
      (void)warm_engine.attach_journal(std::move(warm_journal));
      auto opened = warm_engine.open_durable();
      if (!opened.has_value() || !opened.value().status.is_ok()) {
        std::fprintf(stderr, "benchmark: warm-up engine failed to open\n");
        return result;
      }
    }
    Rng warm_rng(0x5EED1234u + scenario.resources);
    if (auto status = publish_synthetic_authority(warm_engine, scenario, warm_rng); !status.is_ok()) {
      std::fprintf(stderr, "benchmark: warm-up authority failed: %s\n", status.to_string().c_str());
      return result;
    }
    for (std::size_t i = 0; i < pool.size(); ++i) {
      auto warm_decision = warm_engine.admit_local(pool[i]);
      if (!warm_decision.has_value()) {
        std::fprintf(stderr, "benchmark: warm-up admission failed: %s\n",
                     warm_decision.status().to_string().c_str());
        return result;
      }
    }
  }

  const auto started = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < scenario.iterations; ++i) {
    naf::AdmissionRequest request = pool[i % pool.size()];
    const std::uint64_t round = static_cast<std::uint64_t>(i / pool.size()) + 1;
    const std::uint64_t id = request.request.value() + round * 1'000'000'000ull;
    request.request = naf::AdmissionRequestId::from_value(id);
    request.demand = naf::DemandId::from_value(id);
    request.attempt = naf::AttemptId::from_value(id);
    request.provenance.attempt = request.attempt;

    const auto before = std::chrono::steady_clock::now();
    auto decision = engine.admit_local(request);
    const auto after = std::chrono::steady_clock::now();
    latencies.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
    if (!decision.has_value()) {
      result.outcomes["ERROR"]++;
      continue;
    }
    ++result.decisions;
    result.outcomes[std::string(naf::to_string(decision.value().outcome))]++;
    if (naf::is_admitting(decision.value().outcome)) {
      ++result.admissions;
    } else {
      ++result.refusals;
    }
  }
  const auto finished = std::chrono::steady_clock::now();
  result.seconds = std::chrono::duration<double>(finished - started).count();

  std::sort(latencies.begin(), latencies.end());
  if (!latencies.empty()) {
    result.p50_ns = static_cast<double>(latencies[latencies.size() / 2]);
    result.p99_ns = static_cast<double>(latencies[(latencies.size() * 99) / 100]);
    result.max_ns = static_cast<double>(latencies.back());
  }
  if (scenario.durable) {
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
  }
  return result;
}

void report(const Result& result) {
  const double throughput = result.seconds > 0.0 ? static_cast<double>(result.decisions) / result.seconds : 0.0;
  const double ratio = result.decisions > 0
                           ? 100.0 * static_cast<double>(result.admissions) / static_cast<double>(result.decisions)
                           : 0.0;
  std::printf(
      "%-28s %-18s decisions=%-9llu admitted=%-8llu refused=%-8llu throughput=%-12.0f/s "
      "p50=%.0fns p99=%.0fns max=%.0fns\n",
      result.name.c_str(), label(result).c_str(),
      static_cast<unsigned long long>(result.decisions),
      static_cast<unsigned long long>(result.admissions),
      static_cast<unsigned long long>(result.refusals), throughput, result.p50_ns, result.p99_ns,
      result.max_ns);
  for (const auto& [outcome, count] : result.outcomes) {
    std::printf("  outcome %-18s %llu\n", outcome.c_str(),
                static_cast<unsigned long long>(count));
  }
  std::printf("  machine_readable scenario=%s class=%s decisions=%llu admissions=%llu refusals=%llu "
              "seconds=%.6f throughput_per_second=%.1f p50_ns=%.0f p99_ns=%.0f admission_ratio=%.2f\n",
              result.name.c_str(), label(result).c_str(),
              static_cast<unsigned long long>(result.decisions),
              static_cast<unsigned long long>(result.admissions),
              static_cast<unsigned long long>(result.refusals), result.seconds, throughput,
              result.p50_ns, result.p99_ns, ratio);
}

std::vector<Scenario> default_scenarios() {
  std::vector<Scenario> scenarios;

  Scenario single;
  single.name = "single-resource-low-contention";
  single.resources = 1;
  single.obligations_per_resource = 0;
  single.pool = 4096;
  single.iterations = 300000;
  scenarios.push_back(single);

  Scenario dense;
  dense.name = "16-resources-25pct-obligations";
  dense.resources = 16;
  dense.obligations_per_resource = 8;
  dense.obligation_size = 25'000'000;
  dense.pool = 8192;
  dense.iterations = 300000;
  scenarios.push_back(dense);

  Scenario wide;
  wide.name = "256-resources-50pct-obligations";
  wide.resources = 256;
  wide.obligations_per_resource = 16;
  wide.obligation_size = 25'000'000;
  wide.pool = 8192;
  wide.iterations = 200000;
  scenarios.push_back(wide);

  Scenario saturated;
  saturated.name = "saturated-contention";
  saturated.resources = 4;
  saturated.obligations_per_resource = 8;
  saturated.obligation_size = 25'000'000;
  saturated.demand_below_capacity_percent = 50;
  saturated.pool = 4096;
  saturated.iterations = 300000;
  scenarios.push_back(saturated);

  Scenario durable;
  durable.name = "durable-journal";
  durable.resources = 4;
  durable.obligations_per_resource = 2;
  durable.obligation_size = 25'000'000;
  durable.pool = 512;
  durable.iterations = 4000;
  durable.durable = true;
  scenarios.push_back(durable);

  return scenarios;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t scale = 1;
  bool quick = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--quick") == 0) {
      quick = true;
    } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
      scale = std::strtoull(argv[++i], nullptr, 10);
      if (scale == 0) scale = 1;
    }
  }

  std::printf("Network Admission Fabric %s synthetic admission benchmark\n",
              std::string(naf::version_string).c_str());
  std::printf("All populations below are SYNTHETIC. No physical network, switch, NIC, DPU or fabric "
              "was measured or is implied.\n");
  std::printf("Reported figures are completed admission decisions, not enqueue latency.\n\n");

  std::vector<Scenario> scenarios = default_scenarios();
  for (auto& scenario : scenarios) {
    scenario.iterations = static_cast<std::size_t>(
        std::max<std::uint64_t>(1, static_cast<std::uint64_t>(scenario.iterations) * scale));
    if (quick) {
      scenario.iterations = std::min<std::size_t>(scenario.iterations, 20000);
    }
  }

  std::vector<Result> results;
  for (const auto& scenario : scenarios) {
    Result result = run_scenario(scenario);
    report(result);
    results.push_back(result);
  }

  std::printf("\nsummary\n");
  for (const auto& result : results) {
    const double throughput = result.seconds > 0.0 ? static_cast<double>(result.decisions) / result.seconds : 0.0;
    std::printf("  %-28s %-18s %.0f decisions/s over %llu completed decisions\n", result.name.c_str(),
                label(result).c_str(), throughput,
                static_cast<unsigned long long>(result.decisions));
  }
  return 0;
}

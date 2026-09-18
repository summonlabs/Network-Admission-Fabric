// Network Admission Fabric - nafd coordinator daemon.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// nafd serves admission decisions over a real local endpoint. The demo
// authority profile it can publish is a SYNTHETIC development fixture: it is not
// a network model and says nothing about any physical fabric.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "naf/naf.hpp"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) { g_stop.store(true); }

struct Options {
  std::string endpoint = "naf-coordinator";
  bool stdio = false;
  std::string state_directory{};
  bool demo_profile = false;
  std::uint64_t resources = 4;
  std::uint64_t capacity_bps = 1'000'000'000ull;
  std::uint64_t headroom_permille = 50;
  std::uint64_t run_seconds = 0;
  std::uint64_t initial_tick = 0;
  std::uint64_t incarnation = 0;
  bool quiet = false;
};

void usage() {
  std::fputs(
      "nafd - Network Admission Fabric coordinator\n"
      "usage: nafd [options]\n"
      "  --endpoint NAME       local endpoint to listen on (default naf-coordinator)\n"
      "  --stdio               serve a single connection over stdin/stdout\n"
      "  --state-dir DIR       directory for the durable admission journal\n"
      "  --profile demo        publish a SYNTHETIC development authority profile\n"
      "  --resources N         synthetic resource count (default 4)\n"
      "  --capacity BPS        synthetic usable bits/s per resource (default 1000000000)\n"
      "  --headroom PERMILLE   synthetic policy protected headroom (default 50)\n"
      "  --incarnation N       coordinator incarnation identity (default 1)\n"
      "  --tick N              initial logical tick (default 0)\n"
      "  --run-seconds N       exit after N seconds (0 = run until signalled)\n"
      "  --quiet               suppress the ready line\n"
      "  --help                print this message\n",
      stdout);
}

bool parse_u64(const char* text, std::uint64_t& out) {
  if (text == nullptr || *text == '\0') return false;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (end == nullptr || *end != '\0') return false;
  out = static_cast<std::uint64_t>(value);
  return true;
}

bool parse(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char*& value) {
      if (i + 1 >= argc) return false;
      value = argv[++i];
      return true;
    };
    const char* value = nullptr;
    if (arg == "--help" || arg == "-h") {
      usage();
      std::exit(0);
    } else if (arg == "--endpoint") {
      if (!next(value)) return false;
      options.endpoint = value;
    } else if (arg == "--stdio") {
      options.stdio = true;
    } else if (arg == "--state-dir") {
      if (!next(value)) return false;
      options.state_directory = value;
    } else if (arg == "--profile") {
      if (!next(value)) return false;
      options.demo_profile = std::string(value) == "demo";
      if (!options.demo_profile) {
        std::fprintf(stderr, "nafd: unknown profile '%s'\n", value);
        return false;
      }
    } else if (arg == "--resources") {
      if (!next(value) || !parse_u64(value, options.resources)) return false;
    } else if (arg == "--capacity") {
      if (!next(value) || !parse_u64(value, options.capacity_bps)) return false;
    } else if (arg == "--headroom") {
      if (!next(value) || !parse_u64(value, options.headroom_permille)) return false;
    } else if (arg == "--incarnation") {
      if (!next(value) || !parse_u64(value, options.incarnation)) return false;
    } else if (arg == "--tick") {
      if (!next(value) || !parse_u64(value, options.initial_tick)) return false;
    } else if (arg == "--run-seconds") {
      if (!next(value) || !parse_u64(value, options.run_seconds)) return false;
    } else if (arg == "--quiet") {
      options.quiet = true;
    } else {
      std::fprintf(stderr, "nafd: unrecognised argument '%s'\n", arg.c_str());
      return false;
    }
  }
  return true;
}

naf::Status publish_demo_profile(naf::AdmissionEngine& engine, const Options& options) {
  const naf::FabricEpoch epoch = engine.epoch();
  if (epoch.is_none()) {
    return naf::Status::error(naf::StatusCode::EpochMismatch, "engine has no fabric epoch");
  }
  if (options.resources == 0 || options.resources > naf::limits::max_resources) {
    return naf::Status::error(naf::StatusCode::InvalidArgument, "resource count is out of range");
  }
  if (options.headroom_permille > 1000) {
    return naf::Status::error(naf::StatusCode::InvalidArgument, "headroom permille exceeds 1000");
  }

  naf::AdmissionPolicy policy;
  policy.id = naf::PolicyId::from_value(1);
  policy.generation = naf::Generation::first();
  policy.epoch = epoch;
  policy.headroom_permille = static_cast<std::uint32_t>(options.headroom_permille);
  policy.allow_degraded = true;
  policy.allow_preemptible_admission = true;
  policy.on_contention = naf::ContentionAction::Defer;
  policy.defer_horizon_ticks = 100;
  policy.max_explanation_constraints = 8;
  policy.max_explanation_bytes = 2048;
  policy.max_effective_resources = 8;
  naf::Status s = engine.publish_policy(policy);
  if (!s.is_ok()) return s;

  naf::CapacitySnapshot capacity;
  capacity.snapshot = naf::CapacitySnapshotId::from_value(1);
  capacity.generation = naf::Generation::first();
  capacity.epoch = epoch;
  capacity.observed_tick = options.initial_tick;
  for (std::uint64_t i = 1; i <= options.resources; ++i) {
    naf::ResourceCapacity entry;
    entry.resource = naf::ResourceId::from_value(i);
    entry.generation = naf::Generation::first();
    entry.evidence = naf::EvidenceState::Known;
    entry.usable = naf::Rate::from_value(options.capacity_bps);
    entry.mandatory_headroom = naf::Rate{};
    capacity.resources.push_back(entry);
  }
  s = engine.publish_capacity(capacity);
  if (!s.is_ok()) return s;

  naf::ReservationSnapshot reservations;
  reservations.snapshot = naf::ReservationSnapshotId::from_value(1);
  reservations.generation = naf::Generation::first();
  reservations.epoch = epoch;
  s = engine.publish_reservations(reservations);
  if (!s.is_ok()) return s;

  naf::PathCatalog paths;
  paths.path_authority_generation = naf::Generation::first();
  paths.epoch = epoch;
  for (std::uint64_t i = 1; i <= options.resources; ++i) {
    naf::PathAdmissionFact fact;
    fact.path = naf::PathId::from_value(i);
    fact.path_authority_generation = naf::Generation::first();
    fact.state = naf::PathState::Up;
    fact.path_usable_capacity = naf::Rate::from_value(options.capacity_bps);
    fact.resources.push_back(naf::ResourceId::from_value(i));
    paths.paths.push_back(fact);
  }
  s = engine.publish_paths(paths);
  if (!s.is_ok()) return s;

  naf::QoSClassCatalog qos;
  qos.generation = naf::Generation::first();
  qos.epoch = epoch;
  naf::QoSClassFact qos_fact;
  qos_fact.qos = naf::QoSClassId::from_value(1);
  qos_fact.generation = naf::Generation::first();
  qos_fact.minimum_rate = naf::Rate{};
  qos_fact.maximum_rate = naf::Rate::from_value(~std::uint64_t{0});
  qos_fact.maximum_latency = naf::Latency::from_value(~std::uint64_t{0});
  qos_fact.degradation_allowed = true;
  qos.classes.push_back(qos_fact);
  s = engine.publish_qos_catalog(qos);
  if (!s.is_ok()) return s;

  naf::PriorityClassCatalog priorities;
  priorities.generation = naf::Generation::first();
  priorities.epoch = epoch;
  naf::PriorityClassFact priority_fact;
  priority_fact.priority = naf::PriorityClassId::from_value(1);
  priority_fact.generation = naf::Generation::first();
  priority_fact.rank = 1;
  priority_fact.preemptible = true;
  priorities.classes.push_back(priority_fact);
  return engine.publish_priority_catalog(priorities);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse(argc, argv, options)) {
    usage();
    return 2;
  }
  if (options.incarnation == 0) {
    options.incarnation = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() & 0x7FFFFFFFull);
    if (options.incarnation == 0) options.incarnation = 1;
  }

  naf::ipc::ServerConfig config;
  config.endpoint = options.endpoint;
  config.stdio = options.stdio;
  config.state_directory = options.state_directory;
  config.engine.epoch = naf::FabricEpoch::none();
  config.engine.incarnation = naf::CoordinatorIncarnation::from_value(options.incarnation);
  config.engine.initial_tick = options.initial_tick;

  naf::ipc::CoordinatorServer server(config);
  auto started = server.start();
  if (!started) {
    std::fprintf(stderr, "nafd: start failed: %s\n", started.status().to_string().c_str());
    return 1;
  }
  naf::RecoveryReport report = started.value();
  if (!report.status.is_ok()) {
    std::fprintf(stderr, "nafd: recovery refused: %s\n", report.status.to_string().c_str());
    return 1;
  }

  if (options.demo_profile) {
    naf::Status published = publish_demo_profile(server.engine(), options);
    if (!published.is_ok()) {
      std::fprintf(stderr, "nafd: authority profile failed: %s\n", published.to_string().c_str());
      return 1;
    }
  } else {
    // No capacity authority has published. Nothing can be admitted until one
    // does; the coordinator says so rather than guessing.
    std::fprintf(stderr, "nafd: no authority profile published; all admissions will be STALE_INPUT\n");
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  if (!options.quiet) {
    const naf::FabricStatus status = server.engine().status();
    std::printf("nafd ready endpoint=%s transport=%s epoch=%llu incarnation=%llu readiness=%s durable=%d\n",
                options.stdio ? "-" : options.endpoint.c_str(), server.transport_name().c_str(),
                static_cast<unsigned long long>(status.epoch.value()),
                static_cast<unsigned long long>(status.incarnation.value()),
                std::string(naf::to_string(status.readiness)).c_str(), status.durable ? 1 : 0);
    std::fflush(stdout);
  }

  // The accept loop blocks in the transport. A watchdog (optional deadline) or
  // a signal sets the stop flag; a dedicated thread then closes the listener so
  // the blocked accept returns. Shutdown never joins a worker while holding a
  // lock the worker needs.
  std::thread watchdog;
  if (options.run_seconds != 0) {
    watchdog = std::thread([&options]() {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(options.run_seconds);
      while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      g_stop.store(true);
    });
  }
  std::thread stopper([&server]() {
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    server.stop();
  });
  const naf::Status served = server.run();
  g_stop.store(true);
  stopper.join();
  if (watchdog.joinable()) watchdog.join();
  if (!served.is_ok()) {
    std::fprintf(stderr, "nafd: serve ended: %s\n", served.to_string().c_str());
    return 1;
  }
  return 0;
}

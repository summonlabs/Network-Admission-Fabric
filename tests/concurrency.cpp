// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Concurrency, race and shutdown behaviour. No test in this file uses a
// timeout: a hang is a defect to diagnose, not something to paper over.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fixture.hpp"
#include "naf/naf.hpp"
#include "support/temp_dir.hpp"
#include "support/test_framework.hpp"

using namespace naftest;

namespace {

std::string unique_endpoint(const std::string& tag) {
  static std::atomic<std::uint64_t> counter{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return "naf-test-" + tag + "-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1));
}

naf::AdmissionRequest server_request(const naf::FabricStatus& status, std::uint64_t sequence,
                                     std::uint64_t minimum, std::uint64_t desired,
                                     std::uint64_t maximum) {
  naf::AdmissionRequest request;
  request.request = naf::AdmissionRequestId::from_value(sequence);
  request.demand = naf::DemandId::from_value(sequence);
  request.demand_generation = naf::Generation::first();
  request.attempt = naf::AttemptId::from_value(sequence);
  request.rate.minimum = naf::Rate::from_value(minimum);
  request.rate.desired = naf::Rate::from_value(desired);
  request.rate.maximum = naf::Rate::from_value(maximum);
  request.qos = naf::QoSClassId::from_value(1);
  request.qos_generation = naf::Generation::first();
  request.priority = naf::PriorityClassId::from_value(1);
  request.priority_generation = naf::Generation::first();
  naf::ResourceBinding binding;
  binding.resource = naf::ResourceId::from_value(1);
  binding.generation = naf::Generation::first();
  request.resource_bindings.push_back(binding);
  request.expected.capacity_snapshot = status.capacity_snapshot;
  request.expected.capacity_generation = status.capacity_generation;
  request.expected.reservation_snapshot = status.reservation_snapshot;
  request.expected.reservation_generation = status.reservation_generation;
  request.expected.path_authority_generation = status.path_generation;
  request.expected.policy_generation = status.policy_generation;
  request.expected.qos_catalog_generation = status.qos_generation;
  request.expected.priority_catalog_generation = status.priority_generation;
  return request;
}

}  // namespace

NAF_TEST(concurrency, many_threads_cannot_break_accounting_closure) {
  FabricOptions options;
  options.resources = 2;
  options.usable = 10000;
  Fabric fabric(options);

  constexpr int kThreads = 8;
  constexpr int kAttempts = 120;
  std::atomic<std::uint64_t> admitted{0};
  std::atomic<std::uint64_t> refusals{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&fabric, &admitted, &refusals, t]() {
      for (int i = 0; i < kAttempts; ++i) {
        const std::uint64_t sequence =
            static_cast<std::uint64_t>(t) * 100000 + static_cast<std::uint64_t>(i) + 1;
        naf::AdmissionRequest request = fabric.make_request(sequence, sequence, 20, 30, 40);
        auto decision = fabric.engine().admit_local(request);
        if (!decision.has_value()) continue;
        if (decision.value().outcome == naf::AdmissionOutcome::Admit) {
          admitted.fetch_add(1);
        } else {
          refusals.fetch_add(1);
        }
      }
    });
  }
  for (auto& worker : workers) worker.join();

  NAF_CHECK(admitted.load() + refusals.load() ==
            static_cast<std::uint64_t>(kThreads) * static_cast<std::uint64_t>(kAttempts));
  for (std::uint64_t r = 1; r <= 2; ++r) {
    const std::uint64_t load = fabric.engine().admitted_load(naf::ResourceId::from_value(r)).value();
    NAF_CHECK(load <= 10000);
  }
  const naf::FabricStatus status = fabric.engine().status();
  NAF_CHECK_U64(status.admissions, admitted.load());
  NAF_CHECK_U64(status.total_admitted.value(),
                fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value());
}

NAF_TEST(concurrency, capacity_reduction_races_never_break_the_invariant) {
  FabricOptions options;
  options.resources = 1;
  options.usable = 5000;
  Fabric fabric(options);

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> observed_violations{0};
  std::thread admitters([&]() {
    std::uint64_t sequence = 1;
    while (!stop.load()) {
      naf::AdmissionRequest request = fabric.make_request(sequence, sequence, 10, 20, 30);
      auto decision = fabric.engine().admit_local(request);
      if (decision.has_value() && naf::is_admitting(decision.value().outcome)) {
        const std::uint64_t load = fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value();
        if (load > 5000) observed_violations.fetch_add(1);
      }
      ++sequence;
    }
  });

  std::thread publisher([&]() {
    for (std::uint64_t i = 2; i <= 40; ++i) {
      naf::CapacitySnapshot snapshot = fabric.capacity();
      snapshot.snapshot = naf::CapacitySnapshotId::from_value(i);
      snapshot.generation = naf::Generation::from_value(i);
      const std::uint64_t usable = 5000 - (i % 7) * 500;
      snapshot.resources[0].usable = naf::Rate::from_value(usable);
      (void)fabric.engine().publish_capacity(snapshot);
      const std::uint64_t load = fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value();
      if (load > 5000) observed_violations.fetch_add(1);
    }
    stop.store(true);
  });

  admitters.join();
  publisher.join();
  NAF_CHECK_U64(observed_violations.load(), 0);
}

NAF_TEST(concurrency, concurrent_revocations_release_exactly_once) {
  FabricOptions options;
  options.resources = 1;
  options.usable = 100000;
  Fabric fabric(options);

  std::vector<naf::AdmissionDecisionId> decisions;
  for (std::uint64_t i = 1; i <= 16; ++i) {
    const naf::AdmissionDecision decision = fabric.admit_full(fabric.make_request(i, i, 10, 10, 10));
    NAF_REQUIRE(decision.outcome == naf::AdmissionOutcome::Admit);
    decisions.push_back(decision.decision);
  }
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 160);

  std::atomic<std::uint64_t> releases{0};
  std::vector<std::thread> workers;
  for (int t = 0; t < 8; ++t) {
    workers.emplace_back([&fabric, &decisions, &releases]() {
      for (const auto decision : decisions) {
        auto result = fabric.engine().revoke(decision, naf::RevocationReason::OperatorRequest,
                                             naf::ClaimContext::in_process());
        if (result.has_value() && result.value().ledger_released) releases.fetch_add(1);
      }
    });
  }
  for (auto& worker : workers) worker.join();

  NAF_CHECK_U64(releases.load(), 16);
  NAF_CHECK_U64(fabric.engine().admitted_load(naf::ResourceId::from_value(1)).value(), 0);
  NAF_CHECK_U64(fabric.engine().status().live_decisions, 0);
}

NAF_TEST(concurrency, server_stop_unblocks_an_idle_client_and_joins) {
  if (!naf::ipc::local_transport_available()) return;
  TempDir temp("server-stop");
  naf::ipc::ServerConfig config;
  config.endpoint = unique_endpoint("stop");
  config.state_directory = temp.path();
  config.engine.epoch = naf::FabricEpoch::none();
  config.engine.incarnation = naf::CoordinatorIncarnation::from_value(3);

  naf::ipc::CoordinatorServer server(config);
  auto started = server.start();
  NAF_REQUIRE(started.has_value());
  NAF_CHECK(started.value().status.is_ok());

  std::thread runner([&server]() { (void)server.run(); });

  naf::ipc::ClientOptions options;
  options.endpoint = config.endpoint;
  options.publisher = naf::PublisherId::from_value(1);
  options.boot = naf::BootId::from_value(1);
  naf::ipc::Client client(options);
  auto welcome = client.open();
  NAF_REQUIRE(welcome.has_value());
  NAF_CHECK(welcome.value().accepted);

  auto status = client.status();
  NAF_REQUIRE(status.has_value());
  NAF_CHECK(status.value().durable);
  // No authority profile has been published, so the coordinator refuses
  // everything and says exactly why.
  NAF_CHECK(status.value().readiness == naf::EngineReadiness::AwaitingPolicy);

  // stop() must unblock the idle connection and join every worker.
  server.stop();
  runner.join();
  NAF_CHECK(!server.running());

  // The client sees an orderly disconnect rather than hanging.
  auto after = client.status();
  NAF_CHECK(!after.has_value());
  (void)client.close();
  NAF_CHECK_EQ(server.engine().status().decisions, 0);
}

NAF_TEST(concurrency, a_truncated_request_frame_never_produces_a_decision) {
  if (!naf::ipc::local_transport_available()) return;
  TempDir temp("server-truncated");
  naf::ipc::ServerConfig config;
  config.endpoint = unique_endpoint("truncated");
  config.state_directory = temp.path();
  config.engine.epoch = naf::FabricEpoch::none();
  config.engine.incarnation = naf::CoordinatorIncarnation::from_value(4);

  naf::ipc::CoordinatorServer server(config);
  auto started = server.start();
  NAF_REQUIRE(started.has_value());
  std::thread runner([&server]() { (void)server.run(); });

  {
    auto stream = naf::ipc::connect_endpoint(config.endpoint, 30000);
    NAF_REQUIRE(stream.has_value());
    std::unique_ptr<naf::ipc::Stream> connection = std::move(stream).value();

    naf::ipc::HelloMessage hello;
    hello.protocol_version = naf::ipc::protocol_version;
    hello.publisher = naf::PublisherId::from_value(9);
    hello.boot = naf::BootId::from_value(9);
    hello.max_frame = naf::limits::max_frame_bytes;
    NAF_REQUIRE(naf::ipc::write_message(*connection, naf::ipc::MessageKind::Hello, 0,
                                        naf::ipc::encode(hello), config.max_frame)
                    .is_ok());
    auto reply = naf::ipc::read_message(*connection, config.max_frame);
    NAF_REQUIRE(reply.has_value());
    NAF_CHECK(reply.value().kind == naf::ipc::MessageKind::Welcome);

    // Announce an Admit frame and then vanish part-way through the payload.
    naf::ipc::AdmitMessage admit;
    admit.claim.origin = naf::OriginKind::Claimant;
    admit.claim.publisher = naf::PublisherId::from_value(9);
    admit.claim.boot = naf::BootId::from_value(9);
    auto welcome = naf::ipc::decode_welcome(reply.value().payload);
    NAF_REQUIRE(welcome.has_value());
    admit.claim.session = welcome.value().session;
    admit.claim.incarnation = welcome.value().incarnation;
    admit.claim.epoch = welcome.value().epoch;
    admit.request = server_request(server.engine().status(), 1, 10, 20, 30);
    admit.request.attempt = naf::AttemptId::from_value(1);
    const naf::ByteBuffer encoded = naf::ipc::encode(admit);
    const naf::ByteBuffer frame =
        naf::ipc::encode_wire_frame(naf::ipc::MessageKind::Admit, 0, encoded);
    NAF_REQUIRE(connection->write_all(std::span<const std::byte>(frame.data(), frame.size() / 2)).is_ok());
    NAF_REQUIRE(connection->close().is_ok());
  }

  // The server is still healthy and recorded no decision from the partial frame.
  {
    naf::ipc::ClientOptions options;
    options.endpoint = config.endpoint;
    options.publisher = naf::PublisherId::from_value(2);
    options.boot = naf::BootId::from_value(2);
    naf::ipc::Client client(options);
    auto welcome = client.open();
    NAF_REQUIRE(welcome.has_value());
    NAF_CHECK(welcome.value().accepted);
    auto status = client.status();
    NAF_REQUIRE(status.has_value());
    NAF_CHECK_U64(status.value().decisions, 0);
    NAF_CHECK_U64(status.value().ledger_entries, 0);
    (void)client.close();
  }

  server.stop();
  runner.join();
}

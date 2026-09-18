// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// REAL multiprocess validation: a separately spawned nafd process is driven over
// a real OS transport (Windows named pipe, POSIX AF_UNIX socket), killed
// abruptly and restarted, and the fencing behaviour of the restarted
// incarnation is verified end to end.
//
// This is a local-process proof. It is not a multi-node, multi-switch, RDMA,
// NVLink, optical, NIC or DPU validation and says nothing about any physical
// fabric.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "naf/naf.hpp"
#include "support/process.hpp"
#include "support/temp_dir.hpp"
#include "support/test_framework.hpp"

using namespace naftest;

#ifndef NAF_NAFD_EXECUTABLE
#define NAF_NAFD_EXECUTABLE "nafd"
#endif

namespace {

std::string unique_endpoint(const std::string& tag) {
  static std::atomic<std::uint64_t> counter{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return "naf-mp-" + tag + "-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1));
}

naf::AdmissionRequest daemon_request(const naf::FabricStatus& status, std::uint64_t sequence,
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

/// Connects and handshakes, retrying while the freshly spawned daemon finishes
/// binding its endpoint. The retry count is a fixture bound, not a test timeout.
bool open_with_retry(naf::ipc::Client& client, int attempts, naf::ipc::WelcomeMessage& welcome) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    auto result = client.open();
    if (result.has_value()) {
      welcome = result.value();
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

struct DaemonArgs {
  std::string endpoint;
  std::string state_directory;
  std::string incarnation;
};

std::vector<std::string> daemon_command_line(const DaemonArgs& args) {
  std::vector<std::string> command = {"--endpoint", args.endpoint, "--state-dir",
                                      args.state_directory, "--profile", "demo",
                                      "--resources", "2", "--capacity", "1000000000", "--quiet"};
  if (!args.incarnation.empty()) {
    command.push_back("--incarnation");
    command.push_back(args.incarnation);
  }
  return command;
}

}  // namespace

NAF_TEST(multiprocess, restart_in_a_real_process_fences_the_previous_incarnation) {
  if (!naf::ipc::local_transport_available()) return;
  TempDir state("mp-restart");
  const std::string endpoint = unique_endpoint("restart");
  DaemonArgs args;
  args.endpoint = endpoint;
  args.state_directory = state.path().string();
  // No explicit incarnation: the daemon derives one per boot, which is exactly
  // what a restarted coordinator does.

  naf::ipc::ClientOptions options;
  options.endpoint = endpoint;
  options.publisher = naf::PublisherId::from_value(1);
  options.boot = naf::BootId::from_value(1);

  naf::ipc::WelcomeMessage first_welcome;
  std::uint64_t first_decision = 0;
  {
    ChildProcess daemon;
    NAF_REQUIRE(daemon.start(NAF_NAFD_EXECUTABLE, daemon_command_line(args)));
    naf::ipc::Client client(options);
    NAF_REQUIRE(open_with_retry(client, 500, first_welcome));
    NAF_CHECK(first_welcome.accepted);
    NAF_CHECK_U64(first_welcome.epoch.value(), 1);
    NAF_CHECK(first_welcome.session.is_known());

    auto status = client.status();
    NAF_REQUIRE(status.has_value());
    NAF_CHECK(status.value().readiness == naf::EngineReadiness::Ready);
    NAF_CHECK(status.value().durable);

    auto decision = client.admit(daemon_request(status.value(), 1, 100, 200, 300));
    NAF_REQUIRE(decision.has_value());
    NAF_CHECK(decision.value().outcome == naf::AdmissionOutcome::Admit);
    NAF_CHECK_U64(decision.value().granted.value(), 300);
    first_decision = decision.value().decision.value();

    // A duplicate attempt over the real transport is idempotent.
    auto replay = client.admit(daemon_request(status.value(), 1, 100, 200, 300));
    NAF_REQUIRE(replay.has_value());
    NAF_CHECK(replay.value().idempotent_replay);
    NAF_CHECK_U64(replay.value().decision.value(), first_decision);

    NAF_CHECK(daemon.kill());
    NAF_CHECK(!daemon.running());
    (void)client.close();
  }

  {
    ChildProcess daemon;
    NAF_REQUIRE(daemon.start(NAF_NAFD_EXECUTABLE, daemon_command_line(args)));
    naf::ipc::Client client(options);
    naf::ipc::WelcomeMessage second_welcome;
    NAF_REQUIRE(open_with_retry(client, 500, second_welcome));
    NAF_CHECK(second_welcome.accepted);
    NAF_CHECK_U64(second_welcome.epoch.value(), first_welcome.epoch.value() + 1);
    NAF_CHECK(second_welcome.incarnation != first_welcome.incarnation);

    // The previous incarnation's session and epoch are fenced.
    naf::ClaimContext stale;
    stale.origin = naf::OriginKind::Claimant;
    stale.publisher = options.publisher;
    stale.boot = options.boot;
    stale.session = first_welcome.session;
    stale.incarnation = first_welcome.incarnation;
    stale.epoch = first_welcome.epoch;
    client.set_claim_override(stale);
    auto before_claims = client.status();
    NAF_REQUIRE(before_claims.has_value());
    auto fenced = client.admit(daemon_request(before_claims.value(), 50, 100, 200, 300));
    NAF_REQUIRE(fenced.has_value());
    NAF_CHECK(fenced.value().outcome == naf::AdmissionOutcome::FencedClaimant);

    // A claim that reaches forward to an epoch that does not exist yet is a
    // conflicting identity, not a fence.
    naf::ClaimContext future = stale;
    future.epoch = naf::FabricEpoch::from_value(second_welcome.epoch.value() + 5);
    future.session = second_welcome.session;
    future.incarnation = second_welcome.incarnation;
    client.set_claim_override(future);
    auto before_conflict = client.status();
    NAF_REQUIRE(before_conflict.has_value());
    auto conflicting = client.admit(daemon_request(before_conflict.value(), 51, 100, 200, 300));
    NAF_REQUIRE(conflicting.has_value());
    NAF_CHECK(conflicting.value().outcome == naf::AdmissionOutcome::ConflictingInput);

    // A stale session nonce under the current epoch is fenced.
    naf::ClaimContext wrong_session = stale;
    wrong_session.epoch = second_welcome.epoch;
    wrong_session.incarnation = second_welcome.incarnation;
    wrong_session.session = naf::SessionNonce::from_value(999983);
    client.set_claim_override(wrong_session);
    auto before_session = client.status();
    NAF_REQUIRE(before_session.has_value());
    auto session_fenced = client.admit(daemon_request(before_session.value(), 52, 100, 200, 300));
    NAF_REQUIRE(session_fenced.has_value());
    NAF_CHECK(session_fenced.value().outcome == naf::AdmissionOutcome::FencedClaimant);

    client.clear_claim_override();
    auto status = client.status();
    NAF_REQUIRE(status.has_value());
    NAF_CHECK(status.value().epoch.value() == second_welcome.epoch.value());
    NAF_CHECK(status.value().fences >= 2);
    NAF_CHECK(status.value().conflicts >= 1);
    NAF_CHECK(status.value().decisions >= 3);
    NAF_CHECK(status.value().live_decisions >= 1);

    // The durable decision history survived the kill: the pre-restart attempt
    // replays from it, with the original decision identity.
    auto replay = client.admit(daemon_request(status.value(), 1, 100, 200, 300));
    NAF_REQUIRE(replay.has_value());
    NAF_CHECK(replay.value().idempotent_replay);
    NAF_CHECK_U64(replay.value().decision.value(), first_decision);
    NAF_CHECK(replay.value().outcome == naf::AdmissionOutcome::Admit);

    // The restarted incarnation admits normally under its own authority.
    auto fresh = client.admit(daemon_request(status.value(), 60, 100, 100, 100));
    NAF_REQUIRE(fresh.has_value());
    NAF_CHECK(fresh.value().outcome == naf::AdmissionOutcome::Admit);
    NAF_CHECK(fresh.value().epoch.value() == second_welcome.epoch.value());

    (void)client.close();
    NAF_CHECK(daemon.kill());
  }

  // The journal really is on disk and non-empty.
  std::error_code ec;
  const auto size = std::filesystem::file_size(state.file("admission.journal"), ec);
  NAF_REQUIRE(!ec);
  NAF_CHECK(size > 0);
}

NAF_TEST(multiprocess, killing_the_coordinator_mid_request_surfaces_a_failure) {
  if (!naf::ipc::local_transport_available()) return;
  TempDir state("mp-kill");
  const std::string endpoint = unique_endpoint("kill");
  DaemonArgs args;
  args.endpoint = endpoint;
  args.state_directory = state.path().string();
  args.incarnation = "21";

  ChildProcess daemon;
  NAF_REQUIRE(daemon.start(NAF_NAFD_EXECUTABLE, daemon_command_line(args)));

  naf::ipc::ClientOptions options;
  options.endpoint = endpoint;
  options.publisher = naf::PublisherId::from_value(3);
  options.boot = naf::BootId::from_value(3);

  naf::ipc::WelcomeMessage welcome;
  naf::ipc::Client client(options);
  NAF_REQUIRE(open_with_retry(client, 500, welcome));
  NAF_CHECK(welcome.accepted);

  NAF_CHECK(daemon.kill());
  NAF_CHECK(!daemon.running());

  // The next request must fail cleanly instead of hanging or inventing a result.
  auto status = client.status();
  NAF_CHECK(!status.has_value());
  NAF_CHECK(status.status().code() == naf::StatusCode::TransportFailure);
  (void)client.close();

  // A fresh coordinator over the same state directory comes up and serves.
  ChildProcess restarted;
  NAF_REQUIRE(restarted.start(NAF_NAFD_EXECUTABLE, daemon_command_line(args)));
  naf::ipc::Client second(options);
  naf::ipc::WelcomeMessage second_welcome;
  NAF_REQUIRE(open_with_retry(second, 500, second_welcome));
  NAF_CHECK(second_welcome.accepted);
  NAF_CHECK(second_welcome.epoch.value() > welcome.epoch.value());
  auto recovered = second.status();
  NAF_REQUIRE(recovered.has_value());
  NAF_CHECK(recovered.value().readiness == naf::EngineReadiness::Ready);
  (void)second.close();
  NAF_CHECK(restarted.kill());
}

NAF_TEST(multiprocess, a_second_coordinator_process_is_independent) {
  if (!naf::ipc::local_transport_available()) return;
  TempDir state_a("mp-a");
  TempDir state_b("mp-b");
  DaemonArgs args_a;
  args_a.endpoint = unique_endpoint("a");
  args_a.state_directory = state_a.path().string();
  args_a.incarnation = "31";
  DaemonArgs args_b;
  args_b.endpoint = unique_endpoint("b");
  args_b.state_directory = state_b.path().string();
  args_b.incarnation = "32";
  (void)0;

  ChildProcess daemon_a;
  ChildProcess daemon_b;
  NAF_REQUIRE(daemon_a.start(NAF_NAFD_EXECUTABLE, daemon_command_line(args_a)));
  NAF_REQUIRE(daemon_b.start(NAF_NAFD_EXECUTABLE, daemon_command_line(args_b)));

  naf::ipc::ClientOptions options_a;
  options_a.endpoint = args_a.endpoint;
  options_a.publisher = naf::PublisherId::from_value(1);
  options_a.boot = naf::BootId::from_value(1);
  naf::ipc::ClientOptions options_b;
  options_b.endpoint = args_b.endpoint;
  options_b.publisher = naf::PublisherId::from_value(1);
  options_b.boot = naf::BootId::from_value(1);

  naf::ipc::Client client_a(options_a);
  naf::ipc::Client client_b(options_b);
  naf::ipc::WelcomeMessage welcome_a;
  naf::ipc::WelcomeMessage welcome_b;
  NAF_REQUIRE(open_with_retry(client_a, 500, welcome_a));
  NAF_REQUIRE(open_with_retry(client_b, 500, welcome_b));
  NAF_CHECK(welcome_a.accepted);
  NAF_CHECK(welcome_b.accepted);
  NAF_CHECK_U64(welcome_a.incarnation.value(), 31);
  NAF_CHECK_U64(welcome_b.incarnation.value(), 32);

  auto status_a = client_a.status();
  auto status_b = client_b.status();
  NAF_REQUIRE(status_a.has_value());
  NAF_REQUIRE(status_b.has_value());
  NAF_CHECK_U64(status_a.value().decisions, 0);
  NAF_CHECK_U64(status_b.value().decisions, 0);

  // Admitting on one coordinator must not affect the other.
  auto decision = client_a.admit(daemon_request(status_a.value(), 1, 100, 200, 300));
  NAF_REQUIRE(decision.has_value());
  NAF_CHECK(decision.value().outcome == naf::AdmissionOutcome::Admit);

  auto after_b = client_b.status();
  NAF_REQUIRE(after_b.has_value());
  NAF_CHECK_U64(after_b.value().decisions, 0);
  NAF_CHECK_U64(after_b.value().total_admitted.value(), 0);
  // A claim minted by A is meaningless to B.
  naf::ClaimContext cross;
  cross.origin = naf::OriginKind::Claimant;
  cross.publisher = options_a.publisher;
  cross.boot = options_a.boot;
  cross.session = welcome_a.session;
  cross.incarnation = welcome_a.incarnation;
  cross.epoch = welcome_a.epoch;
  client_b.set_claim_override(cross);
  auto cross_decision = client_b.admit(daemon_request(after_b.value(), 2, 100, 200, 300));
  NAF_REQUIRE(cross_decision.has_value());
  NAF_CHECK(cross_decision.value().outcome == naf::AdmissionOutcome::FencedClaimant);

  (void)client_a.close();
  (void)client_b.close();
  NAF_CHECK(daemon_a.kill());
  NAF_CHECK(daemon_b.kill());
}

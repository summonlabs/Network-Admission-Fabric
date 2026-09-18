// Network Admission Fabric - nafctl claimant client.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "naf/naf.hpp"

namespace {

struct Options {
  std::string endpoint = "naf-coordinator";
  std::string command{};
  std::uint64_t publisher = 1;
  std::uint64_t boot = 1;
  std::uint64_t demand = 1;
  std::uint64_t request_id = 1;
  std::uint64_t resource = 0;
  std::uint64_t path = 0;
  std::uint64_t decision = 0;
  std::uint64_t minimum = 0;
  std::uint64_t desired = 0;
  std::uint64_t maximum = 0;
  std::uint64_t attempt = 1;
  std::uint64_t latency = 0;
  std::uint64_t expected_epoch = 0;
  std::uint64_t expected_session = 0;
  bool force_claim = false;
  bool auto_authority = true;
};

void usage() {
  std::fputs(
      "nafctl - Network Admission Fabric client\n"
      "usage: nafctl [--endpoint NAME] [--publisher N] [--boot N] COMMAND [options]\n"
      "commands:\n"
      "  status\n"
      "  admit      --min BPS --desired BPS --max BPS [--resource N] [--path N]\n"
      "             [--demand N] [--request N] [--attempt N] [--latency US]\n"
      "  revalidate --decision N\n"
      "  revoke     --decision N\n"
      "diagnostic options (no authority is granted by them):\n"
      "  --force-claim --epoch N [--session N]   send an explicit claim instead of the granted one\n"
      "  --no-auto-authority                     do not fill the authority expectation from status\n",
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
    const char* value = nullptr;
    auto next = [&]() {
      if (i + 1 >= argc) return false;
      value = argv[++i];
      return true;
    };
    if (arg == "--help" || arg == "-h") {
      usage();
      std::exit(0);
    } else if (arg == "--endpoint") {
      if (!next()) return false;
      options.endpoint = value;
    } else if (arg == "--publisher") {
      if (!next() || !parse_u64(value, options.publisher)) return false;
    } else if (arg == "--boot") {
      if (!next() || !parse_u64(value, options.boot)) return false;
    } else if (arg == "--min") {
      if (!next() || !parse_u64(value, options.minimum)) return false;
    } else if (arg == "--desired") {
      if (!next() || !parse_u64(value, options.desired)) return false;
    } else if (arg == "--max") {
      if (!next() || !parse_u64(value, options.maximum)) return false;
    } else if (arg == "--resource") {
      if (!next() || !parse_u64(value, options.resource)) return false;
    } else if (arg == "--path") {
      if (!next() || !parse_u64(value, options.path)) return false;
    } else if (arg == "--demand") {
      if (!next() || !parse_u64(value, options.demand)) return false;
    } else if (arg == "--request") {
      if (!next() || !parse_u64(value, options.request_id)) return false;
    } else if (arg == "--attempt") {
      if (!next() || !parse_u64(value, options.attempt)) return false;
    } else if (arg == "--decision") {
      if (!next() || !parse_u64(value, options.decision)) return false;
    } else if (arg == "--latency") {
      if (!next() || !parse_u64(value, options.latency)) return false;
    } else if (arg == "--epoch") {
      if (!next() || !parse_u64(value, options.expected_epoch)) return false;
    } else if (arg == "--session") {
      if (!next() || !parse_u64(value, options.expected_session)) return false;
    } else if (arg == "--force-claim") {
      options.force_claim = true;
    } else if (arg == "--no-auto-authority") {
      options.auto_authority = false;
    } else if (arg.starts_with("--")) {
      std::fprintf(stderr, "nafctl: unrecognised option '%s'\n", arg.c_str());
      return false;
    } else if (options.command.empty()) {
      options.command = arg;
    } else {
      std::fprintf(stderr, "nafctl: unexpected argument '%s'\n", arg.c_str());
      return false;
    }
  }
  return !options.command.empty();
}

int fail(const char* what, const naf::Status& status) {
  std::fprintf(stderr, "nafctl: %s failed: %s\n", what, status.to_string().c_str());
  return 1;
}

void fill_expectation(const naf::FabricStatus& status, naf::AdmissionRequest& request) {
  request.expected.capacity_snapshot = status.capacity_snapshot;
  request.expected.capacity_generation = status.capacity_generation;
  request.expected.reservation_snapshot = status.reservation_snapshot;
  request.expected.reservation_generation = status.reservation_generation;
  request.expected.path_authority_generation = status.path_generation;
  request.expected.policy_generation = status.policy_generation;
  request.expected.qos_catalog_generation = status.qos_generation;
  request.expected.priority_catalog_generation = status.priority_generation;
}

int command_status(naf::ipc::Client& client) {
  auto status = client.status();
  if (!status) return fail("status", status.status());
  const naf::FabricStatus& s = status.value();
  std::printf("readiness=%s epoch=%llu incarnation=%llu tick=%llu\n",
              std::string(naf::to_string(s.readiness)).c_str(),
              static_cast<unsigned long long>(s.epoch.value()),
              static_cast<unsigned long long>(s.incarnation.value()),
              static_cast<unsigned long long>(s.tick));
  std::printf("generations policy=%llu capacity=%llu reservations=%llu paths=%llu qos=%llu priority=%llu\n",
              static_cast<unsigned long long>(s.policy_generation.value()),
              static_cast<unsigned long long>(s.capacity_generation.value()),
              static_cast<unsigned long long>(s.reservation_generation.value()),
              static_cast<unsigned long long>(s.path_generation.value()),
              static_cast<unsigned long long>(s.qos_generation.value()),
              static_cast<unsigned long long>(s.priority_generation.value()));
  std::printf("snapshots capacity=%llu reservations=%llu audit=%llu\n",
              static_cast<unsigned long long>(s.capacity_snapshot.value()),
              static_cast<unsigned long long>(s.reservation_snapshot.value()),
              static_cast<unsigned long long>(s.audit_sequence.value()));
  std::printf("counts decisions=%llu admissions=%llu refusals=%llu deferrals=%llu replays=%llu "
              "conflicts=%llu fences=%llu stale=%llu\n",
              static_cast<unsigned long long>(s.decisions),
              static_cast<unsigned long long>(s.admissions),
              static_cast<unsigned long long>(s.refusals),
              static_cast<unsigned long long>(s.deferrals),
              static_cast<unsigned long long>(s.idempotent_replays),
              static_cast<unsigned long long>(s.conflicts),
              static_cast<unsigned long long>(s.fences),
              static_cast<unsigned long long>(s.stale_refusals));
  std::printf("ledger entries=%llu live_decisions=%llu sessions=%llu total_admitted=%llu durable=%d\n",
              static_cast<unsigned long long>(s.ledger_entries),
              static_cast<unsigned long long>(s.live_decisions),
              static_cast<unsigned long long>(s.live_sessions),
              static_cast<unsigned long long>(s.total_admitted.value()), s.durable ? 1 : 0);
  return 0;
}

int command_admit(naf::ipc::Client& client, const Options& options) {
  naf::AdmissionRequest request;
  request.request = naf::AdmissionRequestId::from_value(options.request_id);
  request.demand = naf::DemandId::from_value(options.demand);
  request.demand_generation = naf::Generation::first();
  request.attempt = naf::AttemptId::from_value(options.attempt);
  request.rate.minimum = naf::Rate::from_value(options.minimum);
  request.rate.desired = naf::Rate::from_value(options.desired);
  request.rate.maximum = naf::Rate::from_value(options.maximum);
  request.qos = naf::QoSClassId::from_value(1);
  request.qos_generation = naf::Generation::first();
  request.priority = naf::PriorityClassId::from_value(1);
  request.priority_generation = naf::Generation::first();
  request.maximum_latency = naf::Latency::from_value(options.latency);
  request.provenance.publisher = naf::PublisherId::from_value(options.publisher);
  request.provenance.boot = naf::BootId::from_value(options.boot);
  request.provenance.attempt = request.attempt;
  request.provenance.origin = naf::OriginKind::Claimant;

  if (options.auto_authority) {
    auto status = client.status();
    if (!status) return fail("status", status.status());
    fill_expectation(status.value(), request);
    if (request.resource_bindings.empty()) {
      const std::uint64_t resource = options.resource != 0 ? options.resource : 1;
      naf::ResourceBinding binding;
      binding.resource = naf::ResourceId::from_value(resource);
      if (status.value().has_capacity &&
          resource <= status.value().capacity_generation.value() + 0) {
        binding.generation = status.value().capacity_generation;
      } else {
        binding.generation = status.value().capacity_generation;
      }
      request.resource_bindings.push_back(binding);
    }
  } else if (options.resource != 0) {
    naf::ResourceBinding binding;
    binding.resource = naf::ResourceId::from_value(options.resource);
    binding.generation = naf::Generation::first();
    request.resource_bindings.push_back(binding);
  }
  if (options.path != 0) {
    naf::PathCandidate candidate;
    candidate.path = naf::PathId::from_value(options.path);
    candidate.path_authority_generation = naf::Generation::first();
    request.path_candidates.push_back(candidate);
  }

  auto decision = client.admit(request);
  if (!decision) return fail("admit", decision.status());
  const naf::AdmissionDecision& d = decision.value();
  std::printf("outcome=%s decision=%llu audit=%llu granted=%llu minimum=%llu desired=%llu "
              "path=%llu idempotent=%d epoch=%llu\n",
              std::string(naf::to_string(d.outcome)).c_str(),
              static_cast<unsigned long long>(d.decision.value()),
              static_cast<unsigned long long>(d.audit.value()),
              static_cast<unsigned long long>(d.granted.value()),
              static_cast<unsigned long long>(d.minimum_guaranteed.value()),
              static_cast<unsigned long long>(d.desired.value()),
              static_cast<unsigned long long>(d.selected_path.value()), d.idempotent_replay ? 1 : 0,
              static_cast<unsigned long long>(d.epoch.value()));
  const std::string rendered = d.explanation.render();
  if (!rendered.empty()) std::printf("binding:\n%s\n", rendered.c_str());
  return 0;
}

int command_revalidate(naf::ipc::Client& client, const Options& options) {
  auto result = client.revalidate(naf::AdmissionDecisionId::from_value(options.decision));
  if (!result) return fail("revalidate", result.status());
  std::printf("state=%s reason=%s granted=%llu released=%d\n",
              std::string(naf::to_string(result.value().state)).c_str(),
              std::string(naf::to_string(result.value().reason)).c_str(),
              static_cast<unsigned long long>(result.value().granted.value()),
              result.value().ledger_released ? 1 : 0);
  return 0;
}

int command_revoke(naf::ipc::Client& client, const Options& options) {
  auto result = client.revoke(naf::AdmissionDecisionId::from_value(options.decision),
                              naf::RevocationReason::OperatorRequest);
  if (!result) return fail("revoke", result.status());
  std::printf("state=%s reason=%s granted=%llu released=%d\n",
              std::string(naf::to_string(result.value().state)).c_str(),
              std::string(naf::to_string(result.value().reason)).c_str(),
              static_cast<unsigned long long>(result.value().granted.value()),
              result.value().ledger_released ? 1 : 0);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse(argc, argv, options)) {
    usage();
    return 2;
  }

  naf::ipc::ClientOptions client_options;
  client_options.endpoint = options.endpoint;
  client_options.publisher = naf::PublisherId::from_value(options.publisher);
  client_options.boot = naf::BootId::from_value(options.boot);
  client_options.origin = naf::OriginKind::Claimant;

  naf::ipc::Client client(client_options);
  auto welcome = client.open();
  if (!welcome) return fail("handshake", welcome.status());
  if (!welcome.value().accepted) {
    std::fprintf(stderr, "nafctl: coordinator refused the session: %s\n",
                 welcome.value().reason.c_str());
    return 1;
  }

  if (options.force_claim) {
    naf::ClaimContext claim;
    claim.origin = naf::OriginKind::Claimant;
    claim.publisher = client_options.publisher;
    claim.boot = client_options.boot;
    claim.epoch = naf::FabricEpoch::from_value(options.expected_epoch);
    claim.session = naf::SessionNonce::from_value(options.expected_session);
    client.set_claim_override(claim);
    std::fprintf(stderr,
                 "nafctl: diagnostic explicit claim in use (epoch=%llu session=%llu); it confers no "
                 "authority\n",
                 static_cast<unsigned long long>(options.expected_epoch),
                 static_cast<unsigned long long>(options.expected_session));
  }

  int rc = 0;
  if (options.command == "status") {
    rc = command_status(client);
  } else if (options.command == "admit") {
    rc = command_admit(client, options);
  } else if (options.command == "revalidate") {
    rc = command_revalidate(client, options);
  } else if (options.command == "revoke") {
    rc = command_revoke(client, options);
  } else {
    std::fprintf(stderr, "nafctl: unknown command '%s'\n", options.command.c_str());
    usage();
    rc = 2;
  }
  const naf::Status closed = client.close();
  if (!closed.is_ok() && rc == 0) return fail("close", closed);
  return rc;
}

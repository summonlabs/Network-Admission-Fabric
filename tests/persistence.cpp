// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Durability, recovery classification and restart semantics. Durable state must
// never silently restore process liveness, telemetry freshness, leases, worker
// authority or publisher authority.
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "naf/naf.hpp"
#include "support/temp_dir.hpp"
#include "support/test_framework.hpp"

using namespace naftest;

namespace {

struct Authority {
  std::uint64_t resources = 2;
  std::uint64_t usable = 1000;
  naf::Generation policy_generation = naf::Generation::first();
  naf::Generation capacity_generation = naf::Generation::first();
  naf::Generation reservation_generation = naf::Generation::first();
  naf::Generation path_generation = naf::Generation::first();
  naf::Generation qos_generation = naf::Generation::first();
  naf::Generation priority_generation = naf::Generation::first();
  naf::CapacitySnapshotId capacity_snapshot = naf::CapacitySnapshotId::from_value(1);
  naf::ReservationSnapshotId reservation_snapshot = naf::ReservationSnapshotId::from_value(1);
};

naf::Status publish_authority(naf::AdmissionEngine& engine, const Authority& authority) {
  naf::AdmissionPolicy policy;
  policy.id = naf::PolicyId::from_value(1);
  policy.generation = authority.policy_generation;
  policy.epoch = engine.epoch();
  policy.allow_degraded = true;
  policy.max_explanation_constraints = 8;
  policy.max_explanation_bytes = 1024;
  policy.max_effective_resources = 8;
  naf::Status status = engine.publish_policy(policy);
  if (!status.is_ok()) return status;

  naf::CapacitySnapshot capacity;
  capacity.snapshot = authority.capacity_snapshot;
  capacity.generation = authority.capacity_generation;
  capacity.epoch = engine.epoch();
  for (std::uint64_t i = 1; i <= authority.resources; ++i) {
    naf::ResourceCapacity entry;
    entry.resource = naf::ResourceId::from_value(i);
    entry.generation = naf::Generation::first();
    entry.evidence = naf::EvidenceState::Known;
    entry.usable = naf::Rate::from_value(authority.usable);
    capacity.resources.push_back(entry);
  }
  status = engine.publish_capacity(capacity);
  if (!status.is_ok()) return status;

  naf::ReservationSnapshot reservations;
  reservations.snapshot = authority.reservation_snapshot;
  reservations.generation = authority.reservation_generation;
  reservations.epoch = engine.epoch();
  status = engine.publish_reservations(reservations);
  if (!status.is_ok()) return status;

  naf::PathCatalog paths;
  paths.path_authority_generation = authority.path_generation;
  paths.epoch = engine.epoch();
  for (std::uint64_t i = 1; i <= authority.resources; ++i) {
    naf::PathAdmissionFact fact;
    fact.path = naf::PathId::from_value(i);
    fact.path_authority_generation = naf::Generation::first();
    fact.state = naf::PathState::Up;
    fact.path_usable_capacity = naf::Rate::from_value(authority.usable);
    fact.resources.push_back(naf::ResourceId::from_value(i));
    paths.paths.push_back(fact);
  }
  status = engine.publish_paths(paths);
  if (!status.is_ok()) return status;

  naf::QoSClassCatalog qos;
  qos.generation = authority.qos_generation;
  qos.epoch = engine.epoch();
  naf::QoSClassFact qos_fact;
  qos_fact.qos = naf::QoSClassId::from_value(1);
  qos_fact.generation = naf::Generation::first();
  qos_fact.maximum_rate = naf::Rate::from_value(~std::uint64_t{0});
  qos_fact.maximum_latency = naf::Latency::from_value(~std::uint64_t{0});
  qos.classes.push_back(qos_fact);
  status = engine.publish_qos_catalog(qos);
  if (!status.is_ok()) return status;

  naf::PriorityClassCatalog priorities;
  priorities.generation = authority.priority_generation;
  priorities.epoch = engine.epoch();
  naf::PriorityClassFact priority;
  priority.priority = naf::PriorityClassId::from_value(1);
  priority.generation = naf::Generation::first();
  priority.rank = 1;
  priorities.classes.push_back(priority);
  return engine.publish_priority_catalog(priorities);
}

naf::AdmissionRequest durable_request(const Authority& authority, std::uint64_t sequence,
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
  request.expected.capacity_snapshot = authority.capacity_snapshot;
  request.expected.capacity_generation = authority.capacity_generation;
  request.expected.reservation_snapshot = authority.reservation_snapshot;
  request.expected.reservation_generation = authority.reservation_generation;
  request.expected.path_authority_generation = authority.path_generation;
  request.expected.policy_generation = authority.policy_generation;
  request.expected.qos_catalog_generation = authority.qos_generation;
  request.expected.priority_catalog_generation = authority.priority_generation;
  return request;
}

std::unique_ptr<naf::AdmissionEngine> durable_engine(const std::filesystem::path& journal_path,
                                                     naf::FabricEpoch epoch,
                                                     std::size_t history_capacity =
                                                         naf::limits::max_decision_records,
                                                     std::uint64_t compact_at_records =
                                                         naf::limits::journal_compact_at_records) {
  naf::EngineConfig config;
  config.epoch = epoch;
  config.incarnation = naf::CoordinatorIncarnation::from_value(1);
  config.history_capacity = history_capacity;
  auto engine = std::make_unique<naf::AdmissionEngine>(config);
  naf::JournalOptions options;
  options.compact_at_records = compact_at_records;
  auto journal = std::make_unique<naf::Journal>(journal_path, options);
  const naf::Status attached = engine->attach_journal(std::move(journal));
  (void)attached;
  return engine;
}

/// Walks the raw journal file and returns the byte offset of the nth frame of a
/// given kind. The layout is the documented on-disk format.
std::uint64_t find_frame_offset(const std::filesystem::path& path, std::uint16_t kind,
                                std::uint32_t occurrence) {
  auto read = naf::read_file_bounded(path, 1u << 24);
  if (!read.has_value()) return ~std::uint64_t{0};
  const std::string& bytes = read.value();
  std::size_t offset = 0;
  std::uint32_t seen = 0;
  while (offset + naf::journal_overhead_bytes <= bytes.size()) {
    const auto* base = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
    const std::uint32_t payload_len = static_cast<std::uint32_t>(base[16]) |
                                      (static_cast<std::uint32_t>(base[17]) << 8) |
                                      (static_cast<std::uint32_t>(base[18]) << 16) |
                                      (static_cast<std::uint32_t>(base[19]) << 24);
    const std::uint16_t frame_kind = static_cast<std::uint16_t>(base[6] | (base[7] << 8));
    const std::size_t frame_bytes = naf::journal_overhead_bytes + payload_len;
    if (offset + frame_bytes > bytes.size()) break;
    if (frame_kind == kind && seen++ == occurrence) return offset;
    offset += frame_bytes;
  }
  return ~std::uint64_t{0};
}

void flip_byte(const std::filesystem::path& path, std::uint64_t offset) {
  auto handle = naf::FileHandle::open(path, true, true, false, false);
  NAF_REQUIRE(handle.has_value());
  naf::FileHandle file = std::move(handle).value();
  NAF_REQUIRE(file.seek(offset).is_ok());
  std::byte one[1];
  auto read = file.read_some(std::span<std::byte>(one, 1));
  NAF_REQUIRE(read.has_value());
  NAF_REQUIRE(read.value() == 1);
  const std::byte flipped = static_cast<std::byte>(static_cast<std::uint8_t>(one[0]) ^ 0xFFu);
  NAF_REQUIRE(file.seek(offset).is_ok());
  NAF_REQUIRE(file.write_all(std::span<const std::byte>(&flipped, 1)).is_ok());
  NAF_REQUIRE(file.sync().is_ok());
  NAF_REQUIRE(file.close().is_ok());
}

}  // namespace

NAF_TEST(persistence, restart_restores_history_and_load_but_not_authority) {
  TempDir temp("persist-roundtrip");
  const auto journal_path = temp.file("admission.journal");
  Authority authority;

  std::size_t admitted = 0;
  {
    auto engine = durable_engine(journal_path, naf::FabricEpoch::first());
    auto opened = engine->open_durable();
    NAF_REQUIRE(opened.has_value());
    NAF_CHECK(opened.value().outcome == naf::RecoveryOutcome::FreshStart);
    NAF_REQUIRE(publish_authority(*engine, authority).is_ok());
    for (std::uint64_t i = 1; i <= 3; ++i) {
      auto decision = engine->admit_local(durable_request(authority, i, 100, 200, 200));
      NAF_REQUIRE(decision.has_value());
      NAF_CHECK(decision.value().outcome == naf::AdmissionOutcome::Admit);
      if (decision.value().outcome == naf::AdmissionOutcome::Admit) ++admitted;
    }
    NAF_CHECK_U64(engine->admitted_load(naf::ResourceId::from_value(1)).value(), 600);
    NAF_CHECK_U64(engine->epoch().value(), 1);
  }
  NAF_CHECK_U64(admitted, 3);

  auto engine = durable_engine(journal_path, naf::FabricEpoch::none());
  auto opened = engine->open_durable();
  NAF_REQUIRE(opened.has_value());
  const naf::RecoveryReport report = opened.value();
  NAF_CHECK(report.status.is_ok());
  NAF_CHECK(report.outcome == naf::RecoveryOutcome::Recovered);
  NAF_CHECK(report.epoch_advanced);
  NAF_CHECK_U64(report.recovered_epoch.value(), 2);
  NAF_CHECK_EQ(report.history.size(), std::size_t{3});
  NAF_CHECK_EQ(report.live_ledger.size(), std::size_t{3});
  NAF_CHECK(report.ambiguous.empty());
  NAF_CHECK_U64(report.trailing_bytes_discarded, 0);

  // Live claimant authority is gone: no sessions exist.
  NAF_CHECK_EQ(engine->live_sessions(), std::size_t{0});
  NAF_CHECK_U64(engine->epoch().value(), 2);

  // Capacity, reservations and paths are owned elsewhere and are UNKNOWN again.
  const naf::FabricStatus status = engine->status();
  NAF_CHECK(status.has_policy);
  NAF_CHECK(!status.has_capacity);
  NAF_CHECK(!status.has_reservations);
  NAF_CHECK(!status.has_paths);
  NAF_CHECK(status.readiness == naf::EngineReadiness::AwaitingCapacity);

  // Admission is refused while the owning authorities are UNKNOWN.
  Authority republished;
  auto refused = engine->admit_local(durable_request(republished, 99, 100, 200, 200));
  NAF_REQUIRE(refused.has_value());
  NAF_CHECK(refused.value().outcome == naf::AdmissionOutcome::StaleInput);
  NAF_CHECK_U64(refused.value().granted.value(), 0);

  // Recovered load is present but marked as requiring revalidation.
  const std::vector<naf::LedgerEntry> ledger = engine->ledger_entries();
  NAF_CHECK_EQ(ledger.size(), std::size_t{3});
  for (const auto& entry : ledger) {
    NAF_CHECK(entry.requires_revalidation);
  }
  NAF_CHECK_U64(engine->admitted_load(naf::ResourceId::from_value(1)).value(), 600);

  // The owning authorities republish under the new epoch.
  NAF_REQUIRE(publish_authority(*engine, republished).is_ok());
  NAF_CHECK(engine->status().reconciled);
  NAF_CHECK_U64(engine->admitted_load(naf::ResourceId::from_value(1)).value(), 600);
  NAF_CHECK_EQ(engine->last_reconciliation().revoked, std::size_t{0});

  // Accounting closure survives the restart: 600 is still held.
  auto next = engine->admit_local(durable_request(republished, 4, 401, 401, 401));
  NAF_REQUIRE(next.has_value());
  NAF_CHECK(next.value().outcome == naf::AdmissionOutcome::RejectCapacity);
  auto fits = engine->admit_local(durable_request(republished, 5, 100, 100, 100));
  NAF_REQUIRE(fits.has_value());
  NAF_CHECK(fits.value().outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK_U64(engine->admitted_load(naf::ResourceId::from_value(1)).value(), 700);
}

NAF_TEST(persistence, a_pre_restart_attempt_replays_from_durable_history) {
  TempDir temp("persist-replay");
  const auto journal_path = temp.file("admission.journal");
  Authority authority;
  {
    auto engine = durable_engine(journal_path, naf::FabricEpoch::first());
    auto opened = engine->open_durable();
    NAF_REQUIRE(opened.has_value());
    NAF_REQUIRE(publish_authority(*engine, authority).is_ok());
    auto decision = engine->admit_local(durable_request(authority, 1, 100, 200, 200));
    NAF_REQUIRE(decision.has_value());
    NAF_REQUIRE(decision.value().outcome == naf::AdmissionOutcome::Admit);
  }
  auto engine = durable_engine(journal_path, naf::FabricEpoch::none());
  auto opened = engine->open_durable();
  NAF_REQUIRE(opened.has_value());
  NAF_REQUIRE(publish_authority(*engine, authority).is_ok());

  auto replay = engine->admit_local(durable_request(authority, 1, 100, 200, 200));
  NAF_REQUIRE(replay.has_value());
  NAF_CHECK(replay.value().idempotent_replay);
  NAF_CHECK(replay.value().outcome == naf::AdmissionOutcome::Admit);
  NAF_CHECK_U64(replay.value().granted.value(), 200);
  NAF_CHECK_U64(engine->admitted_load(naf::ResourceId::from_value(1)).value(), 200);
}

NAF_TEST(persistence, restart_fences_every_previous_claimant) {
  TempDir temp("persist-fence");
  const auto journal_path = temp.file("admission.journal");
  Authority authority;
  naf::SessionGrant grant;
  {
    auto engine = durable_engine(journal_path, naf::FabricEpoch::first());
    auto opened = engine->open_durable();
    NAF_REQUIRE(opened.has_value());
    NAF_REQUIRE(publish_authority(*engine, authority).is_ok());
    auto session = engine->register_session(naf::PublisherId::from_value(1), naf::BootId::from_value(1));
    NAF_REQUIRE(session.has_value());
    grant = session.value();
    naf::ClaimContext claim;
    claim.origin = naf::OriginKind::Claimant;
    claim.publisher = naf::PublisherId::from_value(1);
    claim.boot = naf::BootId::from_value(1);
    claim.session = grant.session;
    claim.incarnation = grant.incarnation;
    claim.epoch = grant.epoch;
    auto decision = engine->admit(durable_request(authority, 1, 100, 200, 200), claim);
    NAF_REQUIRE(decision.has_value());
    NAF_CHECK(decision.value().outcome == naf::AdmissionOutcome::Admit);
  }

  auto engine = durable_engine(journal_path, naf::FabricEpoch::none());
  auto opened = engine->open_durable();
  NAF_REQUIRE(opened.has_value());
  NAF_REQUIRE(publish_authority(*engine, authority).is_ok());

  naf::ClaimContext stale;
  stale.origin = naf::OriginKind::Claimant;
  stale.publisher = naf::PublisherId::from_value(1);
  stale.boot = naf::BootId::from_value(1);
  stale.session = grant.session;
  stale.incarnation = grant.incarnation;
  stale.epoch = grant.epoch;
  auto decision = engine->admit(durable_request(authority, 2, 100, 200, 200), stale);
  NAF_REQUIRE(decision.has_value());
  NAF_CHECK(decision.value().outcome == naf::AdmissionOutcome::FencedClaimant);
  NAF_CHECK_EQ(engine->live_sessions(), std::size_t{0});
}

NAF_TEST(persistence, a_lost_commit_is_recovered_as_ambiguous_and_grants_nothing) {
  TempDir temp("persist-ambiguous");
  const auto journal_path = temp.file("admission.journal");
  Authority authority;
  {
    auto engine = durable_engine(journal_path, naf::FabricEpoch::first());
    auto opened = engine->open_durable();
    NAF_REQUIRE(opened.has_value());
    NAF_REQUIRE(publish_authority(*engine, authority).is_ok());
    naf::FaultInjection faults;
    faults.drop_commit_record = true;
    engine->set_fault_injection(faults);
    auto decision = engine->admit_local(durable_request(authority, 1, 100, 200, 200));
    NAF_REQUIRE(decision.has_value());
    NAF_CHECK(decision.value().outcome == naf::AdmissionOutcome::Admit);
  }

  auto engine = durable_engine(journal_path, naf::FabricEpoch::none());
  auto opened = engine->open_durable();
  NAF_REQUIRE(opened.has_value());
  const naf::RecoveryReport report = opened.value();
  NAF_CHECK(report.status.is_ok());
  NAF_CHECK(report.outcome == naf::RecoveryOutcome::RecoveredWithAmbiguity);
  NAF_CHECK_EQ(report.ambiguous.size(), std::size_t{1});
  NAF_CHECK(report.live_ledger.empty());
  NAF_CHECK_U64(engine->admitted_load(naf::ResourceId::from_value(1)).value(), 0);
  NAF_CHECK(report.render().find("ambiguous=1") != std::string::npos);
}

NAF_TEST(persistence, a_corrupted_interior_frame_refuses_recovery) {
  TempDir temp("persist-corrupt");
  const auto journal_path = temp.file("admission.journal");
  Authority authority;
  {
    auto engine = durable_engine(journal_path, naf::FabricEpoch::first());
    auto opened = engine->open_durable();
    NAF_REQUIRE(opened.has_value());
    NAF_REQUIRE(publish_authority(*engine, authority).is_ok());
    for (std::uint64_t i = 1; i <= 2; ++i) {
      auto decision = engine->admit_local(durable_request(authority, i, 100, 200, 200));
      NAF_REQUIRE(decision.has_value());
      NAF_REQUIRE(decision.value().outcome == naf::AdmissionOutcome::Admit);
    }
  }
  const std::uint64_t offset = find_frame_offset(journal_path, 4 /* AdmissionCommit */, 0);
  NAF_REQUIRE(offset != ~std::uint64_t{0});
  flip_byte(journal_path, offset + naf::journal_header_bytes + 2);

  auto engine = durable_engine(journal_path, naf::FabricEpoch::none());
  auto opened = engine->open_durable();
  NAF_REQUIRE(opened.has_value());
  NAF_CHECK(!opened.value().status.is_ok());
  NAF_CHECK(opened.value().status.code() == naf::StatusCode::CorruptJournal);
  NAF_CHECK(opened.value().outcome == naf::RecoveryOutcome::Refused);
  NAF_CHECK(opened.value().corrupt);
}

NAF_TEST(persistence, a_torn_tail_is_repaired_on_recovery) {
  TempDir temp("persist-torn");
  const auto journal_path = temp.file("admission.journal");
  Authority authority;
  {
    auto engine = durable_engine(journal_path, naf::FabricEpoch::first());
    auto opened = engine->open_durable();
    NAF_REQUIRE(opened.has_value());
    NAF_REQUIRE(publish_authority(*engine, authority).is_ok());
    for (std::uint64_t i = 1; i <= 2; ++i) {
      auto decision = engine->admit_local(durable_request(authority, i, 100, 200, 200));
      NAF_REQUIRE(decision.has_value());
      NAF_REQUIRE(decision.value().outcome == naf::AdmissionOutcome::Admit);
    }
  }
  {
    auto handle = naf::FileHandle::open(journal_path, false, true, true, true);
    NAF_REQUIRE(handle.has_value());
    naf::FileHandle file = std::move(handle).value();
    const naf::ByteBuffer partial =
        naf::encode_frame(naf::JournalRecordKind::AuditMark, 999, naf::as_bytes(std::string("torn")));
    NAF_REQUIRE(file.write_all(std::span<const std::byte>(partial.data(), partial.size() / 3)).is_ok());
    NAF_REQUIRE(file.sync().is_ok());
    NAF_REQUIRE(file.close().is_ok());
  }

  auto engine = durable_engine(journal_path, naf::FabricEpoch::none());
  auto opened = engine->open_durable();
  NAF_REQUIRE(opened.has_value());
  NAF_CHECK(opened.value().status.is_ok());
  NAF_CHECK(opened.value().outcome == naf::RecoveryOutcome::RepairedTornTail);
  NAF_CHECK(opened.value().trailing_bytes_discarded > 0);
  NAF_CHECK_EQ(opened.value().live_ledger.size(), std::size_t{2});
  NAF_CHECK_U64(engine->admitted_load(naf::ResourceId::from_value(1)).value(), 400);
}

NAF_TEST(persistence, a_journal_from_a_newer_incarnation_is_refused) {
  TempDir temp("persist-future");
  const auto journal_path = temp.file("admission.journal");
  Authority authority;
  {
    auto engine = durable_engine(journal_path, naf::FabricEpoch::first());
    auto opened = engine->open_durable();
    NAF_REQUIRE(opened.has_value());
    NAF_REQUIRE(publish_authority(*engine, authority).is_ok());
    auto decision = engine->admit_local(durable_request(authority, 1, 100, 200, 200));
    NAF_REQUIRE(decision.has_value());
  }
  {
    auto engine = durable_engine(journal_path, naf::FabricEpoch::none());
    auto opened = engine->open_durable();
    NAF_REQUIRE(opened.has_value());
    NAF_CHECK_U64(opened.value().recovered_epoch.value(), 2);
  }
  // A coordinator that believes it is still epoch 1 must refuse the journal.
  auto engine = durable_engine(journal_path, naf::FabricEpoch::first());
  auto opened = engine->open_durable();
  NAF_REQUIRE(opened.has_value());
  NAF_CHECK(!opened.value().status.is_ok());
  NAF_CHECK(opened.value().status.code() == naf::StatusCode::EpochMismatch);
}

NAF_TEST(persistence, reconciliation_revokes_the_excess_after_a_capacity_reduction) {
  TempDir temp("persist-reconcile");
  const auto journal_path = temp.file("admission.journal");
  Authority authority;
  {
    auto engine = durable_engine(journal_path, naf::FabricEpoch::first());
    auto opened = engine->open_durable();
    NAF_REQUIRE(opened.has_value());
    NAF_REQUIRE(publish_authority(*engine, authority).is_ok());
    for (std::uint64_t i = 1; i <= 4; ++i) {
      auto decision = engine->admit_local(durable_request(authority, i, 100, 200, 200));
      NAF_REQUIRE(decision.has_value());
      NAF_REQUIRE(decision.value().outcome == naf::AdmissionOutcome::Admit);
    }
    NAF_CHECK_U64(engine->admitted_load(naf::ResourceId::from_value(1)).value(), 800);
  }

  auto engine = durable_engine(journal_path, naf::FabricEpoch::none());
  auto opened = engine->open_durable();
  NAF_REQUIRE(opened.has_value());
  NAF_CHECK_U64(engine->admitted_load(naf::ResourceId::from_value(1)).value(), 800);

  Authority reduced;
  reduced.usable = 300;
  reduced.capacity_generation = naf::Generation::from_value(2);
  reduced.capacity_snapshot = naf::CapacitySnapshotId::from_value(2);
  NAF_REQUIRE(publish_authority(*engine, reduced).is_ok());

  NAF_CHECK(engine->status().reconciled);
  const std::uint64_t remaining = engine->admitted_load(naf::ResourceId::from_value(1)).value();
  NAF_CHECK(remaining <= 300);
  NAF_CHECK_U64(remaining, 200);
  NAF_CHECK_EQ(engine->ledger_entries().size(), std::size_t{1});
  NAF_CHECK_U64(engine->status().live_decisions, 1);

  // Exactly three of the four grants were revoked, newest first, and the
  // surviving one is the oldest.
  std::size_t revoked = 0;
  std::size_t survivors = 0;
  for (const auto& record : engine->decision_history()) {
    if (!naf::is_admitting(record.outcome)) continue;
    if (record.state == naf::RevalidationState::Revoked) {
      ++revoked;
      NAF_CHECK(!record.ledger_live);
    } else {
      ++survivors;
    }
  }
  NAF_CHECK_U64(revoked, 3);
  NAF_CHECK_U64(survivors, 1);
}

NAF_TEST(persistence, compaction_bounds_durable_growth_and_keeps_state) {
  TempDir temp("persist-compact");
  const auto journal_path = temp.file("admission.journal");
  Authority authority;
  auto engine = durable_engine(journal_path, naf::FabricEpoch::none(), 32);
  auto opened = engine->open_durable();
  NAF_REQUIRE(opened.has_value());
  NAF_REQUIRE(publish_authority(*engine, authority).is_ok());
  // 100 grants are live and 100 requests are refused. The refusals are
  // unprotected history and must be retired; live grants must never be.
  for (std::uint64_t i = 1; i <= 200; ++i) {
    if (i % 2 == 0) {
      auto refused = engine->admit_local(durable_request(authority, 1000 + i, 1000, 1000, 1000));
      NAF_REQUIRE(refused.has_value());
      NAF_REQUIRE(refused.value().outcome == naf::AdmissionOutcome::RejectCapacity);
    } else {
      auto decision = engine->admit_local(durable_request(authority, i, 10, 10, 10));
      NAF_REQUIRE(decision.has_value());
      NAF_REQUIRE(decision.value().outcome == naf::AdmissionOutcome::Admit);
    }
  }
  NAF_CHECK_U64(engine->admitted_load(naf::ResourceId::from_value(1)).value(), 1000);

  std::error_code ec;
  const auto before = std::filesystem::file_size(journal_path, ec);
  NAF_REQUIRE(!ec);
  NAF_REQUIRE(engine->compact_journal().is_ok());
  const auto after = std::filesystem::file_size(journal_path, ec);
  NAF_REQUIRE(!ec);
  if (after >= before) {
    naftest::Registry::instance().fail(__FILE__, __LINE__,
                                       "compaction did not shrink the journal: " +
                                           std::to_string(before) + " -> " + std::to_string(after));
    return;
  }
  naftest::Registry::instance().count_check();
  NAF_CHECK_U64(engine->admitted_load(naf::ResourceId::from_value(1)).value(), 1000);
  // Retirement never drops a record whose grant is still live.
  NAF_CHECK_EQ(engine->decision_history().size(), std::size_t{100});
  for (const auto& record : engine->decision_history()) {
    NAF_CHECK(naf::is_admitting(record.outcome));
    NAF_CHECK(record.ledger_live);
  }
  // A journal is owned by exactly one engine at a time: release it before the
  // restarted coordinator opens it.
  engine.reset();

  auto reopened = durable_engine(journal_path, naf::FabricEpoch::none());
  auto recovered = reopened->open_durable();
  NAF_REQUIRE(recovered.has_value());
  if (!recovered.value().status.is_ok()) {
    naftest::Registry::instance().fail(__FILE__, __LINE__,
                                       "recovery refused: " + recovered.value().status.to_string());
    return;
  }
  naftest::Registry::instance().count_check();
  NAF_CHECK_EQ(recovered.value().live_ledger.size(), std::size_t{100});
  NAF_CHECK_U64(reopened->admitted_load(naf::ResourceId::from_value(1)).value(), 1000);
  NAF_CHECK_U64(reopened->status().live_decisions, 100);
}

NAF_TEST(persistence, automatic_compaction_bounds_durable_growth) {
  TempDir temp("persist-auto-compact");
  const auto journal_path = temp.file("admission.journal");
  Authority authority;
  authority.usable = 100000;
  auto engine = durable_engine(journal_path, naf::FabricEpoch::none(), 16, 64);
  auto opened = engine->open_durable();
  NAF_REQUIRE(opened.has_value());
  NAF_REQUIRE(publish_authority(*engine, authority).is_ok());

  std::error_code ec;
  std::uint64_t largest = 0;
  for (std::uint64_t i = 1; i <= 400; ++i) {
    auto decision = engine->admit_local(durable_request(authority, i, 10, 10, 10));
    NAF_REQUIRE(decision.has_value());
    NAF_REQUIRE(decision.value().outcome == naf::AdmissionOutcome::Admit);
    const auto size = std::filesystem::file_size(journal_path, ec);
    NAF_REQUIRE(!ec);
    if (size > largest) largest = size;
  }
  NAF_CHECK_U64(engine->status().live_decisions, 400);
  // Without automatic compaction 400 grants would need far more than this.
  NAF_CHECK(largest < 200000);

  engine.reset();
  auto reopened = durable_engine(journal_path, naf::FabricEpoch::none());
  auto recovered = reopened->open_durable();
  NAF_REQUIRE(recovered.has_value());
  NAF_REQUIRE(recovered.value().status.is_ok());
  NAF_CHECK_EQ(recovered.value().live_ledger.size(), std::size_t{400});
  NAF_CHECK_U64(reopened->admitted_load(naf::ResourceId::from_value(1)).value(), 4000);
}

NAF_TEST(persistence, a_non_durable_engine_reports_no_journal) {
  naf::EngineConfig config;
  config.epoch = naf::FabricEpoch::first();
  naf::AdmissionEngine engine(config);
  NAF_CHECK(!engine.status().durable);
  auto opened = engine.open_durable();
  NAF_REQUIRE(opened.has_value());
  NAF_CHECK(!opened.value().status.is_ok());
  NAF_CHECK(opened.value().status.code() == naf::StatusCode::InvalidArgument);
}

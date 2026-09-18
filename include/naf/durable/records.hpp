// Network Admission Fabric - durable record encoding.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Only state that admission actually owns is durable: its policy/configuration,
// its admission history, provenance, fencing, audit sequence and coordinator
// epoch. Capacity, reservation and path snapshots belong to adjacent runtimes
// and are never persisted here; after a restart they are UNKNOWN until their
// owner republishes them.
#ifndef NAF_DURABLE_RECORDS_HPP
#define NAF_DURABLE_RECORDS_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "naf/core/bytes.hpp"
#include "naf/core/identity.hpp"
#include "naf/core/status.hpp"
#include "naf/engine/decision.hpp"
#include "naf/engine/history.hpp"
#include "naf/engine/ledger.hpp"
#include "naf/model/policy.hpp"

namespace naf {

/// Written once per boot ahead of everything else. Carries the fencing epoch, so
/// a coordinator that starts against a journal from a later incarnation refuses
/// to run rather than silently reusing authority.
struct BootRecord {
  FabricEpoch epoch{};
  std::uint16_t format_revision = 0;
  std::string product{};
  std::uint64_t boot_nonce = 0;
  CoordinatorIncarnation incarnation{};

  friend bool operator==(const BootRecord&, const BootRecord&) = default;
};

/// Intent half of the two-phase admission commit.
struct PrepareRecord {
  AdmissionDecisionId decision{};
  DemandId demand{};
  AttemptId attempt{};
  std::uint64_t fingerprint = 0;
  FabricEpoch epoch{};
  Generation bound_capacity_generation{};
  PathId path{};
  std::vector<std::pair<ResourceId, Rate>> amounts{};

  friend bool operator==(const PrepareRecord&, const PrepareRecord&) = default;
};

struct ReleaseRecord {
  AdmissionDecisionId decision{};
  FabricEpoch epoch{};
  RevocationReason reason = RevocationReason::NotRevoked;
  std::vector<LedgerEntry> released{};

  friend bool operator==(const ReleaseRecord&, const ReleaseRecord&) = default;
};

struct RevocationRecord {
  AdmissionDecisionId decision{};
  FabricEpoch epoch{};
  AuditSequence audit{};
  RevocationReason reason = RevocationReason::NotRevoked;
  RevalidationState state = RevalidationState::Unknown;

  friend bool operator==(const RevocationRecord&, const RevocationRecord&) = default;
};

struct AuditMarkRecord {
  AuditSequence sequence{};
  FabricEpoch epoch{};
  std::uint64_t frames = 0;

  friend bool operator==(const AuditMarkRecord&, const AuditMarkRecord&) = default;
};

[[nodiscard]] ByteBuffer encode(const BootRecord& record);
[[nodiscard]] Expected<BootRecord> decode_boot(std::span<const std::byte> bytes);

[[nodiscard]] ByteBuffer encode(const AdmissionPolicy& policy);
[[nodiscard]] Expected<AdmissionPolicy> decode_policy(std::span<const std::byte> bytes);

[[nodiscard]] ByteBuffer encode(const PrepareRecord& record);
[[nodiscard]] Expected<PrepareRecord> decode_prepare(std::span<const std::byte> bytes);

[[nodiscard]] ByteBuffer encode(const DecisionRecord& record);
[[nodiscard]] Expected<DecisionRecord> decode_decision_record(std::span<const std::byte> bytes);

[[nodiscard]] ByteBuffer encode(const ReleaseRecord& record);
[[nodiscard]] Expected<ReleaseRecord> decode_release(std::span<const std::byte> bytes);

[[nodiscard]] ByteBuffer encode(const RevocationRecord& record);
[[nodiscard]] Expected<RevocationRecord> decode_revocation(std::span<const std::byte> bytes);

[[nodiscard]] ByteBuffer encode(const AuditMarkRecord& record);
[[nodiscard]] Expected<AuditMarkRecord> decode_audit_mark(std::span<const std::byte> bytes);

}  // namespace naf

#endif  // NAF_DURABLE_RECORDS_HPP

// Network Admission Fabric - authority vector.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every decision carries the exact set of authority revisions that justified
// it. The vector is bounded, duplicate-free and digestible so that a claimant
// can prove which evidence a decision was bound to.
#ifndef NAF_AUTHORITY_AUTHORITY_HPP
#define NAF_AUTHORITY_AUTHORITY_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "naf/core/identity.hpp"
#include "naf/core/limits.hpp"

namespace naf {

enum class AuthorityKind : std::uint8_t {
  FabricEpoch = 0,
  CoordinatorIncarnation = 1,
  CapacitySnapshot = 2,
  ReservationSnapshot = 3,
  PathAuthority = 4,
  Policy = 5,
  QoSClassCatalog = 6,
  PriorityClassCatalog = 7,
  ResourceCapacity = 8,
  Path = 9,
  Reservation = 10,
  QoSClass = 11,
  PriorityClass = 12,
};

[[nodiscard]] std::string_view to_string(AuthorityKind kind) noexcept;

struct AuthorityRef {
  AuthorityKind kind = AuthorityKind::FabricEpoch;
  /// Identity of the authority subject; 0 for fabric-global entries.
  std::uint64_t subject = 0;
  Generation generation{};
  FabricEpoch epoch{};

  friend bool operator==(const AuthorityRef&, const AuthorityRef&) = default;
};

/// Deterministic, duplicate-free, bounded set of authority references.
class AuthorityVector {
 public:
  AuthorityVector() = default;

  /// Adds a reference. Returns false when the vector is full (the reference is
  /// dropped and the vector is marked truncated). Exact duplicates are merged.
  bool add(AuthorityKind kind, std::uint64_t subject, Generation generation, FabricEpoch epoch);

  [[nodiscard]] std::size_t size() const noexcept { return refs_.size(); }
  [[nodiscard]] bool empty() const noexcept { return refs_.empty(); }
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }
  [[nodiscard]] const std::vector<AuthorityRef>& refs() const noexcept { return refs_; }

  /// Order-insensitive digest so that two vectors describing the same evidence
  /// produce the same digest regardless of insertion order.
  [[nodiscard]] std::uint64_t digest() const noexcept;

  /// Looks up the generation recorded for one subject.
  [[nodiscard]] const AuthorityRef* find(AuthorityKind kind, std::uint64_t subject) const noexcept;

  [[nodiscard]] std::string render() const;

 private:
  std::vector<AuthorityRef> refs_{};
  bool truncated_ = false;
};

}  // namespace naf

#endif  // NAF_AUTHORITY_AUTHORITY_HPP

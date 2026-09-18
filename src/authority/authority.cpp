// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/authority/authority.hpp"

#include <algorithm>
#include <string>

#include "core/text.hpp"
#include "naf/core/hash.hpp"

namespace naf {
namespace {

bool same_subject(const AuthorityRef& a, const AuthorityRef& b) noexcept {
  return a.kind == b.kind && a.subject == b.subject;
}

}  // namespace

std::string_view to_string(AuthorityKind kind) noexcept {
  switch (kind) {
    case AuthorityKind::FabricEpoch: return "fabric_epoch";
    case AuthorityKind::CoordinatorIncarnation: return "coordinator_incarnation";
    case AuthorityKind::CapacitySnapshot: return "capacity_snapshot";
    case AuthorityKind::ReservationSnapshot: return "reservation_snapshot";
    case AuthorityKind::PathAuthority: return "path_authority";
    case AuthorityKind::Policy: return "policy";
    case AuthorityKind::QoSClassCatalog: return "qos_catalog";
    case AuthorityKind::PriorityClassCatalog: return "priority_catalog";
    case AuthorityKind::ResourceCapacity: return "resource_capacity";
    case AuthorityKind::Path: return "path";
    case AuthorityKind::Reservation: return "reservation";
    case AuthorityKind::QoSClass: return "qos_class";
    case AuthorityKind::PriorityClass: return "priority_class";
  }
  return "unknown";
}

bool AuthorityVector::add(AuthorityKind kind, std::uint64_t subject, Generation generation,
                          FabricEpoch epoch) {
  // Fast path: the engine adds resources and paths in ascending order, so a
  // repeated subject is almost always the most recent one. This keeps the
  // common case O(1) while the linear scan still guarantees correctness.
  if (!refs_.empty()) {
    AuthorityRef& last = refs_.back();
    if (last.kind == kind && last.subject == subject) {
      if (generation > last.generation) last.generation = generation;
      last.epoch = epoch;
      return true;
    }
  }
  for (auto& existing : refs_) {
    if (same_subject(existing, AuthorityRef{kind, subject, generation, epoch})) {
      // Conflicting generation for the same subject: keep the strongest
      // (highest) generation so the digest never regresses.
      if (generation > existing.generation) existing.generation = generation;
      existing.epoch = epoch;
      return true;
    }
  }
  if (refs_.size() >= limits::max_authority_refs) {
    truncated_ = true;
    return false;
  }
  refs_.push_back(AuthorityRef{kind, subject, generation, epoch});
  return true;
}

std::uint64_t AuthorityVector::digest() const noexcept {
  std::vector<AuthorityRef> sorted = refs_;
  std::sort(sorted.begin(), sorted.end(), [](const AuthorityRef& a, const AuthorityRef& b) {
    if (a.kind != b.kind) return static_cast<unsigned>(a.kind) < static_cast<unsigned>(b.kind);
    return a.subject < b.subject;
  });
  Fingerprint f;
  f.separator(0xA1);
  for (const auto& ref : sorted) {
    f.u16(static_cast<std::uint16_t>(ref.kind));
    f.u64(ref.subject);
    f.u64(ref.generation.value());
    f.u64(ref.epoch.value());
  }
  f.boolean(truncated_);
  return f.value();
}

const AuthorityRef* AuthorityVector::find(AuthorityKind kind, std::uint64_t subject) const noexcept {
  for (const auto& ref : refs_) {
    if (ref.kind == kind && ref.subject == subject) return &ref;
  }
  return nullptr;
}

std::string AuthorityVector::render() const {
  std::string out;
  out.reserve(refs_.size() * 40);
  bool first = true;
  for (const auto& ref : refs_) {
    if (!first) out += "; ";
    first = false;
    out += to_string(ref.kind);
    out.push_back('#');
    naf::text::append_u64(out, ref.subject);
    out.push_back('@');
    naf::text::append_u64(out, ref.generation.value());
    out.push_back('/');
    naf::text::append_u64(out, ref.epoch.value());
  }
  if (truncated_) out += "; (truncated)";
  return out;
}

}  // namespace naf

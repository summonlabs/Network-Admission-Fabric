// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/model/state.hpp"

#include <map>

#include "naf/core/checked.hpp"

namespace naf {
namespace {

Status malformed(std::string message) { return Status::error(StatusCode::MalformedInput, std::move(message)); }
Status oversized(std::string message) { return Status::error(StatusCode::OversizedInput, std::move(message)); }

Status check_generation(Generation generation, const char* what) {
  if (!generation.is_well_formed()) {
    return malformed(std::string(what) + " generation is not well formed");
  }
  return Status::ok();
}

Status check_epoch(FabricEpoch epoch, const char* what) {
  if (!epoch.is_well_formed()) {
    return malformed(std::string(what) + " epoch is not well formed");
  }
  return Status::ok();
}

}  // namespace

std::string_view to_string(EvidenceState state) noexcept {
  switch (state) {
    case EvidenceState::Unknown: return "unknown";
    case EvidenceState::Known: return "known";
  }
  return "unknown";
}

std::string_view to_string(PathState state) noexcept {
  switch (state) {
    case PathState::Unknown: return "unknown";
    case PathState::Up: return "up";
    case PathState::Down: return "down";
  }
  return "unknown";
}

Status validate(const CapacitySnapshot& snapshot) {
  if (snapshot.snapshot.is_unknown()) return malformed("capacity snapshot id is unknown");
  Status s = check_generation(snapshot.generation, "capacity snapshot");
  if (!s.is_ok()) return s;
  s = check_epoch(snapshot.epoch, "capacity snapshot");
  if (!s.is_ok()) return s;
  if (snapshot.resources.size() > limits::max_resources) {
    return oversized("capacity snapshot has too many resources");
  }
  std::uint64_t previous = 0;
  bool first = true;
  for (const auto& entry : snapshot.resources) {
    if (entry.resource.is_unknown()) return malformed("capacity entry has an unknown resource id");
    if (!first && entry.resource.value() <= previous) {
      return malformed("capacity snapshot resources are not strictly increasing");
    }
    previous = entry.resource.value();
    first = false;
    s = check_generation(entry.generation, "resource capacity");
    if (!s.is_ok()) return s;
    if (entry.evidence == EvidenceState::Unknown) {
      // An UNKNOWN observation carries no numbers at all. Accepting numbers here
      // would let a placeholder silently become positive authority.
      if (!entry.usable.is_zero() || !entry.mandatory_headroom.is_zero()) {
        return malformed("unknown capacity evidence must not carry values");
      }
      continue;
    }
    if (entry.mandatory_headroom > entry.usable) {
      return Status::error(StatusCode::CapacityViolation,
                           "mandatory headroom exceeds usable capacity for resource " +
                               std::to_string(entry.resource.value()));
    }
  }
  return Status::ok();
}

Status validate(const ReservationSnapshot& snapshot) {
  if (snapshot.snapshot.is_unknown()) return malformed("reservation snapshot id is unknown");
  Status s = check_generation(snapshot.generation, "reservation snapshot");
  if (!s.is_ok()) return s;
  s = check_epoch(snapshot.epoch, "reservation snapshot");
  if (!s.is_ok()) return s;
  if (snapshot.obligations.size() > limits::max_obligations) {
    return oversized("reservation snapshot has too many obligations");
  }

  std::uint64_t previous_resource = 0;
  std::uint64_t previous_reservation = 0;
  bool first = true;
  std::map<std::uint64_t, SumLatch> per_resource;
  for (const auto& obligation : snapshot.obligations) {
    if (obligation.reservation.is_unknown()) return malformed("obligation has an unknown reservation id");
    if (obligation.resource.is_unknown()) return malformed("obligation has an unknown resource id");
    s = check_generation(obligation.reservation_generation, "reservation");
    if (!s.is_ok()) return s;
    if (!first) {
      if (obligation.resource.value() < previous_resource) {
        return malformed("reservation snapshot obligations are not ordered by resource");
      }
      if (obligation.resource.value() == previous_resource &&
          obligation.reservation.value() <= previous_reservation) {
        return malformed("reservation snapshot has duplicate obligations for one resource");
      }
    }
    previous_resource = obligation.resource.value();
    previous_reservation = obligation.reservation.value();
    first = false;

    auto& latch = per_resource[obligation.resource.value()];
    latch.add(obligation.reserved.value());
    if (latch.overflowed()) {
      return Status::error(StatusCode::ArithmeticOverflow,
                           "protected obligations overflow for resource " +
                               std::to_string(obligation.resource.value()));
    }
  }
  return Status::ok();
}

Status validate(const PathCatalog& catalog) {
  Status s = check_generation(catalog.path_authority_generation, "path authority");
  if (!s.is_ok()) return s;
  s = check_epoch(catalog.epoch, "path authority");
  if (!s.is_ok()) return s;
  if (catalog.paths.size() > limits::max_paths) return oversized("path catalog has too many paths");
  std::uint64_t previous = 0;
  bool first = true;
  for (const auto& fact : catalog.paths) {
    if (fact.path.is_unknown()) return malformed("path catalog entry has an unknown path id");
    if (!first && fact.path.value() <= previous) {
      return malformed("path catalog paths are not strictly increasing");
    }
    previous = fact.path.value();
    first = false;
    s = check_generation(fact.path_authority_generation, "path");
    if (!s.is_ok()) return s;
    if (fact.resources.size() > limits::max_path_resources) {
      return oversized("path traversal set is too large");
    }
    std::uint64_t previous_resource = 0;
    bool first_resource = true;
    for (const auto& resource : fact.resources) {
      if (resource.is_unknown()) return malformed("path traversal set has an unknown resource id");
      if (!first_resource && resource.value() <= previous_resource) {
        return malformed("path traversal set is not strictly increasing");
      }
      previous_resource = resource.value();
      first_resource = false;
    }
  }
  return Status::ok();
}

Status validate(const QoSClassCatalog& catalog) {
  Status s = check_generation(catalog.generation, "qos catalog");
  if (!s.is_ok()) return s;
  s = check_epoch(catalog.epoch, "qos catalog");
  if (!s.is_ok()) return s;
  if (catalog.classes.size() > limits::max_qos_classes) return oversized("qos catalog is too large");
  std::uint64_t previous = 0;
  bool first = true;
  for (const auto& fact : catalog.classes) {
    if (fact.qos.is_unknown()) return malformed("qos catalog entry has an unknown class id");
    if (!first && fact.qos.value() <= previous) {
      return malformed("qos catalog classes are not strictly increasing");
    }
    previous = fact.qos.value();
    first = false;
    s = check_generation(fact.generation, "qos class");
    if (!s.is_ok()) return s;
    if (fact.minimum_rate > fact.maximum_rate) {
      return malformed("qos class minimum rate exceeds maximum rate");
    }
    if (fact.maximum_rate.is_zero()) {
      return malformed("qos class maximum rate is zero");
    }
  }
  return Status::ok();
}

Status validate(const PriorityClassCatalog& catalog) {
  Status s = check_generation(catalog.generation, "priority catalog");
  if (!s.is_ok()) return s;
  s = check_epoch(catalog.epoch, "priority catalog");
  if (!s.is_ok()) return s;
  if (catalog.classes.size() > limits::max_priority_classes) {
    return oversized("priority catalog is too large");
  }
  std::uint64_t previous = 0;
  bool first = true;
  std::map<std::uint32_t, std::uint64_t> ranks;
  for (const auto& fact : catalog.classes) {
    if (fact.priority.is_unknown()) return malformed("priority catalog entry has an unknown class id");
    if (!first && fact.priority.value() <= previous) {
      return malformed("priority catalog classes are not strictly increasing");
    }
    previous = fact.priority.value();
    first = false;
    s = check_generation(fact.generation, "priority class");
    if (!s.is_ok()) return s;
    auto [it, inserted] = ranks.emplace(fact.rank, fact.priority.value());
    if (!inserted) {
      return malformed("priority catalog has duplicate rank " + std::to_string(fact.rank));
    }
  }
  return Status::ok();
}

}  // namespace naf

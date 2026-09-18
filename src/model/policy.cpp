// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/model/policy.hpp"

namespace naf {
namespace {

Status malformed(std::string message) { return Status::error(StatusCode::MalformedInput, std::move(message)); }

template <class Id>
Status check_sorted_unique(const std::vector<Id>& values, const char* what) {
  if (values.size() > limits::max_allowed_classes) return Status::error(StatusCode::OversizedInput, std::string(what) + " list is too large");
  std::uint64_t previous = 0;
  bool first = true;
  for (const auto& value : values) {
    if (value.is_unknown()) return malformed(std::string(what) + " list contains an unknown id");
    if (!first && value.value() <= previous) {
      return malformed(std::string(what) + " list is not strictly increasing");
    }
    previous = value.value();
    first = false;
  }
  return Status::ok();
}

}  // namespace

std::string_view to_string(ContentionAction action) noexcept {
  switch (action) {
    case ContentionAction::Defer: return "defer";
    case ContentionAction::Reject: return "reject";
  }
  return "unknown";
}

Status validate(const AdmissionPolicy& policy) {
  if (policy.id.is_unknown()) return malformed("policy id is unknown");
  if (!policy.generation.is_well_formed()) return malformed("policy generation is not well formed");
  if (!policy.epoch.is_well_formed()) return malformed("policy epoch is not well formed");
  if (policy.headroom_permille > 1000) return malformed("policy headroom permille exceeds 1000");
  if (policy.max_explanation_constraints == 0 ||
      policy.max_explanation_constraints > limits::max_explanation_constraints) {
    return malformed("policy explanation constraint bound is out of range");
  }
  if (policy.max_explanation_bytes < 128 || policy.max_explanation_bytes > limits::max_explanation_bytes) {
    return malformed("policy explanation byte bound is out of range");
  }
  if (policy.max_effective_resources == 0 ||
      policy.max_effective_resources > limits::max_effective_resources) {
    return malformed("policy effective resource bound is out of range");
  }
  Status s = check_sorted_unique(policy.allowed_qos, "allowed qos");
  if (!s.is_ok()) return s;
  return check_sorted_unique(policy.allowed_priorities, "allowed priority");
}

}  // namespace naf

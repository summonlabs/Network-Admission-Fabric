// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/model/request.hpp"

#include <string>

#include "core/text.hpp"
#include "naf/core/hash.hpp"

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

}  // namespace

std::string_view to_string(ContentionPreference preference) noexcept {
  switch (preference) {
    case ContentionPreference::InheritPolicy: return "inherit_policy";
    case ContentionPreference::Defer: return "defer";
    case ContentionPreference::Reject: return "reject";
  }
  return "unknown";
}

std::string_view to_string(DegradePreference preference) noexcept {
  switch (preference) {
    case DegradePreference::InheritPolicy: return "inherit_policy";
    case DegradePreference::Never: return "never";
    case DegradePreference::DownToMinimum: return "down_to_minimum";
  }
  return "unknown";
}

Status validate(const AdmissionRequest& request) {
  if (request.request.is_unknown()) return malformed("request id is unknown");
  if (request.demand.is_unknown()) return malformed("demand id is unknown");
  if (request.attempt.is_unknown()) return malformed("attempt id is unknown");
  Status s = check_generation(request.demand_generation, "demand");
  if (!s.is_ok()) return s;
  s = check_generation(request.qos_generation, "qos class");
  if (!s.is_ok()) return s;
  s = check_generation(request.priority_generation, "priority class");
  if (!s.is_ok()) return s;
  if (request.qos.is_unknown()) return malformed("qos class id is unknown");
  if (request.priority.is_unknown()) return malformed("priority class id is unknown");

  if (!request.rate.is_coherent()) {
    return malformed("rate bounds are contradictory (require 0 < minimum <= desired <= maximum)");
  }

  if (request.resource_bindings.size() > limits::max_resource_bindings) {
    return oversized("too many resource bindings");
  }
  std::uint64_t previous_resource = 0;
  bool first = true;
  for (const auto& binding : request.resource_bindings) {
    if (binding.resource.is_unknown()) return malformed("resource binding has an unknown resource id");
    if (!first && binding.resource.value() <= previous_resource) {
      return malformed("resource bindings are not strictly increasing");
    }
    previous_resource = binding.resource.value();
    first = false;
    s = check_generation(binding.generation, "resource binding");
    if (!s.is_ok()) return s;
  }

  if (request.path_candidates.size() > limits::max_path_candidates) {
    return oversized("too many path candidates");
  }
  std::vector<std::uint64_t> seen_paths;
  seen_paths.reserve(request.path_candidates.size());
  for (const auto& candidate : request.path_candidates) {
    if (candidate.path.is_unknown()) return malformed("path candidate has an unknown path id");
    for (const auto seen : seen_paths) {
      if (seen == candidate.path.value()) return malformed("duplicate path candidate");
    }
    seen_paths.push_back(candidate.path.value());
    s = check_generation(candidate.path_authority_generation, "path candidate");
    if (!s.is_ok()) return s;
  }

  if (request.resource_bindings.empty() && request.path_candidates.empty()) {
    return malformed("request names neither resource bindings nor path candidates");
  }

  if (request.required_path.is_known()) {
    bool found = false;
    for (const auto seen : seen_paths) {
      if (seen == request.required_path.value()) {
        found = true;
        break;
      }
    }
    if (!found) return malformed("required path is not one of the declared candidates");
    if (request.allow_path_substitution && request.path_candidates.size() > 1) {
      return malformed("required path conflicts with path substitution being allowed");
    }
  }

  if (request.reservation_refs.size() > limits::max_reservation_refs) {
    return oversized("too many reservation references");
  }
  std::uint64_t previous_reservation = 0;
  bool first_reservation = true;
  for (const auto& ref : request.reservation_refs) {
    if (ref.reservation.is_unknown()) return malformed("reservation reference has an unknown id");
    if (!first_reservation && ref.reservation.value() <= previous_reservation) {
      return malformed("reservation references are not strictly increasing");
    }
    previous_reservation = ref.reservation.value();
    first_reservation = false;
    s = check_generation(ref.generation, "reservation reference");
    if (!s.is_ok()) return s;
  }

  if (request.provenance.attempt.is_known() && request.provenance.attempt != request.attempt) {
    return malformed("provenance attempt does not match the request attempt");
  }

  const AuthorityExpectation& expected = request.expected;
  const Generation generations[] = {
      expected.capacity_generation,      expected.reservation_generation,
      expected.path_authority_generation, expected.policy_generation,
      expected.qos_catalog_generation,   expected.priority_catalog_generation,
  };
  for (const Generation generation : generations) {
    if (generation.is_max()) return malformed("authority expectation generation is malformed");
  }
  return Status::ok();
}

std::uint64_t fingerprint(const AdmissionRequest& request) {
  Fingerprint f;
  f.separator(0x11);
  f.u64(request.request.value());
  f.u64(request.demand.value());
  f.u64(request.demand_generation.value());
  f.u64(request.attempt.value());

  f.separator(0x12);
  f.u64(request.rate.minimum.value());
  f.u64(request.rate.desired.value());
  f.u64(request.rate.maximum.value());

  f.separator(0x13);
  f.u64(request.qos.value());
  f.u64(request.qos_generation.value());
  f.u64(request.priority.value());
  f.u64(request.priority_generation.value());
  f.u64(request.maximum_latency.value());

  f.separator(0x14);
  f.u64(request.resource_bindings.size());
  for (const auto& binding : request.resource_bindings) {
    f.u64(binding.resource.value());
    f.u64(binding.generation.value());
  }

  f.separator(0x15);
  f.u64(request.path_candidates.size());
  for (const auto& candidate : request.path_candidates) {
    f.u64(candidate.path.value());
    f.u64(candidate.path_authority_generation.value());
  }

  f.separator(0x16);
  f.u64(request.required_path.value());
  f.u64(request.reservation_refs.size());
  for (const auto& ref : request.reservation_refs) {
    f.u64(ref.reservation.value());
    f.u64(ref.generation.value());
  }

  f.separator(0x17);
  f.u64(request.fairness_group.value());
  f.u64(request.tenant.value());
  f.boolean(request.preemptible);
  f.boolean(request.requests_obligation_preemption);
  f.boolean(request.allow_path_substitution);
  f.u8(static_cast<std::uint8_t>(request.contention));
  f.u8(static_cast<std::uint8_t>(request.degradation));

  f.separator(0x18);
  f.u64(request.deadline_ticks);
  f.u64(request.effective_tick);
  f.u64(request.effective_interval.value());

  f.separator(0x1A);
  f.u64(request.expected.capacity_snapshot.value());
  f.u64(request.expected.capacity_generation.value());
  f.u64(request.expected.reservation_snapshot.value());
  f.u64(request.expected.reservation_generation.value());
  f.u64(request.expected.path_authority_generation.value());
  f.u64(request.expected.policy_generation.value());
  f.u64(request.expected.qos_catalog_generation.value());
  f.u64(request.expected.priority_catalog_generation.value());

  // Provenance fields that legitimately change across a retry -- boot identity,
  // coordinator incarnation and origin kind -- are deliberately excluded, so a
  // duplicate retry after an ambiguous acknowledgement still matches the
  // original attempt. The publisher stays: a different publisher reusing an
  // attempt identity is a genuine conflict.
  f.separator(0x19);
  f.u64(request.provenance.publisher.value());

  return f.value();
}

std::string render_request(const AdmissionRequest& request) {
  std::string out;
  out.reserve(160);
  out += "request=";
  naf::text::append_u64(out, request.request.value());
  out += " demand=";
  naf::text::append_u64(out, request.demand.value());
  out += " attempt=";
  naf::text::append_u64(out, request.attempt.value());
  out += " min=";
  naf::text::append_u64(out, request.rate.minimum.value());
  out += " desired=";
  naf::text::append_u64(out, request.rate.desired.value());
  out += " max=";
  naf::text::append_u64(out, request.rate.maximum.value());
  out += " resources=";
  naf::text::append_u64(out, request.resource_bindings.size());
  out += " paths=";
  naf::text::append_u64(out, request.path_candidates.size());
  return out;
}

}  // namespace naf

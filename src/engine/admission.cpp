// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The admission evaluation itself. Deterministic: the outcome depends only on
// the request, the current authoritative state and the logical tick, never on
// wall-clock time, allocation addresses or thread scheduling.
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "engine_core.hpp"

#include "naf/core/checked.hpp"
#include "naf/core/hash.hpp"
#include "naf/durable/recovery.hpp"

namespace naf::detail {
namespace {

BindingConstraint constraint_of(BindingConstraintKind kind, std::string subject, std::uint64_t id,
                                std::string detail) {
  BindingConstraint c;
  c.kind = kind;
  c.subject = std::move(subject);
  c.subject_id = id;
  c.detail = std::move(detail);
  return c;
}

std::string num(std::uint64_t value) { return std::to_string(value); }

}  // namespace

// ---------------------------------------------------------------------------
// Step 1: caller fencing
// ---------------------------------------------------------------------------

bool EngineCore::check_fence(const ClaimContext& claim, AdmissionOutcome& outcome,
                             BindingConstraint& constraint) const {
  const auto fence = [&](std::string detail) {
    outcome = AdmissionOutcome::FencedClaimant;
    constraint = constraint_of(BindingConstraintKind::Fence, "claim", claim.session.value(),
                               std::move(detail));
    constraint.observed = Generation::from_value(claim.epoch.value());
    constraint.authoritative = Generation::from_value(epoch.value());
    return false;
  };

  if (claim.origin == OriginKind::InProcess && config.accept_in_process_claims) {
    if (!claim.epoch.is_none() && claim.epoch != epoch) {
      return fence("in-process claim epoch does not match the fabric epoch");
    }
    if (!claim.incarnation.is_unknown() && claim.incarnation != incarnation) {
      return fence("in-process claim incarnation does not match the coordinator incarnation");
    }
    return true;
  }

  if (claim.epoch.is_none()) return fence("claim carries no fabric epoch");
  if (claim.epoch < epoch) return fence("claim epoch is older than the current fabric epoch");
  if (claim.epoch > epoch) {
    outcome = AdmissionOutcome::ConflictingInput;
    constraint = constraint_of(BindingConstraintKind::IdentityConflict, "claim_epoch",
                               claim.epoch.value(), "claim epoch is ahead of the current fabric epoch");
    constraint.observed = Generation::from_value(claim.epoch.value());
    constraint.authoritative = Generation::from_value(epoch.value());
    return false;
  }
  if (claim.incarnation != incarnation) {
    return fence("claim targets a previous coordinator incarnation");
  }
  if (claim.session.is_unknown()) return fence("claim carries no session nonce");
  const auto it = sessions.find(claim.session.value());
  if (it == sessions.end()) return fence("claim session is not live in this incarnation");
  if (it->second.publisher != claim.publisher || it->second.boot != claim.boot) {
    return fence("claim identity does not match the registered session");
  }
  return true;
}

// ---------------------------------------------------------------------------
// Step 2: readiness
// ---------------------------------------------------------------------------

bool EngineCore::check_readiness(AdmissionOutcome& outcome, BindingConstraint& constraint) const {
  switch (readiness()) {
    case EngineReadiness::AwaitingPolicy:
      outcome = AdmissionOutcome::RejectPolicy;
      constraint = constraint_of(BindingConstraintKind::Policy, "policy", 0,
                                 "no admission policy has been published");
      return false;
    case EngineReadiness::AwaitingCapacity:
      outcome = AdmissionOutcome::StaleInput;
      constraint = constraint_of(BindingConstraintKind::UnknownEvidence, "capacity_snapshot", 0,
                                 "capacity authority has published nothing; UNKNOWN capacity cannot authorize");
      return false;
    case EngineReadiness::AwaitingReservations:
      outcome = AdmissionOutcome::StaleInput;
      constraint = constraint_of(BindingConstraintKind::UnknownEvidence, "reservation_snapshot", 0,
                                 "protected obligations are UNKNOWN and cannot be proven preserved");
      return false;
    case EngineReadiness::AwaitingCatalogs:
      outcome = AdmissionOutcome::StaleInput;
      constraint = constraint_of(BindingConstraintKind::UnknownEvidence, "catalogs", 0,
                                 "qos or priority catalog is UNKNOWN");
      return false;
    case EngineReadiness::AwaitingPaths:
      outcome = AdmissionOutcome::RejectPath;
      constraint = constraint_of(BindingConstraintKind::Path, "path_catalog", 0,
                                 "policy requires an authorized path binding but no path catalog is published");
      return false;
    case EngineReadiness::WaitingForReconciliation:
      outcome = AdmissionOutcome::StaleInput;
      constraint = constraint_of(BindingConstraintKind::UnknownEvidence, "reconciliation", 0,
                                 "recovered admitted load has not been reconciled against republished capacity");
      return false;
    case EngineReadiness::Ready:
      return true;
  }
  outcome = AdmissionOutcome::StaleInput;
  constraint = constraint_of(BindingConstraintKind::UnknownEvidence, "readiness", 0,
                             "engine readiness is not established");
  return false;
}

// ---------------------------------------------------------------------------
// Step 3: authority binding
// ---------------------------------------------------------------------------

bool EngineCore::check_expectations(const AdmissionRequest& request, AdmissionOutcome& outcome,
                                    BindingConstraint& constraint) const {
  const auto stale = [&](std::string subject, std::uint64_t id, Generation observed,
                         Generation authoritative, std::string detail) {
    outcome = AdmissionOutcome::StaleInput;
    constraint = constraint_of(BindingConstraintKind::StaleGeneration, std::move(subject), id,
                               std::move(detail));
    constraint.observed = observed;
    constraint.authoritative = authoritative;
    return false;
  };
  const auto unknown = [&](std::string subject, std::uint64_t id, std::string detail) {
    outcome = AdmissionOutcome::StaleInput;
    constraint = constraint_of(BindingConstraintKind::UnknownEvidence, std::move(subject), id,
                               std::move(detail));
    return false;
  };
  const auto require_generation = [&](Generation observed, Generation authoritative,
                                      const char* subject) -> bool {
    if (observed.is_unknown()) {
      return unknown(subject, 0, std::string("claimant did not state the ") + subject + " it observed");
    }
    if (observed != authoritative) {
      return stale(subject, 0, observed, authoritative,
                   std::string("claimant relied on a ") + subject + " that is no longer current");
    }
    return true;
  };

  const AuthorityExpectation& expected = request.expected;

  if (capacity.has_value()) {
    if (expected.capacity_generation.is_unknown()) {
      return unknown("capacity_generation", 0, "claimant did not state the capacity generation");
    }
    if (expected.capacity_snapshot.is_unknown()) {
      return unknown("capacity_snapshot", 0, "claimant did not state the capacity snapshot identity");
    }
    if (expected.capacity_snapshot != capacity->snapshot) {
      return stale("capacity_snapshot", expected.capacity_snapshot.value(),
                   Generation::from_value(expected.capacity_snapshot.value()),
                   Generation::from_value(capacity->snapshot.value()),
                   "claimant relied on a superseded capacity snapshot identity");
    }
    if (!require_generation(expected.capacity_generation, capacity->generation, "capacity_generation")) {
      return false;
    }
  } else {
    return unknown("capacity_snapshot", 0, "capacity authority has published nothing");
  }

  if (reservations.has_value()) {
    if (expected.reservation_generation.is_unknown()) {
      return unknown("reservation_generation", 0, "claimant did not state the reservation generation");
    }
    if (expected.reservation_snapshot.is_unknown()) {
      return unknown("reservation_snapshot", 0, "claimant did not state the reservation snapshot identity");
    }
    if (expected.reservation_snapshot != reservations->snapshot) {
      return stale("reservation_snapshot", expected.reservation_snapshot.value(),
                   Generation::from_value(expected.reservation_snapshot.value()),
                   Generation::from_value(reservations->snapshot.value()),
                   "claimant relied on a superseded reservation snapshot identity");
    }
    if (!require_generation(expected.reservation_generation, reservations->generation,
                            "reservation_generation")) {
      return false;
    }
  } else {
    return unknown("reservation_snapshot", 0, "protected obligations are UNKNOWN");
  }

  if (!require_generation(expected.policy_generation,
                          policy.has_value() ? policy->generation : Generation::unknown(),
                          "policy_generation")) {
    return false;
  }
  if (!require_generation(expected.qos_catalog_generation,
                          qos.has_value() ? qos->generation : Generation::unknown(),
                          "qos_catalog_generation")) {
    return false;
  }
  if (!require_generation(expected.priority_catalog_generation,
                          priorities.has_value() ? priorities->generation : Generation::unknown(),
                          "priority_catalog_generation")) {
    return false;
  }

  const bool path_required = policy.has_value() && policy->require_path_binding;
  if (path_required || !request.path_candidates.empty()) {
    if (!paths.has_value()) {
      return unknown("path_authority", 0, "no path catalog has been published");
    }
    if (!require_generation(expected.path_authority_generation, paths->path_authority_generation,
                            "path_authority_generation")) {
      return false;
    }
  }

  // Per-resource binding generations.
  for (const auto& binding : request.resource_bindings) {
    const ResourceCapacity* entry = find_capacity(binding.resource);
    if (entry == nullptr) {
      return unknown("resource", binding.resource.value(),
                     "claimant named a resource the capacity authority has not published");
    }
    if (entry->evidence != EvidenceState::Known) {
      return unknown("resource", binding.resource.value(),
                     "capacity for this resource is UNKNOWN and cannot authorize admission");
    }
    if (binding.generation != entry->generation) {
      return stale("resource", binding.resource.value(), binding.generation, entry->generation,
                   "claimant relied on a superseded resource capacity generation");
    }
  }

  // Per-path binding generations.
  for (const auto& candidate : request.path_candidates) {
    const PathAdmissionFact* fact = find_path(candidate.path);
    if (fact == nullptr) {
      return unknown("path", candidate.path.value(),
                     "claimant named a path the path authority has not published");
    }
    if (candidate.path_authority_generation != fact->path_authority_generation) {
      return stale("path", candidate.path.value(), candidate.path_authority_generation,
                   fact->path_authority_generation,
                   "claimant relied on a superseded path authority generation");
    }
  }

  const QoSClassFact* qos_fact = find_qos(request.qos);
  if (qos_fact == nullptr) {
    return unknown("qos_class", request.qos.value(), "qos class is not published by the catalog");
  }
  if (request.qos_generation != qos_fact->generation) {
    return stale("qos_class", request.qos.value(), request.qos_generation, qos_fact->generation,
                 "claimant relied on a superseded qos class generation");
  }
  const PriorityClassFact* priority_fact = find_priority(request.priority);
  if (priority_fact == nullptr) {
    return unknown("priority_class", request.priority.value(),
                   "priority class is not published by the catalog");
  }
  if (request.priority_generation != priority_fact->generation) {
    return stale("priority_class", request.priority.value(), request.priority_generation,
                 priority_fact->generation,
                 "claimant relied on a superseded priority class generation");
  }

  // Reservation references must exist at exactly the generation claimed.
  for (const auto& ref : request.reservation_refs) {
    bool found = false;
    for (const auto& obligation : reservations->obligations) {
      if (obligation.reservation != ref.reservation) continue;
      found = true;
      if (obligation.reservation_generation != ref.generation) {
        return stale("reservation", ref.reservation.value(), ref.generation,
                     obligation.reservation_generation,
                     "claimant relied on a superseded reservation generation");
      }
      break;
    }
    if (!found) {
      return unknown("reservation", ref.reservation.value(),
                     "claimant referenced a reservation the reservation authority has not published");
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Step 4: policy, qos and priority
// ---------------------------------------------------------------------------

bool EngineCore::check_policy(const AdmissionRequest& request, AdmissionOutcome& outcome,
                              BindingConstraint& constraint) const {
  if (!policy.has_value()) {
    outcome = AdmissionOutcome::RejectPolicy;
    constraint = constraint_of(BindingConstraintKind::Policy, "policy", 0, "no policy is published");
    return false;
  }
  const AdmissionPolicy& p = *policy;

  if (!p.allowed_qos.empty()) {
    const bool allowed = std::find(p.allowed_qos.begin(), p.allowed_qos.end(), request.qos) != p.allowed_qos.end();
    if (!allowed) {
      outcome = AdmissionOutcome::RejectQoS;
      constraint = constraint_of(BindingConstraintKind::QoSClass, "qos_class", request.qos.value(),
                                 "policy does not permit this qos class");
      return false;
    }
  }
  if (!p.allowed_priorities.empty()) {
    const bool allowed = std::find(p.allowed_priorities.begin(), p.allowed_priorities.end(),
                                   request.priority) != p.allowed_priorities.end();
    if (!allowed) {
      outcome = AdmissionOutcome::RejectPolicy;
      constraint = constraint_of(BindingConstraintKind::PriorityClass, "priority_class",
                                 request.priority.value(),
                                 "policy does not permit this priority class");
      return false;
    }
  }
  if (p.require_reservation_reference && request.reservation_refs.empty()) {
    outcome = AdmissionOutcome::RejectPolicy;
    constraint = constraint_of(BindingConstraintKind::Policy, "reservation_reference", 0,
                               "policy requires the demand to reference an existing reservation");
    return false;
  }
  if (request.preemptible && !p.allow_preemptible_admission) {
    outcome = AdmissionOutcome::RejectPolicy;
    constraint = constraint_of(BindingConstraintKind::Policy, "preemptible", 0,
                               "policy does not permit preemptible admission");
    return false;
  }
  if (request.requests_obligation_preemption) {
    outcome = AdmissionOutcome::RejectObligation;
    constraint = constraint_of(BindingConstraintKind::Obligation, "obligation_preemption", 0,
                               "admission does not own reservation lifecycle and never preempts a "
                               "protected obligation");
    return false;
  }

  const QoSClassFact* qos_fact = find_qos(request.qos);
  if (qos_fact == nullptr) {
    outcome = AdmissionOutcome::RejectQoS;
    constraint = constraint_of(BindingConstraintKind::UnknownEvidence, "qos_class", request.qos.value(),
                               "qos class is not published");
    return false;
  }
  if (request.rate.minimum < qos_fact->minimum_rate) {
    outcome = AdmissionOutcome::RejectQoS;
    constraint = constraint_of(BindingConstraintKind::QoSClass, "qos_minimum", request.qos.value(),
                               "demand minimum is below the floor of the requested qos class");
    constraint.required = qos_fact->minimum_rate;
    constraint.available = request.rate.minimum;
    return false;
  }
  if (request.rate.maximum > qos_fact->maximum_rate) {
    outcome = AdmissionOutcome::RejectQoS;
    constraint = constraint_of(BindingConstraintKind::QoSClass, "qos_maximum", request.qos.value(),
                               "demand maximum exceeds the ceiling of the requested qos class");
    constraint.required = request.rate.maximum;
    constraint.available = qos_fact->maximum_rate;
    return false;
  }
  if (request.maximum_latency > qos_fact->maximum_latency) {
    outcome = AdmissionOutcome::RejectQoS;
    constraint = constraint_of(BindingConstraintKind::QoSClass, "qos_latency", request.qos.value(),
                               "demand latency budget exceeds what the qos class guarantees");
    constraint.required = Rate::from_value(request.maximum_latency.value());
    constraint.available = Rate::from_value(qos_fact->maximum_latency.value());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Step 5: authorized path selection
// ---------------------------------------------------------------------------

AdmissionOutcome EngineCore::select_path(const AdmissionRequest& request, PathId& selected,
                                         Generation& selected_generation,
                                         std::vector<ResourceId>& traversal, bool& path_selected,
                                         bool& capacity_blocked,
                                         BindingConstraint& constraint) const {
  path_selected = false;
  capacity_blocked = false;
  traversal.clear();

  const bool path_required = policy.has_value() && policy->require_path_binding;
  if (request.path_candidates.empty()) {
    if (path_required) {
      constraint = constraint_of(BindingConstraintKind::Policy, "path_binding", 0,
                                 "policy requires an authorized path binding and none was declared");
      return AdmissionOutcome::RejectPolicy;
    }
    return AdmissionOutcome::Admit;
  }

  const bool substitution_allowed =
      request.allow_path_substitution && (!policy.has_value() || policy->allow_path_substitution);

  std::vector<std::size_t> order;
  if (request.required_path.is_known()) {
    for (std::size_t i = 0; i < request.path_candidates.size(); ++i) {
      if (request.path_candidates[i].path == request.required_path) {
        order.push_back(i);
        break;
      }
    }
  } else if (!substitution_allowed) {
    if (!request.path_candidates.empty()) order.push_back(0);
  } else {
    for (std::size_t i = 0; i < request.path_candidates.size(); ++i) order.push_back(i);
  }

  BindingConstraint unknown_evidence{};
  bool have_unknown = false;
  BindingConstraint capacity_failure{};
  std::vector<ResourceId> capacity_failure_traversal;
  bool have_capacity_failure = false;
  BindingConstraint path_failure{};
  bool have_path_failure = false;

  for (const std::size_t index : order) {
    const PathCandidate& candidate = request.path_candidates[index];
    const PathAdmissionFact* fact = find_path(candidate.path);
    if (fact == nullptr) {
      if (!have_path_failure) {
        path_failure = constraint_of(BindingConstraintKind::Path, "path", candidate.path.value(),
                                     "path is not published by the path authority");
        have_path_failure = true;
      }
      continue;
    }
    if (fact->state == PathState::Unknown) {
      if (!have_unknown) {
        unknown_evidence = constraint_of(BindingConstraintKind::UnknownEvidence, "path",
                                         candidate.path.value(),
                                         "path state is UNKNOWN and cannot authorize admission");
        have_unknown = true;
      }
      continue;
    }
    if (fact->state != PathState::Up) {
      if (!have_path_failure) {
        path_failure = constraint_of(BindingConstraintKind::Path, "path", candidate.path.value(),
                                     "path is not up");
        have_path_failure = true;
      }
      continue;
    }
    bool resource_unknown = false;
    for (const auto& resource : fact->resources) {
      const ResourceCapacity* entry = find_capacity(resource);
      if (entry == nullptr || entry->evidence != EvidenceState::Known) {
        resource_unknown = true;
        if (!have_unknown) {
          unknown_evidence = constraint_of(BindingConstraintKind::UnknownEvidence, "resource",
                                           resource.value(),
                                           "capacity on the traversed resource is UNKNOWN");
          have_unknown = true;
        }
        break;
      }
    }
    if (resource_unknown) continue;

    if (fact->path_latency > request.maximum_latency) {
      if (!have_path_failure) {
        path_failure = constraint_of(BindingConstraintKind::Path, "path", candidate.path.value(),
                                     "path latency exceeds the demand budget");
        path_failure.required = Rate::from_value(request.maximum_latency.value());
        path_failure.available = Rate::from_value(fact->path_latency.value());
        have_path_failure = true;
      }
      continue;
    }

    const std::uint64_t path_free =
        sat_sub(fact->path_usable_capacity.value(), admitted_on_path(candidate.path).value());
    if (Rate::from_value(path_free) < request.rate.minimum) {
      if (!have_capacity_failure) {
        capacity_failure = constraint_of(BindingConstraintKind::Capacity, "path", candidate.path.value(),
                                         "authorized path capacity is exhausted below the demand minimum");
        capacity_failure.required = request.rate.minimum;
        capacity_failure.available = Rate::from_value(path_free);
        capacity_failure_traversal = fact->resources;
        have_capacity_failure = true;
      }
      continue;
    }

    bool resource_short = false;
    for (const auto& resource : fact->resources) {
      const ResourceCapacity* entry = find_capacity(resource);
      const Rate headroom = headroom_for(*entry);
      const Rate allow = entry->usable.saturating_sub(headroom).saturating_sub(obligations_for(resource));
      if (allow < request.rate.minimum) {
        resource_short = true;
        break;
      }
    }
    if (resource_short) {
      if (!have_capacity_failure) {
        capacity_failure = constraint_of(BindingConstraintKind::Capacity, "path", candidate.path.value(),
                                         "a resource traversed by this path cannot serve the demand minimum");
        capacity_failure.required = request.rate.minimum;
        capacity_failure_traversal = fact->resources;
        have_capacity_failure = true;
      }
      continue;
    }

    selected = candidate.path;
    selected_generation = candidate.path_authority_generation;
    traversal = fact->resources;
    path_selected = true;
    return AdmissionOutcome::Admit;
  }

  if (have_unknown) {
    constraint = unknown_evidence;
    return AdmissionOutcome::StaleInput;
  }
  if (have_capacity_failure) {
    // Fall through to the capacity stage so the caller sees a capacity or
    // obligation verdict rather than a path verdict.
    capacity_blocked = true;
    constraint = capacity_failure;
    traversal = capacity_failure_traversal;
    return AdmissionOutcome::Admit;
  }
  constraint = have_path_failure
                   ? path_failure
                   : constraint_of(BindingConstraintKind::Path, "path", 0, "no authorized path is usable");
  return AdmissionOutcome::RejectPath;
}

// ---------------------------------------------------------------------------
// Step 6: capacity, headroom and obligation arithmetic
// ---------------------------------------------------------------------------

AdmissionOutcome EngineCore::evaluate_capacity(const AdmissionRequest& request,
                                               const std::vector<ResourceId>& targets,
                                               const PathAdmissionFact* selected_path,
                                               std::vector<ResourceView>& views, Rate& ceiling,
                                               BindingConstraint& constraint) const {
  views.clear();
  ceiling = request.rate.maximum;

  const Rate minimum = request.rate.minimum;
  Rate limiting = request.rate.maximum;
  BindingConstraintKind limiting_kind = BindingConstraintKind::Capacity;
  std::string limiting_subject = "grant";
  std::uint64_t limiting_id = 0;

  const bool defer_preferred =
      request.contention == ContentionPreference::Defer ||
      (request.contention == ContentionPreference::InheritPolicy && policy.has_value() &&
       policy->on_contention == ContentionAction::Defer);
  const std::uint64_t willing =
      policy.has_value() ? std::min(request.deadline_ticks, policy->defer_horizon_ticks) : std::uint64_t{0};
  const std::uint64_t horizon = sat_add(tick, willing);

  for (const auto& resource : targets) {
    const ResourceCapacity* entry = find_capacity(resource);
    if (entry == nullptr || entry->evidence != EvidenceState::Known) {
      constraint = constraint_of(BindingConstraintKind::UnknownEvidence, "resource", resource.value(),
                                 "capacity for this resource is UNKNOWN and cannot authorize admission");
      return AdmissionOutcome::StaleInput;
    }
    ResourceView view;
    view.resource = resource;
    view.generation = entry->generation;
    view.usable = entry->usable;
    view.headroom = headroom_for(*entry);
    view.obligations = obligations_for(resource);
    view.admitted = ledger.admitted(resource);

    const Rate headroom_available = entry->usable.saturating_sub(view.headroom);
    view.available_after_obligations = headroom_available.saturating_sub(view.obligations);
    view.available_for_new = view.available_after_obligations.saturating_sub(view.admitted);
    views.push_back(view);

    if (entry->usable < minimum) {
      constraint = constraint_of(BindingConstraintKind::Capacity, "resource", resource.value(),
                                 "authoritative usable capacity is below the demand minimum");
      constraint.required = minimum;
      constraint.available = entry->usable;
      return AdmissionOutcome::RejectCapacity;
    }
    if (headroom_available < minimum) {
      constraint = constraint_of(BindingConstraintKind::Headroom, "resource", resource.value(),
                                 "protected headroom leaves less than the demand minimum");
      constraint.required = minimum;
      constraint.available = headroom_available;
      return AdmissionOutcome::RejectCapacity;
    }
    if (view.available_after_obligations < minimum) {
      if (defer_preferred && obligation_releases_within(resource, horizon)) {
        constraint = constraint_of(BindingConstraintKind::Obligation, "resource", resource.value(),
                                   "a protected obligation releases within the deferral horizon");
        constraint.required = minimum;
        constraint.available = view.available_after_obligations;
        return AdmissionOutcome::Defer;
      }
      constraint = constraint_of(BindingConstraintKind::Obligation, "resource", resource.value(),
                                 "protected obligations hold capacity below the demand minimum");
      constraint.required = minimum;
      constraint.available = view.available_after_obligations;
      return AdmissionOutcome::RejectObligation;
    }
    if (view.available_for_new < minimum) {
      if (defer_preferred && willing != 0) {
        constraint = constraint_of(BindingConstraintKind::AdmittedLoad, "resource", resource.value(),
                                   "existing admitted load is expected to drain within the deferral horizon");
        constraint.required = minimum;
        constraint.available = view.available_for_new;
        return AdmissionOutcome::Defer;
      }
      constraint = constraint_of(BindingConstraintKind::AdmittedLoad, "resource", resource.value(),
                                 "existing admitted load leaves less than the demand minimum");
      constraint.required = minimum;
      constraint.available = view.available_for_new;
      return AdmissionOutcome::RejectCapacity;
    }
    if (view.available_for_new < limiting) {
      limiting = view.available_for_new;
      limiting_kind = BindingConstraintKind::AdmittedLoad;
      limiting_subject = "resource";
      limiting_id = resource.value();
    }
  }

  if (selected_path != nullptr) {
    const std::uint64_t path_free =
        sat_sub(selected_path->path_usable_capacity.value(), admitted_on_path(selected_path->path).value());
    if (Rate::from_value(path_free) < minimum) {
      constraint = constraint_of(BindingConstraintKind::Capacity, "path", selected_path->path.value(),
                                 "authorized path capacity is exhausted below the demand minimum");
      constraint.required = minimum;
      constraint.available = Rate::from_value(path_free);
      return AdmissionOutcome::RejectCapacity;
    }
    if (Rate::from_value(path_free) < limiting) {
      limiting = Rate::from_value(path_free);
      limiting_kind = BindingConstraintKind::Capacity;
      limiting_subject = "path";
      limiting_id = selected_path->path.value();
    }
  }

  ceiling = limiting;
  constraint = constraint_of(limiting_kind, std::move(limiting_subject), limiting_id,
                             limiting < request.rate.maximum
                                 ? "the granted rate is capped by this binding constraint"
                                 : "capacity is sufficient for the requested maximum");
  constraint.required = request.rate.maximum;
  constraint.available = limiting;
  return AdmissionOutcome::Admit;
}

// ---------------------------------------------------------------------------
// Admission
// ---------------------------------------------------------------------------

Expected<AdmissionDecision> EngineCore::admit_locked(const AdmissionRequest& request,
                                                     const ClaimContext& claim) {
  // Step 0: structural contract. A malformed request is refused, never guessed.
  const Status contract = validate(request);
  if (!contract.is_ok()) {
    AdmissionDecision decision =
        refusal(request, claim, AdmissionOutcome::RejectPolicy,
                constraint_of(BindingConstraintKind::RequestContract, "request", request.request.value(),
                              contract.message()));
    const Status written = finish_refusal(decision, request, false);
    if (!written.is_ok()) return written;
    return decision;
  }

  // Step 1: fencing.
  {
    AdmissionOutcome outcome = AdmissionOutcome::Admit;
    BindingConstraint constraint;
    if (!check_fence(claim, outcome, constraint)) {
      AdmissionDecision decision = refusal(request, claim, outcome, std::move(constraint));
      const Status written = finish_refusal(decision, request, true);
      if (!written.is_ok()) return written;
      return decision;
    }
  }

  // Step 2: idempotency and identity reuse.
  AttemptRecord probe;
  probe.request = request.request;
  probe.demand = request.demand;
  probe.demand_generation = request.demand_generation;
  probe.attempt = request.attempt;
  probe.fingerprint = fingerprint(request);
  const AttemptRegistry::Classification classification = attempts.classify(probe);
  if (classification.disposition == AttemptDisposition::Replay) {
    ++counters.idempotent_replays;
    const auto cached = decision_cache.find(classification.existing.decision.value());
    if (cached != decision_cache.end()) {
      AdmissionDecision replay = cached->second;
      replay.idempotent_replay = true;
      return replay;
    }
    const DecisionRecord* record = history.find(classification.existing.decision);
    if (record == nullptr) {
      AdmissionDecision decision =
          refusal(request, claim, AdmissionOutcome::ConflictingInput,
                  constraint_of(BindingConstraintKind::IdentityConflict, "attempt", request.attempt.value(),
                                "attempt was acknowledged but its decision record has been retired"));
      const Status written = finish_refusal(decision, request, false);
      if (!written.is_ok()) return written;
      return decision;
    }
    AdmissionDecision replay;
    replay.decision = record->decision;
    replay.audit = record->audit;
    replay.outcome = record->outcome;
    replay.request = record->request;
    replay.demand = record->demand;
    replay.demand_generation = record->demand_generation;
    replay.attempt = record->attempt;
    replay.epoch = record->epoch;
    replay.incarnation = record->incarnation;
    replay.decided_tick = record->decided_tick;
    replay.selected_path = record->path;
    replay.selected_path_generation = record->bound_path_generation;
    replay.granted = record->granted;
    replay.minimum_guaranteed = request.rate.minimum;
    replay.desired = request.rate.desired;
    replay.maximum = request.rate.maximum;
    replay.bound_capacity_generation = record->bound_capacity_generation;
    replay.revalidation_required = record->state == RevalidationState::Invalidated;
    replay.request_fingerprint = record->fingerprint;
    replay.idempotent_replay = true;
    configure_explanation(replay.explanation);
    fill_authority(replay.authority);
    replay.authority.add(AuthorityKind::CapacitySnapshot, record->decision.value(),
                         record->bound_capacity_generation, record->epoch);
    BindingConstraint note =
        constraint_of(BindingConstraintKind::None, "idempotent_replay", record->decision.value(),
                      "identical attempt replayed from the durable decision history");
    replay.explanation.add(std::move(note));
    return replay;
  }
  if (classification.disposition == AttemptDisposition::Conflict) {
    BindingConstraint conflict =
        constraint_of(BindingConstraintKind::IdentityConflict, "attempt", request.attempt.value(),
                      classification.request_id_conflict
                          ? "request identity was reused for different content"
                          : "attempt identity was reused with different content");
    conflict.observed = Generation::from_value(classification.existing.fingerprint);
    conflict.authoritative = Generation::from_value(probe.fingerprint);
    AdmissionDecision decision = refusal(request, claim, AdmissionOutcome::ConflictingInput,
                                         std::move(conflict));
    const Status written = finish_refusal(decision, request, false);
    if (!written.is_ok()) return written;
    return decision;
  }

  // Step 3: readiness. UNKNOWN evidence never authorizes.
  {
    AdmissionOutcome outcome = AdmissionOutcome::Admit;
    BindingConstraint constraint;
    if (!check_readiness(outcome, constraint)) {
      AdmissionDecision decision = refusal(request, claim, outcome, std::move(constraint));
      const Status written = finish_refusal(decision, request, true);
      if (!written.is_ok()) return written;
      return decision;
    }
  }

  // Step 4: authority binding.
  {
    AdmissionOutcome outcome = AdmissionOutcome::Admit;
    BindingConstraint constraint;
    if (!check_expectations(request, outcome, constraint)) {
      AdmissionDecision decision = refusal(request, claim, outcome, std::move(constraint));
      const Status written = finish_refusal(decision, request, true);
      if (!written.is_ok()) return written;
      return decision;
    }
  }

  // Step 5: policy, qos and priority.
  {
    AdmissionOutcome outcome = AdmissionOutcome::Admit;
    BindingConstraint constraint;
    if (!check_policy(request, outcome, constraint)) {
      AdmissionDecision decision = refusal(request, claim, outcome, std::move(constraint));
      const Status written = finish_refusal(decision, request, true);
      if (!written.is_ok()) return written;
      return decision;
    }
  }

  // Step 6: authorized path selection.
  PathId selected_path{};
  Generation selected_path_generation{};
  std::vector<ResourceId> traversal;
  bool path_selected = false;
  bool capacity_blocked = false;
  {
    BindingConstraint constraint;
    const AdmissionOutcome outcome = select_path(request, selected_path, selected_path_generation,
                                                 traversal, path_selected, capacity_blocked, constraint);
    if (outcome != AdmissionOutcome::Admit) {
      AdmissionDecision decision = refusal(request, claim, outcome, std::move(constraint));
      const Status written = finish_refusal(decision, request, true);
      if (!written.is_ok()) return written;
      return decision;
    }
  }

  // Effective resource demand: declared bindings plus whatever the selected path
  // traverses. Admission never invents a resource or a path.
  std::vector<ResourceId> targets;
  targets.reserve(request.resource_bindings.size() + traversal.size());
  for (const auto& binding : request.resource_bindings) targets.push_back(binding.resource);
  for (const auto& resource : traversal) targets.push_back(resource);
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

  if (targets.empty()) {
    AdmissionDecision decision =
        refusal(request, claim, AdmissionOutcome::RejectPolicy,
                constraint_of(BindingConstraintKind::RequestContract, "request", request.request.value(),
                              "request resolves to an empty governed resource set"));
    const Status written = finish_refusal(decision, request, true);
    if (!written.is_ok()) return written;
    return decision;
  }

  // Step 7: capacity, headroom and obligation arithmetic.
  std::vector<ResourceView> views;
  Rate ceiling{};
  BindingConstraint binding;
  const PathAdmissionFact* selected_fact = path_selected ? find_path(selected_path) : nullptr;
  const AdmissionOutcome capacity_outcome =
      evaluate_capacity(request, targets, selected_fact, views, ceiling, binding);
  if (capacity_outcome != AdmissionOutcome::Admit) {
    AdmissionDecision decision = refusal(request, claim, capacity_outcome, std::move(binding));
    fill_effective(views, decision);
    fill_target_authority(decision.authority, views, selected_path, selected_path_generation);
    const Status written = finish_refusal(decision, request, true);
    if (!written.is_ok()) return written;
    return decision;
  }

  // Step 8: verdict and grant.
  const Rate grant = ceiling.clamped_to(request.rate.maximum);
  if (grant < request.rate.minimum) {
    AdmissionDecision decision = refusal(request, claim, AdmissionOutcome::RejectCapacity,
                                         std::move(binding));
    fill_effective(views, decision);
    fill_target_authority(decision.authority, views, selected_path, selected_path_generation);
    const Status written = finish_refusal(decision, request, true);
    if (!written.is_ok()) return written;
    return decision;
  }

  AdmissionOutcome verdict = AdmissionOutcome::Admit;
  const QoSClassFact* qos_fact = find_qos(request.qos);
  if (grant < request.rate.desired) {
    const bool degrade_allowed = policy.has_value() && policy->allow_degraded &&
                                 request.degradation != DegradePreference::Never &&
                                 (qos_fact == nullptr || qos_fact->degradation_allowed);
    if (!degrade_allowed) {
      BindingConstraint refusal_constraint =
          constraint_of(BindingConstraintKind::Degradation, "grant", 0,
                        "the requested rate cannot be met and degradation is not permitted");
      refusal_constraint.required = request.rate.desired;
      refusal_constraint.available = grant;
      AdmissionDecision decision =
          refusal(request, claim, AdmissionOutcome::RejectCapacity, std::move(refusal_constraint));
      fill_effective(views, decision);
    fill_target_authority(decision.authority, views, selected_path, selected_path_generation);
      const Status written = finish_refusal(decision, request, true);
      if (!written.is_ok()) return written;
      return decision;
    }
    verdict = AdmissionOutcome::AdmitDegraded;
    BindingConstraint degraded =
        constraint_of(BindingConstraintKind::Degradation, "grant", 0,
                      "admitted below the desired rate but at or above the demand minimum");
    degraded.required = request.rate.desired;
    degraded.available = grant;
    binding = std::move(degraded);
  }

  AdmissionDecision decision = begin_decision(request, claim, verdict);
  decision.selected_path = selected_path;
  decision.selected_path_generation = selected_path_generation;
  decision.granted = grant;
  decision.minimum_guaranteed = request.rate.minimum;
  decision.bound_capacity_generation = capacity.has_value() ? capacity->generation : Generation::unknown();
  decision.revalidation_required = true;
  fill_effective(views, decision);
    fill_target_authority(decision.authority, views, selected_path, selected_path_generation);
  decision.explanation.add(std::move(binding));

  std::vector<std::pair<ResourceId, Rate>> amounts;
  amounts.reserve(targets.size());
  for (const auto& resource : targets) amounts.emplace_back(resource, grant);

  const Status committed = finish_admission(decision, request, amounts);
  if (!committed.is_ok()) return committed;
  return decision;
}

// ---------------------------------------------------------------------------
// Revalidation and revocation
// ---------------------------------------------------------------------------

Status EngineCore::release_decision_locked(AdmissionDecisionId decision, RevocationReason reason,
                                           RevalidationState state, Explanation& explanation) {
  if (!ledger.holds(decision)) {
    return Status::error(StatusCode::NotFound, "decision holds no admitted load");
  }
  std::vector<LedgerEntry> held;
  for (const auto& entry : ledger.entries()) {
    if (entry.decision == decision) held.push_back(entry);
  }
  ReleaseRecord release;
  release.decision = decision;
  release.epoch = epoch;
  release.reason = reason;
  release.released = held;
  Status written = write_release(release);
  if (!written.is_ok()) return written;

  std::vector<LedgerEntry> released;
  Status removed = ledger.release(decision, released);
  if (!removed.is_ok()) return removed;
  SumLatch latch;
  for (const auto& entry : released) latch.add(entry.amount.value());

  RevocationRecord revocation;
  revocation.decision = decision;
  revocation.epoch = epoch;
  revocation.audit = audit;
  revocation.reason = reason;
  revocation.state = state;
  Status journaled = write_revocation(revocation);
  if (!journaled.is_ok()) return journaled;

  const DecisionRecord* existing = history.find(decision);
  if (existing != nullptr) {
    DecisionRecord updated = *existing;
    updated.state = state;
    updated.reason = reason;
    updated.ledger_live = false;
    history.update(decision, updated);
  }
  history.note_release();
  auto cached = decision_cache.find(decision.value());
  if (cached != decision_cache.end()) decision_cache.erase(cached);

  BindingConstraint note = constraint_of(BindingConstraintKind::Obligation, "revocation",
                                         decision.value(), std::string(to_string(reason)));
  note.available = Rate::from_value(latch.saturated_total());
  explanation.add(std::move(note));
  return Status::ok();
}

Expected<RevalidationResult> EngineCore::revalidate_locked(AdmissionDecisionId decision,
                                                           const ClaimContext& claim) {
  (void)claim;
  const DecisionRecord* record = history.find(decision);
  if (record == nullptr) {
    return Status::error(StatusCode::NotFound, "decision is not retained in history");
  }
  if (!is_admitting(record->outcome)) {
    return Status::error(StatusCode::InvalidArgument, "decision did not admit traffic and holds no capacity");
  }

  RevalidationResult result;
  result.decision = decision;
  result.original_outcome = record->outcome;
  result.granted = record->granted;
  result.tick = tick;
  configure_explanation(result.explanation);
  fill_authority(result.authority);

  if (record->state == RevalidationState::Revoked || record->state == RevalidationState::Expired) {
    result.state = record->state;
    result.reason = record->reason;
    result.explanation.add(constraint_of(BindingConstraintKind::Obligation, "revalidation",
                                         decision.value(),
                                         "decision was already released; nothing further to revoke"));
    return result;
  }
  if (!ledger.holds(decision)) {
    result.state = RevalidationState::Revoked;
    result.reason = RevocationReason::ClaimantDeath;
    result.explanation.add(constraint_of(BindingConstraintKind::Obligation, "revalidation",
                                         decision.value(),
                                         "decision holds no admitted load in this incarnation"));
    const DecisionRecord* existing = history.find(decision);
    if (existing != nullptr) {
      DecisionRecord updated = *existing;
      updated.state = result.state;
      updated.reason = result.reason;
      updated.ledger_live = false;
      history.update(decision, updated);
    }
    return result;
  }
  if (record->epoch != epoch || record->incarnation != incarnation) {
    // A coordinator restart invalidates live admission authority. The durable
    // grant is not evidence that the fabric still holds the capacity.
    result.state = RevalidationState::Revoked;
    result.reason = RevocationReason::CoordinatorRestart;
    result.explanation.add(constraint_of(BindingConstraintKind::Fence, "revalidation", decision.value(),
                                         "decision was issued by a previous coordinator incarnation"));
    const Status released = release_decision_locked(decision, result.reason, result.state,
                                                    result.explanation);
    if (!released.is_ok()) return released;
    result.ledger_released = true;
    return result;
  }

  bool requires_revalidation = false;
  for (const auto& entry : ledger.entries()) {
    if (entry.decision == decision && entry.requires_revalidation) {
      requires_revalidation = true;
      break;
    }
  }
  if (requires_revalidation) {
    result.state = RevalidationState::Ambiguous;
    result.explanation.add(constraint_of(BindingConstraintKind::UnknownEvidence, "revalidation",
                                         decision.value(),
                                         "recovered load has not yet been reconciled against "
                                         "republished capacity"));
    return result;
  }

  // Substantive checks. A missing authority is a refusal, never a pass.
  if (!capacity.has_value()) {
    result.state = RevalidationState::Revoked;
    result.reason = RevocationReason::CapacityReduction;
    result.explanation.add(constraint_of(BindingConstraintKind::UnknownEvidence, "capacity_snapshot", 0,
                                         "capacity authority is UNKNOWN; the grant cannot be justified"));
    const Status released = release_decision_locked(decision, result.reason, result.state,
                                                    result.explanation);
    if (!released.is_ok()) return released;
    result.ledger_released = true;
    return result;
  }
  if (!reservations.has_value() || !policy.has_value()) {
    result.state = RevalidationState::Revoked;
    result.reason = RevocationReason::PolicyChange;
    result.explanation.add(constraint_of(BindingConstraintKind::UnknownEvidence, "authority", 0,
                                         "obligation or policy authority is UNKNOWN"));
    const Status released = release_decision_locked(decision, result.reason, result.state,
                                                    result.explanation);
    if (!released.is_ok()) return released;
    result.ledger_released = true;
    return result;
  }

  bool invalidated = false;
  if (record->bound_capacity_generation != capacity->generation) invalidated = true;
  if (record->bound_policy_generation != policy->generation) invalidated = true;
  if (qos.has_value() && record->bound_qos_generation != qos->generation) invalidated = true;
  if (priorities.has_value() && record->bound_priority_generation != priorities->generation) {
    invalidated = true;
  }
  if (reservations.has_value() && record->bound_reservation_generation != reservations->generation) {
    invalidated = true;
  }
  if (paths.has_value() && record->bound_path_generation != paths->path_authority_generation) {
    invalidated = true;
  }

  if (record->path.is_known()) {
    const PathAdmissionFact* fact = find_path(record->path);
    if (fact == nullptr) {
      result.state = RevalidationState::Revoked;
      result.reason = RevocationReason::PathRevoked;
      result.explanation.add(constraint_of(BindingConstraintKind::Path, "path", record->path.value(),
                                           "the path that justified this grant is no longer published"));
      const Status released = release_decision_locked(decision, result.reason, result.state,
                                                      result.explanation);
      if (!released.is_ok()) return released;
      result.ledger_released = true;
      return result;
    }
    if (fact->state != PathState::Up) {
      result.state = RevalidationState::Revoked;
      result.reason = RevocationReason::PathRevoked;
      result.explanation.add(constraint_of(BindingConstraintKind::Path, "path", record->path.value(),
                                           "the path that justified this grant is not up"));
      const Status released = release_decision_locked(decision, result.reason, result.state,
                                                      result.explanation);
      if (!released.is_ok()) return released;
      result.ledger_released = true;
      return result;
    }
  }

  // The invariant must still hold with this grant in place.
  for (const auto& entry : ledger.entries()) {
    if (entry.decision != decision) continue;
    const ResourceCapacity* cap = find_capacity(entry.resource);
    if (cap == nullptr || cap->evidence != EvidenceState::Known) {
      result.state = RevalidationState::Revoked;
      result.reason = RevocationReason::CapacityReduction;
      result.explanation.add(constraint_of(BindingConstraintKind::UnknownEvidence, "resource",
                                           entry.resource.value(),
                                           "capacity for a granted resource is UNKNOWN"));
      const Status released = release_decision_locked(decision, result.reason, result.state,
                                                      result.explanation);
      if (!released.is_ok()) return released;
      result.ledger_released = true;
      return result;
    }
    const Rate allowance = cap->usable.saturating_sub(headroom_for(*cap)).saturating_sub(
        obligations_for(entry.resource));
    const Rate admitted = ledger.admitted(entry.resource);
    if (admitted > allowance) {
      result.state = RevalidationState::Revoked;
      result.reason = RevocationReason::CapacityReduction;
      BindingConstraint note = constraint_of(BindingConstraintKind::Capacity, "resource",
                                             entry.resource.value(),
                                             "admitted load plus obligations and headroom now exceeds "
                                             "authoritative usable capacity");
      note.required = admitted;
      note.available = allowance;
      result.explanation.add(std::move(note));
      const Status released = release_decision_locked(decision, result.reason, result.state,
                                                      result.explanation);
      if (!released.is_ok()) return released;
      result.ledger_released = true;
      return result;
    }
  }

  result.state = invalidated ? RevalidationState::Invalidated : RevalidationState::Valid;
  result.reason = invalidated ? RevocationReason::GenerationAdvance : RevocationReason::NotRevoked;
  result.explanation.add(constraint_of(
      invalidated ? BindingConstraintKind::StaleGeneration : BindingConstraintKind::None, "revalidation",
      decision.value(),
      invalidated ? "authority advanced past the generation this grant was bound to"
                  : "authority generations still match the grant"));
  if (invalidated) {
    RevocationRecord revocation;
    revocation.decision = decision;
    revocation.epoch = epoch;
    revocation.audit = audit;
    revocation.reason = result.reason;
    revocation.state = result.state;
    const Status journaled = write_revocation(revocation);
    if (!journaled.is_ok()) return journaled;
    const DecisionRecord* existing = history.find(decision);
    if (existing != nullptr) {
      DecisionRecord updated = *existing;
      updated.state = result.state;
      updated.reason = result.reason;
      history.update(decision, updated);
    }
  }
  return result;
}

Expected<RevalidationResult> EngineCore::revoke_locked(AdmissionDecisionId decision,
                                                       RevocationReason reason,
                                                       const ClaimContext& claim) {
  (void)claim;
  const DecisionRecord* record = history.find(decision);
  if (record == nullptr) {
    return Status::error(StatusCode::NotFound, "decision is not retained in history");
  }
  RevalidationResult result;
  result.decision = decision;
  result.original_outcome = record->outcome;
  result.granted = record->granted;
  result.tick = tick;
  result.reason = reason;
  configure_explanation(result.explanation);
  fill_authority(result.authority);

  if (!is_admitting(record->outcome)) {
    result.state = RevalidationState::Valid;
    result.explanation.add(constraint_of(BindingConstraintKind::None, "revocation", decision.value(),
                                         "decision never admitted traffic; nothing to release"));
    return result;
  }
  if (record->state == RevalidationState::Revoked || record->state == RevalidationState::Expired ||
      !ledger.holds(decision)) {
    result.state = RevalidationState::Revoked;
    result.reason = record->reason != RevocationReason::NotRevoked ? record->reason : reason;
    result.explanation.add(constraint_of(BindingConstraintKind::None, "revocation", decision.value(),
                                         "decision already holds no admitted load"));
    return result;
  }
  result.state = RevalidationState::Revoked;
  const Status released = release_decision_locked(decision, reason, result.state, result.explanation);
  if (!released.is_ok()) return released;
  result.ledger_released = true;
  return result;
}

// ---------------------------------------------------------------------------
// Restart reconciliation
// ---------------------------------------------------------------------------

Status EngineCore::enforce_accounting_locked(EnforcementTrigger trigger) {
  ReconciliationReport report;
  report.trigger = trigger;
  if (faults.fail_reconciliation) {
    faults.fail_reconciliation = false;
    return Status::error(StatusCode::DurableFailure, "injected enforcement failure");
  }
  if (!capacity.has_value()) {
    // Capacity is UNKNOWN. Nothing can be proven, so nothing is revoked and no
    // new admission is permitted until the capacity authority republishes.
    reconciliation = report;
    return Status::ok();
  }
  report.performed = true;

  RevocationReason reason = RevocationReason::CapacityReduction;
  switch (trigger) {
    case EnforcementTrigger::ReservationPublication:
      reason = RevocationReason::ObligationChange;
      break;
    case EnforcementTrigger::PolicyPublication:
      reason = RevocationReason::PolicyChange;
      break;
    case EnforcementTrigger::RestartRecovery:
      reason = RevocationReason::CoordinatorRestart;
      break;
    case EnforcementTrigger::CapacityPublication:
    case EnforcementTrigger::None:
      reason = RevocationReason::CapacityReduction;
      break;
  }

  // Index the ledger once so the pass is linear in decisions plus entries.
  std::map<std::uint64_t, std::vector<LedgerEntry>> held_by_decision;
  for (const auto& entry : ledger.entries()) {
    held_by_decision[entry.decision.value()].push_back(entry);
  }

  const std::vector<DecisionRecord> ordered = history.in_audit_order();
  for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
    const DecisionRecord& record = *it;
    if (!is_admitting(record.outcome)) continue;
    const auto held = held_by_decision.find(record.decision.value());
    if (held == held_by_decision.end()) continue;

    bool fits = true;
    BindingConstraint note;
    for (const auto& entry : held->second) {
      const ResourceCapacity* cap = find_capacity(entry.resource);
      if (cap == nullptr || cap->evidence != EvidenceState::Known) {
        fits = false;
        note = constraint_of(BindingConstraintKind::UnknownEvidence, "resource", entry.resource.value(),
                             "owned load touches a resource whose capacity is UNKNOWN");
        break;
      }
      const Rate allowance =
          cap->usable.saturating_sub(headroom_for(*cap)).saturating_sub(obligations_for(entry.resource));
      const Rate admitted = ledger.admitted(entry.resource);
      if (admitted > allowance) {
        fits = false;
        note = constraint_of(BindingConstraintKind::Capacity, "resource", entry.resource.value(),
                             "admitted load plus obligations and headroom exceeds authoritative "
                             "usable capacity; revoking newest first");
        note.required = admitted;
        note.available = allowance;
        break;
      }
    }
    if (fits) continue;

    SumLatch latch;
    for (const auto& entry : held->second) latch.add(entry.amount.value());

    Explanation explanation;
    configure_explanation(explanation);
    explanation.add(note);
    const Status released =
        release_decision_locked(record.decision, reason, RevalidationState::Revoked, explanation);
    if (!released.is_ok()) return released;
    held_by_decision.erase(held);
    report.revoked_decisions.push_back(record.decision);
    report.constraints.push_back(note);
    ++report.revoked;
    report.released = Rate::from_value(sat_add(report.released.value(), latch.saturated_total()));
  }

  // Resources where the authoritative inputs contradict each other: protected
  // obligations plus headroom already exceed usable capacity. Admission owns
  // nothing there and refuses everything that targets them.
  SumLatch infeasible;
  for (const auto& entry : capacity->resources) {
    if (entry.evidence != EvidenceState::Known) continue;
    const Rate committed =
        obligations_for(entry.resource).saturating_add(headroom_for(entry));
    if (committed > entry.usable) infeasible.add(1);
  }
  report.infeasible_resources = static_cast<std::size_t>(infeasible.total());
  report.retained = ledger.live_decisions();
  ledger.clear_revalidation_flags();
  pending_reconciliation = false;
  reconciliation = report;
  if (report.revoked != 0) {
    const Status marked = write_audit_mark();
    if (!marked.is_ok()) return marked;
  }
  return Status::ok();
}

}  // namespace naf::detail

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

namespace naf {

Expected<AdmissionDecision> AdmissionEngine::admit(const AdmissionRequest& request,
                                                   const ClaimContext& claim) {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->admit_locked(request, claim);
}

Expected<AdmissionDecision> AdmissionEngine::admit_local(const AdmissionRequest& request) {
  ClaimContext claim = ClaimContext::in_process();
  claim.trace = request.provenance.trace;
  claim.attempt = request.attempt;
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->admit_locked(request, claim);
}

Expected<RevalidationResult> AdmissionEngine::revalidate(AdmissionDecisionId decision,
                                                         const ClaimContext& claim) {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->revalidate_locked(decision, claim);
}

Expected<RevalidationResult> AdmissionEngine::revoke(AdmissionDecisionId decision,
                                                     RevocationReason reason,
                                                     const ClaimContext& claim) {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->revoke_locked(decision, reason, claim);
}

}  // namespace naf

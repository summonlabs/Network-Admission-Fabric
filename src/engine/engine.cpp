// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Engine core: lookups, authority publication, durability, sessions and
// introspection. The admission evaluation itself lives in admission.cpp.
#include "engine_core.hpp"

#include <algorithm>
#include <string>

#include "naf/core/checked.hpp"
#include "naf/core/hash.hpp"
#include "naf/durable/recovery.hpp"

namespace naf::detail {
namespace {

template <class T, class Project>
const T* find_by(const std::vector<T>& items, std::uint64_t key, Project project) {
  const auto it = std::lower_bound(items.begin(), items.end(), key,
                                   [&](const T& item, std::uint64_t value) {
                                     return project(item) < value;
                                   });
  if (it == items.end() || project(*it) != key) return nullptr;
  return &*it;
}

}  // namespace

BootRecord make_boot_record(FabricEpoch epoch, CoordinatorIncarnation incarnation) {
  BootRecord record;
  record.epoch = epoch;
  record.format_revision = format_revision;
  record.product = std::string(product_name);
  record.boot_nonce = incarnation.value();
  record.incarnation = incarnation;
  return record;
}

bool content_equal(const AdmissionPolicy& a, const AdmissionPolicy& b) noexcept {
  return a.id == b.id && a.generation == b.generation && a.headroom_permille == b.headroom_permille &&
         a.headroom_floor == b.headroom_floor && a.allow_degraded == b.allow_degraded &&
         a.allow_preemptible_admission == b.allow_preemptible_admission &&
         a.require_path_binding == b.require_path_binding &&
         a.require_reservation_reference == b.require_reservation_reference &&
         a.allow_path_substitution == b.allow_path_substitution &&
         a.on_contention == b.on_contention && a.allowed_qos == b.allowed_qos &&
         a.allowed_priorities == b.allowed_priorities &&
         a.max_explanation_constraints == b.max_explanation_constraints &&
         a.max_explanation_bytes == b.max_explanation_bytes &&
         a.max_effective_resources == b.max_effective_resources &&
         a.defer_horizon_ticks == b.defer_horizon_ticks;
}

bool content_equal(const CapacitySnapshot& a, const CapacitySnapshot& b) noexcept {
  return a.snapshot == b.snapshot && a.generation == b.generation && a.resources == b.resources;
}

bool content_equal(const ReservationSnapshot& a, const ReservationSnapshot& b) noexcept {
  return a.snapshot == b.snapshot && a.generation == b.generation && a.obligations == b.obligations;
}

bool content_equal(const PathCatalog& a, const PathCatalog& b) noexcept {
  return a.path_authority_generation == b.path_authority_generation && a.paths == b.paths;
}

bool content_equal(const QoSClassCatalog& a, const QoSClassCatalog& b) noexcept {
  return a.generation == b.generation && a.classes == b.classes;
}

bool content_equal(const PriorityClassCatalog& a, const PriorityClassCatalog& b) noexcept {
  return a.generation == b.generation && a.classes == b.classes;
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

const ResourceCapacity* EngineCore::find_capacity(ResourceId resource) const {
  if (!capacity.has_value()) return nullptr;
  return find_by(capacity->resources, resource.value(),
                 [](const ResourceCapacity& entry) { return entry.resource.value(); });
}

const PathAdmissionFact* EngineCore::find_path(PathId path) const {
  if (!paths.has_value()) return nullptr;
  return find_by(paths->paths, path.value(),
                 [](const PathAdmissionFact& fact) { return fact.path.value(); });
}

const QoSClassFact* EngineCore::find_qos(QoSClassId id) const {
  if (!qos.has_value()) return nullptr;
  return find_by(qos->classes, id.value(), [](const QoSClassFact& fact) { return fact.qos.value(); });
}

const PriorityClassFact* EngineCore::find_priority(PriorityClassId id) const {
  if (!priorities.has_value()) return nullptr;
  return find_by(priorities->classes, id.value(),
                 [](const PriorityClassFact& fact) { return fact.priority.value(); });
}

void EngineCore::rebuild_obligation_index() {
  obligation_totals_.clear();
  obligation_indices_.clear();
  if (!reservations.has_value()) return;
  for (std::size_t i = 0; i < reservations->obligations.size(); ++i) {
    const ProtectedObligation& obligation = reservations->obligations[i];
    auto& total = obligation_totals_[obligation.resource.value()];
    total = Rate::from_value(sat_add(total.value(), obligation.reserved.value()));
    obligation_indices_[obligation.resource.value()].push_back(i);
  }
}

Rate EngineCore::obligations_for(ResourceId resource) const {
  const auto it = obligation_totals_.find(resource.value());
  return it == obligation_totals_.end() ? Rate{} : it->second;
}

bool EngineCore::obligation_releases_within(ResourceId resource, std::uint64_t horizon) const {
  if (!reservations.has_value()) return false;
  const auto bucket = obligation_indices_.find(resource.value());
  if (bucket == obligation_indices_.end()) return false;
  for (const std::size_t index : bucket->second) {
    const ProtectedObligation& obligation = reservations->obligations[index];
    if (obligation.inviolable) continue;
    if (obligation.release_tick <= tick) continue;
    if (obligation.release_tick <= horizon) return true;
  }
  return false;
}

Rate EngineCore::headroom_for(const ResourceCapacity& entry) const {
  Rate headroom = entry.mandatory_headroom;
  if (policy.has_value()) {
    const Rate percent = entry.usable.permille(policy->headroom_permille);
    if (percent > headroom) headroom = percent;
    if (policy->headroom_floor > headroom) headroom = policy->headroom_floor;
  }
  return headroom.clamped_to(entry.usable);
}

Rate EngineCore::admitted_on_path(PathId path) const { return ledger.admitted_on_path(path); }

EngineReadiness EngineCore::readiness() const {
  if (!policy.has_value()) return EngineReadiness::AwaitingPolicy;
  if (!capacity.has_value()) return EngineReadiness::AwaitingCapacity;
  if (!reservations.has_value()) return EngineReadiness::AwaitingReservations;
  if (!qos.has_value() || !priorities.has_value()) return EngineReadiness::AwaitingCatalogs;
  if (policy->require_path_binding && !paths.has_value()) return EngineReadiness::AwaitingPaths;
  if (pending_reconciliation) return EngineReadiness::WaitingForReconciliation;
  return EngineReadiness::Ready;
}

// ---------------------------------------------------------------------------
// Decision plumbing
// ---------------------------------------------------------------------------

void EngineCore::fill_authority(AuthorityVector& vector) const {
  vector.add(AuthorityKind::FabricEpoch, 0, Generation::first(), epoch);
  vector.add(AuthorityKind::CoordinatorIncarnation, incarnation.value(), Generation::first(), epoch);
  if (policy.has_value()) {
    vector.add(AuthorityKind::Policy, policy->id.value(), policy->generation, epoch);
  }
  if (capacity.has_value()) {
    vector.add(AuthorityKind::CapacitySnapshot, capacity->snapshot.value(), capacity->generation, epoch);
  }
  if (reservations.has_value()) {
    vector.add(AuthorityKind::ReservationSnapshot, reservations->snapshot.value(),
               reservations->generation, epoch);
  }
  if (paths.has_value()) {
    vector.add(AuthorityKind::PathAuthority, 0, paths->path_authority_generation, epoch);
  }
  if (qos.has_value()) vector.add(AuthorityKind::QoSClassCatalog, 0, qos->generation, epoch);
  if (priorities.has_value()) {
    vector.add(AuthorityKind::PriorityClassCatalog, 0, priorities->generation, epoch);
  }
}

void EngineCore::fill_target_authority(AuthorityVector& vector,
                                       const std::vector<ResourceView>& views, PathId path,
                                       Generation path_generation) const {
  for (const auto& view : views) {
    vector.add(AuthorityKind::ResourceCapacity, view.resource.value(), view.generation, epoch);
  }
  if (path.is_known()) {
    vector.add(AuthorityKind::Path, path.value(), path_generation, epoch);
  }
  // Reservations that hold capacity on an evaluated resource are part of the
  // justification even when the demand does not reference them directly.
  if (reservations.has_value()) {
    for (const auto& view : views) {
      const auto bucket = obligation_indices_.find(view.resource.value());
      if (bucket == obligation_indices_.end()) continue;
      for (const std::size_t index : bucket->second) {
        const ProtectedObligation& obligation = reservations->obligations[index];
        vector.add(AuthorityKind::Reservation, obligation.reservation.value(),
                   obligation.reservation_generation, epoch);
      }
    }
  }
}

void EngineCore::configure_explanation(Explanation& explanation) const {
  if (policy.has_value()) {
    explanation.configure(policy->max_explanation_constraints, policy->max_explanation_bytes);
  }
}

AdmissionDecision EngineCore::begin_decision(const AdmissionRequest& request,
                                             const ClaimContext& claim,
                                             AdmissionOutcome outcome) {
  AdmissionDecision decision;
  decision.decision = AdmissionDecisionId::from_value(next_decision_sequence);
  decision.audit = audit;
  decision.outcome = outcome;
  decision.request = request.request;
  decision.demand = request.demand;
  decision.demand_generation = request.demand_generation;
  decision.attempt = request.attempt;
  decision.trace = claim.trace.is_known() ? claim.trace : request.provenance.trace;
  decision.epoch = epoch;
  decision.incarnation = incarnation;
  decision.decided_tick = tick;
  decision.desired = request.rate.desired;
  decision.maximum = request.rate.maximum;
  decision.holding_interval = request.effective_interval;
  decision.request_fingerprint = fingerprint(request);
  configure_explanation(decision.explanation);
  fill_authority(decision.authority);
  // Identity is consumed immediately so that a failed durable write can never
  // lead to an identity being reused by a later, unrelated attempt.
  ++next_decision_sequence;
  audit = AuditSequence::from_value(audit.value() + 1);
  return decision;
}

DecisionRecord EngineCore::to_record(const AdmissionDecision& decision) const {
  DecisionRecord record;
  record.decision = decision.decision;
  record.audit = decision.audit;
  record.outcome = decision.outcome;
  record.request = decision.request;
  record.demand = decision.demand;
  record.demand_generation = decision.demand_generation;
  record.attempt = decision.attempt;
  record.path = decision.selected_path;
  record.granted = decision.granted;
  record.epoch = decision.epoch;
  record.incarnation = decision.incarnation;
  record.decided_tick = decision.decided_tick;
  record.fingerprint = decision.request_fingerprint;
  record.bound_capacity_generation = decision.bound_capacity_generation;
  if (policy.has_value()) record.bound_policy_generation = policy->generation;
  if (qos.has_value()) record.bound_qos_generation = qos->generation;
  if (priorities.has_value()) record.bound_priority_generation = priorities->generation;
  if (paths.has_value()) record.bound_path_generation = paths->path_authority_generation;
  if (reservations.has_value()) record.bound_reservation_generation = reservations->generation;
  return record;
}

void EngineCore::remember(const AdmissionDecision& decision) {
  if (decision_cache.size() >= limits::max_decision_records && !decision_cache.empty()) {
    decision_cache.erase(decision_cache.begin());
  }
  decision_cache[decision.decision.value()] = decision;
}

void EngineCore::count(const AdmissionDecision& decision) {
  ++counters.decisions;
  switch (decision.outcome) {
    case AdmissionOutcome::Admit:
    case AdmissionOutcome::AdmitDegraded:
      ++counters.admissions;
      break;
    case AdmissionOutcome::Defer:
      ++counters.deferrals;
      break;
    case AdmissionOutcome::StaleInput:
      ++counters.stale_refusals;
      ++counters.refusals;
      break;
    case AdmissionOutcome::ConflictingInput:
      ++counters.conflicts;
      ++counters.refusals;
      break;
    case AdmissionOutcome::FencedClaimant:
      ++counters.fences;
      ++counters.refusals;
      break;
    default:
      ++counters.refusals;
      break;
  }
}

void EngineCore::fill_effective(const std::vector<ResourceView>& views, AdmissionDecision& decision) const {
  const std::size_t bound =
      policy.has_value() ? policy->max_effective_resources : limits::max_effective_resources;
  for (const auto& view : views) {
    if (decision.effective.size() >= bound) {
      decision.effective_truncated = true;
      break;
    }
    EffectiveResource entry;
    entry.resource = view.resource;
    entry.generation = view.generation;
    entry.usable = view.usable;
    entry.headroom = view.headroom;
    entry.obligations = view.obligations;
    entry.admitted = view.admitted;
    entry.available_for_new = view.available_for_new;
    decision.effective.push_back(entry);
  }
}

AdmissionDecision EngineCore::refusal(const AdmissionRequest& request, const ClaimContext& claim,
                                      AdmissionOutcome outcome, BindingConstraint constraint) {
  AdmissionDecision decision = begin_decision(request, claim, outcome);
  decision.explanation.add(std::move(constraint));
  return decision;
}

Status EngineCore::finish_refusal(AdmissionDecision& decision, const AdmissionRequest& request,
                                  bool record_attempt) {
  const DecisionRecord record = to_record(decision);
  Status written = write_refusal(record);
  if (!written.is_ok()) return written;
  DecisionRecord stored = record;
  stored.state = RevalidationState::Fresh;
  history.insert(stored);
  trim_history();
  if (record_attempt) {
    AttemptRecord attempt;
    attempt.request = request.request;
    attempt.demand = request.demand;
    attempt.demand_generation = request.demand_generation;
    attempt.attempt = request.attempt;
    attempt.fingerprint = decision.request_fingerprint;
    attempt.decision = decision.decision;
    attempt.outcome = decision.outcome;
    attempt.recorded_tick = tick;
    attempts.record(attempt);
  }
  remember(decision);
  count(decision);
  return Status::ok();
}

Status EngineCore::finish_admission(AdmissionDecision& decision, const AdmissionRequest& request,
                                    const std::vector<std::pair<ResourceId, Rate>>& amounts) {
  PrepareRecord prepare;
  prepare.decision = decision.decision;
  prepare.demand = decision.demand;
  prepare.attempt = decision.attempt;
  prepare.fingerprint = decision.request_fingerprint;
  prepare.epoch = epoch;
  prepare.bound_capacity_generation = decision.bound_capacity_generation;
  prepare.path = decision.selected_path;
  prepare.amounts = amounts;

  Status prepared = write_prepare(prepare);
  if (!prepared.is_ok()) return prepared;

  Status reserved = ledger.reserve(decision.decision, decision.demand, epoch,
                                   decision.bound_capacity_generation, decision.selected_path, amounts);
  if (!reserved.is_ok()) return reserved;

  DecisionRecord record = to_record(decision);
  record.ledger_live = true;
  Status committed = write_commit(record);
  if (!committed.is_ok()) {
    // The intent is durable but the commit is not: the outcome is genuinely
    // ambiguous. Undo the in-memory reservation so the account matches what a
    // restart would reconstruct, and refuse to acknowledge the admission.
    std::vector<LedgerEntry> released;
    ledger.release(decision.decision, released);
    return committed;
  }

  history.insert(record);
  trim_history();
  AttemptRecord attempt;
  attempt.request = request.request;
  attempt.demand = request.demand;
  attempt.demand_generation = request.demand_generation;
  attempt.attempt = request.attempt;
  attempt.fingerprint = decision.request_fingerprint;
  attempt.decision = decision.decision;
  attempt.outcome = decision.outcome;
  attempt.recorded_tick = tick;
  attempts.record(attempt);
  remember(decision);
  count(decision);
  Status compacted = compact_if_needed_locked();
  if (!compacted.is_ok()) return compacted;
  return Status::ok();
}

void EngineCore::trim_history() {
  if (!history.needs_trim()) return;
  history.trim(ledger);
}

Status EngineCore::compact_if_needed_locked() {
  if (journal == nullptr || !journal_ready) return Status::ok();
  if (journal->frame_count() < journal->options().compact_at_records) return Status::ok();
  return compact_locked();
}

// ---------------------------------------------------------------------------
// Durable writers
// ---------------------------------------------------------------------------

Status EngineCore::write_record(JournalRecordKind kind, const ByteBuffer& payload) {
  if (journal == nullptr || !journal_ready) return Status::ok();
  if (faults.fail_next_journal_append) {
    faults.fail_next_journal_append = false;
    return Status::error(StatusCode::DurableFailure, "injected journal append failure");
  }
  return journal->append(kind, payload);
}

Status EngineCore::write_boot() {
  return write_record(JournalRecordKind::BootEpoch, encode(make_boot_record(epoch, incarnation)));
}

Status EngineCore::write_policy_record(const AdmissionPolicy& value) {
  return write_record(JournalRecordKind::PolicyConfig, encode(value));
}

Status EngineCore::write_prepare(const PrepareRecord& record) {
  if (faults.drop_prepare_record) {
    faults.drop_prepare_record = false;
    return Status::ok();
  }
  return write_record(JournalRecordKind::AdmissionPrepare, encode(record));
}

Status EngineCore::write_commit(const DecisionRecord& record) {
  if (faults.drop_commit_record) {
    faults.drop_commit_record = false;
    return Status::ok();
  }
  if (faults.fail_next_commit) {
    faults.fail_next_commit = false;
    return Status::error(StatusCode::DurableFailure, "injected commit append failure");
  }
  ByteBuffer payload = encode(record);
  if (faults.corrupt_next_commit) {
    faults.corrupt_next_commit = false;
    if (!payload.empty()) {
      const std::size_t index = payload.size() / 2;
      payload[index] = static_cast<std::byte>(static_cast<std::uint8_t>(payload[index]) ^ 0xFFu);
    }
  }
  return write_record(JournalRecordKind::AdmissionCommit, payload);
}

Status EngineCore::write_refusal(const DecisionRecord& record) {
  return write_record(JournalRecordKind::AdmissionRefused, encode(record));
}

Status EngineCore::write_release(const ReleaseRecord& record) {
  return write_record(JournalRecordKind::LedgerRelease, encode(record));
}

Status EngineCore::write_revocation(const RevocationRecord& record) {
  return write_record(JournalRecordKind::Revocation, encode(record));
}

Status EngineCore::write_audit_mark() {
  AuditMarkRecord record;
  record.sequence = audit;
  record.epoch = epoch;
  record.frames = journal != nullptr ? journal->frame_count() : 0;
  return write_record(JournalRecordKind::AuditMark, encode(record));
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

FabricStatus EngineCore::status_locked() const {
  FabricStatus status;
  status.readiness = readiness();
  status.epoch = epoch;
  status.incarnation = incarnation;
  status.tick = tick;
  status.has_policy = policy.has_value();
  status.has_capacity = capacity.has_value();
  status.has_reservations = reservations.has_value();
  status.has_paths = paths.has_value();
  status.has_qos = qos.has_value();
  status.has_priority = priorities.has_value();
  if (policy.has_value()) status.policy_generation = policy->generation;
  if (capacity.has_value()) {
    status.capacity_generation = capacity->generation;
    status.capacity_snapshot = capacity->snapshot;
  }
  if (reservations.has_value()) {
    status.reservation_generation = reservations->generation;
    status.reservation_snapshot = reservations->snapshot;
  }
  if (paths.has_value()) status.path_generation = paths->path_authority_generation;
  if (qos.has_value()) status.qos_generation = qos->generation;
  if (priorities.has_value()) status.priority_generation = priorities->generation;
  status.audit_sequence = audit;
  status.decisions = counters.decisions;
  status.admissions = counters.admissions;
  status.refusals = counters.refusals;
  status.deferrals = counters.deferrals;
  status.idempotent_replays = counters.idempotent_replays;
  status.conflicts = counters.conflicts;
  status.fences = counters.fences;
  status.stale_refusals = counters.stale_refusals;
  status.ledger_entries = ledger.entry_count();
  status.live_decisions = ledger.live_decisions();
  status.live_sessions = sessions.size();
  status.durable = journal != nullptr && journal_ready;
  status.reconciled = !pending_reconciliation;
  SumLatch latch;
  for (const auto& entry : ledger.entries()) latch.add(entry.amount.value());
  status.total_admitted = Rate::from_value(latch.saturated_total());
  status.infeasible_resources = reconciliation.infeasible_resources;
  if (capacity.has_value()) {
    for (const auto& entry : capacity->resources) {
      if (entry.evidence != EvidenceState::Known) continue;
      if (obligations_for(entry.resource).saturating_add(headroom_for(entry)) > entry.usable) {
        ++status.infeasible_resources;
      }
    }
  }
  return status;
}

}  // namespace naf::detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace naf {

std::string_view to_string(EngineReadiness readiness) noexcept {
  switch (readiness) {
    case EngineReadiness::AwaitingPolicy: return "awaiting_policy";
    case EngineReadiness::AwaitingCapacity: return "awaiting_capacity";
    case EngineReadiness::AwaitingReservations: return "awaiting_reservations";
    case EngineReadiness::AwaitingCatalogs: return "awaiting_catalogs";
    case EngineReadiness::AwaitingPaths: return "awaiting_paths";
    case EngineReadiness::WaitingForReconciliation: return "waiting_for_reconciliation";
    case EngineReadiness::Ready: return "ready";
  }
  return "unknown";
}

std::string_view to_string(EnforcementTrigger trigger) noexcept {
  switch (trigger) {
    case EnforcementTrigger::None: return "none";
    case EnforcementTrigger::CapacityPublication: return "capacity_publication";
    case EnforcementTrigger::ReservationPublication: return "reservation_publication";
    case EnforcementTrigger::PolicyPublication: return "policy_publication";
    case EnforcementTrigger::RestartRecovery: return "restart_recovery";
  }
  return "unknown";
}

std::string_view to_string(RecoveryOutcome outcome) noexcept {
  switch (outcome) {
    case RecoveryOutcome::FreshStart: return "fresh_start";
    case RecoveryOutcome::Recovered: return "recovered";
    case RecoveryOutcome::RecoveredWithAmbiguity: return "recovered_with_ambiguity";
    case RecoveryOutcome::RepairedTornTail: return "repaired_torn_tail";
    case RecoveryOutcome::Refused: return "refused";
  }
  return "unknown";
}

std::string RecoveryReport::render() const {
  std::string out;
  out.reserve(256);
  out += "recovery=";
  out += to_string(outcome);
  out += " frames=";
  out += std::to_string(frames_scanned);
  out += " replayed=";
  out += std::to_string(frames_replayed);
  out += " epoch=";
  out += std::to_string(recovered_epoch.value());
  out += " audit=";
  out += std::to_string(audit_sequence.value());
  out += " live_ledger=";
  out += std::to_string(live_ledger.size());
  out += " history=";
  out += std::to_string(history.size());
  out += " ambiguous=";
  out += std::to_string(ambiguous.size());
  out += " discarded_bytes=";
  out += std::to_string(trailing_bytes_discarded);
  if (!status.is_ok()) {
    out += " status=";
    out += status.to_string();
  }
  return out;
}

AdmissionEngine::AdmissionEngine(EngineConfig config) : core_(std::make_unique<detail::EngineCore>()) {
  core_->config = config;
  core_->epoch = config.epoch;
  core_->incarnation = config.incarnation;
  core_->tick = config.initial_tick;
  core_->history = DecisionHistory(config.history_capacity);
  core_->attempts = AttemptRegistry(config.attempt_capacity);
}

AdmissionEngine::~AdmissionEngine() = default;

Status AdmissionEngine::publish_policy(AdmissionPolicy policy) {
  Status valid = validate(policy);
  if (!valid.is_ok()) return valid;
  std::lock_guard<std::mutex> guard(core_->mutex_);
  if (core_->epoch.is_none()) {
    return Status::error(StatusCode::EpochMismatch, "engine has no fabric epoch");
  }
  if (policy.epoch != core_->epoch) {
    return Status::error(StatusCode::EpochMismatch, "policy epoch does not match the fabric epoch");
  }
  if (core_->policy.has_value()) {
    if (policy.generation < core_->policy->generation) {
      return Status::error(StatusCode::StaleGeneration, "policy generation would move backwards");
    }
    if (policy.generation == core_->policy->generation) {
      if (detail::content_equal(policy, *core_->policy)) return Status::ok();
      return Status::error(StatusCode::ConflictIdentity,
                           "policy generation republished with different content");
    }
  }
  Status written = core_->write_policy_record(policy);
  if (!written.is_ok()) return written;
  core_->policy = std::move(policy);
  core_->tick = std::max(core_->tick, core_->policy->observed_tick);
  return core_->enforce_accounting_locked(EnforcementTrigger::PolicyPublication);
}

Status AdmissionEngine::publish_capacity(const CapacitySnapshot& snapshot) {
  Status valid = validate(snapshot);
  if (!valid.is_ok()) return valid;
  std::lock_guard<std::mutex> guard(core_->mutex_);
  if (core_->epoch.is_none()) {
    return Status::error(StatusCode::EpochMismatch, "engine has no fabric epoch");
  }
  if (snapshot.epoch != core_->epoch) {
    // A capacity observation produced under a different fencing epoch is not
    // authority for this incarnation, even if its generation is higher.
    return Status::error(StatusCode::EpochMismatch,
                         "capacity epoch does not match the fabric epoch");
  }
  if (core_->capacity.has_value()) {
    if (snapshot.generation < core_->capacity->generation) {
      return Status::error(StatusCode::StaleGeneration, "capacity generation would move backwards");
    }
    if (snapshot.generation == core_->capacity->generation) {
      if (detail::content_equal(snapshot, *core_->capacity)) return Status::ok();
      return Status::error(StatusCode::ConflictIdentity,
                           "capacity generation republished with different content");
    }
  }
  core_->capacity = snapshot;
  core_->tick = std::max(core_->tick, snapshot.observed_tick);
  return core_->enforce_accounting_locked(EnforcementTrigger::CapacityPublication);
}

Status AdmissionEngine::publish_reservations(const ReservationSnapshot& snapshot) {
  Status valid = validate(snapshot);
  if (!valid.is_ok()) return valid;
  std::lock_guard<std::mutex> guard(core_->mutex_);
  if (core_->epoch.is_none()) {
    return Status::error(StatusCode::EpochMismatch, "engine has no fabric epoch");
  }
  if (core_->reservations.has_value()) {
    if (snapshot.generation < core_->reservations->generation) {
      return Status::error(StatusCode::StaleGeneration, "reservation generation would move backwards");
    }
    if (snapshot.generation == core_->reservations->generation) {
      if (detail::content_equal(snapshot, *core_->reservations)) return Status::ok();
      return Status::error(StatusCode::ConflictIdentity,
                           "reservation generation republished with different content");
    }
  }
  core_->reservations = snapshot;
  core_->rebuild_obligation_index();
  core_->tick = std::max(core_->tick, snapshot.observed_tick);
  return core_->enforce_accounting_locked(EnforcementTrigger::ReservationPublication);
}

Status AdmissionEngine::publish_paths(const PathCatalog& catalog) {
  Status valid = validate(catalog);
  if (!valid.is_ok()) return valid;
  std::lock_guard<std::mutex> guard(core_->mutex_);
  if (core_->epoch.is_none()) {
    return Status::error(StatusCode::EpochMismatch, "engine has no fabric epoch");
  }
  if (core_->paths.has_value()) {
    if (catalog.path_authority_generation < core_->paths->path_authority_generation) {
      return Status::error(StatusCode::StaleGeneration,
                           "path authority generation would move backwards");
    }
    if (catalog.path_authority_generation == core_->paths->path_authority_generation) {
      if (detail::content_equal(catalog, *core_->paths)) return Status::ok();
      return Status::error(StatusCode::ConflictIdentity,
                           "path authority generation republished with different content");
    }
  }
  core_->paths = catalog;
  core_->tick = std::max(core_->tick, catalog.observed_tick);
  return Status::ok();
}

Status AdmissionEngine::publish_qos_catalog(const QoSClassCatalog& catalog) {
  Status valid = validate(catalog);
  if (!valid.is_ok()) return valid;
  std::lock_guard<std::mutex> guard(core_->mutex_);
  if (core_->epoch.is_none()) {
    return Status::error(StatusCode::EpochMismatch, "engine has no fabric epoch");
  }
  if (core_->qos.has_value()) {
    if (catalog.generation < core_->qos->generation) {
      return Status::error(StatusCode::StaleGeneration, "qos catalog generation would move backwards");
    }
    if (catalog.generation == core_->qos->generation) {
      if (detail::content_equal(catalog, *core_->qos)) return Status::ok();
      return Status::error(StatusCode::ConflictIdentity,
                           "qos catalog generation republished with different content");
    }
  }
  core_->qos = catalog;
  core_->tick = std::max(core_->tick, catalog.observed_tick);
  return Status::ok();
}

Status AdmissionEngine::publish_priority_catalog(const PriorityClassCatalog& catalog) {
  Status valid = validate(catalog);
  if (!valid.is_ok()) return valid;
  std::lock_guard<std::mutex> guard(core_->mutex_);
  if (core_->epoch.is_none()) {
    return Status::error(StatusCode::EpochMismatch, "engine has no fabric epoch");
  }
  if (core_->priorities.has_value()) {
    if (catalog.generation < core_->priorities->generation) {
      return Status::error(StatusCode::StaleGeneration,
                           "priority catalog generation would move backwards");
    }
    if (catalog.generation == core_->priorities->generation) {
      if (detail::content_equal(catalog, *core_->priorities)) return Status::ok();
      return Status::error(StatusCode::ConflictIdentity,
                           "priority catalog generation republished with different content");
    }
  }
  core_->priorities = catalog;
  core_->tick = std::max(core_->tick, catalog.observed_tick);
  return Status::ok();
}

Status AdmissionEngine::invalidate_capacity() {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  core_->capacity.reset();
  return Status::ok();
}

Status AdmissionEngine::invalidate_paths() {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  core_->paths.reset();
  return Status::ok();
}

// ---------------------------------------------------------------------------
// Durability
// ---------------------------------------------------------------------------

Status AdmissionEngine::attach_journal(std::unique_ptr<Journal> journal) {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  if (core_->journal_ready) {
    return Status::error(StatusCode::AlreadyExists, "a journal is already attached");
  }
  core_->journal = std::move(journal);
  return Status::ok();
}

Expected<RecoveryReport> AdmissionEngine::open_durable() {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  RecoveryReport report;
  if (core_->journal == nullptr) {
    report.status = Status::error(StatusCode::InvalidArgument, "no journal attached");
    report.outcome = RecoveryOutcome::Refused;
    return report;
  }
  Status opened = core_->journal->open();
  if (!opened.is_ok()) {
    report.status = opened;
    report.outcome = RecoveryOutcome::Refused;
    report.corrupt = opened.code() == StatusCode::CorruptJournal;
    return report;
  }
  core_->journal_ready = true;
  report.journal_present = !core_->journal->scan().created;
  report.frames_scanned = core_->journal->scan().frames;
  report.trailing_bytes_discarded = core_->journal->scan().truncated_tail_bytes;

  std::vector<JournalFrame> frames;
  Status replayed =
      core_->journal->replay(frames, limits::max_journal_records, core_->journal->options().max_file_bytes);
  if (!replayed.is_ok()) {
    report.status = replayed;
    report.outcome = RecoveryOutcome::Refused;
    report.corrupt = true;
    return report;
  }

  struct PendingPrepare {
    PrepareRecord record{};
    std::uint64_t sequence = 0;
  };
  std::map<std::uint64_t, PendingPrepare> pending;
  std::map<std::uint64_t, PrepareRecord> committed_prepares;
  std::vector<AdmissionDecisionId> released;
  FabricEpoch recovered_epoch = FabricEpoch::none();
  std::uint64_t max_decision_sequence = 0;
  AuditSequence recovered_audit{};

  for (const auto& frame : frames) {
    switch (frame.kind) {
      case JournalRecordKind::BootEpoch: {
        auto decoded = decode_boot(frame.payload);
        if (!decoded) {
          report.status = decoded.status();
          report.outcome = RecoveryOutcome::Refused;
          report.corrupt = true;
          return report;
        }
        if (decoded.value().format_revision != format_revision) {
          report.status = Status::error(StatusCode::Unsupported,
                                        "journal was written by an incompatible format revision");
          report.outcome = RecoveryOutcome::Refused;
          return report;
        }
        if (decoded.value().product != product_name) {
          report.status = Status::error(StatusCode::Unsupported, "journal belongs to a different product");
          report.outcome = RecoveryOutcome::Refused;
          return report;
        }
        if (decoded.value().epoch > recovered_epoch) recovered_epoch = decoded.value().epoch;
        break;
      }
      case JournalRecordKind::PolicyConfig: {
        auto decoded = decode_policy(frame.payload);
        if (!decoded) {
          report.status = decoded.status();
          report.outcome = RecoveryOutcome::Refused;
          report.corrupt = true;
          return report;
        }
        Status valid = validate(decoded.value());
        if (!valid.is_ok()) {
          report.status = valid;
          report.outcome = RecoveryOutcome::Refused;
          return report;
        }
        core_->policy = decoded.value();
        break;
      }
      case JournalRecordKind::AdmissionPrepare: {
        auto decoded = decode_prepare(frame.payload);
        if (!decoded) {
          report.status = decoded.status();
          report.outcome = RecoveryOutcome::Refused;
          report.corrupt = true;
          return report;
        }
        PendingPrepare entry;
        entry.record = decoded.value();
        entry.sequence = frame.sequence;
        pending[entry.record.decision.value()] = entry;
        break;
      }
      case JournalRecordKind::AdmissionCommit: {
        auto decoded = decode_decision_record(frame.payload);
        if (!decoded) {
          report.status = decoded.status();
          report.outcome = RecoveryOutcome::Refused;
          report.corrupt = true;
          return report;
        }
        DecisionRecord record = decoded.value();
        const auto prepared = pending.find(record.decision.value());
        if (is_admitting(record.outcome) && prepared == pending.end()) {
          // A committed admission with no durable intent cannot be trusted.
          report.status = Status::error(StatusCode::CorruptJournal,
                                        "commit without a matching prepare record");
          report.outcome = RecoveryOutcome::Refused;
          report.corrupt = true;
          return report;
        }
        if (prepared != pending.end()) {
          committed_prepares[record.decision.value()] = prepared->second.record;
          pending.erase(prepared);
        }
        record.restored_from_disk = true;
        record.ledger_live = is_admitting(record.outcome) && record.state != RevalidationState::Revoked;
        core_->history.insert(record);
        if (record.decision.value() >= max_decision_sequence) {
          max_decision_sequence = record.decision.value();
        }
        if (record.audit > recovered_audit) recovered_audit = record.audit;
        AttemptRecord attempt;
        attempt.request = record.request;
        attempt.demand = record.demand;
        attempt.demand_generation = record.demand_generation;
        attempt.attempt = record.attempt;
        attempt.fingerprint = record.fingerprint;
        attempt.decision = record.decision;
        attempt.outcome = record.outcome;
        attempt.recorded_tick = record.decided_tick;
        if (record.attempt.is_known() && record.fingerprint != 0) core_->attempts.record(attempt);
        break;
      }
      case JournalRecordKind::AdmissionRefused: {
        auto decoded = decode_decision_record(frame.payload);
        if (!decoded) {
          report.status = decoded.status();
          report.outcome = RecoveryOutcome::Refused;
          report.corrupt = true;
          return report;
        }
        DecisionRecord record = decoded.value();
        record.restored_from_disk = true;
        record.ledger_live = false;
        core_->history.insert(record);
        if (record.decision.value() >= max_decision_sequence) {
          max_decision_sequence = record.decision.value();
        }
        if (record.audit > recovered_audit) recovered_audit = record.audit;
        AttemptRecord attempt;
        attempt.request = record.request;
        attempt.demand = record.demand;
        attempt.demand_generation = record.demand_generation;
        attempt.attempt = record.attempt;
        attempt.fingerprint = record.fingerprint;
        attempt.decision = record.decision;
        attempt.outcome = record.outcome;
        attempt.recorded_tick = record.decided_tick;
        if (record.attempt.is_known() && record.fingerprint != 0) core_->attempts.record(attempt);
        break;
      }
      case JournalRecordKind::LedgerRelease: {
        auto decoded = decode_release(frame.payload);
        if (!decoded) {
          report.status = decoded.status();
          report.outcome = RecoveryOutcome::Refused;
          report.corrupt = true;
          return report;
        }
        released.push_back(decoded.value().decision);
        break;
      }
      case JournalRecordKind::Revocation: {
        auto decoded = decode_revocation(frame.payload);
        if (!decoded) {
          report.status = decoded.status();
          report.outcome = RecoveryOutcome::Refused;
          report.corrupt = true;
          return report;
        }
        const DecisionRecord* existing = core_->history.find(decoded.value().decision);
        if (existing != nullptr) {
          DecisionRecord updated = *existing;
          updated.state = decoded.value().state;
          updated.reason = decoded.value().reason;
          updated.ledger_live = false;
          core_->history.update(decoded.value().decision, updated);
        }
        if (decoded.value().audit > recovered_audit) recovered_audit = decoded.value().audit;
        break;
      }
      case JournalRecordKind::AuditMark: {
        auto decoded = decode_audit_mark(frame.payload);
        if (!decoded) {
          report.status = decoded.status();
          report.outcome = RecoveryOutcome::Refused;
          report.corrupt = true;
          return report;
        }
        if (decoded.value().sequence > recovered_audit) recovered_audit = decoded.value().sequence;
        break;
      }
      case JournalRecordKind::CoordinatorConfig:
      case JournalRecordKind::CompactionMarker:
      case JournalRecordKind::Reserved:
        break;
    }
    ++report.frames_replayed;
  }

  // Ambiguous: an intent whose commit never appeared. The load is NOT restored.
  for (const auto& [key, entry] : pending) {
    (void)key;
    AmbiguousAttempt ambiguous;
    ambiguous.prepare = entry.record;
    ambiguous.prepare_sequence = entry.sequence;
    report.ambiguous.push_back(ambiguous);
  }

  // Live ledger: committed admissions minus explicit releases.
  for (const auto& [key, prepare] : committed_prepares) {
    (void)key;
    const DecisionRecord* record = core_->history.find(prepare.decision);
    if (record == nullptr || !is_admitting(record->outcome)) continue;
    if (record->state == RevalidationState::Revoked || record->state == RevalidationState::Expired) continue;
    if (std::find(released.begin(), released.end(), prepare.decision) != released.end()) continue;
    Status reserved = core_->ledger.reserve(prepare.decision, prepare.demand, prepare.epoch,
                                            prepare.bound_capacity_generation, prepare.path,
                                            prepare.amounts);
    if (!reserved.is_ok()) {
      report.status = reserved;
      report.outcome = RecoveryOutcome::Refused;
      return report;
    }
  }
  core_->ledger.mark_all_requiring_revalidation();
  core_->trim_history();
  report.live_ledger = core_->ledger.entries();
  report.history = core_->history.in_audit_order();
  report.audit_sequence = recovered_audit;

  // Fencing. The journal is the record of past incarnations: a coordinator may
  // not open a journal that already belongs to a later incarnation than the one
  // it was configured for, and it always advances the epoch when it opens one.
  if (!recovered_epoch.is_none() && !core_->config.epoch.is_none() &&
      recovered_epoch > core_->config.epoch) {
    report.status = Status::error(StatusCode::EpochMismatch,
                                  "journal epoch is ahead of the configured coordinator epoch");
    report.outcome = RecoveryOutcome::Refused;
    return report;
  }
  FabricEpoch next;
  if (!recovered_epoch.is_none()) {
    next = FabricEpoch::from_value(recovered_epoch.value() + 1);
    if (!next.is_well_formed()) {
      report.status = Status::error(StatusCode::ArithmeticOverflow, "fabric epoch is exhausted");
      report.outcome = RecoveryOutcome::Refused;
      return report;
    }
  } else if (!core_->config.epoch.is_none()) {
    next = core_->config.epoch;
  } else {
    next = FabricEpoch::first();
  }
  core_->epoch = next;
  report.recovered_epoch = next;
  report.epoch_advanced = !recovered_epoch.is_none();
  core_->audit = recovered_audit;
  core_->next_decision_sequence = max_decision_sequence + 1;
  report.next_decision_sequence = core_->next_decision_sequence;
  core_->sessions.clear();
  core_->next_session_nonce = 1;

  Status booted = core_->write_boot();
  if (!booted.is_ok()) {
    report.status = booted;
    report.outcome = RecoveryOutcome::Refused;
    return report;
  }
  core_->pending_reconciliation = core_->ledger.live_decisions() != 0;
  report.awaiting_authority_republish = true;
  report.trigger = EnforcementTrigger::RestartRecovery;

  if (report.ambiguous.empty() && report.trailing_bytes_discarded == 0 && report.journal_present) {
    report.outcome = RecoveryOutcome::Recovered;
  } else if (!report.ambiguous.empty()) {
    report.outcome = RecoveryOutcome::RecoveredWithAmbiguity;
  } else if (report.trailing_bytes_discarded != 0) {
    report.outcome = RecoveryOutcome::RepairedTornTail;
  } else {
    report.outcome = RecoveryOutcome::FreshStart;
  }
  report.status = Status::ok();
  return report;
}

Status AdmissionEngine::compact_journal() {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->compact_locked();
}

}  // namespace naf

namespace naf::detail {

Status EngineCore::compact_locked() {
  if (journal == nullptr || !journal_ready) {
    return Status::error(StatusCode::InvalidArgument, "no journal attached");
  }
  // Only state that must survive a restart is rewritten: the fencing epoch, the
  // owned policy, the bounded decision history, and the intent plus commit for
  // every grant that is still live. Superseded frames are retired, so durable
  // growth is bounded by the history capacity rather than by uptime.
  std::vector<JournalFrame> frames;
  JournalFrame boot;
  boot.kind = JournalRecordKind::BootEpoch;
  boot.payload = encode(make_boot_record(epoch, incarnation));
  frames.push_back(std::move(boot));

  if (policy.has_value()) {
    JournalFrame frame;
    frame.kind = JournalRecordKind::PolicyConfig;
    frame.payload = encode(*policy);
    frames.push_back(std::move(frame));
  }

  std::map<std::uint64_t, std::vector<std::pair<ResourceId, Rate>>> live_amounts;
  for (const auto& entry : ledger.entries()) {
    live_amounts[entry.decision.value()].emplace_back(entry.resource, entry.amount);
  }

  const std::vector<DecisionRecord> ordered = history.in_audit_order();
  for (const auto& record : ordered) {
    const auto held = live_amounts.find(record.decision.value());
    if (is_admitting(record.outcome) && held != live_amounts.end()) {
      PrepareRecord prepare;
      prepare.decision = record.decision;
      prepare.demand = record.demand;
      prepare.attempt = record.attempt;
      prepare.fingerprint = record.fingerprint;
      prepare.epoch = record.epoch;
      prepare.bound_capacity_generation = record.bound_capacity_generation;
      prepare.path = record.path;
      prepare.amounts = held->second;
      JournalFrame prepare_frame;
      prepare_frame.kind = JournalRecordKind::AdmissionPrepare;
      prepare_frame.payload = encode(prepare);
      frames.push_back(std::move(prepare_frame));

      JournalFrame commit_frame;
      commit_frame.kind = JournalRecordKind::AdmissionCommit;
      commit_frame.payload = encode(record);
      frames.push_back(std::move(commit_frame));
    } else {
      JournalFrame frame;
      frame.kind = JournalRecordKind::AdmissionRefused;
      frame.payload = encode(record);
      frames.push_back(std::move(frame));
    }
  }

  AuditMarkRecord mark;
  mark.sequence = audit;
  mark.epoch = epoch;
  mark.frames = static_cast<std::uint64_t>(frames.size());
  JournalFrame frame;
  frame.kind = JournalRecordKind::AuditMark;
  frame.payload = encode(mark);
  frames.push_back(std::move(frame));

  return journal->compact(frames);
}

}  // namespace naf::detail

namespace naf {

void AdmissionEngine::set_fault_injection(FaultInjection faults) {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  core_->faults = faults;
}

FaultInjection AdmissionEngine::fault_injection() const {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->faults;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

Expected<SessionGrant> AdmissionEngine::register_session(PublisherId publisher, BootId boot) {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  if (publisher.is_unknown() || boot.is_unknown()) {
    return Status::error(StatusCode::InvalidArgument, "publisher and boot identities are required");
  }
  if (core_->epoch.is_none()) {
    return Status::error(StatusCode::EpochMismatch, "engine has no fabric epoch");
  }
  if (core_->sessions.size() >= core_->config.session_capacity) {
    // Refuse rather than evict: evicting a live session would silently unfence a
    // claimant that is still running.
    return Status::error(StatusCode::Busy, "session capacity is exhausted");
  }
  SessionGrant grant;
  grant.session = SessionNonce::from_value(core_->next_session_nonce++);
  grant.incarnation = core_->incarnation;
  grant.epoch = core_->epoch;
  grant.publisher = publisher;
  grant.boot = boot;
  detail::SessionRecord record;
  record.nonce = grant.session;
  record.publisher = publisher;
  record.boot = boot;
  core_->sessions.emplace(grant.session.value(), record);
  return grant;
}

Status AdmissionEngine::retire_session(SessionNonce session) {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  const auto it = core_->sessions.find(session.value());
  if (it == core_->sessions.end()) {
    return Status::error(StatusCode::NotFound, "session is not live");
  }
  core_->sessions.erase(it);
  return Status::ok();
}

std::size_t AdmissionEngine::live_sessions() const {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->sessions.size();
}

Status AdmissionEngine::begin_new_incarnation(CoordinatorIncarnation incarnation) {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  FabricEpoch next = FabricEpoch::from_value(core_->epoch.value() + 1);
  if (!next.is_well_formed()) {
    return Status::error(StatusCode::ArithmeticOverflow, "fabric epoch is exhausted");
  }
  core_->epoch = next;
  core_->incarnation = incarnation;
  // Sessions are never persisted and are dropped here: every claimant that was
  // live under the previous epoch is fenced by construction.
  core_->sessions.clear();
  core_->next_session_nonce = 1;
  return core_->write_boot();
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

void AdmissionEngine::advance_tick(std::uint64_t ticks) {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  core_->tick = sat_add(core_->tick, ticks);
}

std::uint64_t AdmissionEngine::tick() const {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->tick;
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

FabricStatus AdmissionEngine::status() const {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->status_locked();
}

Rate AdmissionEngine::admitted_load(ResourceId resource) const {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->ledger.admitted(resource);
}

std::optional<AdmissionPolicy> AdmissionEngine::policy() const {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->policy;
}

std::optional<DecisionRecord> AdmissionEngine::decision_record(AdmissionDecisionId decision) const {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  const DecisionRecord* record = core_->history.find(decision);
  if (record == nullptr) return std::nullopt;
  return *record;
}

std::vector<DecisionRecord> AdmissionEngine::decision_history() const {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->history.in_audit_order();
}

std::vector<LedgerEntry> AdmissionEngine::ledger_entries() const {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->ledger.entries();
}

FabricEpoch AdmissionEngine::epoch() const {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->epoch;
}

CoordinatorIncarnation AdmissionEngine::incarnation() const {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->incarnation;
}

ReconciliationReport AdmissionEngine::last_reconciliation() const {
  std::lock_guard<std::mutex> guard(core_->mutex_);
  return core_->reconciliation;
}

}  // namespace naf

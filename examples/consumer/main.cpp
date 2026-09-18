// Network Admission Fabric - downstream consumer example.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A minimal, independent user of the installed package: it publishes an
// authoritative capacity observation, asks for an admission decision, and checks
// the accounting closure invariant on the answer.
#include <cstdio>
#include <string>

#include "naf/naf.hpp"

namespace {

naf::AdmissionRequest build_request(const naf::FabricStatus& fabric, std::uint64_t sequence,
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
  request.expected.capacity_snapshot = fabric.capacity_snapshot;
  request.expected.capacity_generation = fabric.capacity_generation;
  request.expected.reservation_snapshot = fabric.reservation_snapshot;
  request.expected.reservation_generation = fabric.reservation_generation;
  request.expected.policy_generation = fabric.policy_generation;
  request.expected.qos_catalog_generation = fabric.qos_generation;
  request.expected.priority_catalog_generation = fabric.priority_generation;
  return request;
}

}  // namespace

int main() {
  std::printf("Network Admission Fabric %s consumer\n", std::string(naf::version_string).c_str());

  naf::EngineConfig config;
  config.epoch = naf::FabricEpoch::first();
  config.incarnation = naf::CoordinatorIncarnation::from_value(1);
  naf::AdmissionEngine engine(config);

  naf::AdmissionPolicy policy;
  policy.id = naf::PolicyId::from_value(1);
  policy.generation = naf::Generation::first();
  policy.epoch = engine.epoch();
  policy.headroom_permille = 100;
  policy.allow_degraded = true;
  policy.max_explanation_constraints = 8;
  policy.max_explanation_bytes = 1024;
  policy.max_effective_resources = 4;
  if (!engine.publish_policy(policy).is_ok()) return 1;

  naf::CapacitySnapshot capacity;
  capacity.snapshot = naf::CapacitySnapshotId::from_value(1);
  capacity.generation = naf::Generation::first();
  capacity.epoch = engine.epoch();
  naf::ResourceCapacity entry;
  entry.resource = naf::ResourceId::from_value(1);
  entry.generation = naf::Generation::first();
  entry.evidence = naf::EvidenceState::Known;
  entry.usable = naf::Rate::from_value(10'000);
  capacity.resources.push_back(entry);
  if (!engine.publish_capacity(capacity).is_ok()) return 1;

  naf::ReservationSnapshot reservations;
  reservations.snapshot = naf::ReservationSnapshotId::from_value(1);
  reservations.generation = naf::Generation::first();
  reservations.epoch = engine.epoch();
  if (!engine.publish_reservations(reservations).is_ok()) return 1;

  naf::QoSClassCatalog qos;
  qos.generation = naf::Generation::first();
  qos.epoch = engine.epoch();
  naf::QoSClassFact qos_fact;
  qos_fact.qos = naf::QoSClassId::from_value(1);
  qos_fact.generation = naf::Generation::first();
  qos_fact.maximum_rate = naf::Rate::from_value(1'000'000);
  qos_fact.maximum_latency = naf::Latency::from_value(10'000);
  qos.classes.push_back(qos_fact);
  if (!engine.publish_qos_catalog(qos).is_ok()) return 1;

  naf::PriorityClassCatalog priorities;
  priorities.generation = naf::Generation::first();
  priorities.epoch = engine.epoch();
  naf::PriorityClassFact priority;
  priority.priority = naf::PriorityClassId::from_value(1);
  priority.generation = naf::Generation::first();
  priority.rank = 1;
  priorities.classes.push_back(priority);
  if (!engine.publish_priority_catalog(priorities).is_ok()) return 1;

  const naf::FabricStatus fabric = engine.status();
  if (fabric.readiness != naf::EngineReadiness::Ready) {
    std::printf("engine is not ready: %s\n", std::string(naf::to_string(fabric.readiness)).c_str());
    return 1;
  }

  auto decision = engine.admit_local(build_request(fabric, 1, 1000, 4000, 5000));
  if (!decision.has_value()) {
    std::printf("admission returned an error: %s\n", decision.status().to_string().c_str());
    return 1;
  }
  std::printf("outcome=%s granted=%llu binding=%s\n",
              std::string(naf::to_string(decision.value().outcome)).c_str(),
              static_cast<unsigned long long>(decision.value().granted.value()),
              decision.value().explanation.render().c_str());

  if (decision.value().outcome != naf::AdmissionOutcome::Admit) return 1;
  if (decision.value().granted.value() != 5000) return 1;

  // Accounting closure: admitted load plus headroom fits inside usable capacity.
  const std::uint64_t admitted = engine.admitted_load(naf::ResourceId::from_value(1)).value();
  if (admitted != 5000) return 1;
  std::printf("closure: admitted=%llu usable=10000 headroom=1000\n",
              static_cast<unsigned long long>(admitted));

  // A stale generation must be refused, never repaired.
  naf::AdmissionRequest stale = build_request(fabric, 2, 100, 200, 300);
  stale.expected.capacity_generation = naf::Generation::from_value(99);
  auto refused = engine.admit_local(stale);
  if (!refused.has_value()) return 1;
  if (refused.value().outcome != naf::AdmissionOutcome::StaleInput) return 1;
  std::printf("stale input refused: %s\n", refused.value().explanation.render().c_str());

  std::printf("consumer ok\n");
  return 0;
}

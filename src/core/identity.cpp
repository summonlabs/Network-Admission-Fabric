// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/core/identity.hpp"

#include <string>

#include "core/text.hpp"

namespace naf {
namespace {

}  // namespace

std::string_view to_string(OriginKind kind) noexcept {
  switch (kind) {
    case OriginKind::InProcess: return "in_process";
    case OriginKind::Claimant: return "claimant";
    case OriginKind::Operator: return "operator";
    case OriginKind::Recovery: return "recovery";
    case OriginKind::Replay: return "replay";
  }
  return "unknown";
}

std::string render_provenance(const Provenance& provenance) {
  std::string out;
  out.reserve(96);
  out += "origin=";
  out += to_string(provenance.origin);
  out += " publisher=";
  naf::text::append_u64(out, provenance.publisher.value());
  out += " boot=";
  naf::text::append_u64(out, provenance.boot.value());
  out += " incarnation=";
  naf::text::append_u64(out, provenance.incarnation.value());
  out += " epoch=";
  naf::text::append_u64(out, provenance.epoch.value());
  out += " attempt=";
  naf::text::append_u64(out, provenance.attempt.value());
  out += " seq=";
  naf::text::append_u64(out, provenance.sequence);
  return out;
}

}  // namespace naf

// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/core/status.hpp"

namespace naf {

std::string_view to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok: return "ok";
    case StatusCode::InvalidArgument: return "invalid_argument";
    case StatusCode::MalformedInput: return "malformed_input";
    case StatusCode::OversizedInput: return "oversized_input";
    case StatusCode::UnknownResource: return "unknown_resource";
    case StatusCode::UnknownPath: return "unknown_path";
    case StatusCode::DuplicateIdentity: return "duplicate_identity";
    case StatusCode::ConflictIdentity: return "conflict_identity";
    case StatusCode::StaleGeneration: return "stale_generation";
    case StatusCode::EpochMismatch: return "epoch_mismatch";
    case StatusCode::Fenced: return "fenced";
    case StatusCode::NotFound: return "not_found";
    case StatusCode::AlreadyExists: return "already_exists";
    case StatusCode::ArithmeticOverflow: return "arithmetic_overflow";
    case StatusCode::ArithmeticUnderflow: return "arithmetic_underflow";
    case StatusCode::CapacityViolation: return "capacity_violation";
    case StatusCode::AuthorityViolation: return "authority_violation";
    case StatusCode::DurableFailure: return "durable_failure";
    case StatusCode::CorruptJournal: return "corrupt_journal";
    case StatusCode::TruncatedFrame: return "truncated_frame";
    case StatusCode::OversizedFrame: return "oversized_frame";
    case StatusCode::ProtocolViolation: return "protocol_violation";
    case StatusCode::TransportFailure: return "transport_failure";
    case StatusCode::Unsupported: return "unsupported";
    case StatusCode::ShuttingDown: return "shutting_down";
    case StatusCode::Busy: return "busy";
    case StatusCode::Internal: return "internal";
  }
  return "unknown";
}

std::string Status::to_string() const {
  std::string out(naf::to_string(code_));
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

}  // namespace naf

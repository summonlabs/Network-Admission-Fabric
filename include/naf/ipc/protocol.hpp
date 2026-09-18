// Network Admission Fabric - wire protocol payload codecs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every decoder is bounds-checked and rejects trailing bytes, malformed
// enumerations, oversized strings and oversized collections. A payload that
// fails to decode is never partially applied.
#ifndef NAF_IPC_PROTOCOL_HPP
#define NAF_IPC_PROTOCOL_HPP

#include <cstdint>
#include <string>

#include "naf/core/bytes.hpp"
#include "naf/core/identity.hpp"
#include "naf/core/status.hpp"
#include "naf/engine/decision.hpp"
#include "naf/engine/engine.hpp"
#include "naf/model/request.hpp"

namespace naf::ipc {

inline constexpr std::uint16_t protocol_version = format_revision;

struct HelloMessage {
  std::uint16_t protocol_version = 0;
  PublisherId publisher{};
  BootId boot{};
  std::uint32_t max_frame = limits::max_frame_bytes;
  std::uint64_t client_nonce = 0;
};

struct WelcomeMessage {
  bool accepted = false;
  FabricEpoch epoch{};
  CoordinatorIncarnation incarnation{};
  SessionNonce session{};
  std::string reason{};
};

struct AdmitMessage {
  ClaimContext claim{};
  AdmissionRequest request{};
};

struct RevalidateMessage {
  ClaimContext claim{};
  AdmissionDecisionId decision{};
};

struct RevokeMessage {
  ClaimContext claim{};
  AdmissionDecisionId decision{};
  RevocationReason reason = RevocationReason::OperatorRequest;
};

struct ErrorMessage {
  std::uint16_t code = 0;
  std::string text{};
};

[[nodiscard]] ByteBuffer encode(const HelloMessage& message);
[[nodiscard]] Expected<HelloMessage> decode_hello(std::span<const std::byte> bytes);
[[nodiscard]] ByteBuffer encode(const WelcomeMessage& message);
[[nodiscard]] Expected<WelcomeMessage> decode_welcome(std::span<const std::byte> bytes);
[[nodiscard]] ByteBuffer encode(const AdmitMessage& message);
[[nodiscard]] Expected<AdmitMessage> decode_admit(std::span<const std::byte> bytes);
[[nodiscard]] ByteBuffer encode(const RevalidateMessage& message);
[[nodiscard]] Expected<RevalidateMessage> decode_revalidate(std::span<const std::byte> bytes);
[[nodiscard]] ByteBuffer encode(const RevokeMessage& message);
[[nodiscard]] Expected<RevokeMessage> decode_revoke(std::span<const std::byte> bytes);
[[nodiscard]] ByteBuffer encode(const ErrorMessage& message);
[[nodiscard]] Expected<ErrorMessage> decode_error(std::span<const std::byte> bytes);

[[nodiscard]] ByteBuffer encode(const AdmissionDecision& decision);
[[nodiscard]] Expected<AdmissionDecision> decode_decision(std::span<const std::byte> bytes);
[[nodiscard]] ByteBuffer encode(const RevalidationResult& result);
[[nodiscard]] Expected<RevalidationResult> decode_revalidation(std::span<const std::byte> bytes);
[[nodiscard]] ByteBuffer encode(const FabricStatus& status);
[[nodiscard]] Expected<FabricStatus> decode_status(std::span<const std::byte> bytes);

}  // namespace naf::ipc

#endif  // NAF_IPC_PROTOCOL_HPP

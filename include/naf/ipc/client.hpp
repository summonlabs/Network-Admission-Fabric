// Network Admission Fabric - claimant-side client.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_IPC_CLIENT_HPP
#define NAF_IPC_CLIENT_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "naf/core/status.hpp"
#include "naf/engine/engine.hpp"
#include "naf/ipc/frame.hpp"
#include "naf/ipc/protocol.hpp"
#include "naf/ipc/stream.hpp"

namespace naf::ipc {

struct ClientOptions {
  std::string endpoint{};
  PublisherId publisher{};
  BootId boot{};
  std::uint32_t max_frame = limits::max_frame_bytes;
  std::uint32_t connect_timeout_ms = 5000;
  OriginKind origin = OriginKind::Claimant;
};

/// A claimant process's connection to a coordinator.
///
/// The client is synchronous: one outstanding request at a time. The claim it
/// attaches is derived from the session the coordinator granted, unless an
/// explicit override is installed. The override exists so that fencing can be
/// diagnosed and tested from a real second process; it grants no authority.
class Client {
 public:
  explicit Client(ClientOptions options);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  Status connect();
  /// Performs the handshake and captures the grant. Must be called after connect.
  Expected<WelcomeMessage> hello();
  /// Connects and handshakes in one call.
  Expected<WelcomeMessage> open();

  Expected<AdmissionDecision> admit(const AdmissionRequest& request);
  Expected<RevalidationResult> revalidate(AdmissionDecisionId decision);
  Expected<RevalidationResult> revoke(AdmissionDecisionId decision, RevocationReason reason);
  Expected<FabricStatus> status();

  Status close();

  /// Installs an explicit claim, overriding the granted session. Diagnostic use.
  void set_claim_override(const ClaimContext& claim);
  void clear_claim_override();
  void set_origin(OriginKind origin) noexcept { options_.origin = origin; }

  [[nodiscard]] const WelcomeMessage& welcome() const noexcept { return welcome_; }
  [[nodiscard]] bool is_open() const noexcept { return stream_ != nullptr && stream_->is_open(); }
  [[nodiscard]] const ClientOptions& options() const noexcept { return options_; }
  [[nodiscard]] std::string transport_name() const;

 private:
  ClaimContext build_claim() const;
  Expected<WireFrame> request(MessageKind kind, const ByteBuffer& payload);

  ClientOptions options_{};
  std::unique_ptr<Stream> stream_{};
  WelcomeMessage welcome_{};
  ClaimContext override_{};
  bool has_override_ = false;
  std::uint64_t sequence_ = 0;
};

}  // namespace naf::ipc

#endif  // NAF_IPC_CLIENT_HPP

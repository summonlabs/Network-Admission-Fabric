// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/ipc/client.hpp"

#include <utility>

#include "naf/ipc/endpoint.hpp"
#include "naf/ipc/frame.hpp"

namespace naf::ipc {

Client::Client(ClientOptions options) : options_(std::move(options)) {}

Client::~Client() { (void)close(); }

std::string Client::transport_name() const { return std::string(local_transport_name()); }

Status Client::connect() {
  if (stream_ != nullptr) return Status::ok();
  auto stream = connect_endpoint(options_.endpoint, options_.connect_timeout_ms);
  if (!stream) return stream.status();
  stream_ = std::move(stream).value();
  return Status::ok();
}

Expected<WireFrame> Client::request(MessageKind kind, const ByteBuffer& payload) {
  if (stream_ == nullptr) {
    return Status::error(StatusCode::TransportFailure, "client is not connected");
  }
  Status written = write_message(*stream_, kind, 0, payload, options_.max_frame);
  if (!written.is_ok()) return written;
  auto frame = read_message(*stream_, options_.max_frame);
  if (!frame) return frame.status();
  if (frame.value().kind == MessageKind::Error) {
    auto message = decode_error(frame.value().payload);
    if (!message) return message.status();
    return Status::error(static_cast<StatusCode>(message.value().code), message.value().text);
  }
  return frame;
}

Expected<WelcomeMessage> Client::hello() {
  HelloMessage hello;
  hello.protocol_version = protocol_version;
  hello.publisher = options_.publisher;
  hello.boot = options_.boot;
  hello.max_frame = options_.max_frame;
  hello.client_nonce = options_.boot.value();
  const ByteBuffer payload = encode(hello);
  auto frame = request(MessageKind::Hello, payload);
  if (!frame) return frame.status();
  if (frame.value().kind != MessageKind::Welcome) {
    return Status::error(StatusCode::ProtocolViolation, "expected a Welcome message");
  }
  auto welcome = decode_welcome(frame.value().payload);
  if (!welcome) return welcome.status();
  welcome_ = welcome.value();
  return welcome_;
}

Expected<WelcomeMessage> Client::open() {
  Status connected = connect();
  if (!connected.is_ok()) return connected;
  return hello();
}

ClaimContext Client::build_claim() const {
  ClaimContext claim;
  claim.origin = options_.origin;
  claim.publisher = options_.publisher;
  claim.boot = options_.boot;
  if (has_override_) {
    claim = override_;
    claim.origin = options_.origin;
    claim.publisher = options_.publisher;
    claim.boot = options_.boot;
    return claim;
  }
  claim.session = welcome_.session;
  claim.incarnation = welcome_.incarnation;
  claim.epoch = welcome_.epoch;
  return claim;
}

Expected<AdmissionDecision> Client::admit(const AdmissionRequest& value) {
  AdmitMessage message;
  message.claim = build_claim();
  message.request = value;
  message.claim.attempt = value.attempt;
  message.claim.trace = value.provenance.trace;
  message.claim.sequence = ++sequence_;
  const ByteBuffer payload = encode(message);
  auto frame = request(MessageKind::Admit, payload);
  if (!frame) return frame.status();
  if (frame.value().kind != MessageKind::Decision) {
    return Status::error(StatusCode::ProtocolViolation, "expected a Decision message");
  }
  return decode_decision(frame.value().payload);
}

Expected<RevalidationResult> Client::revalidate(AdmissionDecisionId decision) {
  RevalidateMessage message;
  message.claim = build_claim();
  message.decision = decision;
  const ByteBuffer payload = encode(message);
  auto frame = request(MessageKind::Revalidate, payload);
  if (!frame) return frame.status();
  if (frame.value().kind != MessageKind::Revalidation) {
    return Status::error(StatusCode::ProtocolViolation, "expected a Revalidation message");
  }
  return decode_revalidation(frame.value().payload);
}

Expected<RevalidationResult> Client::revoke(AdmissionDecisionId decision, RevocationReason reason) {
  RevokeMessage message;
  message.claim = build_claim();
  message.decision = decision;
  message.reason = reason;
  const ByteBuffer payload = encode(message);
  auto frame = request(MessageKind::Revoke, payload);
  if (!frame) return frame.status();
  if (frame.value().kind != MessageKind::Revalidation) {
    return Status::error(StatusCode::ProtocolViolation, "expected a Revalidation message");
  }
  return decode_revalidation(frame.value().payload);
}

Expected<FabricStatus> Client::status() {
  auto frame = request(MessageKind::StatusRequest, ByteBuffer{});
  if (!frame) return frame.status();
  if (frame.value().kind != MessageKind::StatusReply) {
    return Status::error(StatusCode::ProtocolViolation, "expected a StatusReply message");
  }
  return decode_status(frame.value().payload);
}

Status Client::close() {
  if (stream_ == nullptr) return Status::ok();
  if (stream_->is_open()) {
    (void)write_message(*stream_, MessageKind::Bye, 0, {}, options_.max_frame);
  }
  Status closed = stream_->close();
  stream_.reset();
  return closed;
}

void Client::set_claim_override(const ClaimContext& claim) {
  override_ = claim;
  has_override_ = true;
}

void Client::clear_claim_override() {
  has_override_ = false;
  override_ = ClaimContext{};
}

}  // namespace naf::ipc

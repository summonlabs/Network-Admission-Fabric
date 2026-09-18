// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/ipc/server.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "naf/ipc/frame.hpp"
#include "naf/ipc/protocol.hpp"

namespace naf::ipc {
namespace {

Status transport(std::string text) { return Status::error(StatusCode::TransportFailure, std::move(text)); }

}  // namespace

CoordinatorServer::CoordinatorServer(ServerConfig config)
    : config_(std::move(config)), engine_(config_.engine) {}

CoordinatorServer::~CoordinatorServer() {
  stop();
}

std::string CoordinatorServer::transport_name() const {
  if (config_.stdio) return "stdio";
  return std::string(local_transport_name());
}

Expected<RecoveryReport> CoordinatorServer::start() {
  if (running_.load()) {
    RecoveryReport report;
    report.status = Status::error(StatusCode::AlreadyExists, "server is already running");
    report.outcome = RecoveryOutcome::Refused;
    return report;
  }
  if (config_.engine.epoch.is_none()) {
    config_.engine.epoch = FabricEpoch::none();
  }
  if (!config_.state_directory.empty()) {
    Status created = ensure_directory(config_.state_directory);
    if (!created.is_ok()) {
      RecoveryReport report;
      report.status = created;
      report.outcome = RecoveryOutcome::Refused;
      return report;
    }
    JournalOptions options;
    options.sync_on_append = true;
    auto journal = std::make_unique<Journal>(config_.state_directory / "admission.journal", options);
    Status attached = engine_.attach_journal(std::move(journal));
    if (!attached.is_ok()) {
      RecoveryReport report;
      report.status = attached;
      report.outcome = RecoveryOutcome::Refused;
      return report;
    }
    auto recovered = engine_.open_durable();
    if (!recovered) {
      RecoveryReport report;
      report.status = recovered.status();
      report.outcome = RecoveryOutcome::Refused;
      return report;
    }
    RecoveryReport report = recovered.value();
    if (!report.status.is_ok()) return report;
    return bind_transport(std::move(report));
  }

  RecoveryReport report;
  report.outcome = RecoveryOutcome::FreshStart;
  report.recovered_epoch = config_.engine.epoch;
  report.awaiting_authority_republish = true;
  report.status = Status::ok();
  return bind_transport(std::move(report));
}

Expected<RecoveryReport> CoordinatorServer::bind_transport(RecoveryReport report) {
  if (config_.stdio) {
    // One duplex stream over stdin/stdout: reads come from stdin, writes go to
    // stdout, which is exactly how a parent process drives the coordinator.
    stdio_stream_ = std::make_shared<StdioStream>();
  } else {
    auto listener = create_listener(config_.endpoint, EndpointOptions{});
    if (!listener) {
      report.status = listener.status();
      report.outcome = RecoveryOutcome::Refused;
      return report;
    }
    listener_ = std::move(listener).value();
  }
  stopping_.store(false);
  running_.store(true);
  return report;
}

void CoordinatorServer::reap_finished() {
  for (auto it = workers_.begin(); it != workers_.end();) {
    if (it->done != nullptr && it->done->load()) {
      if (it->thread.joinable()) it->thread.join();
      it = workers_.erase(it);
    } else {
      ++it;
    }
  }
}

void CoordinatorServer::join_all() {
  for (auto& worker : workers_) {
    if (worker.thread.joinable()) worker.thread.join();
  }
  workers_.clear();
}

void CoordinatorServer::stop() {
  stopping_.store(true);
  if (listener_ != nullptr) listener_->close();
  if (stdio_stream_ != nullptr) stdio_stream_->close();
  std::vector<std::shared_ptr<Stream>> streams;
  {
    std::lock_guard<std::mutex> guard(workers_mutex_);
    for (const auto& worker : workers_) {
      if (worker.stream != nullptr) streams.push_back(worker.stream);
    }
  }
  for (const auto& stream : streams) stream->close();
  {
    // The workers never take this lock themselves, so joining under it cannot
    // deadlock; it only prevents a concurrent accept from adding a worker while
    // the join is in progress.
    std::lock_guard<std::mutex> guard(workers_mutex_);
    join_all();
  }
  running_.store(false);
}

Status CoordinatorServer::send_error(Stream& stream, StatusCode code, const std::string& text) {
  ErrorMessage message;
  message.code = static_cast<std::uint16_t>(code);
  message.text = text.size() > limits::max_string_bytes ? text.substr(0, limits::max_string_bytes) : text;
  const ByteBuffer payload = encode(message);
  return write_message(stream, MessageKind::Error, 0, payload, config_.max_frame);
}

Status CoordinatorServer::send_decision(Stream& stream, const AdmissionDecision& decision) {
  const ByteBuffer payload = encode(decision);
  if (payload.size() > config_.max_frame) {
    return send_error(stream, StatusCode::OversizedFrame, "decision payload exceeds the frame bound");
  }
  return write_message(stream, MessageKind::Decision, 0, payload, config_.max_frame);
}

Status CoordinatorServer::send_revalidation(Stream& stream, const RevalidationResult& result) {
  const ByteBuffer payload = encode(result);
  if (payload.size() > config_.max_frame) {
    return send_error(stream, StatusCode::OversizedFrame, "revalidation payload exceeds the frame bound");
  }
  return write_message(stream, MessageKind::Revalidation, 0, payload, config_.max_frame);
}

void CoordinatorServer::handle_connection(Stream& stream, const WelcomeMessage& welcome,
                                          bool& registered) {
  (void)welcome;
  while (!stopping_.load()) {
    auto frame = read_message(stream, config_.max_frame);
    if (!frame) {
      if (frame.status().message().empty()) break;  // orderly peer disconnect
      if (stopping_.load()) break;
      (void)send_error(stream, frame.status().code(), frame.status().message());
      break;
    }
    switch (frame.value().kind) {
      case MessageKind::Admit: {
        auto decoded = decode_admit(frame.value().payload);
        if (!decoded) {
          (void)send_error(stream, decoded.status().code(), decoded.status().message());
          break;
        }
        auto decision = engine_.admit(decoded.value().request, decoded.value().claim);
        if (!decision) {
          (void)send_error(stream, decision.status().code(), decision.status().message());
          break;
        }
        if (!send_decision(stream, decision.value()).is_ok()) return;
        break;
      }
      case MessageKind::Revalidate: {
        auto decoded = decode_revalidate(frame.value().payload);
        if (!decoded) {
          (void)send_error(stream, decoded.status().code(), decoded.status().message());
          break;
        }
        auto result = engine_.revalidate(decoded.value().decision, decoded.value().claim);
        if (!result) {
          (void)send_error(stream, result.status().code(), result.status().message());
          break;
        }
        if (!send_revalidation(stream, result.value()).is_ok()) return;
        break;
      }
      case MessageKind::Revoke: {
        auto decoded = decode_revoke(frame.value().payload);
        if (!decoded) {
          (void)send_error(stream, decoded.status().code(), decoded.status().message());
          break;
        }
        auto result =
            engine_.revoke(decoded.value().decision, decoded.value().reason, decoded.value().claim);
        if (!result) {
          (void)send_error(stream, result.status().code(), result.status().message());
          break;
        }
        if (!send_revalidation(stream, result.value()).is_ok()) return;
        break;
      }
      case MessageKind::StatusRequest: {
        if (!frame.value().payload.empty()) {
          (void)send_error(stream, StatusCode::ProtocolViolation, "status request must be empty");
          break;
        }
        const ByteBuffer payload = encode(engine_.status());
        if (!write_message(stream, MessageKind::StatusReply, 0, payload, config_.max_frame).is_ok()) {
          return;
        }
        break;
      }
      case MessageKind::Ping: {
        if (!write_message(stream, MessageKind::Pong, 0, {}, config_.max_frame).is_ok()) return;
        break;
      }
      case MessageKind::Bye:
        registered = false;
        return;
      default:
        (void)send_error(stream, StatusCode::ProtocolViolation,
                         std::string("unexpected message ") +
                             std::string(to_string(frame.value().kind)));
        return;
    }
  }
}

void CoordinatorServer::serve(std::shared_ptr<Stream> stream) {
  WelcomeMessage welcome;
  bool registered = false;
  auto frame = read_message(*stream, config_.max_frame);
  if (!frame) {
    stream->close();
    return;
  }
  if (frame.value().kind != MessageKind::Hello) {
    (void)send_error(*stream, StatusCode::ProtocolViolation, "the first message must be Hello");
    stream->close();
    return;
  }
  auto hello = decode_hello(frame.value().payload);
  if (!hello) {
    (void)send_error(*stream, hello.status().code(), hello.status().message());
    stream->close();
    return;
  }
  if (hello.value().max_frame < limits::min_frame_bytes) {
    (void)send_error(*stream, StatusCode::ProtocolViolation, "client frame bound is too small");
    stream->close();
    return;
  }
  auto grant = engine_.register_session(hello.value().publisher, hello.value().boot);
  if (!grant) {
    welcome.accepted = false;
    welcome.epoch = engine_.epoch();
    welcome.incarnation = engine_.incarnation();
    welcome.reason = grant.status().message();
    const ByteBuffer payload = encode(welcome);
    (void)write_message(*stream, MessageKind::Welcome, 0, payload, config_.max_frame);
    stream->close();
    return;
  }
  welcome.accepted = true;
  welcome.epoch = grant.value().epoch;
  welcome.incarnation = grant.value().incarnation;
  welcome.session = grant.value().session;
  welcome.reason = "session established";
  const ByteBuffer payload = encode(welcome);
  if (!write_message(*stream, MessageKind::Welcome, 0, payload, config_.max_frame).is_ok()) {
    engine_.retire_session(grant.value().session);
    stream->close();
    return;
  }
  registered = true;
  handle_connection(*stream, welcome, registered);
  if (registered) engine_.retire_session(grant.value().session);
  stream->close();
}

Status CoordinatorServer::run() {
  if (!running_.load()) {
    return Status::error(StatusCode::InvalidArgument, "server has not been started");
  }
  if (config_.stdio) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    serve(stdio_stream_);
    done->store(true);
    running_.store(false);
    return Status::ok();
  }
  if (listener_ == nullptr) {
    running_.store(false);
    return Status::error(StatusCode::InvalidArgument, "no listener is bound");
  }
  Status outcome = Status::ok();
  while (!stopping_.load()) {
    auto accepted = listener_->accept();
    if (!accepted) {
      if (stopping_.load()) break;
      outcome = accepted.status();
      break;
    }
    auto stream = std::shared_ptr<Stream>(std::move(accepted).value());
    std::lock_guard<std::mutex> guard(workers_mutex_);
    reap_finished();
    if (workers_.size() >= config_.max_connections) {
      Stream& raw = *stream;
      (void)send_error(raw, StatusCode::Busy, "server is at its connection bound");
      raw.close();
      continue;
    }
    Worker worker;
    worker.done = std::make_shared<std::atomic<bool>>(false);
    worker.stream = stream;
    worker.thread = std::thread([this, stream, done = worker.done]() {
      serve(stream);
      done->store(true);
    });
    workers_.push_back(std::move(worker));
    connections_.fetch_add(1);
  }
  stop();
  return outcome;
}

}  // namespace naf::ipc

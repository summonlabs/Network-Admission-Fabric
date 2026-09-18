// Network Admission Fabric - coordinator server.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_IPC_SERVER_HPP
#define NAF_IPC_SERVER_HPP

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "naf/core/status.hpp"
#include "naf/durable/recovery.hpp"
#include "naf/engine/engine.hpp"
#include "naf/ipc/endpoint.hpp"
#include "naf/ipc/protocol.hpp"
#include "naf/ipc/stream.hpp"

namespace naf::ipc {

struct ServerConfig {
  /// Bare endpoint name for the local transport. Ignored when stdio is true.
  std::string endpoint{};
  /// Serve one connection over stdin/stdout instead of a local endpoint.
  bool stdio = false;
  /// Directory holding the durable journal. Empty disables durability.
  std::filesystem::path state_directory{};
  EngineConfig engine{};
  std::uint32_t max_frame = limits::max_frame_bytes;
  /// Upper bound on concurrently served connections.
  std::size_t max_connections = 8;
};

/// Serves admission decisions over a real OS transport.
///
/// Shutdown contract: stop() stops accepting, unblocks every in-flight read by
/// closing the connection, then joins the workers. No lock is held while a
/// worker is joined, and a worker never takes the connection registry lock, so
/// shutdown cannot deadlock against the work it is waiting for.
class CoordinatorServer {
 public:
  explicit CoordinatorServer(ServerConfig config);
  ~CoordinatorServer();

  CoordinatorServer(const CoordinatorServer&) = delete;
  CoordinatorServer& operator=(const CoordinatorServer&) = delete;

  /// Opens durable state (advancing the fencing epoch) and binds the transport.
  Expected<RecoveryReport> start();

  /// Serves until stop() is called or the transport fails.
  Status run();

  /// Idempotent. Safe to call from another thread while run() is blocked.
  void stop();

  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  [[nodiscard]] bool stopping() const noexcept { return stopping_.load(); }
  [[nodiscard]] AdmissionEngine& engine() noexcept { return engine_; }
  [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }
  [[nodiscard]] std::uint64_t connections_served() const noexcept { return connections_.load(); }
  [[nodiscard]] std::string transport_name() const;

 private:
  struct Worker {
    std::thread thread{};
    std::shared_ptr<std::atomic<bool>> done{};
    std::shared_ptr<Stream> stream{};
  };

  Expected<RecoveryReport> bind_transport(RecoveryReport report);
  void serve(std::shared_ptr<Stream> stream);
  void handle_connection(Stream& stream, const WelcomeMessage& welcome, bool& registered);
  Status send_error(Stream& stream, StatusCode code, const std::string& text);
  Status send_decision(Stream& stream, const AdmissionDecision& decision);
  Status send_revalidation(Stream& stream, const RevalidationResult& result);
  void reap_finished();
  void join_all();

  ServerConfig config_{};
  AdmissionEngine engine_{};
  std::unique_ptr<Listener> listener_{};
  std::shared_ptr<Stream> stdio_stream_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> connections_{0};
  std::atomic<std::uint64_t> next_nonce_{1};

  std::mutex workers_mutex_{};
  std::vector<Worker> workers_{};
};

}  // namespace naf::ipc

#endif  // NAF_IPC_SERVER_HPP

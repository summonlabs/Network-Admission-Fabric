// Network Admission Fabric - real local transport endpoints.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Windows uses a named pipe; POSIX uses an AF_UNIX stream socket. This is a real
// OS transport: a claimant in another process really connects, and killing that
// process really breaks the connection.
#ifndef NAF_IPC_ENDPOINT_HPP
#define NAF_IPC_ENDPOINT_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "naf/core/status.hpp"
#include "naf/ipc/stream.hpp"

namespace naf::ipc {

struct EndpointOptions {
  std::size_t backlog = 16;
  std::uint32_t read_buffer_bytes = 64u * 1024u;
  std::uint32_t write_buffer_bytes = 64u * 1024u;
};

/// Accepts connections on a local endpoint. accept() blocks until a peer
/// connects or the listener is closed.
class Listener {
 public:
  Listener() = default;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  virtual ~Listener() = default;

  virtual Expected<std::unique_ptr<Stream>> accept() = 0;
  virtual Status close() = 0;
  [[nodiscard]] virtual bool is_open() const = 0;
  [[nodiscard]] virtual const std::string& endpoint() const noexcept = 0;
};

/// Creates a listener for a bare endpoint name. On Windows the name becomes
/// \\.\pipe\<name>; on POSIX it becomes a filesystem socket path.
Expected<std::unique_ptr<Listener>> create_listener(const std::string& endpoint,
                                                    EndpointOptions options = {});

/// Connects to an endpoint. timeout_ms bounds how long the connect may take.
Expected<std::unique_ptr<Stream>> connect_endpoint(const std::string& endpoint,
                                                   std::uint32_t timeout_ms = 5000);

/// True when this build has a local endpoint transport.
[[nodiscard]] bool local_transport_available() noexcept;
[[nodiscard]] std::string_view local_transport_name() noexcept;

}  // namespace naf::ipc

#endif  // NAF_IPC_ENDPOINT_HPP

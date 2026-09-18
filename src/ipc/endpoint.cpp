// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/ipc/endpoint.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace naf::ipc {

#if defined(_WIN32)

namespace {

std::wstring pipe_path(const std::string& endpoint) {
  std::wstring out = L"\\\\.\\pipe\\";
  for (const char c : endpoint) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  return out;
}

/// Named pipe connection over overlapped I/O. Overlapped handles are what make
/// shutdown possible: CancelIoEx really does abort a pending read, so a blocking
/// worker is released without any thread closing a handle another thread is
/// using. Synchronous pipe I/O cannot be cancelled at all.
class PipeStream final : public Stream {
 public:
  explicit PipeStream(HANDLE handle) : handle_(handle) {
    read_overlap_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    write_overlap_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  ~PipeStream() override {
    if (handle_ != nullptr) {
      (void)CancelIoEx(handle_, nullptr);
      (void)DisconnectNamedPipe(handle_);
      (void)CloseHandle(handle_);
      handle_ = nullptr;
    }
    if (read_overlap_.hEvent != nullptr) CloseHandle(read_overlap_.hEvent);
    if (write_overlap_.hEvent != nullptr) CloseHandle(write_overlap_.hEvent);
  }

  Expected<std::size_t> read_some(std::span<std::byte> out) override {
    if (handle_ == nullptr) return Status::error(StatusCode::TransportFailure, "pipe is closed");
    if (closed_.load()) return std::size_t{0};
    if (out.empty()) return std::size_t{0};
    (void)ResetEvent(read_overlap_.hEvent);
    read_overlap_.Offset = 0;
    read_overlap_.OffsetHigh = 0;
    DWORD read = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size(), 0xFFFFFFFFull));
    if (ReadFile(handle_, out.data(), want, &read, &read_overlap_) != 0) {
      return static_cast<std::size_t>(read);
    }
    DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
      if (GetOverlappedResult(handle_, &read_overlap_, &read, TRUE) != 0) {
        return static_cast<std::size_t>(read);
      }
      error = GetLastError();
    }
    if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
        error == ERROR_OPERATION_ABORTED || error == ERROR_INVALID_HANDLE) {
      return std::size_t{0};
    }
    return Status::error(StatusCode::TransportFailure,
                         "ReadFile failed with error " + std::to_string(error));
  }

  Status write_all(std::span<const std::byte> data) override {
    if (handle_ == nullptr) return Status::error(StatusCode::TransportFailure, "pipe is closed");
    std::size_t offset = 0;
    while (offset < data.size()) {
      (void)ResetEvent(write_overlap_.hEvent);
      write_overlap_.Offset = 0;
      write_overlap_.OffsetHigh = 0;
      DWORD wrote = 0;
      const DWORD chunk =
          static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 0xFFFFFFFFull));
      if (WriteFile(handle_, data.data() + offset, chunk, &wrote, &write_overlap_) == 0) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
          return Status::error(StatusCode::TransportFailure,
                               "WriteFile failed with error " + std::to_string(error));
        }
        if (GetOverlappedResult(handle_, &write_overlap_, &wrote, TRUE) == 0) {
          return Status::error(StatusCode::TransportFailure,
                               "WriteFile failed with error " + std::to_string(GetLastError()));
        }
      }
      if (wrote == 0) return Status::error(StatusCode::TransportFailure, "WriteFile wrote nothing");
      offset += wrote;
    }
    return Status::ok();
  }

  /// Idempotent, and safe to call while another thread is blocked in read_some.
  Status close() override {
    if (handle_ == nullptr) return Status::ok();
    closed_.store(true);
    (void)CancelIoEx(handle_, nullptr);
    return Status::ok();
  }

  [[nodiscard]] bool is_open() const override { return handle_ != nullptr && !closed_.load(); }

 private:
  HANDLE handle_ = nullptr;
  OVERLAPPED read_overlap_{};
  OVERLAPPED write_overlap_{};
  std::atomic<bool> closed_{false};
};

/// Accepts on a Windows named pipe without ever holding a lock across the
/// blocking call. close() unblocks a pending ConnectNamedPipe with CancelIoEx;
/// the handle itself is always closed by the thread that owns the accept, so no
/// thread ever closes a handle another thread is blocked on.
class PipeListener final : public Listener {
 public:
  PipeListener(std::string endpoint, EndpointOptions options)
      : endpoint_(std::move(endpoint)), options_(options) {}

  ~PipeListener() override { (void)close(); }

  Status open() {
    Status status = arm();
    if (!status.is_ok()) return status;
    return Status::ok();
  }

  Expected<std::unique_ptr<Stream>> accept() override {
    if (closed_.load()) {
      return Status::error(StatusCode::TransportFailure, "listener is closed");
    }
    if (pending_.load() == nullptr) {
      Status status = arm();
      if (!status.is_ok()) return status;
    }
    if (closed_.load()) {
      release_pending();
      return Status::error(StatusCode::TransportFailure, "listener was closed before accept");
    }
    const HANDLE handle = pending_.load();
    if (handle == nullptr) {
      return Status::error(StatusCode::TransportFailure, "listener has no pending pipe instance");
    }
    // Overlapped connect: the pending operation really is cancellable, which is
    // what lets close() release a blocked accept.
    OVERLAPPED overlap{};
    overlap.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlap.hEvent == nullptr) {
      release_pending();
      return Status::error(StatusCode::TransportFailure, "cannot create a connect event");
    }
    BOOL connected = ConnectNamedPipe(handle, &overlap);
    DWORD error = connected != 0 ? 0 : GetLastError();
    if (connected == 0 && error == ERROR_IO_PENDING) {
      (void)WaitForSingleObject(overlap.hEvent, INFINITE);
      DWORD transferred = 0;
      if (GetOverlappedResult(handle, &overlap, &transferred, FALSE) != 0) {
        connected = 1;
        error = 0;
      } else {
        error = GetLastError();
      }
    } else if (connected == 0 && error == ERROR_PIPE_CONNECTED) {
      connected = 1;
      error = 0;
    }
    (void)CloseHandle(overlap.hEvent);
    if (connected == 0) {
      pending_.store(nullptr);
      (void)CloseHandle(handle);
      if (closed_.load()) {
        return Status::error(StatusCode::TransportFailure, "listener was closed during accept");
      }
      return Status::error(StatusCode::TransportFailure,
                           "ConnectNamedPipe failed with error " + std::to_string(error));
    }
    if (closed_.load()) {
      pending_.store(nullptr);
      (void)DisconnectNamedPipe(handle);
      (void)CloseHandle(handle);
      return Status::error(StatusCode::TransportFailure, "listener was closed during accept");
    }
    // Ownership of the handle moves to the connection; the next accept arms a
    // fresh instance on demand.
    pending_.store(nullptr);
    return std::unique_ptr<Stream>(new PipeStream(handle));
  }

  /// Idempotent and safe to call from another thread while accept() is blocked.
  Status close() override {
    closed_.store(true);
    const HANDLE handle = pending_.load();
    if (handle != nullptr) {
      // CancelIoEx makes the blocked ConnectNamedPipe return. Closing the handle
      // here would race with the accepting thread, so it does not.
      (void)CancelIoEx(handle, nullptr);
    }
    return Status::ok();
  }

  [[nodiscard]] bool is_open() const override { return !closed_.load(); }
  [[nodiscard]] const std::string& endpoint() const noexcept override { return endpoint_; }

 private:
  HANDLE create_instance() const {
    const std::wstring path = pipe_path(endpoint_);
    return CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            static_cast<DWORD>(options_.backlog), options_.write_buffer_bytes,
                            options_.read_buffer_bytes, 0, nullptr);
  }

  Status arm() {
    const HANDLE handle = create_instance();
    if (handle == INVALID_HANDLE_VALUE) {
      return Status::error(StatusCode::TransportFailure, "CreateNamedPipe failed");
    }
    pending_.store(handle);
    return Status::ok();
  }

  void release_pending() {
    const HANDLE handle = pending_.exchange(nullptr);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      (void)DisconnectNamedPipe(handle);
      (void)CloseHandle(handle);
    }
  }

  std::string endpoint_;
  EndpointOptions options_{};
  std::atomic<HANDLE> pending_{nullptr};
  std::atomic<bool> closed_{false};
};

}  // namespace

Expected<std::unique_ptr<Listener>> create_listener(const std::string& endpoint,
                                                    EndpointOptions options) {
  if (endpoint.empty()) {
    return Status::error(StatusCode::InvalidArgument, "endpoint name is empty");
  }
  auto listener = std::unique_ptr<PipeListener>(new PipeListener(endpoint, options));
  Status opened = listener->open();
  if (!opened.is_ok()) return opened;
  return std::unique_ptr<Listener>(listener.release());
}

Expected<std::unique_ptr<Stream>> connect_endpoint(const std::string& endpoint,
                                                   std::uint32_t timeout_ms) {
  const std::wstring path = pipe_path(endpoint);
  if (WaitNamedPipeW(path.c_str(), timeout_ms) == 0) {
    return Status::error(StatusCode::TransportFailure,
                         "WaitNamedPipe timed out with error " + std::to_string(GetLastError()));
  }
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status::error(StatusCode::TransportFailure,
                         "CreateFile on the pipe failed with error " +
                             std::to_string(GetLastError()));
  }
  return std::unique_ptr<Stream>(new PipeStream(handle));
}

bool local_transport_available() noexcept { return true; }
std::string_view local_transport_name() noexcept { return "windows_named_pipe"; }

#else  // POSIX

namespace {

class SocketStream final : public Stream {
 public:
  explicit SocketStream(int fd) : fd_(fd) {}
  ~SocketStream() override { (void)close(); }

  Expected<std::size_t> read_some(std::span<std::byte> out) override {
    if (fd_ < 0) return Status::error(StatusCode::TransportFailure, "socket is closed");
    if (out.empty()) return std::size_t{0};
    const ssize_t got = ::read(fd_, out.data(), out.size());
    if (got < 0) {
      if (errno == EINTR) return std::size_t{0};
      return Status::error(StatusCode::TransportFailure, "read failed");
    }
    return static_cast<std::size_t>(got);
  }

  Status write_all(std::span<const std::byte> data) override {
    if (fd_ < 0) return Status::error(StatusCode::TransportFailure, "socket is closed");
    std::size_t offset = 0;
    while (offset < data.size()) {
      const ssize_t wrote = ::write(fd_, data.data() + offset, data.size() - offset);
      if (wrote <= 0) {
        if (wrote < 0 && errno == EINTR) continue;
        return Status::error(StatusCode::TransportFailure, "write failed");
      }
      offset += static_cast<std::size_t>(wrote);
    }
    return Status::ok();
  }

  Status close() override {
    if (fd_ < 0) return Status::ok();
    const int fd = fd_;
    fd_ = -1;
    (void)::shutdown(fd, SHUT_RDWR);
    (void)::close(fd);
    return Status::ok();
  }

  [[nodiscard]] bool is_open() const override { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

/// Accepts on an AF_UNIX socket without holding a lock across the blocking
/// call. close() shuts the listening socket down, which makes a blocked accept
/// return; the descriptor is closed by the accepting thread itself.
class SocketListener final : public Listener {
 public:
  SocketListener(std::string endpoint, int fd) : endpoint_(std::move(endpoint)), fd_(fd) {}
  ~SocketListener() override { (void)close(); }

  Expected<std::unique_ptr<Stream>> accept() override {
    const int fd = fd_.load();
    if (fd < 0) return Status::error(StatusCode::TransportFailure, "listener is closed");
    const int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) {
      if (closed_.load()) {
        (void)::close(fd);
        fd_.store(-1);
        return Status::error(StatusCode::TransportFailure, "listener was closed during accept");
      }
      return Status::error(StatusCode::TransportFailure, "accept failed");
    }
    if (closed_.load()) {
      (void)::close(client);
      (void)::close(fd);
      fd_.store(-1);
      return Status::error(StatusCode::TransportFailure, "listener was closed during accept");
    }
    return std::unique_ptr<Stream>(new SocketStream(client));
  }

  Status close() override {
    closed_.store(true);
    const int fd = fd_.load();
    if (fd >= 0) {
      // shutdown() unblocks a pending accept without racing the accepting
      // thread's ownership of the descriptor.
      (void)::shutdown(fd, SHUT_RDWR);
      (void)::unlink(endpoint_.c_str());
    }
    return Status::ok();
  }

  [[nodiscard]] bool is_open() const override { return !closed_.load(); }
  [[nodiscard]] const std::string& endpoint() const noexcept override { return endpoint_; }

 private:
  std::string endpoint_;
  std::atomic<int> fd_{-1};
  std::atomic<bool> closed_{false};
};

}  // namespace

Expected<std::unique_ptr<Listener>> create_listener(const std::string& endpoint,
                                                    EndpointOptions options) {
  if (endpoint.empty()) {
    return Status::error(StatusCode::InvalidArgument, "endpoint name is empty");
  }
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return Status::error(StatusCode::TransportFailure, "socket() failed");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (endpoint.size() >= sizeof(address.sun_path)) {
    ::close(fd);
    return Status::error(StatusCode::InvalidArgument, "endpoint path is too long");
  }
  std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
  (void)::unlink(endpoint.c_str());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return Status::error(StatusCode::TransportFailure, "bind failed");
  }
  if (::listen(fd, static_cast<int>(options.backlog)) != 0) {
    ::close(fd);
    return Status::error(StatusCode::TransportFailure, "listen failed");
  }
  return std::unique_ptr<Listener>(new SocketListener(endpoint, fd));
}

Expected<std::unique_ptr<Stream>> connect_endpoint(const std::string& endpoint,
                                                   std::uint32_t timeout_ms) {
  (void)timeout_ms;
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return Status::error(StatusCode::TransportFailure, "socket() failed");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (endpoint.size() >= sizeof(address.sun_path)) {
    ::close(fd);
    return Status::error(StatusCode::InvalidArgument, "endpoint path is too long");
  }
  std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return Status::error(StatusCode::TransportFailure, "connect failed");
  }
  return std::unique_ptr<Stream>(new SocketStream(fd));
}

bool local_transport_available() noexcept { return true; }
std::string_view local_transport_name() noexcept { return "posix_unix_socket"; }

#endif

}  // namespace naf::ipc

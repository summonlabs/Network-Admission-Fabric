// Network Admission Fabric - status and expected-value plumbing.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_CORE_STATUS_HPP
#define NAF_CORE_STATUS_HPP

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace naf {

/// Machine-readable failure taxonomy. A Status is returned for conditions that
/// prevent a decision from being produced at all; conditions that *are* a
/// decision are reported through AdmissionOutcome instead.
enum class StatusCode : std::uint16_t {
  Ok = 0,
  InvalidArgument,
  MalformedInput,
  OversizedInput,
  UnknownResource,
  UnknownPath,
  DuplicateIdentity,
  ConflictIdentity,
  StaleGeneration,
  EpochMismatch,
  Fenced,
  NotFound,
  AlreadyExists,
  ArithmeticOverflow,
  ArithmeticUnderflow,
  CapacityViolation,
  AuthorityViolation,
  DurableFailure,
  CorruptJournal,
  TruncatedFrame,
  OversizedFrame,
  ProtocolViolation,
  TransportFailure,
  Unsupported,
  ShuttingDown,
  Busy,
  Internal,
};

[[nodiscard]] std::string_view to_string(StatusCode code) noexcept;

class Status {
 public:
  Status() noexcept = default;

  static Status ok() noexcept { return Status{}; }

  static Status error(StatusCode code, std::string message) {
    Status s;
    s.code_ = code;
    s.message_ = std::move(message);
    return s;
  }

  [[nodiscard]] bool is_ok() const noexcept { return code_ == StatusCode::Ok; }
  explicit operator bool() const noexcept { return is_ok(); }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] std::string to_string() const;

 private:
  StatusCode code_ = StatusCode::Ok;
  std::string message_;
};

/// Minimal expected-value type. Either holds a value or a Status, never both.
template <class T>
class Expected {
 public:
  Expected(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Expected(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  T& value() & { return *value_; }
  const T& value() const& { return *value_; }
  T&& value() && { return std::move(*value_); }

  const Status& status() const noexcept { return status_; }

  T value_or(T fallback) const { return value_.has_value() ? *value_ : std::move(fallback); }

 private:
  std::optional<T> value_;
  Status status_;
};

}  // namespace naf

#endif  // NAF_CORE_STATUS_HPP

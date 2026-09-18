// Network Admission Fabric - framed transport.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Wire frame layout (little endian):
//   magic        u32   0x3146414E  ('N','A','F','1')
//   version      u16
//   kind         u16
//   flags        u16
//   reserved     u16   must be zero
//   length       u32   payload bytes
//   payload_crc  u32   CRC-32C over the payload
//   payload      length bytes
//
// A frame is only delivered once its full length is present and its checksum
// verifies. Oversized, truncated and corrupt frames are refused; they are never
// partially interpreted.
#ifndef NAF_IPC_FRAME_HPP
#define NAF_IPC_FRAME_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <span>

#include "naf/core/bytes.hpp"
#include "naf/core/limits.hpp"
#include "naf/core/status.hpp"
#include "naf/ipc/stream.hpp"
#include "naf/version.hpp"

namespace naf::ipc {

enum class MessageKind : std::uint16_t {
  Reserved = 0,
  Hello = 1,
  Welcome = 2,
  Admit = 3,
  Decision = 4,
  Revalidate = 5,
  Revalidation = 6,
  Revoke = 7,
  StatusRequest = 8,
  StatusReply = 9,
  Ping = 10,
  Pong = 11,
  Bye = 12,
  Error = 13,
};

[[nodiscard]] std::string_view to_string(MessageKind kind) noexcept;

inline constexpr std::uint32_t wire_magic = 0x3146414Eu;
inline constexpr std::size_t wire_header_bytes = 20;

struct WireFrame {
  MessageKind kind = MessageKind::Reserved;
  std::uint16_t flags = 0;
  ByteBuffer payload{};
};

struct WireHeader {
  MessageKind kind = MessageKind::Reserved;
  std::uint16_t flags = 0;
  std::uint32_t length = 0;
  std::uint32_t payload_crc = 0;
};

/// Encodes one complete wire frame.
[[nodiscard]] ByteBuffer encode_wire_frame(MessageKind kind, std::uint16_t flags,
                                           std::span<const std::byte> payload);

/// Decodes a fixed-size header. Returns false on magic, version or reserved
/// field violations, or when the declared length exceeds max_frame.
[[nodiscard]] bool decode_wire_header(std::span<const std::byte> header, WireHeader& out,
                                      std::uint32_t max_frame);

/// Writes one frame. Refuses payloads larger than max_frame.
Status write_message(Stream& stream, MessageKind kind, std::uint16_t flags,
                     std::span<const std::byte> payload, std::uint32_t max_frame);

/// Reads exactly one frame. Returns StatusCode::TransportFailure with an empty
/// message when the peer closed cleanly before a frame started, so callers can
/// distinguish an orderly disconnect from a truncated frame.
Expected<WireFrame> read_message(Stream& stream, std::uint32_t max_frame);

}  // namespace naf::ipc

#endif  // NAF_IPC_FRAME_HPP

// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/ipc/frame.hpp"

#include <cstring>

#include "naf/core/crc32c.hpp"

namespace naf::ipc {
namespace {

void put_u16(ByteBuffer& out, std::uint16_t v) {
  for (int i = 0; i < 2; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}

void put_u32(ByteBuffer& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}

std::uint16_t get_u16(std::span<const std::byte> data, std::size_t offset) {
  std::uint16_t v = 0;
  for (int i = 0; i < 2; ++i) {
    v |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[offset + static_cast<std::size_t>(i)]))
         << (8 * i);
  }
  return v;
}

std::uint32_t get_u32(std::span<const std::byte> data, std::size_t offset) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + static_cast<std::size_t>(i)]))
         << (8 * i);
  }
  return v;
}

/// Reads up to size bytes, retrying until the buffer is full, the peer closes
/// or the stream is closed. Returns the number of bytes actually read.
Expected<std::size_t> read_full(Stream& stream, std::span<std::byte> out) {
  std::size_t filled = 0;
  while (filled < out.size()) {
    auto got = stream.read_some(out.subspan(filled));
    if (!got) return got.status();
    if (got.value() == 0) {
      if (!stream.is_open()) {
        return Status::error(StatusCode::TransportFailure, "stream closed while reading a frame");
      }
      break;
    }
    filled += got.value();
  }
  return filled;
}

}  // namespace

std::string_view to_string(MessageKind kind) noexcept {
  switch (kind) {
    case MessageKind::Reserved: return "reserved";
    case MessageKind::Hello: return "hello";
    case MessageKind::Welcome: return "welcome";
    case MessageKind::Admit: return "admit";
    case MessageKind::Decision: return "decision";
    case MessageKind::Revalidate: return "revalidate";
    case MessageKind::Revalidation: return "revalidation";
    case MessageKind::Revoke: return "revoke";
    case MessageKind::StatusRequest: return "status_request";
    case MessageKind::StatusReply: return "status_reply";
    case MessageKind::Ping: return "ping";
    case MessageKind::Pong: return "pong";
    case MessageKind::Bye: return "bye";
    case MessageKind::Error: return "error";
  }
  return "unknown";
}

ByteBuffer encode_wire_frame(MessageKind kind, std::uint16_t flags,
                             std::span<const std::byte> payload) {
  ByteBuffer out;
  out.reserve(wire_header_bytes + payload.size());
  put_u32(out, wire_magic);
  put_u16(out, format_revision);
  put_u16(out, static_cast<std::uint16_t>(kind));
  put_u16(out, flags);
  put_u16(out, 0);
  put_u32(out, static_cast<std::uint32_t>(payload.size()));
  put_u32(out, crc32c(payload));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool decode_wire_header(std::span<const std::byte> header, WireHeader& out, std::uint32_t max_frame) {
  if (header.size() < wire_header_bytes) return false;
  if (get_u32(header, 0) != wire_magic) return false;
  if (get_u16(header, 4) != format_revision) return false;
  const std::uint16_t kind = get_u16(header, 6);
  if (kind > static_cast<std::uint16_t>(MessageKind::Error)) return false;
  if (get_u16(header, 10) != 0) return false;
  const std::uint32_t length = get_u32(header, 12);
  if (length > max_frame) return false;
  out.kind = static_cast<MessageKind>(kind);
  out.flags = get_u16(header, 8);
  out.length = length;
  out.payload_crc = get_u32(header, 16);
  return true;
}

Status write_message(Stream& stream, MessageKind kind, std::uint16_t flags,
                     std::span<const std::byte> payload, std::uint32_t max_frame) {
  if (payload.size() > max_frame) {
    return Status::error(StatusCode::OversizedFrame, "outgoing payload exceeds the frame bound");
  }
  const ByteBuffer frame = encode_wire_frame(kind, flags, payload);
  return stream.write_all(frame);
}

Expected<WireFrame> read_message(Stream& stream, std::uint32_t max_frame) {
  std::byte header_bytes[wire_header_bytes];
  auto header_read = read_full(stream, std::span<std::byte>(header_bytes, wire_header_bytes));
  if (!header_read) return header_read.status();
  if (header_read.value() == 0) {
    return Status::error(StatusCode::TransportFailure, "");
  }
  if (header_read.value() != wire_header_bytes) {
    return Status::error(StatusCode::TruncatedFrame, "frame header is incomplete");
  }
  WireHeader header;
  if (!decode_wire_header(std::span<const std::byte>(header_bytes, wire_header_bytes), header,
                          max_frame)) {
    return Status::error(StatusCode::ProtocolViolation,
                         "frame header failed validation (magic, version, kind or length)");
  }
  WireFrame frame;
  frame.kind = header.kind;
  frame.flags = header.flags;
  if (header.length == 0) {
    if (header.payload_crc != crc32c(std::span<const std::byte>())) {
      return Status::error(StatusCode::ProtocolViolation, "empty frame checksum mismatch");
    }
    return frame;
  }
  frame.payload.resize(header.length);
  auto body = read_full(stream, std::span<std::byte>(frame.payload.data(), frame.payload.size()));
  if (!body) return body.status();
  if (body.value() != frame.payload.size()) {
    return Status::error(StatusCode::TruncatedFrame, "frame payload is incomplete");
  }
  if (crc32c(std::span<const std::byte>(frame.payload.data(), frame.payload.size())) !=
      header.payload_crc) {
    return Status::error(StatusCode::CorruptJournal, "frame payload checksum mismatch");
  }
  return frame;
}

}  // namespace naf::ipc

// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/durable/journal.hpp"

#include <cstring>
#include <vector>

#include "naf/core/crc32c.hpp"

namespace naf {
namespace {

void put_u16(ByteBuffer& out, std::uint16_t v) {
  for (int i = 0; i < 2; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}

void put_u32(ByteBuffer& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}

void put_u64(ByteBuffer& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}

std::uint16_t get_u16(std::span<const std::byte> data, std::size_t offset) {
  std::uint16_t v = 0;
  for (int i = 0; i < 2; ++i) v |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[offset + static_cast<std::size_t>(i)])) << (8 * i);
  return v;
}

std::uint32_t get_u32(std::span<const std::byte> data, std::size_t offset) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + static_cast<std::size_t>(i)])) << (8 * i);
  return v;
}

std::uint64_t get_u64(std::span<const std::byte> data, std::size_t offset) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[offset + static_cast<std::size_t>(i)])) << (8 * i);
  return v;
}

}  // namespace

std::string_view to_string(JournalRecordKind kind) noexcept {
  switch (kind) {
    case JournalRecordKind::Reserved: return "reserved";
    case JournalRecordKind::BootEpoch: return "boot_epoch";
    case JournalRecordKind::PolicyConfig: return "policy_config";
    case JournalRecordKind::AdmissionPrepare: return "admission_prepare";
    case JournalRecordKind::AdmissionCommit: return "admission_commit";
    case JournalRecordKind::AdmissionRefused: return "admission_refused";
    case JournalRecordKind::LedgerRelease: return "ledger_release";
    case JournalRecordKind::Revocation: return "revocation";
    case JournalRecordKind::AuditMark: return "audit_mark";
    case JournalRecordKind::CompactionMarker: return "compaction_marker";
    case JournalRecordKind::CoordinatorConfig: return "coordinator_config";
  }
  return "unknown";
}

ByteBuffer encode_frame(JournalRecordKind kind, std::uint64_t sequence,
                        std::span<const std::byte> payload) {
  ByteBuffer out;
  out.reserve(journal_overhead_bytes + payload.size());
  put_u32(out, journal_magic);
  put_u16(out, format_revision);
  put_u16(out, static_cast<std::uint16_t>(kind));
  put_u64(out, sequence);
  put_u32(out, static_cast<std::uint32_t>(payload.size()));
  const std::span<const std::byte> header(out.data(), journal_header_bytes - 4);
  put_u32(out, crc32c(header));
  out.insert(out.end(), payload.begin(), payload.end());
  put_u32(out, crc32c(payload));
  return out;
}

bool decode_frame(std::span<const std::byte> buffer, JournalFrame& out, std::size_t max_payload) {
  if (buffer.size() < journal_overhead_bytes) return false;
  if (get_u32(buffer, 0) != journal_magic) return false;
  if (get_u16(buffer, 4) != format_revision) return false;
  const std::uint16_t kind = get_u16(buffer, 6);
  const std::uint64_t sequence = get_u64(buffer, 8);
  const std::uint32_t payload_len = get_u32(buffer, 16);
  if (payload_len > max_payload) return false;
  if (buffer.size() != journal_overhead_bytes + payload_len) return false;
  const std::uint32_t header_crc = get_u32(buffer, 20);
  if (header_crc != crc32c(buffer.first(journal_header_bytes - 4))) return false;
  const auto payload = buffer.subspan(journal_header_bytes, payload_len);
  if (get_u32(buffer, journal_header_bytes + payload_len) != crc32c(payload)) return false;
  out.kind = static_cast<JournalRecordKind>(kind);
  out.sequence = sequence;
  out.payload.assign(payload.begin(), payload.end());
  return true;
}

Journal::Journal(std::filesystem::path path, JournalOptions options)
    : path_(std::move(path)), options_(options) {}

Status Journal::open() {
  if (!path_.has_parent_path() || path_.parent_path().empty()) {
    // nothing to create
  } else {
    Status dir = ensure_directory(path_.parent_path());
    if (!dir.is_ok()) return dir;
  }
  const bool existed = std::filesystem::exists(path_);
  // One handle serves reads, appends and repair. A second concurrent CRT handle
  // on the same path is a Windows sharing violation, so there is exactly one.
  auto opened = FileHandle::open(path_, true, true, true, true);
  if (!opened) return opened.status();
  file_ = std::move(opened).value();

  scan_ = JournalScanResult{};
  scan_.opened = true;
  scan_.created = !existed;

  std::vector<std::byte> data;
  const std::uint64_t total = file_.size();
  if (total > options_.max_file_bytes) {
    return Status::error(StatusCode::OversizedInput, "journal exceeds the configured size bound");
  }
  data.resize(static_cast<std::size_t>(total));
  Status sought = file_.seek(0);
  if (!sought.is_ok()) return sought;
  std::size_t filled = 0;
  while (filled < data.size()) {
    auto got = file_.read_some(std::span<std::byte>(data.data() + filled, data.size() - filled));
    if (!got) return got.status();
    if (got.value() == 0) break;
    filled += got.value();
  }
  data.resize(filled);

  const std::span<const std::byte> bytes(data.data(), data.size());
  std::size_t offset = 0;
  std::uint64_t last_sequence = 0;
  std::uint64_t good_bytes = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    if (remaining < journal_header_bytes) {
      scan_.truncated_tail_bytes = remaining;
      break;
    }
    if (get_u32(bytes, offset) != journal_magic) {
      // A frame boundary that no longer holds a magic value is damage in the
      // middle of history, not a torn tail.
      scan_.corrupt = true;
      return Status::error(StatusCode::CorruptJournal,
                           "journal frame magic is invalid at offset " + std::to_string(offset));
    }
    const std::uint16_t version = get_u16(bytes, offset + 4);
    if (version != format_revision) {
      scan_.corrupt = true;
      return Status::error(StatusCode::CorruptJournal,
                           "journal frame version " + std::to_string(version) + " is not supported");
    }
    const std::uint32_t payload_len = get_u32(bytes, offset + 16);
    if (payload_len > options_.max_payload) {
      scan_.corrupt = true;
      return Status::error(StatusCode::CorruptJournal, "journal frame payload exceeds the bound");
    }
    const std::size_t frame_bytes = journal_overhead_bytes + payload_len;
    if (remaining < frame_bytes) {
      scan_.truncated_tail_bytes = remaining;
      break;
    }
    const auto frame = bytes.subspan(offset, frame_bytes);
    const std::uint32_t header_crc = get_u32(frame, 20);
    if (header_crc != crc32c(frame.first(journal_header_bytes - 4))) {
      if (remaining == frame_bytes) {
        // Final frame with a damaged header: an interrupted append.
        scan_.truncated_tail_bytes = remaining;
        break;
      }
      scan_.corrupt = true;
      return Status::error(StatusCode::CorruptJournal,
                           "journal header checksum mismatch at offset " + std::to_string(offset));
    }
    const auto payload = frame.subspan(journal_header_bytes, payload_len);
    if (get_u32(frame, journal_header_bytes + payload_len) != crc32c(payload)) {
      if (remaining == frame_bytes) {
        scan_.truncated_tail_bytes = remaining;
        break;
      }
      scan_.corrupt = true;
      return Status::error(StatusCode::CorruptJournal,
                           "journal payload checksum mismatch at offset " + std::to_string(offset));
    }
    last_sequence = get_u64(bytes, offset + 8);
    ++scan_.frames;
    offset += frame_bytes;
    good_bytes = offset;
  }

  scan_.bytes = bytes.size();
  scan_.last_sequence = last_sequence;
  next_sequence_ = last_sequence + 1;
  frame_count_ = scan_.frames;

  if (scan_.truncated_tail_bytes != 0) {
    // Repair: drop the incomplete tail so the next append starts on a boundary.
    Status truncated = file_.truncate(good_bytes);
    if (!truncated.is_ok()) return truncated;
    Status synced = file_.sync();
    if (!synced.is_ok()) return synced;
    scan_.repaired_bytes = scan_.truncated_tail_bytes;
    scan_.bytes = good_bytes;
  }
  return Status::ok();
}

Status Journal::append(JournalRecordKind kind, std::span<const std::byte> payload) {
  if (!file_.is_open()) return Status::error(StatusCode::InvalidArgument, "journal is not open");
  if (payload.size() > options_.max_payload) {
    return Status::error(StatusCode::OversizedInput, "journal payload exceeds the configured bound");
  }
  const ByteBuffer frame = encode_frame(kind, next_sequence_, payload);
  Status written = file_.write_all(frame);
  if (!written.is_ok()) return written;
  if (options_.sync_on_append) {
    Status synced = file_.sync();
    if (!synced.is_ok()) return synced;
  }
  ++next_sequence_;
  ++frame_count_;
  return Status::ok();
}

Status Journal::flush() { return file_.flush(); }

Status Journal::sync() { return file_.sync(); }

Status Journal::replay(std::vector<JournalFrame>& out, std::size_t max_frames,
                       std::uint64_t max_bytes) {
  out.clear();
  if (!file_.is_open()) return Status::error(StatusCode::InvalidArgument, "journal is not open");
  const std::uint64_t total = file_.size();
  if (total > options_.max_file_bytes) {
    return Status::error(StatusCode::OversizedInput, "journal exceeds the configured size bound");
  }
  std::vector<std::byte> data(static_cast<std::size_t>(total));
  Status sought = file_.seek(0);
  if (!sought.is_ok()) return sought;
  std::size_t filled = 0;
  while (filled < data.size()) {
    auto got = file_.read_some(std::span<std::byte>(data.data() + filled, data.size() - filled));
    if (!got) return got.status();
    if (got.value() == 0) break;
    filled += got.value();
  }
  data.resize(filled);

  const std::span<const std::byte> bytes(data.data(), data.size());
  std::size_t offset = 0;
  std::uint64_t accumulated = 0;
  while (offset + journal_header_bytes <= bytes.size()) {
    const std::uint32_t payload_len = get_u32(bytes, offset + 16);
    if (payload_len > options_.max_payload) {
      return Status::error(StatusCode::CorruptJournal, "journal frame payload exceeds the bound");
    }
    const std::size_t frame_bytes = journal_overhead_bytes + payload_len;
    if (offset + frame_bytes > bytes.size()) break;
    JournalFrame frame;
    if (!decode_frame(bytes.subspan(offset, frame_bytes), frame, options_.max_payload)) {
      return Status::error(StatusCode::CorruptJournal, "journal frame failed to decode during replay");
    }
    if (out.size() >= max_frames) {
      return Status::error(StatusCode::OversizedInput, "journal has more frames than the replay bound");
    }
    accumulated += frame_bytes;
    if (accumulated > max_bytes) {
      return Status::error(StatusCode::OversizedInput, "journal replay exceeds the byte bound");
    }
    out.push_back(std::move(frame));
    offset += frame_bytes;
  }
  return Status::ok();
}

Status Journal::compact(const std::vector<JournalFrame>& live) {
  if (!file_.is_open()) return Status::error(StatusCode::InvalidArgument, "journal is not open");
  std::filesystem::path temp = path_;
  temp += ".compact";

  {
    auto opened = FileHandle::open(temp, false, true, false, true);
    if (!opened) return opened.status();
    FileHandle writer = std::move(opened).value();
    std::uint64_t sequence = 1;
    for (const auto& frame : live) {
      const ByteBuffer encoded = encode_frame(frame.kind, sequence, frame.payload);
      Status written = writer.write_all(encoded);
      if (!written.is_ok()) return written;
      ++sequence;
    }
    Status synced = writer.sync();
    if (!synced.is_ok()) return synced;
    Status closed = writer.close();
    if (!closed.is_ok()) return closed;
  }

  Status closed = file_.close();
  if (!closed.is_ok()) return closed;
  Status replaced = atomic_replace_file(temp, path_);
  if (!replaced.is_ok()) return replaced;
  return open();
}

Status Journal::close() {
  if (!file_.is_open()) return Status::ok();
  Status synced = file_.sync();
  Status closed = file_.close();
  if (!synced.is_ok()) return synced;
  return closed;
}

}  // namespace naf

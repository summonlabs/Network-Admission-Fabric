// Network Admission Fabric - durable, integrity-checked, crash-safe journal.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Frame layout (little endian):
//   magic        u32   0x4A46414E
//   version      u16
//   kind         u16
//   sequence     u64
//   payload_len  u32
//   header_crc   u32   CRC-32C over the preceding 20 bytes
//   payload      payload_len bytes
//   payload_crc  u32   CRC-32C over the payload
//
// A frame is only visible to recovery once its full length is present and both
// CRCs verify. A torn tail is truncated back to the last good frame; a corrupt
// frame that is not at the tail fails the open so that damaged history can never
// be silently reinterpreted.
#ifndef NAF_DURABLE_JOURNAL_HPP
#define NAF_DURABLE_JOURNAL_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "naf/core/bytes.hpp"
#include "naf/core/file.hpp"
#include "naf/core/limits.hpp"
#include "naf/core/status.hpp"
#include "naf/version.hpp"

namespace naf {

enum class JournalRecordKind : std::uint16_t {
  Reserved = 0,
  BootEpoch = 1,
  PolicyConfig = 2,
  AdmissionPrepare = 3,
  AdmissionCommit = 4,
  AdmissionRefused = 5,
  LedgerRelease = 6,
  Revocation = 7,
  AuditMark = 8,
  CompactionMarker = 9,
  CoordinatorConfig = 10,
};

[[nodiscard]] std::string_view to_string(JournalRecordKind kind) noexcept;

inline constexpr std::uint32_t journal_magic = 0x4A46414Eu;  // 'N','A','F','J'
inline constexpr std::size_t journal_header_bytes = 24;
inline constexpr std::size_t journal_trailer_bytes = 4;
inline constexpr std::size_t journal_overhead_bytes = journal_header_bytes + journal_trailer_bytes;

struct JournalOptions {
  std::uint32_t max_payload = limits::max_journal_payload;
  /// When true each append is followed by a real fsync before it returns. A
  /// caller may only acknowledge durable work after an append that returned ok.
  bool sync_on_append = true;
  /// Advisory threshold; the owner decides when to compact.
  std::uint64_t compact_at_records = limits::journal_compact_at_records;
  /// Reject a journal larger than this rather than scanning without bound.
  std::uint64_t max_file_bytes = 1ull << 32;
};

struct JournalFrame {
  JournalRecordKind kind = JournalRecordKind::Reserved;
  std::uint64_t sequence = 0;
  ByteBuffer payload{};
};

struct JournalScanResult {
  bool opened = false;
  bool created = false;
  std::uint64_t frames = 0;
  std::uint64_t bytes = 0;
  /// Bytes belonging to an incomplete trailing frame that were discarded.
  std::uint64_t truncated_tail_bytes = 0;
  /// Total bytes removed from the file by tail repair.
  std::uint64_t repaired_bytes = 0;
  bool corrupt = false;
  std::uint64_t last_sequence = 0;
};

class Journal {
 public:
  explicit Journal(std::filesystem::path path, JournalOptions options = {});

  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  /// Opens the journal, scanning every frame and repairing a torn tail.
  Status open();

  /// Appends one frame. With sync_on_append the call returns only after the
  /// bytes are on stable storage.
  Status append(JournalRecordKind kind, std::span<const std::byte> payload);

  Status flush();
  Status sync();

  /// Returns every intact frame in order, bounded by max_frames and max_bytes.
  /// Not const: it repositions the single journal handle.
  Status replay(std::vector<JournalFrame>& out, std::size_t max_frames, std::uint64_t max_bytes);

  /// Rewrites the file with exactly the supplied frames, then atomically
  /// replaces the old file. Used to bound durable growth.
  Status compact(const std::vector<JournalFrame>& live);

  Status close();

  [[nodiscard]] const JournalScanResult& scan() const noexcept { return scan_; }
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }
  [[nodiscard]] std::uint64_t frame_count() const noexcept { return frame_count_; }
  [[nodiscard]] bool is_open() const noexcept { return file_.is_open(); }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] const JournalOptions& options() const noexcept { return options_; }

 private:
  std::filesystem::path path_{};
  JournalOptions options_{};
  JournalScanResult scan_{};
  FileHandle file_{};
  std::uint64_t next_sequence_ = 1;
  std::uint64_t frame_count_ = 0;
};

/// Encodes one frame into a flat buffer (header + payload + trailer).
[[nodiscard]] ByteBuffer encode_frame(JournalRecordKind kind, std::uint64_t sequence,
                                      std::span<const std::byte> payload);

/// Decodes a frame from a complete buffer. Returns false on any structural or
/// integrity failure.
[[nodiscard]] bool decode_frame(std::span<const std::byte> buffer, JournalFrame& out,
                                std::size_t max_payload);

}  // namespace naf

#endif  // NAF_DURABLE_JOURNAL_HPP

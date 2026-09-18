// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "naf/naf.hpp"
#include "support/temp_dir.hpp"
#include "support/test_framework.hpp"

using namespace naftest;

namespace {

std::vector<std::byte> payload_of(const std::string& text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (const char c : text) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  return out;
}

std::uint64_t size_of(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  return ec ? 0 : static_cast<std::uint64_t>(size);
}

}  // namespace

NAF_TEST(journal, append_and_replay_roundtrip) {
  TempDir temp("journal-roundtrip");
  const auto path = temp.file("admission.journal");
  naf::Journal journal(path);
  NAF_REQUIRE_OK(journal.open());
  NAF_CHECK(journal.scan().created);

  for (int i = 0; i < 5; ++i) {
    const std::string text = "frame-" + std::to_string(i);
    NAF_REQUIRE(journal.append(naf::JournalRecordKind::AuditMark, payload_of(text)).is_ok());
  }
  NAF_CHECK_U64(journal.frame_count(), 5);
  NAF_REQUIRE(journal.close().is_ok());

  naf::Journal reopened(path);
  NAF_REQUIRE(reopened.open().is_ok());
  NAF_CHECK(!reopened.scan().created);
  NAF_CHECK_U64(reopened.scan().frames, 5);
  NAF_CHECK_U64(reopened.scan().repaired_bytes, 0);
  NAF_CHECK_U64(reopened.next_sequence(), 6);

  std::vector<naf::JournalFrame> frames;
  NAF_REQUIRE(reopened.replay(frames, 64, 1u << 20).is_ok());
  NAF_CHECK_EQ(frames.size(), std::size_t{5});
  for (int i = 0; i < 5; ++i) {
    const auto index = static_cast<std::size_t>(i);
    const std::string text(reinterpret_cast<const char*>(frames[index].payload.data()),
                           frames[index].payload.size());
    NAF_CHECK(text == "frame-" + std::to_string(i));
    NAF_CHECK_U64(frames[index].sequence, static_cast<std::uint64_t>(i) + 1);
  }
  NAF_REQUIRE(reopened.close().is_ok());
}

NAF_TEST(journal, torn_tail_is_repaired_and_reported) {
  TempDir temp("journal-torn");
  const auto path = temp.file("admission.journal");
  std::uint64_t good_bytes = 0;
  {
    naf::Journal journal(path);
    NAF_REQUIRE(journal.open().is_ok());
    NAF_REQUIRE(journal.append(naf::JournalRecordKind::AuditMark, payload_of("one")).is_ok());
    NAF_REQUIRE(journal.append(naf::JournalRecordKind::AuditMark, payload_of("two")).is_ok());
    NAF_REQUIRE(journal.close().is_ok());
    good_bytes = size_of(path);
    NAF_CHECK(good_bytes > 0);
  }

  // Simulate a crash part-way through a third append.
  {
    auto handle = naf::FileHandle::open(path, false, true, true, true);
    NAF_REQUIRE(handle.has_value());
    naf::FileHandle file = std::move(handle).value();
    const naf::ByteBuffer partial =
        naf::encode_frame(naf::JournalRecordKind::AdmissionCommit, 3, payload_of("three"));
    NAF_REQUIRE(file.write_all(std::span<const std::byte>(partial.data(), partial.size() / 2)).is_ok());
    NAF_REQUIRE(file.sync().is_ok());
    NAF_REQUIRE(file.close().is_ok());
  }
  NAF_CHECK(size_of(path) > good_bytes);

  naf::Journal journal(path);
  NAF_REQUIRE(journal.open().is_ok());
  NAF_CHECK_U64(journal.scan().frames, 2);
  NAF_CHECK(journal.scan().truncated_tail_bytes > 0);
  NAF_CHECK_U64(journal.scan().repaired_bytes, journal.scan().truncated_tail_bytes);
  NAF_CHECK_U64(size_of(path), good_bytes);
  NAF_CHECK(!journal.scan().corrupt);
  NAF_REQUIRE(journal.append(naf::JournalRecordKind::AuditMark, payload_of("three")).is_ok());
  NAF_REQUIRE(journal.close().is_ok());

  naf::Journal final_journal(path);
  NAF_REQUIRE(final_journal.open().is_ok());
  NAF_CHECK_U64(final_journal.scan().frames, 3);
  NAF_REQUIRE(final_journal.close().is_ok());
}

NAF_TEST(journal, corrupt_interior_frame_is_refused) {
  TempDir temp("journal-corrupt");
  const auto path = temp.file("admission.journal");
  {
    naf::Journal journal(path);
    NAF_REQUIRE(journal.open().is_ok());
    NAF_REQUIRE(journal.append(naf::JournalRecordKind::AuditMark, payload_of("alpha")).is_ok());
    NAF_REQUIRE(journal.append(naf::JournalRecordKind::AuditMark, payload_of("beta")).is_ok());
    NAF_REQUIRE(journal.append(naf::JournalRecordKind::AuditMark, payload_of("gamma")).is_ok());
    NAF_REQUIRE(journal.close().is_ok());
  }
  // Flip a byte inside the first frame payload. More frames follow, so this is
  // damage to history, not a torn tail, and must not be silently discarded.
  {
    auto handle = naf::FileHandle::open(path, true, true, false, false);
    NAF_REQUIRE(handle.has_value());
    naf::FileHandle file = std::move(handle).value();
    const std::uint64_t offset = naf::journal_header_bytes + 2;
    NAF_REQUIRE(file.seek(offset).is_ok());
    std::byte one[1];
    auto read = file.read_some(std::span<std::byte>(one, 1));
    NAF_REQUIRE(read.has_value());
    NAF_REQUIRE(read.value() == 1);
    const std::byte flipped = static_cast<std::byte>(static_cast<std::uint8_t>(one[0]) ^ 0xFFu);
    NAF_REQUIRE(file.seek(offset).is_ok());
    NAF_REQUIRE(file.write_all(std::span<const std::byte>(&flipped, 1)).is_ok());
    NAF_REQUIRE(file.sync().is_ok());
    NAF_REQUIRE(file.close().is_ok());
  }
  naf::Journal journal(path);
  const naf::Status opened = journal.open();
  NAF_CHECK(!opened.is_ok());
  NAF_CHECK(opened.code() == naf::StatusCode::CorruptJournal);
  NAF_CHECK(journal.scan().corrupt);
}

NAF_TEST(journal, oversized_payload_is_refused) {
  TempDir temp("journal-oversize");
  const auto path = temp.file("admission.journal");
  naf::Journal journal(path);
  NAF_REQUIRE(journal.open().is_ok());
  std::vector<std::byte> huge(naf::limits::max_journal_payload + 1, std::byte{0});
  const naf::Status status = journal.append(naf::JournalRecordKind::AuditMark, huge);
  NAF_CHECK(!status.is_ok());
  NAF_CHECK(status.code() == naf::StatusCode::OversizedInput);
  NAF_CHECK_U64(journal.frame_count(), 0);
  NAF_REQUIRE(journal.close().is_ok());
}

NAF_TEST(journal, compaction_rewrites_only_live_frames) {
  TempDir temp("journal-compact");
  const auto path = temp.file("admission.journal");
  naf::Journal journal(path);
  NAF_REQUIRE(journal.open().is_ok());
  for (int i = 0; i < 100; ++i) {
    NAF_REQUIRE(journal.append(naf::JournalRecordKind::AdmissionRefused, payload_of("noise")).is_ok());
  }
  const std::uint64_t before = size_of(path);

  std::vector<naf::JournalFrame> live;
  naf::JournalFrame frame;
  frame.kind = naf::JournalRecordKind::AuditMark;
  frame.payload = payload_of("live");
  live.push_back(frame);
  NAF_REQUIRE(journal.compact(live).is_ok());
  NAF_CHECK(size_of(path) < before);
  NAF_CHECK_U64(journal.scan().frames, 1);

  std::vector<naf::JournalFrame> replayed;
  NAF_REQUIRE(journal.replay(replayed, 8, 1u << 20).is_ok());
  NAF_CHECK_EQ(replayed.size(), std::size_t{1});
  NAF_CHECK(replayed[0].kind == naf::JournalRecordKind::AuditMark);
  NAF_REQUIRE(journal.close().is_ok());
}

NAF_TEST(journal, replay_bounds_are_enforced) {
  TempDir temp("journal-bounds");
  const auto path = temp.file("admission.journal");
  naf::Journal journal(path);
  NAF_REQUIRE(journal.open().is_ok());
  for (int i = 0; i < 10; ++i) {
    NAF_REQUIRE(journal.append(naf::JournalRecordKind::AuditMark, payload_of("x")).is_ok());
  }
  std::vector<naf::JournalFrame> frames;
  const naf::Status bounded = journal.replay(frames, 4, 1u << 20);
  NAF_CHECK(!bounded.is_ok());
  NAF_CHECK(bounded.code() == naf::StatusCode::OversizedInput);

  const naf::Status bytes_bounded = journal.replay(frames, 100, 8);
  NAF_CHECK(!bytes_bounded.is_ok());
  NAF_REQUIRE(journal.close().is_ok());
}

NAF_TEST(journal, frame_codec_rejects_structural_damage) {
  const std::string body = "payload";
  const naf::ByteBuffer frame =
      naf::encode_frame(naf::JournalRecordKind::AuditMark, 5, naf::as_bytes(body));
  naf::JournalFrame decoded;
  NAF_CHECK(naf::decode_frame(frame, decoded, naf::limits::max_journal_payload));
  NAF_CHECK_U64(decoded.sequence, 5);
  NAF_CHECK(decoded.kind == naf::JournalRecordKind::AuditMark);

  NAF_CHECK(!naf::decode_frame(std::span<const std::byte>(frame.data(), frame.size() - 1), decoded,
                               naf::limits::max_journal_payload));
  NAF_CHECK(!naf::decode_frame(std::span<const std::byte>(frame.data(), 4), decoded,
                               naf::limits::max_journal_payload));
  NAF_CHECK(!naf::decode_frame(frame, decoded, 2));
  naf::ByteBuffer wrong_version = frame;
  wrong_version[4] = std::byte{0x7F};
  NAF_CHECK(!naf::decode_frame(wrong_version, decoded, naf::limits::max_journal_payload));
}

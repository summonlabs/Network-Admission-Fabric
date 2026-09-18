// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "naf/naf.hpp"
#include "support/test_framework.hpp"

using namespace naftest;

NAF_TEST(checked, multiplication_is_exact) {
  const naf::U128 small = naf::mul_u64(0xFFFFFFFFull, 0xFFFFFFFFull);
  NAF_CHECK_U64(small.hi, 0);
  NAF_CHECK_U64(small.lo, 0xFFFFFFFE00000001ull);

  const naf::U128 big = naf::mul_u64(~std::uint64_t{0}, ~std::uint64_t{0});
  NAF_CHECK_U64(big.hi, 0xFFFFFFFFFFFFFFFEull);
  NAF_CHECK_U64(big.lo, 0x0000000000000001ull);

  // The portable and the intrinsic paths must agree.
  const naf::U128 portable = naf::mul_u64_portable(0x123456789ABCDEFull, 0xFEDCBA987654321ull);
  const naf::U128 intrinsic = naf::mul_u64(0x123456789ABCDEFull, 0xFEDCBA987654321ull);
  NAF_CHECK_U64(portable.hi, intrinsic.hi);
  NAF_CHECK_U64(portable.lo, intrinsic.lo);
}

NAF_TEST(checked, division_detects_overflow_and_zero) {
  std::uint64_t q = 0;
  std::uint64_t r = 0;
  NAF_CHECK(!naf::divmod_u128(naf::U128{0, 10}, 0, q, r));
  NAF_CHECK(!naf::divmod_u128(naf::U128{2, 0}, 1, q, r));
  NAF_CHECK(!naf::divmod_u128(naf::U128{5, 0}, 5, q, r));
  NAF_CHECK(naf::divmod_u128(naf::U128{0, 100}, 7, q, r));
  NAF_CHECK_U64(q, 14);
  NAF_CHECK_U64(r, 2);
  NAF_CHECK(naf::divmod_u128(naf::U128{1, 0}, 2, q, r));
  NAF_CHECK_U64(q, 0x8000000000000000ull);
  NAF_CHECK_U64(r, 0);

  // The portable path must agree with the intrinsic path.
  std::uint64_t pq = 0;
  std::uint64_t pr = 0;
  std::uint64_t iq = 0;
  std::uint64_t ir = 0;
  const naf::U128 value{0xFFFFFFFFull, 0x1234567890ABCDEFull};
  NAF_CHECK(naf::divmod_u128_portable(value, 0x987654321ull, pq, pr));
  NAF_CHECK(naf::divmod_u128(value, 0x987654321ull, iq, ir));
  NAF_CHECK_U64(pq, iq);
  NAF_CHECK_U64(pr, ir);
}

NAF_TEST(checked, mul_div_is_checked) {
  std::uint64_t out = 0;
  NAF_CHECK(!naf::mul_div_floor(1, 1, 0, out));
  NAF_CHECK(naf::mul_div_floor(1000, 50, 1000, out));
  NAF_CHECK_U64(out, 50);
  NAF_CHECK(naf::mul_div_floor(~std::uint64_t{0}, 1000, 1000, out));
  NAF_CHECK_U64(out, ~std::uint64_t{0});
  NAF_CHECK(!naf::mul_div_floor(~std::uint64_t{0}, 2, 1, out));

  NAF_CHECK(naf::mul_div_ceil(1001, 1, 1000, out));
  NAF_CHECK_U64(out, 2);
  NAF_CHECK(naf::mul_div_ceil(1000, 1, 1000, out));
  NAF_CHECK_U64(out, 1);
}

NAF_TEST(checked, saturating_helpers_never_wrap) {
  NAF_CHECK_U64(naf::sat_add(~std::uint64_t{0}, 1), ~std::uint64_t{0});
  NAF_CHECK_U64(naf::sat_sub(0, 1), 0);
  NAF_CHECK_U64(naf::sat_sub(5, 3), 2);
  NAF_CHECK_U64(naf::sat_mul_div_floor(~std::uint64_t{0}, 2, 1), ~std::uint64_t{0});

  std::uint64_t out = 0;
  NAF_CHECK(!naf::add_u64(~std::uint64_t{0}, 1, out));
  NAF_CHECK(!naf::sub_u64(0, 1, out));
  NAF_CHECK(naf::sub_u64(1, 1, out));
  NAF_CHECK_U64(out, 0);
}

NAF_TEST(checked, sum_latch_reports_overflow) {
  naf::SumLatch latch;
  latch.add(10);
  latch.add(20);
  NAF_CHECK(!latch.overflowed());
  NAF_CHECK_U64(latch.total(), 30);
  latch.add(~std::uint64_t{0});
  NAF_CHECK(latch.overflowed());
  NAF_CHECK_U64(latch.total(), 30);
  NAF_CHECK_U64(latch.saturated_total(), ~std::uint64_t{0});
}

NAF_TEST(identity, generations_are_guarded) {
  naf::Generation generation = naf::Generation::first();
  NAF_CHECK(generation.is_well_formed());
  NAF_CHECK(generation.advance());
  NAF_CHECK_U64(generation.value(), 2);

  naf::Generation unknown;
  NAF_CHECK(unknown.is_unknown());
  NAF_CHECK(!unknown.is_well_formed());

  naf::Generation top = naf::Generation::from_value(naf::limits::max_generation);
  NAF_CHECK(top.is_max());
  NAF_CHECK(!top.is_well_formed());
  NAF_CHECK(!top.advance());
  NAF_CHECK_U64(top.value(), naf::limits::max_generation);
  NAF_CHECK_U64(top.next().value(), naf::limits::max_generation);

  naf::FabricEpoch epoch;
  NAF_CHECK(epoch.is_none());
  NAF_CHECK(!epoch.is_well_formed());
  epoch = naf::FabricEpoch::first();
  NAF_CHECK(epoch.is_well_formed());
  NAF_CHECK(epoch.advance());
  NAF_CHECK_U64(epoch.value(), 2);
}

NAF_TEST(identity, typed_ids_compare_and_order) {
  const naf::ResourceId a = naf::ResourceId::from_value(7);
  const naf::ResourceId b = naf::ResourceId::from_value(9);
  NAF_CHECK(a < b);
  NAF_CHECK(a != b);
  NAF_CHECK(a.is_known());
  NAF_CHECK(naf::ResourceId{}.is_unknown());
  static_assert(!std::is_convertible_v<std::uint64_t, naf::ResourceId>);
  static_assert(!std::is_same_v<naf::ResourceId, naf::PathId>);
}

NAF_TEST(identity, provenance_renders) {
  naf::Provenance provenance;
  provenance.publisher = naf::PublisherId::from_value(3);
  provenance.boot = naf::BootId::from_value(4);
  provenance.incarnation = naf::CoordinatorIncarnation::from_value(5);
  provenance.epoch = naf::FabricEpoch::from_value(6);
  provenance.attempt = naf::AttemptId::from_value(7);
  provenance.sequence = 8;
  provenance.origin = naf::OriginKind::Claimant;
  const std::string text = naf::render_provenance(provenance);
  NAF_CHECK(text.find("publisher=3") != std::string::npos);
  NAF_CHECK(text.find("epoch=6") != std::string::npos);
  NAF_CHECK(text.find("origin=claimant") != std::string::npos);
}

NAF_TEST(authority, vector_digest_is_order_insensitive) {
  naf::AuthorityVector first;
  NAF_CHECK(first.add(naf::AuthorityKind::Policy, 1, naf::Generation::from_value(2), naf::FabricEpoch::first()));
  NAF_CHECK(first.add(naf::AuthorityKind::CapacitySnapshot, 4, naf::Generation::from_value(7), naf::FabricEpoch::first()));

  naf::AuthorityVector second;
  NAF_CHECK(second.add(naf::AuthorityKind::CapacitySnapshot, 4, naf::Generation::from_value(7), naf::FabricEpoch::first()));
  NAF_CHECK(second.add(naf::AuthorityKind::Policy, 1, naf::Generation::from_value(2), naf::FabricEpoch::first()));

  NAF_CHECK_U64(first.digest(), second.digest());
  NAF_CHECK_EQ(first.size(), second.size());

  // Re-adding the same subject merges rather than duplicating.
  NAF_CHECK(first.add(naf::AuthorityKind::Policy, 1, naf::Generation::from_value(3), naf::FabricEpoch::first()));
  NAF_CHECK_EQ(first.size(), std::size_t{2});
  const naf::AuthorityRef* ref = first.find(naf::AuthorityKind::Policy, 1);
  NAF_REQUIRE(ref != nullptr);
  NAF_CHECK_U64(ref->generation.value(), 3);
  NAF_CHECK(first.find(naf::AuthorityKind::Policy, 99) == nullptr);

  // A different generation changes the digest.
  NAF_CHECK(first.digest() != second.digest());
}

NAF_TEST(authority, vector_is_bounded) {
  naf::AuthorityVector vector;
  for (std::size_t i = 0; i < naf::limits::max_authority_refs + 10; ++i) {
    vector.add(naf::AuthorityKind::ResourceCapacity, i + 1, naf::Generation::first(), naf::FabricEpoch::first());
  }
  NAF_CHECK_U64(vector.size(), naf::limits::max_authority_refs);
  NAF_CHECK(vector.truncated());
}

NAF_TEST(codec, byte_roundtrip_and_bounds) {
  naf::ByteWriter writer;
  writer.u8(0x12);
  writer.u16(0x3456);
  writer.u32(0x789ABCDEu);
  writer.u64(0x0123456789ABCDEFull);
  writer.boolean(true);
  NAF_CHECK(writer.text("hello"));
  NAF_CHECK(!writer.text(std::string(naf::limits::max_string_bytes + 1, 'x')));

  naf::ByteReader reader(writer.buffer());
  std::uint8_t u8 = 0;
  std::uint16_t u16 = 0;
  std::uint32_t u32 = 0;
  std::uint64_t u64 = 0;
  bool flag = false;
  std::string text;
  NAF_CHECK(reader.u8(u8));
  NAF_CHECK_U64(u8, 0x12);
  NAF_CHECK(reader.u16(u16));
  NAF_CHECK_U64(u16, 0x3456);
  NAF_CHECK(reader.u32(u32));
  NAF_CHECK_U64(u32, 0x789ABCDEu);
  NAF_CHECK(reader.u64(u64));
  NAF_CHECK_U64(u64, 0x0123456789ABCDEFull);
  NAF_CHECK(reader.boolean(flag));
  NAF_CHECK(flag);
  NAF_CHECK(reader.text(text));
  NAF_CHECK(text == "hello");
  NAF_CHECK(reader.exhausted());

  // Reading past the end must fail rather than return a default.
  naf::ByteReader empty(std::span<const std::byte>{});
  NAF_CHECK(!empty.u8(u8));
  NAF_CHECK(!empty.u64(u64));
  NAF_CHECK(!empty.text(text));

  // A boolean must be exactly 0 or 1.
  const std::byte bad[] = {std::byte{2}};
  naf::ByteReader strict(std::span<const std::byte>(bad, 1));
  NAF_CHECK(!strict.boolean(flag));
}

NAF_TEST(codec, crc32c_matches_the_known_vector) {
  const std::string text = "123456789";
  NAF_CHECK_U64(naf::crc32c(naf::as_bytes(text)), 0xE3069283u);
  NAF_CHECK_U64(naf::crc32c(std::span<const std::byte>{}), 0u);
}

NAF_TEST(codec, fingerprint_is_stable_and_field_sensitive) {
  naf::AdmissionRequest request;
  request.request = naf::AdmissionRequestId::from_value(1);
  request.demand = naf::DemandId::from_value(2);
  request.attempt = naf::AttemptId::from_value(3);
  request.demand_generation = naf::Generation::first();
  request.qos = naf::QoSClassId::from_value(1);
  request.qos_generation = naf::Generation::first();
  request.priority = naf::PriorityClassId::from_value(1);
  request.priority_generation = naf::Generation::first();
  request.rate.minimum = naf::Rate::from_value(10);
  request.rate.desired = naf::Rate::from_value(20);
  request.rate.maximum = naf::Rate::from_value(30);

  const std::uint64_t first = naf::fingerprint(request);
  const std::uint64_t second = naf::fingerprint(request);
  NAF_CHECK_U64(first, second);

  // Provenance is deliberately excluded: a retry after an ambiguous
  // acknowledgement may come from a restarted claimant.
  request.provenance.boot = naf::BootId::from_value(999);
  request.provenance.origin = naf::OriginKind::Replay;
  NAF_CHECK_U64(naf::fingerprint(request), first);

  request.rate.desired = naf::Rate::from_value(21);
  NAF_CHECK(naf::fingerprint(request) != first);
}

NAF_TEST(codec, wire_frame_roundtrip_and_rejections) {
  const std::string body = "admission-payload";
  const naf::ByteBuffer frame =
      naf::ipc::encode_wire_frame(naf::ipc::MessageKind::Admit, 7, naf::as_bytes(body));
  NAF_CHECK_U64(frame.size(), naf::ipc::wire_header_bytes + body.size());

  naf::ipc::WireHeader header;
  NAF_CHECK(naf::ipc::decode_wire_header(std::span<const std::byte>(frame.data(), naf::ipc::wire_header_bytes),
                                         header, naf::limits::max_frame_bytes));
  NAF_CHECK(header.kind == naf::ipc::MessageKind::Admit);
  NAF_CHECK_U64(header.flags, 7);
  NAF_CHECK_U64(header.length, body.size());

  // Oversized declaration is refused.
  NAF_CHECK(!naf::ipc::decode_wire_header(
      std::span<const std::byte>(frame.data(), naf::ipc::wire_header_bytes), header, 4));
  // Corrupted magic is refused.
  naf::ByteBuffer broken = frame;
  broken[0] = std::byte{0};
  NAF_CHECK(!naf::ipc::decode_wire_header(
      std::span<const std::byte>(broken.data(), naf::ipc::wire_header_bytes), header,
      naf::limits::max_frame_bytes));

  naf::ipc::MemoryStream stream;
  NAF_CHECK(stream.feed(frame).is_ok());
  auto decoded = naf::ipc::read_message(stream, naf::limits::max_frame_bytes);
  NAF_REQUIRE(decoded.has_value());
  NAF_CHECK(decoded.value().kind == naf::ipc::MessageKind::Admit);
  NAF_CHECK_U64(decoded.value().payload.size(), body.size());
  NAF_CHECK(std::memcmp(decoded.value().payload.data(), body.data(), body.size()) == 0);
}

NAF_TEST(codec, wire_frame_refuses_truncation_and_corruption) {
  const std::string body = "payload";
  const naf::ByteBuffer frame =
      naf::ipc::encode_wire_frame(naf::ipc::MessageKind::Decision, 0, naf::as_bytes(body));

  {
    naf::ipc::MemoryStream stream;
    NAF_CHECK(stream.feed(std::span<const std::byte>(frame.data(), frame.size() - 2)).is_ok());
    stream.close_peer();
    auto decoded = naf::ipc::read_message(stream, naf::limits::max_frame_bytes);
    NAF_CHECK(!decoded.has_value());
    NAF_CHECK(decoded.status().code() == naf::StatusCode::TruncatedFrame);
  }
  {
    naf::ipc::MemoryStream stream;
    naf::ByteBuffer corrupt = frame;
    corrupt[corrupt.size() - 1] = static_cast<std::byte>(static_cast<std::uint8_t>(corrupt[corrupt.size() - 1]) ^ 0x5Au);
    NAF_CHECK(stream.feed(corrupt).is_ok());
    auto decoded = naf::ipc::read_message(stream, naf::limits::max_frame_bytes);
    NAF_CHECK(!decoded.has_value());
    NAF_CHECK(decoded.status().code() == naf::StatusCode::CorruptJournal);
  }
  {
    naf::ipc::MemoryStream stream;
    stream.close_peer();
    auto decoded = naf::ipc::read_message(stream, naf::limits::max_frame_bytes);
    NAF_CHECK(!decoded.has_value());
    NAF_CHECK(decoded.status().code() == naf::StatusCode::TransportFailure);
    NAF_CHECK(decoded.status().message().empty());
  }
}

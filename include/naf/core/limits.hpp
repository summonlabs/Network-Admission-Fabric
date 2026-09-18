// Network Admission Fabric - hard resource bounds.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_CORE_LIMITS_HPP
#define NAF_CORE_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace naf::limits {

// Every externally influenced collection is bounded. Inputs that exceed these
// bounds are rejected as oversized rather than truncated, so that a decision can
// never be produced from a partially parsed request.

inline constexpr std::size_t max_resources = 4096;
inline constexpr std::size_t max_resource_bindings = 1024;
inline constexpr std::size_t max_path_candidates = 256;
inline constexpr std::size_t max_path_resources = 512;
inline constexpr std::size_t max_paths = 4096;
inline constexpr std::size_t max_obligations = 16384;
inline constexpr std::size_t max_reservations = 8192;
inline constexpr std::size_t max_qos_classes = 256;
inline constexpr std::size_t max_priority_classes = 64;
inline constexpr std::size_t max_allowed_classes = 256;
inline constexpr std::size_t max_reservation_refs = 64;
inline constexpr std::size_t max_explanation_constraints = 48;
inline constexpr std::size_t max_explanation_bytes = 8192;
inline constexpr std::size_t max_effective_resources = 64;
inline constexpr std::size_t max_authority_refs = 320;
inline constexpr std::size_t max_ledger_entries = 65536;
inline constexpr std::size_t max_attempt_records = 65536;
inline constexpr std::size_t max_decision_records = 65536;
inline constexpr std::size_t max_session_records = 1024;
inline constexpr std::size_t max_string_bytes = 96;
inline constexpr std::size_t max_detail_bytes = 192;

inline constexpr std::uint32_t max_frame_bytes = 1u << 20;        // 1 MiB
inline constexpr std::uint32_t min_frame_bytes = 24;
inline constexpr std::uint32_t max_journal_payload = 1u << 16;    // 64 KiB
inline constexpr std::uint64_t max_journal_records = 1u << 20;    // compaction trigger
inline constexpr std::uint64_t journal_compact_at_records = 1u << 14;

inline constexpr std::uint64_t max_rate_bits_per_second = ~std::uint64_t{0};

// Generation 0 is reserved and always means UNKNOWN. A generation of
// max_generation is malformed and refused.
inline constexpr std::uint64_t unknown_generation = 0;
inline constexpr std::uint64_t max_generation = ~std::uint64_t{0};

inline constexpr std::uint64_t unknown_epoch = 0;
inline constexpr std::uint64_t first_epoch = 1;

}  // namespace naf::limits

#endif  // NAF_CORE_LIMITS_HPP

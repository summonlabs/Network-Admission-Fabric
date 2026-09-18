// Network Admission Fabric - version and build surface.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef NAF_VERSION_HPP
#define NAF_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace naf {

inline constexpr int version_major = 1;
inline constexpr int version_minor = 0;
inline constexpr int version_patch = 0;

/// Monotonic wire/persistence format revision. Bumped whenever any persisted
/// or transmitted frame layout changes in an incompatible way.
inline constexpr std::uint16_t format_revision = 1;

/// Identifies the runtime. Persisted alongside durable state so that a foreign
/// or newer layout is refused rather than misinterpreted.
inline constexpr std::string_view product_name = "network-admission-fabric";
inline constexpr std::string_view version_string = "1.0.0";

}  // namespace naf

#endif  // NAF_VERSION_HPP

// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "naf/model/quantity.hpp"

#include <string>

#include "core/text.hpp"

namespace naf {
namespace {

}  // namespace

std::string render_rate(Rate rate) {
  std::string out;
  naf::text::append_u64(out, rate.value());
  out += " bps";
  return out;
}

std::string render_latency(Latency latency) {
  std::string out;
  naf::text::append_u64(out, latency.value());
  out += " us";
  return out;
}

}  // namespace naf

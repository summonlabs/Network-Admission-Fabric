// Network Admission Fabric - umbrella header.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Owns the yes/defer/no decision for new traffic entering governed fabric
// capacity. Adjacent runtimes own topology truth, path legality and computation,
// route lifecycle, reservation lifecycle, global TE optimisation, bandwidth
// arbitration, scheduling, placement, rate enforcement, QoS and priority
// definition, telemetry and device programming. Their facts arrive here as
// authoritative, generation-stamped inputs; UNKNOWN never becomes authority.
#ifndef NAF_NAF_HPP
#define NAF_NAF_HPP

#include "naf/version.hpp"

#include "naf/core/bytes.hpp"
#include "naf/core/checked.hpp"
#include "naf/core/crc32c.hpp"
#include "naf/core/file.hpp"
#include "naf/core/hash.hpp"
#include "naf/core/identity.hpp"
#include "naf/core/limits.hpp"
#include "naf/core/status.hpp"

#include "naf/model/policy.hpp"
#include "naf/model/quantity.hpp"
#include "naf/model/request.hpp"
#include "naf/model/state.hpp"

#include "naf/authority/authority.hpp"

#include "naf/engine/decision.hpp"
#include "naf/engine/engine.hpp"
#include "naf/engine/history.hpp"
#include "naf/engine/ledger.hpp"

#include "naf/durable/journal.hpp"
#include "naf/durable/records.hpp"
#include "naf/durable/recovery.hpp"

#include "naf/ipc/client.hpp"
#include "naf/ipc/endpoint.hpp"
#include "naf/ipc/frame.hpp"
#include "naf/ipc/protocol.hpp"
#include "naf/ipc/server.hpp"
#include "naf/ipc/stream.hpp"

#endif  // NAF_NAF_HPP

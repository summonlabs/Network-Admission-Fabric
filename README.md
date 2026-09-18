# Network Admission Fabric

Open-source, vendor-neutral C++20 runtime for generation-bound network admission
decisions across bandwidth, reservations, path capacity, service obligations,
policy, headroom, and stale-state authority.

**Core question.** Given a proposed traffic demand, current authoritative
capacity, reservations, service obligations, path and resource generations,
headroom, policy, and existing admitted load: may this traffic enter now without
violating stronger obligations, and when must admission be deferred, rejected,
revalidated, revoked, or fenced as stale?

**What this is not.** Admission is not a reservation, not a route, not a
placement and not a forwarding command.

## Systems boundary

This runtime owns the yes/defer/no decision for new traffic entering governed
fabric capacity.

It does **not** own topology truth, path legality or computation, route
lifecycle, global TE optimisation, reservation lifecycle, instantaneous
bandwidth arbitration, flow scheduling, path placement, rate enforcement,
priority or QoS definition, packet scheduling, congestion control, telemetry, or
device programming.

Inputs from adjacent runtimes are authoritative facts. Each arrives stamped with
its own identity, generation and fencing epoch. Admission never invents,
repairs, extrapolates or silently refreshes one of them. **UNKNOWN is a
first-class value and never becomes positive authority.**

## Building

Requirements: CMake 3.20+, a C++20 compiler (MSVC 19.3x, GCC 12+, Clang 15+),
and nothing else. There are no third-party dependencies.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| @B@NAF_BUILD_TESTS@B@ | ON (top level) | Build the test suite |
| @B@NAF_BUILD_TOOLS@B@ | ON (top level) | Build @B@nafd@B@ and @B@nafctl@B@ |
| @B@NAF_BUILD_BENCHMARKS@B@ | ON (top level) | Build the synthetic benchmark |
| @B@NAF_WARNINGS_AS_ERRORS@B@ | ON | @B@/W4 /WX@B@ on MSVC, @B@-Wall -Wextra -Werror@B@ elsewhere |
| @B@NAF_ENABLE_ASAN@B@ | OFF | AddressSanitizer where the toolchain supports it |

## Installing and consuming

```
cmake --install build --prefix /some/prefix
```

The install exports a CMake package. A downstream project only needs:

```
find_package(NAF 1.0 REQUIRED)
target_link_libraries(my_target PRIVATE NAF::naf)
```

@B@examples/consumer@B@ is a complete, separate downstream project that uses only
the installed package. It publishes an authoritative capacity observation, asks
for a decision, checks accounting closure and confirms that a stale generation is
refused:

```
cmake -S examples/consumer -B consumer-build -DCMAKE_PREFIX_PATH=/some/prefix
cmake --build consumer-build
./consumer-build/naf_consumer
```

## Model

Every identity is a distinct type produced from a distinct tag, so
cross-assigning one kind for another is a compile error. Value 0 is reserved and
means UNKNOWN.

- Identities: @B@AdmissionRequestId@B@, @B@AdmissionDecisionId@B@, @B@DemandId@B@,
  @B@ResourceId@B@, @B@PathId@B@, @B@CapacitySnapshotId@B@,
  @B@ReservationSnapshotId@B@, @B@ReservationId@B@, @B@QoSClassId@B@,
  @B@PriorityClassId@B@, @B@PolicyId@B@, @B@PublisherId@B@, @B@BootId@B@,
  @B@SessionNonce@B@, @B@AttemptId@B@, @B@FairnessGroupId@B@, @B@AuditSequence@B@,
  @B@TenantId@B@, @B@TraceId@B@.
- Revisions: @B@Generation@B@ (0 = UNKNOWN, all-ones = malformed),
  @B@FabricEpoch@B@, @B@CoordinatorIncarnation@B@.
- Provenance: publisher, boot, coordinator incarnation, epoch, attempt, trace,
  sequence and origin kind, carried by every externally supplied input.

A request carries a @B@RateBounds@B@ (minimum, desired, maximum), a QoS class, a
priority class, a latency budget, authorized resource bindings, authorized path
candidates, reservation references, fairness group, tenant, preemptibility, an
effective tick and interval, and the exact @B@AuthorityExpectation@B@ the
claimant observed.

Authority inputs, each published by the runtime that owns that truth:

- @B@CapacitySnapshot@B@: per-resource usable capacity and mandatory headroom,
  plus an explicit evidence state.
- @B@ReservationSnapshot@B@: protected obligations with reservation identity,
  generation, priority, scheduled release tick and inviolability.
- @B@PathCatalog@B@: authorized paths with state, usable capacity, latency and
  the authoritative traversal set.
- @B@QoSClassCatalog@B@, @B@PriorityClassCatalog@B@.
- @B@AdmissionPolicy@B@: headroom floor and permille, degradation permission,
  preemptibility, path binding and reservation reference requirements,
  contention action, allowed class sets, deferral horizon and explanation bounds.

Every accepted snapshot becomes current authority only if it validates: ordered,
duplicate-free, consistent evidence, non-overflowing obligation sums and
well-formed generations. Publishing an older generation is refused as stale.
Publishing the same generation with different decision-relevant content is
refused as identity reuse; the fencing epoch and observation tick are metadata
this coordinator re-stamps and are excluded from that comparison.

## Outcomes

| Outcome | Meaning |
| --- | --- |
| @B@ADMIT@B@ | The demand enters at the requested ceiling |
| @B@ADMIT_DEGRADED@B@ | The demand enters below its desired rate but at or above its minimum |
| @B@DEFER@B@ | Capacity is expected to free up inside the deferral horizon |
| @B@REJECT_CAPACITY@B@ | Authoritative usable capacity, headroom or admitted load is insufficient |
| @B@REJECT_POLICY@B@ | Policy forbids the request, or the request violates its own contract |
| @B@REJECT_OBLIGATION@B@ | Protected obligations block, or the demand asked to preempt one |
| @B@REJECT_PATH@B@ | No authorized path can carry the demand |
| @B@REJECT_QoS@B@ | The QoS class does not permit the demand |
| @B@STALE_INPUT@B@ | The claimant relied on superseded or UNKNOWN evidence |
| @B@CONFLICTING_INPUT@B@ | An identity was reused for different content, or an epoch reaches forward |
| @B@FENCED_CLAIMANT@B@ | The claimant belongs to a superseded session or coordinator incarnation |

Every result carries an @B@AuthorityVector@B@: the exact generations that
justified it (fence, snapshots, policy, catalogs, and the specific resources,
reservations and path the decision actually depended on), plus bounded
@B@BindingConstraint@B@ entries with the numbers and revisions behind the verdict,
and a bounded effective-capacity summary. Explanations are bounded by policy in
both constraint count and bytes; overflow is recorded as a truncation count
rather than growing the payload.

## Decision procedure

1. **Contract.** Structurally invalid requests are refused, never guessed.
2. **Fencing.** The claim's epoch, coordinator incarnation, session nonce,
   publisher and boot identity must match a live session in this incarnation.
3. **Idempotency.** The same (demand, attempt, fingerprint) is a replay and
   returns the original decision. Reused identity with different content is
   @B@CONFLICTING_INPUT@B@.
4. **Readiness.** While an authority the decision needs has not published,
   admission refuses and says which one.
5. **Authority binding.** Every stated generation must equal current authority.
6. **Policy, QoS and priority.** Class permissions, class rate floor and
   ceiling, latency budget and preemptibility.
7. **Path selection.** The first authorized candidate in declared preference
   order that is up, generation-matched, latency-capable and capacity-capable. A
   path is only ever chosen from the declared candidates; admission never
   computes, invents or reassigns one.
8. **Accounting.** For every targeted resource:
   @B@admitted + protected obligations + headroom <= authoritative usable capacity@B@,
   evaluated with checked arithmetic that saturates instead of wrapping.
9. **Verdict.** The grant is the smallest ceiling across the targeted resources
   and the selected path, clamped to the requested maximum.

## Invariants

- Admitted load plus protected obligations plus headroom never exceeds
  authoritative usable capacity.
- No admitted demand relies on a stale path, resource, policy, capacity or
  reservation generation.
- UNKNOWN capacity, path, obligation or catalog state cannot authorize admission.
- Duplicate identical attempts are idempotent; conflicting identity reuse is
  refused.
- Admission never creates a reservation and never chooses a path owned elsewhere.
- Arithmetic cannot overflow and cannot go negative.
- Generation advance invalidates dependent decisions.

The first invariant is maintained, not merely reported: whenever capacity,
reservations or policy change, an accounting-enforcement pass revokes owned
admissions newest-first until the invariant holds again, and records why. Where
the authoritative inputs are themselves contradictory (protected obligations plus
headroom alone exceed usable capacity), admission holds nothing there, refuses
everything that targets it, and reports the resource as infeasible rather than
pretending the state is fine.

## Durability

Only state that admission actually owns is persisted:

- owned policy and configuration;
- durable admission history;
- provenance and audit sequence;
- fencing: the coordinator epoch.

Capacity, reservation and path snapshots belong to adjacent runtimes and are
**never** persisted here. After a restart they are UNKNOWN until their owner
republishes them, so admission refuses rather than reusing yesterday's numbers.

The journal is a versioned, integrity-checked, append-only frame log with a
CRC-32C per frame (header and payload) and real @B@fsync@B@ before any
acknowledgement. Admission commits are two-phase: a durable intent record, then
the work, then a durable commit record. Recovery classifies explicitly:

| Recovered state | Treatment |
| --- | --- |
| Configuration and policy | Restored |
| Committed admissions | Load restored, marked as requiring revalidation |
| Intent without commit | **Ambiguous**: not restored, reported, claimant must revalidate |
| Explicit releases and revocations | Applied |
| Live sessions | **Never restored**; every previous claimant is fenced |
| Epoch | Advanced by construction; a journal from a later incarnation is refused |
| Torn tail | Truncated back to the last intact frame and reported |
| Corrupt interior frame | Recovery refused; damaged history is never reinterpreted |

A journal is owned by exactly one engine at a time; the engine holds a single
handle for scanning, appending and repair.

Durable growth is bounded: decision history retires oldest-first within a
configured window (never retiring a record whose grant is still live), and the
journal is compacted automatically once it passes a frame threshold, rewriting
only the state that must survive a restart.

## Tools and transport

@B@nafd@B@ is the coordinator daemon and @B@nafctl@B@ is the client. Both speak a
framed, length-prefixed, CRC-checked protocol over a real OS transport: a Windows
named pipe or a POSIX @B@AF_UNIX@B@ socket, with a stdio mode for a parent process
that drives the coordinator directly. Frames are bounded; a truncated, oversized,
mis-versioned or checksum-failing frame is refused and never partially applied.

```
nafd --endpoint my-coordinator --state-dir ./state --profile demo &
nafctl --endpoint my-coordinator status
nafctl --endpoint my-coordinator admit --min 1000 --desired 4000 --max 5000
```

@B@--profile demo@B@ publishes a **SYNTHETIC** development authority set. It is
not a network model.

The named-pipe transport uses overlapped I/O, which is what makes shutdown
possible: @B@CancelIoEx@B@ genuinely aborts a pending read or connect, so a blocked
worker is released without any thread closing a handle another thread is using.
Synchronous pipe I/O cannot be cancelled at all, and a listener that holds a lock
across a blocking accept deadlocks its own shutdown.

## Validation and proof surface

Run @B@ctest@B@ for the whole suite. Labeling is deliberate and exact.

**REAL** (validated on this machine, Windows 11 / MSVC 19.44 x64):

- 105 tests across nine executables: unit, engine behaviour, seeded randomized
  property, adversarial, concurrency, persistence and multiprocess.
- Real multiprocess fencing: @B@nafd@B@ is spawned as a separate OS process and
  driven over a real named pipe; it is killed abruptly with @B@TerminateProcess@B@,
  restarted against the same state directory, and the restarted incarnation's
  epoch advance, session fencing, incarnation fencing, forward-epoch conflict and
  cross-coordinator isolation are all verified end to end. The pre-restart attempt
  replays from durable history with its original decision identity.
- Real crash-shaped persistence tests: a torn tail, a corrupt interior frame, an
  intent without a commit, a journal from a later incarnation and a journal from a
  foreign format revision.
- Real concurrency: eight threads admitting against one engine, capacity
  reduction racing against admission, concurrent revocation of the same decision
  from eight threads, server shutdown with an idle client attached, and a
  truncated request frame over a real transport.
- Install and downstream consumption through @B@find_package(NAF 1.0)@B@.
- Release and Debug builds under MSVC @B@/W4 /WX@B@ (@B@/permissive-@B@,
  @B@/Zc:__cplusplus@B@, @B@/utf-8@B@): zero warnings in both configurations.
- **AddressSanitizer**: the whole suite was rebuilt with
  @B@-DNAF_ENABLE_ASAN=ON@B@ and run under the MSVC x64 ASan runtime — 105 tests,
  zero ASan reports. Two environment notes, stated precisely: the ASan runtime
  DLL is not present in the Visual Studio Community install this project builds
  with and had to be taken from the co-installed Build Tools toolset of the same
  version (14.44.35207), and @B@detect_leaks@B@ is not supported on this platform
  and was therefore not exercised.
- **Static analysis**: @B@-DNAF_ENABLE_STATIC_ANALYSIS=ON@B@ runs MSVC
  @B@/analyze@B@. The library and tools build clean; every first-party finding
  raised by the analyser was fixed rather than suppressed.

**SYNTHETIC** (generated populations, no physical hardware involved):

- Every benchmark population: demand counts, resource counts, obligation
  densities and contention levels are generated numbers.
- The @B@--profile demo@B@ authority set used by @B@nafd@B@.

**UNSUPPORTED / NOT VALIDATED HERE** (stated precisely rather than implied):

- Multi-node, multi-switch, RDMA, NVLink, optical, NIC, DPU and physical-network
  behaviour. Nothing in this repository measures or claims any of it.
- The POSIX @B@AF_UNIX@B@ transport path is implemented and compiles but was not
  exercised on this machine, which is Windows-only.
- AddressSanitizer leak detection: unsupported on Windows, so memory leaks were
  not checked under ASan (addressability, use-after-free, buffer overflow and
  stack-use-after-return were).
- The mingw/GCC and Clang warning sets in @B@CMakeLists.txt@B@ are not exercised
  here.

## Benchmark

@B@naf_bench@B@ measures **completed admission decisions**, not enqueue latency.
All populations are SYNTHETIC.

```
cmake --build build --config Release --target naf_bench
./build/bench/Release/naf_bench
```

Representative results (Windows 11, MSVC 19.44, x64, Release, single process):

| Scenario (SYNTHETIC) | Decisions | Throughput | p50 | p99 |
| --- | --- | --- | --- | --- |
| single resource, low contention | 300,000 | 383,309 /s | 2.1 us | 4.6 us |
| 16 resources, 25% obligation density | 300,000 | 356,430 /s | 2.3 us | 4.3 us |
| 256 resources, 50% obligation density | 200,000 | 303,379 /s | 2.6 us | 5.2 us |
| saturated contention | 300,000 | 367,975 /s | 2.3 us | 4.2 us |
| durable journal (SYNTHETIC+DURABLE) | 4,000 | 1,095 /s | 817 us | 1.78 ms |

The non-durable figures are flat in resource count, which is the intended
characteristic: a decision costs what its own evidence costs, not what the fabric
contains. The durable figure is dominated by the cost of truth: every admission
is two @B@fsync@B@ calls before the caller is told anything, and on this machine
that is roughly 0.8 ms per decision. A deployment that needs both throughput and
durability needs a different acknowledgement contract than this one, and this
runtime does not pretend otherwise.

## Known limitations

- A journal is owned by one engine at a time. Two engines in one process cannot
  open the same journal file; the platform denies the second handle.
- Admission is synchronous. There is no batching, pipelining or group commit;
  durable throughput is therefore bounded by fsync latency.
- The in-memory decision cache that serves exact idempotent replays is bounded. A
  replay whose original decision has been retired is reconstructed from the
  durable record, which preserves the outcome, identity, grant and authority
  binding but not the original free-text explanation.
- Runtime counters in @B@FabricStatus@B@ describe the current incarnation; durable
  history is recovered separately and is not folded into them.
- The POSIX endpoint implementation and the non-MSVC warning sets are unverified
  here, as noted above.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

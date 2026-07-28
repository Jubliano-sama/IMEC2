# Architecture Reset Plan

Status: In progress; Stage 4 radio-owner source migration implemented
Date: 2026-07-28
Behavioral effect of the implemented stages: Internal ownership, scheduling,
and focused file boundaries only. Wire format, role behavior, radio timing,
retry policy, LEDs, and power policy are unchanged.

## Decision

Do not perform a big-bang rewrite of the product or replace the connected mesh
protocol while defects are still being diagnosed. Freeze growth in the current
orchestration monoliths, preserve the Mesh Connected Routing Contract, and
replace one state owner at a time behind the existing wire and hardware
boundaries. Delete an old path only after the replacement has native
equivalence, adversarial liveness coverage, exact-role builds, and the required
bench evidence.

This is a controlled rewrite of orchestration, not another sequence of local
patches. The distinction is that every stage has one owner, one typed state
object, one event/action boundary, and a measurable deletion target.

## Why the recurring failures happen

The July file split improved navigation but retained giant C translation units
through textual `.inc` composition:

| Translation unit | Composed lines | Responsibilities mixed together |
|---|---:|---|
| `app_mesh_report.c` | 18,699 | routing, transport, event timing, delivery, RX, gateway contact |
| `app_anchor.c` | 10,921 | commands, survey, gateway control, radio, initialization |
| `mesh_relay.c` | 9,245 | routes, RX, delivery, exact custody |
| `dwm3000_driver.c` | 4,863 | radio lifecycle, frames, I/O, DS-TWR |

The compiler still sees each row as one private-state island, so behavior is
shared through many globals and callbacks instead of module contracts. A fix
can satisfy its local branch while leaving another work owner, retry timer,
generation, deadline, or custody record live. Native coverage is extensive but
historically depended on ignored local shim headers and no full native CI, so a
busy checkout could look green while a fresh clone did not even compile.

`firmware/architecture_boundaries.json` now freezes every existing include
fragment, every oversized source, and the composed totals at their current
ceilings. The checker compares that mutable inventory with an immutable Git
baseline, so the same change cannot add an exception or raise a ceiling. The
gate rejects new include fragments, source-like textual includes, oversized
headers, out-of-root production sources, and oversized new C files. This
records debt; it does not approve the architecture.

The immutable policy object is commit
`4b29225ce1efa4e1731887ab4df806434b63edca`. Release branches and verification
clones must retain that exact commit; squash, rebase, or history pruning that
drops it fails closed because the checker can no longer prove that the mutable
inventory did not relax its original ceilings. An intentional rebaseline uses
two preserved commits: the first is the separately reviewed source/manifest
baseline, and the second updates the checker pin plus its guidance and tests.
Reconstructing a baseline from the candidate checkout would let the same change
approve its own debt growth and is forbidden.

The CMake ownership gate is a conservative static parser. It rejects the
source-discovery, variable, language, vendor-root, and include-root mechanisms
currently supported by this repository, but it is not configured CMake
file-api attestation. A new custom source or compile-option include mechanism
must first gain a failing architecture regression; configured input attestation
remains a separate hardening stage.

## Contract invariants preserved throughout

Every migration stage preserves the current Mesh Connected Routing Contract:

- Channel 5 remains the control and click-preemption lane; Channel 9 remains
  scheduled payload traffic, and connected anchors keep bounded Channel 5
  receive opportunities.
- One logical packet has exactly one custody owner. RF start, hop ACK, gateway
  confirmation, caller terminal state, and durable host admission cannot
  disagree.
- Local click work outranks transit, but accepted transit remains explicit and
  retryable rather than being silently displaced.
- A deferred operation retains an independent liveness owner, immutable
  identity, actual-attempt accounting, a 64-bit absolute deadline, and exactly
  one terminal result.
- Malformed frames, missing routes, capacity exhaustion, partial airtime,
  collisions, stale generations, and persistence failures fail explicitly.
  No seeded route or direct-delivery fallback may conceal a broken path.
- Gateway host admission commits before the ACK that releases upstream custody.

An intentional change to any item above needs a separate decision, explicit
permission, affected-role analysis, compatibility plan, and new test and
hardware gates.

## Target ownership model

Every long-running operation becomes a plain C state machine with one context:

```text
identity and generation
current phase
serialized owner
immutable input or custody snapshot
64-bit absolute deadline
attempt and progress counters
pending action token
last externally visible progress
terminal status and reason
```

The pure machine consumes typed events and emits bounded actions. Zephyr
adapters schedule those actions and return completions carrying the same
generation and token. Work handlers do not contain policy, start nested retry
machines, or mutate another owner's state. No blocking radio operation runs on
the system workqueue.

The extraction boundaries are:

- `firmware/src/gateway_survey_machine.c/.h`: plan, dispatch, observe, rerun,
  abort, and terminal survey transitions without Zephyr work primitives.
  This boundary is implemented; the retired `app_gateway_survey_round` owner
  and its spelling-based tests are deleted.
- `firmware/app/src/app_mesh_route_owner_queue.c/.h`: the existing
  `mesh_route` actor boundary used by survey polling, host retry, result
  timeout, and abort cleanup. This is implemented without adding a workqueue or
  stack.
- `firmware/app/src/app_mesh_radio_owner.c/.h`: the sole application boundary
  for radio admission, safe-boundary preemption, and completion handoff.
- `firmware/src/mesh_delivery_custody.c/.h`: exact packet custody, RF-start
  accounting, ACK state, persistence boundary, and one terminal result.
- Role adapters in `firmware/app/src/` that translate between those pure
  actions and DWM3000, workqueue, BLE, and settings APIs.

These names are targets, not permission to create parallel owners. A new module
must remove or delegate the corresponding legacy owner in the same stage.

Stage 4 gives every audited application-side DWM3000 driver or port operation
one nonzero client/generation lease and requires release of that exact lease.
Raw receive-abort request and clear calls exist only in the radio-owner platform
binding. Logical abort is a level held by one or more exact abort leases; the
physical level clears only after the final lease releases. A driver
`-ECANCELED` ends and unwinds the complete logical operation rather than being
cleared or retried inside a receive loop.

The lease spans a complete logical operation rather than one driver call. The
scheduled clicker DS-TWR burst now keeps one lease across its bounded sample
gaps, parks the DWM3000 before exact release, and only then runs optional
post-burst diagnostics under their own leases. Pause or abort therefore cannot
declare quiescence while the clicker radio remains awake-idle.

Gateway control uses the same owner for an exact admitted-command handoff. The
safe-boundary grant carries the frozen work identity and owner generation into
the scheduled worker. Every submit failure retains a liveness edge for that
admission or fails closed; a grant or schedule failure retires only the
affected admission range, preserves newer admitted work, and restores the
gateway scan. Transport quiescence follows the COMM-10 sequence
`pause -> abort request -> abort release -> resume`; an error at any owner step
keeps custody and deadlines owned, leaves transport closed, and deliberately
stops feeding the watchdog.

The focused clicker BLE-courtesy and radio/power boundaries are now
`app_clicker_ble_courtesy.c/.h` and `app_clicker_radio_power.c/.h`.
System-on idle enters retained standby under an exact lease and releases before
button wake is armed; terminal System-OFF retains its final lease through pin
parking and `sys_poweroff`. A terminal-lease claim failure retries; after claim,
standby and pin parking are best-effort logged preparation and power-off
continues with the lease held. Click-session and button policy remain in
`app_clicker.c` and are a later extraction target. Gateway host admission and
collection state still need the planned
`app_gateway_host_admission.c/.h` and
`app_gateway_collection_owner.c/.h` boundaries. These secondary migrations use
the same one-owner-and-deletion rule and are not mechanical file moves.

## Migration stages

1. **Make truth executable.** Keep `verify_changes.py`, fresh-clone native and
   sanitizer CI, deterministic stress, document registry, issue overlay, and
   architecture-growth gates green. Verification runs from an immutable source
   snapshot with write-and-restore detection, requires every west dependency to
   match the repository-owned `firmware/west_projects.lock.json`, rejects
   hidden index flags and unapproved ignored dependency inputs, pins west
   configuration, binds the live west manifest byte-for-byte to the immutable
   source snapshot, rejects ambient build overrides, and write-guards those
   inputs through each Zephyr matrix. The active locked project set is resolved
   again after the matrix. Temporary, exclusive build roots prevent concurrent
   pruning and stale reuse. Source-backed wiki sections, their citations, and
   tracked validation/context artifacts bind to one preserved source commit
   and one TOC-plus-pages digest. This stage is complete only when a clean
   worktree reproduces the same result as a developer checkout.
2. **Characterize ownership.** Add contract-level tests for every survey
   terminal, cancellation, stale generation, zero-RF deadline, BLE pressure,
   reset boundary, and radio-owner handoff. Survey lifecycle, queue, deadline,
   stale-generation, cleanup, rerun, and terminal cases are covered. Exact
   radio leases, stale release, abort lifetime, whole-operation cancellation,
   gateway handoff, and transport-gate failure scenarios are source-guarded and
   natively exercised; reset and hardware qualification remain open.
3. **Extract the gateway survey machine.** Route one survey operation through
   the pure machine while the legacy coordinator remains an adapter. Prove
   event/action trace equivalence across success, rerun, abort, capacity,
   route-loss, reset, and deadline cases, then delete the retired coordinator
   state and work handlers. **Implemented in source:** one pure machine now
   owns discovery and pair-round lifecycle, all lifecycle work runs on the
   `mesh_route` actor, and the old round owner is deleted. Native, sanitizer,
   mesh-integration, hardware-model, and exact-role compile gates pass.
   Multi-board RF, stack, reset, and power qualification remains required
   before deployment.
4. **Centralize radio ownership.** **Implemented in source for the current
   audited application DWM3000 paths:** admission, exact generation leases,
   level-triggered receive abort, whole-operation cancellation, gateway
   safe-boundary handoff, and transport pause/abort/resume now pass through
   `app_mesh_radio_owner` and its policy. The old gateway-priority owner modules
   are deleted; the remaining radio-handoff adapters delegate to the exact
   owner rather than owning state. Click priority, the connected Channel 5/9 rhythm,
   bounded continuous gateway RX slices, watchdog leases, external wire
   behavior, and timing are unchanged. Native and source-invariant coverage
   exists, but exact-role hardware, stack, power, and long-running deployment
   qualification remain required.
5. **Extract delivery custody.** Make packet identity, persistence, RF starts,
   local ACK, gateway ACK, caller terminal, pause, and cancel one state machine.
   Remove duplicate retry and terminal accounting from callers.
6. **Convert textual fragments to real modules.** Turn the existing
   coordination, transport, route-control, event, delivery, RX, anchor-survey,
   anchor-control, relay-route, and relay-custody fragments into separately
   compiled modules with explicit context and internal APIs. Each conversion
   must reduce a composed translation unit and may not add a replacement
   monolith.
7. **Delete migrated internal adapters.** Remove only the legacy state,
   aliases, and source-spelling tests superseded by the new owner, and do so
   only after production presets, required regression builds, and hardware
   evidence show the migration is complete. The generic legacy roles, bench
   traffic sources, ML collection presets, and supported debug builds remain
   required build lines unless a separate explicitly approved decision retires
   them.

Each stage is one reviewable commit series. Do not mix an ownership migration
with a wire-format change, timing retune, power-policy change, or unrelated bug
fix.

## Fixed-topology simplification experiment

The office anchors are fixed after commissioning, so a simpler production
protocol may eventually be safer: commissioning could establish and persist a
gateway-rooted spanning tree plus a bounded superframe, leaving dynamic repair
as an explicit maintenance operation instead of continuous distributed
negotiation.

That is an experiment, not the current decision. Build it first in the native
simulator behind a new protocol version and compare:

- code, static RAM, stack, radio airtime, and idle power;
- click latency and custody under collision, loss, BLE pressure, and reset;
- behavior when an anchor moves, disappears, rejoins, or loses persisted state;
- commissioning recovery, mixed-version deployment, and rollback;
- the number of independent owners, retry machines, and terminal paths.

Adopt it only if it removes substantial runtime state and still meets the
current robustness, click-priority, deployment, and recovery requirements. If
anchors must re-form routes autonomously during normal operation, retain the
connected-routing protocol and complete the orchestration migration instead.

## Completion criteria

The reset is complete when no `.inc` composition remains in the four target
translation units, no project C file exceeds the agreed ceiling without an
explicit debt record, all long-running operations expose one owner and terminal
state, clean CI runs the complete native and stress gates, and exact-role plus
hardware evidence passes without legacy fallback paths.

The survey and radio-owner stages demonstrate the intended pattern but do not
complete the reset. Gateway survey policy and the audited application radio
boundary now have exact owners. General delivery custody, the remaining
click-session/button policy, gateway host admission, and the composed
translation units still need their own staged deletions.

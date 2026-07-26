# Architecture Reset Plan

Status: Accepted for staged implementation  
Date: 2026-07-26  
Behavioral effect of this decision: None until a migration stage passes its
defined equivalence and hardware gates.

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
| `app_mesh_report.c` | 18,668 | routing, transport, event timing, delivery, RX, gateway contact |
| `app_anchor.c` | 10,876 | commands, survey, gateway control, radio, initialization |
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
ceilings. The gate rejects new include fragments and oversized new C files.
This records debt; it does not approve the architecture.

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

The first extraction boundaries are:

- `firmware/src/gateway_survey_machine.c/.h`: plan, dispatch, observe, rerun,
  abort, and terminal survey transitions without Zephyr work primitives.
- `firmware/app/src/app_mesh_radio_owner.c/.h`: the sole application boundary
  for radio admission, safe-boundary preemption, and completion handoff.
- `firmware/src/mesh_delivery_custody.c/.h`: exact packet custody, RF-start
  accounting, ACK state, persistence boundary, and one terminal result.
- Role adapters in `firmware/app/src/` that translate between those pure
  actions and DWM3000, workqueue, BLE, and settings APIs.

These names are targets, not permission to create parallel owners. A new module
must remove or delegate the corresponding legacy owner in the same stage.

## Migration stages

1. **Make truth executable.** Keep `verify_changes.py`, fresh-clone native and
   sanitizer CI, deterministic stress, document registry, issue overlay, and
   architecture-growth gates green. This stage is complete only when a clean
   worktree reproduces the same result as a developer checkout.
2. **Characterize ownership.** Add contract-level tests for every survey
   terminal, cancellation, stale generation, zero-RF deadline, BLE pressure,
   reset boundary, and radio-owner handoff. Record existing behavior without
   adding another retry or fallback.
3. **Extract the gateway survey machine.** Route one survey operation through
   the pure machine while the legacy coordinator remains an adapter. Prove
   event/action trace equivalence across success, rerun, abort, capacity,
   route-loss, reset, and deadline cases, then delete the retired coordinator
   state and work handlers.
4. **Centralize radio ownership.** Move admission and safe-boundary handoff
   behind `app_mesh_radio_owner`. Preserve click priority, the connected
   Channel 5/9 rhythm, bounded continuous gateway RX slices, and watchdog
   leases. Delete direct radio starts outside approved driver/owner seams.
5. **Extract delivery custody.** Make packet identity, persistence, RF starts,
   local ACK, gateway ACK, caller terminal, pause, and cancel one state machine.
   Remove duplicate retry and terminal accounting from callers.
6. **Convert textual fragments to real modules.** Turn the existing
   coordination, transport, route-control, event, delivery, RX, anchor-survey,
   anchor-control, relay-route, and relay-custody fragments into separately
   compiled modules with explicit context and internal APIs. Each conversion
   must reduce a composed translation unit and may not add a replacement
   monolith.
7. **Delete compatibility paths.** Remove legacy state, aliases, tests that
   assert obsolete source spelling, and superseded documentation only after
   production presets, required legacy regression builds, and hardware evidence
   show the new owner is complete.

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

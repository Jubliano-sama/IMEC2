# Enumeration-aware survey and assignment timing

This is the implemented timing contract for enumeration and survey first contact. It keeps the existing routing, custody, acknowledgement, and pair-ranging behavior, and changes when probes and topology-operation connections are allowed to start.

## The small model

Enumeration already gives the system the three facts needed to plan a survey:

- `S` is the occupied stable-slot span: the highest occupied enumeration slot plus one. Holes are kept; slots are never renumbered just for a survey.
- `N` is the number of enumerated anchors.
- `D` is the maximum route hop depth in the final enumeration estimate.

The survey START names the exact enumeration epoch, table sequence, commitment, and `S`. An anchor participates only when all four values match its committed enumeration state. This prevents two anchors from interpreting the same time as different slots.

The GUI keeps the complete topology estimate in RAM because it already acts as the gateway's external memory. Anchors need only their stable slot, their current route depth and candidates, and the survey identity. This adds no NVS writer.

This fits the current code without adding a second routing system. Enumeration already publishes stable slot and hop count, the route table already carries three candidates, the Channel-9 acknowledgement owner already has two connection entries for the upstream and downstream sides, survey reports already use bounded custody, and the GUI operation policy already calculates a survey budget. Those are the seams to reuse; the new behavior belongs in their timing policy, not in a parallel coordinator.

## 1. Probe in exact 200 ms slots

The physical discovery phase has four identical rounds. Each round contains `S` slots of exactly 200 ms, so its duration is:

`probe time = 4 x S x 200 ms`

Every boundary is calculated from the same shared survey start. An anchor that prepares late does not slide its local rounds or move the report phase for everyone else.

An anchor may send its Channel-5 probe only inside its own stable slot. A small random delay is fine, but its range must be clamped to `slot end - frame airtime - guard`, so the complete frame still fits before that slot ends. If firmware wakes too late, it skips that transmission and records the miss; it never spills into the next anchor's slot.

All other anchors listen during the slot. Stable-slot holes remain quiet. This costs a little time when enumeration has holes, but it makes the schedule unambiguous and avoids another mapping that could disagree after a reset or partial update.

## 2. Let the child make contact when its parent can reasonably accept it

After the fourth probe round, each anchor creates its discovery report and keeps it in bounded RAM until custody has moved toward the gateway or the operation ends.

Only the child starts a report connection. A relay does not keep a list of expected children and does not reserve future child connections. It retains only the existing active connection state: at most one connection toward the gateway and one away from it.

First attempts are ordered by lower hop depth first, then lower stable slot. In plain terms, direct anchors get a chance first, then one-relay anchors, then two-relay anchors, with stable enumeration order breaking ties. This gives a parent time to establish its own upstream path before a child asks it to receive a report.

Firmware owns one admission allowance, `A = 2270 ms`. It is long enough for one uncontended wake/propose/ACCEPT/report handoff to succeed and release the relevant downstream side, and it is not a GUI setting.

A compact implementation of the first-attempt rule is:

`not before = report phase start + (hop depth x S + stable slot) x A`

This is only a not-before time. Correctness never depends on the previous report finishing within `A`. If a route becomes deeper before the first RF attempt, the anchor postpones the attempt. A route change must never move an already announced attempt earlier.

## 3. Keep failures random and child-owned

Once an anchor makes its first RF attempt, deterministic slotting is finished for that topology-operation response, covering both enumeration and survey:

1. It tries its selected parent at the scheduled first opportunity.
2. If no ACCEPT arrives, it makes one randomized retry at that same parent.
3. After two unanswered attempts, it moves to the next known route candidate and applies the same bounded behavior.
4. After all known candidates are exhausted, it uses route discovery as a last resort, followed by a longer randomized backoff.

The relay sends no BUSY or NACK frame. Silence can mean busy, deaf, collided, or unavailable, and the child handles all four cases the same way. Waiting for the initial not-before time consumes no retry attempt or retry airtime.

This deliberately does not promise deterministic service for every possible shared-parent bottleneck. Those topologies remain valid and continue retrying; they simply fall outside the conservative rectangular/chain timing model.

## 4. Scale the estimate with the topology

The GUI shows an estimated full-survey duration before start and a countdown while the survey runs. The estimate is a service target, not the operation timeout.

One deliberately simple preflight model is:

`estimate = depth start guard + 4 x S x 200 ms + (((D + 1) x S) + N + D) x A + pair estimate`

The start guard is the smallest proven shared-control horizon, not a fixed terminal wait: `max(20,000 ms, (D + 4) x 2,000 ms + 1,104 ms)`. This keeps depths one through five at 20 seconds and adds only the time required for depths six through eight.

The terms have direct meanings:

- `(D + 1) x S` covers the depth-first, stable-slot first-contact grid.
- `N` gives every anchor one gateway-delivery allowance.
- `D` covers filling and draining the deepest report pipeline.
- Before discovery reports produce the real pair plan, the pair estimate uses the conservative `N x (N - 1) / 2` pair count and the existing proven per-pair allowance. Once the gateway has the real plan, the GUI replaces that count and tightens the countdown.

This models any number of ordinary chain-like paths such as `Fn--...--F3--F2--F1--D--gateway`; the number of chains is not a separate input. A large triangular topology in which many children share one parent is allowed, but is intentionally not part of this timing promise.

Completion stays data-driven. If every expected report or pair result arrives early, the survey completes immediately. If the model target passes, the GUI shows `Over estimate by ... - retrying` and the firmware keeps working. The existing 30-minute absolute safety ceiling remains the only final timeout.

The GUI shows the phase as well as time, for example `Probing 2/4`, `Collecting reports 5/8`, and `Ranging pairs 3/7`. When `D` is high, or the model approaches the 30-minute ceiling, it warns before start that adding route redundancy may improve reliability. The warning does not make the topology invalid or prevent the survey, and it needs no new redundancy telemetry.

## What stays unchanged

- Survey identity and generation checks, report format, pair planning, five-sample ranging, and final result semantics stay unchanged.
- Existing hop ACK, gateway ACK, bounded RAM custody, route candidates, and general event scheduling still own delivery.
- The one-retry rule is local to enumeration and survey connection setup. It does not lower the global route retry constant or change normal click, sticky-ACK, or transit delivery.
- A relay gets no persistent child roster, no new BUSY reply, and no wider connection-owner table.
- Normal clicks and non-survey mesh work keep their existing priorities and behavior.
- Runtime packets, retries, counters, and topology estimates are not written to NVS.

## Verification and promotion gates

Native and model tests cover exact 200 ms slots for sparse and high stable-slot values, four-round duration, bounded random delay, no late spill, depth/slot ordering, route-depth postponement, retry accounting, candidate rotation, last-resort discovery, one-upstream/one-downstream ownership, and `S`/`N`/`D`-scaled countdown estimates. They also keep the estimate separate from the terminal 30-minute ceiling.

Bench qualification uses one exact build and runs DDD first, then F2F1D, F1F1D, and F1DD. DDD isolates the slot map, direct report order, and GUI countdown; F2F1D exercises parent readiness and two relay depths; F1F1D exercises two children contending for one parent's downstream side; F1DD proves a single child does not lose its response while two direct anchors service gateway control. A topology-specific regression may be rerun first while diagnosing that exact case, but it does not replace the ordered full set. Each run must show Here-I-Am, three current-run enumeration confirmations, a typed enumeration terminal, and a typed survey terminal with the same assignment identity and no stale replay.

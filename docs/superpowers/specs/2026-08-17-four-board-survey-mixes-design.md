# Four-board survey mixes

Date: 2026-08-17
Status: draft

## Goal

On the four probed boards, each of these firmware mixes must complete one automatic survey:

- `direct direct direct`
- `forced-1 direct direct`
- `forced-2 forced-1 direct`
- `forced-1 forced-1 direct`

Success is the existing provision bar: `--require-survey-success --expected-anchors 3 --expected-pairs 3`. That means three unique discovery reports, three pair starts, three usable host pair distances, and a lossless terminal counter. Mid-survey route repair is not an allowed way to get there.

## What this is not

- Not a second simultaneous Channel-9 downstream cadence. The mesh contract still allows one upstream rhythm and one downstream rhythm per anchor. Gateway links stay non-cadence.
- Not a revival of the untracked sibling initiator START. Current dispatch is one tracked responder START, then one tracked initiator START, one identity per endpoint.
- Not an antenna-delay or geometry-solver project. Negative ToF from calibration is out of scope unless it is the only thing failing an otherwise delivered pair.

## Topologies

Four boards: one `mesh_gateway` plus three anchors.

`forced-2 forced-1 direct` is the legal linear chain. The production direct is the gateway child. The hop-1 image parents off that direct. The hop-2 image parents off the hop-1 image. Each non-gateway node has at most one child.

`forced-1 direct direct` is one hop-1 child of one direct, plus a second production direct on the gateway. No sibling contention.

`direct direct direct` is three gateway children. No inter-anchor cadence.

`forced-1 forced-1 direct` is legal only because the two hop-1 children share the direct’s one downstream cadence in series. They do not both own a live Channel-9 connection. First come, first served. The waiter attaches only after the occupant’s connection is actually closed.

## Phases

A phase is one host-visible uplink burst that uses the contended Channel-9 slot:

- Assignment response: the node’s CLAIM response, plus any same-epoch child response it still owns as transit.
- Survey discovery: the node’s discovery report, plus any same-generation transit discovery report it still owns.
- Survey pair batch: this node’s remaining pair-result / ACK-confirm / pair-radio lease for the current synchronized batch only. Do not hold the slot until the whole survey generation finishes; that would deadlock `forced-1 forced-1 direct` while the sibling waits to send its own result.

PREPARE, START, TABLE, and DS-TWR stay on Channel 5. They do not by themselves keep a Channel-9 child slot.

## Preferred close

After a node’s current phase has no remaining durable gateway-bound work, that node should close its upstream Channel-9 connection with `EVENT_END`.

Do not close while any of these still hold:

- origin or transit durable uplink for this phase
- a live ACK-confirm for that uplink
- a survey pair radio lease on this node

The parent that receives `EVENT_END` must clear that peer’s downstream timing so the slot is free. `EVENT_END` is only for a cadence parent (another anchor). Gateway children stay on the non-cadence batch path; do not send `EVENT_END` to the gateway as phase-complete close.

`CONFIG_IMEC_MESH_ROUTE_TEST` is already on production `mesh_anchor`. The missing piece is the close trigger: `app_mesh_ch9_ack_complete_should_close_timing()` currently always returns false. Teach that path, or a sibling helper, to close after the occupant’s current phase has no remaining local or transit uplink. Do not add a second downstream cadence.

## Fallback close

If the occupant never sends `EVENT_END`, the existing idle / missed-event / supervision close must still free the slot. First-come, first-served still waits for a real close, not for a single packet ACK.

## Waiter

The sibling that lost the race keeps its selected parent and retries Channel-9 `PROPOSE` until the slot is free or the waiting uplink’s own assignment/survey custody deadline expires. Do not cap that wait on the 20 s discovery instant, the 15 s START barrier, or the current 6 s `PROPOSE` retry window.

A rejected `PROPOSE` because the parent already has a downstream rhythm is not a route failure. It must not increment gateway-ACK failure toward hold-down, must not select an alternate parent, and must not start route discovery.

## No route repair during survey

While a survey generation is active, a gateway-ACK timeout on a still-selected parent retries that parent through the operation deadline. It must not take `ROUTE_DELIVERY_DISCOVER` or place the parent in the 60-second hold-down.

Re-installing Channel-9 timing on the same parent after a phase-complete close is cadence setup, not route repair. The route stays.

Assignment uses the same rule while its response window is open: a hop-1 child that is waiting for the direct’s slot retries attach, it does not rediscover.

## Pair control

Keep the current tracked sequence:

`PREPARE initiator → PREPARE responder → START responder → START initiator → OBSERVE`

One command identity, one delivery handle, one cleanup obligation per endpoint per attempt. Retries keep the same sequence, transaction, digest, and absolute execution instant.

Do not submit an untracked initiator START beside the responder START.

## HIL order

Fix what the current tree actually fails, in this order:

1. `direct direct direct`. Latest spaced run delivered three discovery reports and finished 2 of 3 pairs. A later startfix run saw zero reports. Diagnose from new traces, not from the deleted sibling-START audit.
2. `forced-1 direct direct`. One hop-1 child of one direct; no sibling slot share.
3. `forced-1 forced-1 direct`, after the phase-complete close and waiter retries exist. This is the first mix that needs the serial downstream slot.
4. `forced-2 forced-1 direct` last. Depth-3 chain, after the shallower mixes are green.

Leftover audit items (wrong-depth frame still ending a scan, duplicate cleanup, sample before `OBSERVING`) are in scope only if current code or a new trace still has them.

## Tests

Native, before relying on the path:

- Occupant with no remaining phase work sends `EVENT_END`; parent downstream timing is clear.
- Occupant with remaining transit or ACK-confirm work does not close.
- Waiter `PROPOSE` while the slot is occupied stays on the same parent and does not discover.
- Active survey generation does not convert a gateway-ACK timeout into hold-down or discovery.
- Pair dispatch still has exactly one START identity per endpoint.

`mesh_integration` / `hardware_models` if routing, Channel-9 scheduling, ACK retry, or survey pair control changes.

Hardware: the four mixes on the four probed boards, each through `provision_mesh_anchor.py --command survey --require-survey-success --expected-anchors 3 --expected-pairs 3`.

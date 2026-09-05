# Real-hop mesh audit — 2026-09-05

The forced-hop qualification does not establish recovery through genuinely hidden sleeping relays or dense 50-anchor contention. This read-only audit found three concrete application/protocol seams and one wake-budget defect. No hardware was accessed and no firmware or timing policy was changed by the auditor.

## Scope and authority

Reviewed shared, dirty `master` around checkpoint `d602e3faa`, including the ongoing enumeration radio-reservation and DWM receive-error corrections. Source references below describe the inspected state; the delivery owner is being simplified concurrently.

`Documentation/Channel 5 Delivery Protocol.md` is the newer work-in-progress design and explicitly supersedes the Channel-9 delivery, ACK_CONFIRM, direct-probe and route-request-flood descriptions in `Documentation/Mesh Connected Routing Contract.md`. The latter remains useful history for activation, enumeration and survey constraints. Existing compiled legacy route-request machinery is an implementation fact, not evidence that it satisfies the newer local-solicit design.

The reported DD/F1D/F1F1D/F2F1D bench passes use decode-layer RF isolation. Physically colocated forbidden transmitters can still interfere, and clickers remain visible to every layer. This audit did not repeat those runs.

## Ranked findings

### 1. Production route fallback records unobserved gateway success

**Confirmed; removal in progress in the main task, not verified fixed by this audit.**

`firmware/app/src/app_mesh_report_event_tx.inc:4643` calls `mesh_relay_note_direct_gateway_route()` when next-hop selection fails, except in forced-hop builds. `firmware/src/mesh_relay_routes.inc:1539` then calls `route_record_candidate_success_at()`, clearing failures and hold-down, and marks discovery ready without RF evidence. In a real `G—B—C` chain with no `G—C` link, exhaustion of C's known parents can therefore resurrect an unreachable gateway candidate. The forced-hop configuration skips precisely this branch.

False success and hold-down cancellation are established by code; repeated application livelock has not been reproduced. Existing `test_direct_gateway_route_probe_clears_hold_down` correctly verifies this helper after a successful probe, but the application calls it without that prerequisite.

**Smallest repair:** remove the speculative successful-route installation; retain the report bank while actual route acquisition runs. The main task intends to use existing `mesh_schedule_route_request()` and correlated `ROUTE_REPLY`, rather than wire incomplete `MSG_ROUTE_SOLICIT` behavior. That is an interim implementation choice relative to the newer design, not completion of its local-repair protocol.

**Regression:** ordinary production configuration, hidden C, failed/held-down direct candidate, then a new retained report. Assert no fabricated contact success, no payload loss or duplicate owner, and route recovery through B.

### 2. New route solicit does not work through the sleeping-parent application boundary

**Confirmed; open, outside the chosen legacy recovery path.**

`firmware/app/src/app_mesh_c5_priority.c:122` explicitly admits a broadcast `MSG_ROUTE_REQ` after wake, but has no corresponding `MSG_ROUTE_SOLICIT` branch. Its uplink capture branch at line 148 excludes broadcast destinations. Even when a solicit reaches ordinary RX, `firmware/src/mesh_relay_delivery.inc:2348` delivers it locally without forwarding; `firmware/app/src/app_mesh_report_delivery.inc:5385` invokes only `mesh_gateway_note_route_solicit()`, which does nothing on anchors. `mesh_relay_build_solicit_reply()` has no application callers.

Thus C can wake B and still receive no route answer. At the gateway, the current helper requests a rate-limited global Here-I-Am wave, which also differs from section 2.4 of the newer design: finite-depth neighbors should answer locally and a solicit must not trigger a gateway wave.

**Smallest current implementation route:** `mesh_schedule_route_request(GATEWAY_ID, reason)` (`app_mesh_report_coordination.inc:3851`) reaches `mesh_request_route_owned()` (`app_mesh_report_direct_gateway.inc:1344`), which performs an actual direct probe, full wake, legacy request and correlated reply listener. Do not treat the new solicit builder alone as a replacement. Implementing the newer protocol requires a deliberate application receive/reply design and the admission correction in finding 3.

**Regression:** C has retained data, hears no clicker or gateway, and B starts asleep. Exercise both finite-depth B and routeless B; verify the chosen protocol's explicit success or retained retry behavior through the actual post-wake capture boundary, rather than injecting directly into the relay core.

### 3. Solicit replies either look stale or start an unintended wave

**Confirmed by native reproduction; open.**

`firmware/src/mesh_relay_flow.inc:644` reuses the responder's existing gateway advertisement sequence. An existing child rejects an equal sequence from a retained parent at `mesh_relay_route_rx.inc:1282`, so this reply cannot repair that parent's held-down candidate. A fresh child instead accepts it as a new advertisement and raises `MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV`. The ordinary depth-complement TTL does not suppress forwarding, contrary to the builder comment and section 9.5 of the newer design. The reply also starts with message age zero, so downstream wave timing would be based on this fresh local receipt.

**Smallest repair:** explicitly correlate solicited replies and admit their parent observation without launching a new wave. Reusing the existing correlated legacy reply avoids introducing this incomplete protocol during the delivery-owner simplification. Changing the gateway's global sequence merely to force acceptance would create another propagation/freshness problem.

**Regression:** feed a built reply into a receiver with the same parent's retained sequence and hold-down; then into a fresh receiver. Require successful exact-parent repair and no flood action in both cases. Existing tests in `test_mesh_c5_delivery.c` inspect reply construction and unreachable responders, but do not exercise these receiver transitions.

### 4. Two-scan wake guarantee exceeds permitted settings and uses the wrong node's cadence

**Confirmed arithmetic/configuration defect; RF failure sequence unproven, open.**

`app_config.h:252-260` permits scan intervals through `floor((500000 - 2500 - 170 - 1) / 1000) = 497 ms`. The newer wake model at lines 265-281 adds 40 ms rearm and 10 ms RX, making two periods 1094 ms; the train is silently clipped to the 1000 ms wire cap. `app_mesh_report_coordination.inc:744` additionally derives the train from the sender's local interval, although the sleeping receiver may have a different command-applied interval (`app_anchor_commands.inc:3407`). Sender 380 ms / receiver 497 ms produces an 860 ms train against a 547 ms receiver period. One lost hunt can then leave no complete receive opportunity inside the train.

**Smallest repair requiring a timing choice:** enforce a shared receiver-cadence maximum that includes actual rearm cost, and derive the guaranteed train from that bound. Do not silently clip a stated two-opportunity guarantee. No timing or scan policy was changed in this audit.

**Regression:** sweep independent sender/receiver settings and scan phases, with one blocked scan. Existing wake scenarios assert a uniform 380 ms cadence and cannot prove this mixed-setting bound. Ordinary HIA and legacy route requests still use the separate 500 ms train, so those need their own receive-gap coverage.

## Native reproduction

The temporary file `/tmp/imec_real_hop_solicit_audit.c` initializes a gateway and child at epoch 3, sends advertisement sequence 77 through `mesh_relay_handle_rx()`, sets the gateway's retained wave sequence to 77, builds a solicit reply, and feeds it to the same child and then a freshly initialized child.

```sh
cc -Ifirmware/include /tmp/imec_real_hop_solicit_audit.c firmware/build/libcore.a -lm -o /tmp/imec_real_hop_solicit_audit
/tmp/imec_real_hop_solicit_audit
```

Observed output:

```text
initial adv ret=0 status=0 actions=40000
same-parent solicit reply ret=0 status=-9 actions=40 (STALE=-9)
fresh-node solicit reply ret=0 status=0 forward-wave=1
```

This linked the existing native library; it was not a fresh full production build. The inspected source independently contains both branches. The temporary source is not a durable regression test.

## Qualification gaps and already-covered mechanisms

The mixed-50 radio-scale test at `firmware/tests/mesh_integration/test_mesh_gateway_control_stress_scenarios.c:178` injects every advertisement into relay state before RF simulation, then globally serializes transmissions with precisely placed receive windows. It proves capacity and encoding, not contention among simultaneous activation trains or workqueue/PHY ownership at 50 nodes. The next useful scenario is several mutually hidden same-depth parents sharing a sleeping child, with independent scan phases, followed by an eight-hop narrow bridge carrying concurrent retained reports and controls. This audit did not establish a new queue/credit overflow or stale-clock bug in that scenario.

Existing protections include complete HIA activation per relay, receiver-local countdown translation, bounded capacity refusals, ancestry/TTL validation and exact custody identities. These mechanisms, and the reported enumeration reservation/PHY fixes, are meaningful evidence but do not compose automatically into large physical-hop qualification. The concurrent route-cost/hop-depth/255 eligibility corrections belong to the main task and are not duplicated here as findings.

## Fixed versus open at the initial audit handoff

- **Verified fixed by this audit:** none; the audit is read-only except this document and temporary reproduction files.
- **Main-task correction underway:** remove the assume-direct-success shortcut and keep one report owner during existing correlated route acquisition. Verify the final diff and tests before marking finding 1 closed.
- **Open design/application work:** new solicit admission/reply behavior and reply freshness/forward suppression (findings 2–3).
- **Open timing decision:** common receiver cadence and guaranteed wake coverage (finding 4).
- **Unproven qualification:** dense hidden terminals, independent receiver settings and integrated 50-anchor/eight-hop operation.

## Follow-up: report-bank integration review

A subsequent read-only pass inspected the new `app_mesh_report_c5_batch.inc` delivery engine, `app_mesh_report_delivery_state.h`, and report worker. The new engine retains bytes in its bank during route acquisition, reselects each packet on every attempt, and splits a burst when packet-specific routes diverge. Its missing-route branch calls `mesh_schedule_route_request()` without transferring the report bytes; the async request's unbound owner kind permits this. No additional TTL/ancestry defect was established in this pass: incoming forwarding remains responsible for hop transformation and the bank uses packet-specific route selection.

Two integration defects were sent to the main task immediately. Their status below records the inspected version, not any subsequent repair:

1. **ACK waiting consumes unrelated traffic and treats PHY collisions as hard exits.** `mesh_c5_wait_batch_ack()` decodes directly from the shared receive buffer, discards nonmatching frames with `continue`, and returns immediately for every nonzero receive result. It does not queue unrelated controls/reports or hand standard-wake activity to the click probe. A hidden child's report or gateway control arriving during the 250 ms ACK window can therefore be consumed without admission. In addition, the engine charges parent failure only for `-ETIMEDOUT`; repeated typed PHY `-EIO` outcomes can keep retrying the same parent without exhausting its failure budget. Restore bounded receive classification and causal-frame handoff, preserving the distinction between a PHY decode failure and hard driver failure. Test an unrelated valid control/report followed by the exact ACK, and a consistently colliding parent with an available alternate. These are confirmed source paths; their full RF failure sequences were not reproduced.
2. **Admission deduplication does not include the new bank.** `report_tx_queue_contains_semantic_locked()` searches only the recovery slot and message queue. After a packet moves into `mesh_report_delivery.entries`, an exact producer redrive—or child retry after the small generic relay dedup cache expires or is displaced—can enqueue another owner of the same packet. Include digest-bound bank membership in admission under synchronization consistent with bank mutation. Test exact re-admission while a bank waits for a hidden parent, including generic-cache eviction. The ordinary immediate duplicate still receives a terminal hop-ACK from the existing core cache; the problem is the longer-lived ownership boundary after that cache no longer supplies protection.

The main task reported passing production-harness cases for singles, partial ACKs, queue fullness, absent routes and completion callback failures. This follow-up did not rerun those cases or establish that they cover the two transitions above. No firmware was changed by the auditor.

### Repair review of the two integration findings

**Source repair reviewed; focused tests and hardware validation pending.** A bounded second pass found the following corrections in the new engine:

- `mesh_c5_wait_batch_ack()` now queues nonmatching valid frames through `mesh_queue_from_frame_at_internal(..., submit_work=true)` and returns so the lease can be released for causal RX work. Typed PHY activity runs a standard-wake probe; click handoff occurs only after radio release and scratch unlock, and failed release suppresses that handoff. The observed flow-control fields remain local until positive packet-identity matching or correlated refusal matching succeeds. Persistent typed activity now eventually reaches the unchanged outer deadline rather than escaping each attempt as an uncharged PHY error.
- `report_tx_queue_contains_semantic_locked()` now includes digest-bound `mesh_report_delivery_contains()`. `report_tx_queue_commit_head_locked()` publishes the new bank member under the same overflow lock that removes the queue item, closing the ownership gap. Completion invokes producer callbacks outside the lock and compacts the bank under it. This closes the inspected duplicate-owner seam; no additional hidden-parent discovery deadlock was established.
- The speculative `DBG_UPLINK_ASSUME_DIRECT`/`mesh_relay_note_direct_gateway_route()` shortcut is absent from the inspected `app_mesh_report_event_tx.inc`; the bank's absent-route branch retains bytes and schedules real acquisition. Finding 1 is therefore source-corrected in this follow-up, with end-to-end validation still pending.

**Timing caveat found in this review, subsequently fixed below:** the ACK loop's 250 ms deadline does not bound the duration of its nested probe. `mesh_probe_standard_wake_claim()` has no caller deadline and uses `ANCHOR_UWB_WAKE_ACTIVITY_HOLD_MS` (1000 ms), plus PHY retuning. An error immediately before ACK expiry can therefore extend radio ownership by that separate bounded probe allowance. The main task was asked to cap the probe to the remaining budget or explicitly document and test the larger worst-case allowance; this audit does not claim a 250 ms wall-clock bound. Findings 2–4 and the physical-scale qualification gaps remain open.

## Main-task validation after the audit

The final source also passes the original absolute ACK deadline into the shared standard-wake probe, using the driver's existing `receive_frame_continuous_extend_on_activity_until` API. Activity no longer grants an additional one-second probe window; radio retune and lease cleanup still take their normal bounded execution time. Native regression coverage exercises both clipped persistent activity and a subsequent valid ACK.

The four focused native tests passed, including actual ACK route feedback and the production C5 delivery include. All 165 mesh integration checks passed across the full run and the one updated extraction-boundary recheck; all 157 hardware-model checks passed in one run. The C5 harness reproduces the earlier unrelated-frame, PHY-recovery, click-handoff and partial-ACK retry-key failures against the captured earlier source. This validates the bounded repairs, while the open large-system and local-solicit findings above remain open.

The first hardware requalification exposed a remaining transit-report bypass: the application queued forwarded reports only for a gateway next hop or while the core already owned another TX. An idle parent relay therefore used the old tracked owner. F2F1D completed ten clicks but delivered only nine reports, with the relay repeating route acquisition while retaining a core-owned report. The queue predicate now depends only on the report contract, so gateway, anchor and absent next hops all enter the same bank. The extracted production-helper regression covers eighteen report/type/route/activity combinations; the final full integration (165) and hardware-model (157) runs passed. The failed trace is retained as `logs/c5_delivery_cleanup_20260905/f2f1d-clicks`; subsequent `final-*` runs qualify the corrected source.

Final hardware outcome: both F1F1D and F2F1D passed enumeration, complete five-sample surveys, ten completed clicks, thirty reports and thirty exact host receipts on the corrected anchors. F2F1D required an extra drain capture after 25/30 reports at 100 seconds, so latency remains an explicit limitation. Four-member batches and the complete forced C→B→A→gateway chain were observed; physical hidden-hop scale remains unproven. See [bench evidence](../../logs/c5_delivery_cleanup_20260905/results.md).

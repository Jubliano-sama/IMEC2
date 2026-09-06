# Recovery, physical-hop, 30-node and power audit

Audited source: `474ef8600ef25b50da5c3dbc1dc30241178d166a`, 2026-09-05. The existing suites pass, but new probes reproduce survey hangs, incorrect survey custody, blocked successor operations, and starvation in the current C5 report bank. This checkout should not yet be considered qualified for unattended 30-node deployment.

This is an audit. Production source and timing settings were left unchanged; no boards were flashed or measured. The power subagent's separate findings are in [power-audit.md](power-audit.md). Reproduction sources and captured outputs are in [probes](probes/).

## Scope and evidence

The review followed production `mesh_clicker`, `mesh_anchor`, `mesh_gateway`, shared protocol libraries, and the gateway GUI. It traced admission, partial failure, timeout, cancellation, resource release, retained ownership, and admission of the next operation. Restart recovery means a successor can obtain clean ownership; it does not require the interrupted operation or its RAM data to survive.

[function-inventory.tsv](function-inventory.tsv) mechanically indexes 4,806 definitions in 178 files: 4,171 C definitions and 635 Python definitions. C preprocessor branches were not resolved by this inventory. It is a navigation/triage index, **not a claim of exhaustive semantic verification of every definition**. Imported SDKs and ML/demo behavior were outside the production audit. Manual review concentrated on protocol and peripheral lifecycle paths; the native analyzer covered 126 compilable translation units/configurations, not the complete Zephyr role images.

The newer `Documentation/Channel 5 Delivery Protocol.md` supersedes older C9 delivery descriptions. Existing `.inc` names and old tests still contain C9 terminology. The current survey also differs from older survey timing prose; calculations below use current code. Historical findings were rechecked before inclusion. In particular, the previously reported fabricated direct-gateway success path is absent from the current bank's missing-route path, so it is not reported as an open defect here.

Evidence labels below distinguish a **native reproduction**, a **source-confirmed branch**, and an **unqualified system scenario**. P1 means potential data loss or indefinite obstruction of essential work; P2 means a recovery defect, conditional loss of progress, or unsupported operating bound. Priorities are engineering judgments, not measured occurrence rates.

## Findings

### F1 — P1: survey bundle scheduling can loop forever

**Native reproduction.** `survey_response_lane_prepare_round()` in `firmware/src/survey_response_lane.c:200-267` greedily chooses offsets 0–70 with at least 10 ms spacing. Its unbounded placement loop cannot recover when previous choices leave no legal offset. Five bundles and seed 265 already hang; this does not require exceeding declared capacity.

A valid 30-anchor roster occupying slots 20–49 creates 118 aggregated neighbor/signal records, six bundles, and hangs with seed 8. The same probe with compact slots 0–29 produces 75 records/four bundles and returns. The maximum 162-record/nine-bundle lane cannot fit any legal arrangement in that window. All hanging cases were killed after one second; passing cases complete in under a millisecond on the host.

`app_survey.c:452` calls the scheduler synchronously while running the radio lane, so later cancellation and deadline checks cannot rescue that invocation. Hardware watchdog recovery was not tested. Use bounded placement with an explicit deferred/error outcome and prove spacing for all legal bundle counts and seeds. See [survey_lane_notes.md](probes/survey_lane_notes.md) for exact inputs and controls.

### F2 — P1: rejecting a survey bundle can retain new data under an old ACK

**Native reproduction.** `survey_response_lane_merge_bundle()` at `survey_response_lane.c:152-194` adds records one at a time. A later conflicting record returns before ACK/attempt masks are invalidated. In the probe, the lane grows from one record to two, returns `PROTO_ERR_STALE`, and still reports everything acknowledged. Retrying the valid new record alone returns success with `added=false`; it remains skipped upstream.

Both conflicting records pass wire codecs. This is failed-admission mutation, not malformed serialization. Preflight the entire bundle or roll back all custody and scheduling state. A rejection must leave the lane unchanged, and successful changes must invalidate every affected upstream bundle. The existing successful-merge tests do not cover this invariant.

### F3 — P1: the C5 bank can block later local reports indefinitely

**Native application-seam reproduction.** `mesh_report_delivery_step()` in `firmware/app/src/app_mesh_report_c5_batch.inc:335-557` replenishes only an empty bank. Unacknowledged entries have no terminal age check. The normal core's expiry helper returns 86,400 seconds for the test packet, but after advancing past that horizon the bank still transmits old transit sequence 99 while local sequence 100 remains queued.

Queue-level promotion cannot move local traffic past an occupied bank. A permanently failing destination can therefore consume retries and obstruct later clicks for longer than the core's intended lifetime. The probe uses the existing fake radio/clock seam; it is not a one-day hardware soak. The seam's age-refresh helper is a stub, while source inspection confirms that the real refresh updates transmitted age without retiring the bank owner.

Give bank ownership the same bounded age/failure outcome as other custody and an explicit local-priority admission rule. Do not invent a successful ACK to release it. Completion-callback errors also need bounded recovery so an acknowledged entry cannot freeze bank admission forever.

### F4 — P1: survey events lose host custody at BLE transmission completion

**Exact-helper reproduction plus source-confirmed completion path.** `gateway_survey_emit_event()` marks `MSG_SURVEY_EVENT` ACK-required, and `gateway_ble_stream_packet()` validates and enqueues it as retained. But `gateway_host_custody_supported()` in `app_gateway_ble.c:987` omits survey events. The exact extracted helper returns false for an ACK-required survey event and true for a command result.

The completion handler at `app_gateway_ble.c:1234-1261` consequently calls `gateway_ble_stream_mark_sent()` after ATT completion instead of awaiting GUI acceptance. The survey-specific receipt completion branch at `app_gateway_result_runtime.inc:1348` cannot supply the missing HOST_NOTIFIED transition. A GUI process failure, reconnect, or deferred semantic application after transport completion can lose the only retained copy of survey data. The GUI deliberately buffers early events until the controlling command is accepted, which makes this distinction meaningful.

Restore consistent survey classification at admission, notification, receipt and retirement; test a deferred GUI apply and reconnect between notification and receipt. See [ble_survey_classification_results.json](probes/ble_survey_classification_results.json). No physical BLE interruption was exercised.

### F5 — P1: survey publication errors can leave firmware and GUI disagreeing forever

**Source-confirmed failure path.** `gateway_terminal_publish()` at `app_survey.c:1340` freezes the terminal event, clears active ownership, ignores `ops.emit_event()` failure, and releases the gateway owner. PLAN_ACCEPTED and graph/batch publication likewise lack a retained retry owner when enqueue fails (`app_survey.c:1578,1783`). The BLE stream can reject admission when its six slots or 1,756-byte payload pool are occupied; retention only applies after successful admission.

The GUI expires command-result waits, but does not automatically reconcile an active survey through GET_STATUS. A lost terminal can leave it active after the gateway has finished; cancelling then receives invalid-state/ENOENT, and `survey_runtime.py:523` retains the ranging phase after a rejected cancel. `_clear_survey_data()` also refuses while active. Lost graph/plan events can stop automatic progression earlier.

Retain one pending authoritative event in bounded RAM until admitted, and reconcile host ownership after a missing event or reconnect. Preserve exact event identity during retries. GET_STATUS already exposes state, so this does not justify NVS packet storage. This path was traced in source; no integrated fault-injected gateway/GUI run was performed.

### F6 — P2: gateway restart can admit HIA while the new TABLE is blocked by the old survey

**Native application-seam reproduction.** An anchor keeps its old survey active while a restarted gateway begins a fresh enumeration. HIA prearm accepts the fresh epoch, but `app_survey_anchor_note_ram_roster()` at `app_survey.c:2348` rejects the new roster with `-EBUSY`. Clearing the roster is a no-op while the survey is active; TABLE handling returns before committing the new assignment (`app_anchor_commands.inc:3757,3962`).

The probe returns `new_hia=0`, `new_table_roster=-16`, and succeeds only after the old lease expires. Its fixture uses a short lease; production START has a 180-second initial expiry and plans allow up to 1,800 seconds. Thus the next protocol can be blocked even though the restarted gateway has no record of the old owner. The current expiry does eventually recover this branch.

Choose a single successor rule: either a verified fresh enumeration supersedes the old survey, or the gateway discovers/waits for the remaining owner before starting enumeration. Preserve radio ownership until it is actually released. This is an authority/recovery choice; no existing timing was shortened by the audit.

### F7 — P2: raw survey custody does not distinguish reused lane contents

**Native API reproduction; on-air occurrence unqualified.** `survey_response_lane_note_ack()` at `survey_response_lane.c:328` checks network, generation, peers, kind, sequence and the attempted bit. It binds neither assignment, batch nor content. Reinitializing a lane with different valid range bytes under the same generation lets an earlier ACK mark the replacement data acknowledged. Changing the generation rejects the same ACK, as expected.

`app_survey.c:1135` reuses the survey generation for range lanes across execution strides and batches, while gateway initialization clears the generation and the next start begins at one (`app_survey.c:1817,1857`). Raw bundles use the same reduced identity. DS-TWR nonces and the richer control identity do not protect this separate result lane. A stale participant or delayed raw exchange therefore lacks an identity fence at this boundary; the probe does not prove a particular physical delayed-ACK trace.

Bind results and acknowledgments to the accepted assignment/batch and immutable bundle contents, or prove an equivalent non-reuse rule across the complete receive lifetime. Do not add a frequently written persistent counter as a shortcut.

### F8 — P2: a receipt behind a command cannot bypass full result admission

**Exact-worker reproduction, with source-backed saturation preconditions.** `gateway_ble_rx_work_handler()` at `app_gateway_ble.c:2030` bypasses result reservation only when a receipt is at the FIFO head. If a command precedes that receipt and `gateway_command_result_reserve_ingress()` returns `-ENOSPC`, the worker breaks without reaching the receipt. Ten invocations of the extracted worker leave both frames queued; moving the receipt first in the fixture restores progress. See [ble_receipt_notes.md](probes/ble_receipt_notes.md) for controls and stub boundaries.

The circular-wait scenario requires both full command-result storage and a BLE stream blocked on an ACK-required retained head, such as an assignment event or command result. Flushing the result queue then cannot free a reservation until the stream advances, while the receipt needed to advance it sits behind the blocked command. Receipt timeout/reconnect rewinds retained transmission; it does not reorder this RX queue. Survey events are not the right retained-head example in the current code because of F4.

Preserve a bounded receipt service path independently of command-result credit, without acknowledging discarded commands. A full end-to-end BLE client sequence was not captured; do not interpret this as an observed ordinary single-command GUI deadlock.

### F9 — P2: periodic battery sampling does not establish cleanup after every GPIO failure

**Source-confirmed peripheral failure path, investigated by the power subagent.** `battery_adc_divider_enable()` can configure the divider on and then fail a second GPIO operation. `battery_sample_lithium_mv()` returns immediately on that error without cleanup (`app_board.c:193-234`). A later disable failure is also treated by the indicator like an ordinary unavailable ADC reading. The periodic work keeps retrying and servicing the idle watchdog, so divider power can remain unproven indefinitely.

Attempt cleanup after every enable attempt and preserve cleanup failure separately from measurement failure. Verify bounded recovery and the next click/sample, not only the initial error return. The clicker's transactional idle-entry cleanup does not cover later periodic samples. Actual divider current was not measured.

### F10 — P2: permitted scan settings invalidate the claimed two-opportunity wake bound

**Configuration arithmetic and source-confirmed caller.** `app_config.h:252-280` permits a 497 ms scan interval, then calculates two periods as `2 × (497 + 40 + 10) = 1,094 ms`, clipped to the 1,000 ms wire limit. `mesh_uplink_wake_train_ms()` in `app_mesh_report_coordination.inc` uses the sender's interval even though the sleeping receiver can have a different configured interval. A 380 ms sender chooses 860 ms against a 547 ms receiver period.

The stated guarantee of surviving one lost scan is therefore not established for permitted settings. Actual RF failure was not reproduced. Use a receiver-cadence bound that includes rearm and complete-frame reception, with an explicit configuration guard instead of silent clipping. Selecting that bound is a timing-policy decision.

## Forced hops versus physical hops

The RF-scope filter permits clickers to reach all layers and permits other nodes only within their configured adjacent layers (`firmware/src/uwb_rf_scope.c`). The DWM receive boundary applies that filter after physical frame reception (`dwm3000_driver_radio.inc:1756-1815`). It cannot remove forbidden RF energy, model weak or asymmetric links, or establish the behavior of a relay that never hears the clicker. Colocated forced-hop success remains useful forwarding evidence, but does not qualify hidden-parent activation or realistic collision behavior.

There is also a configuration mismatch at the upper boundary: CMake accepts eight forced relay hops (`firmware/app/CMakeLists.txt:293`), and `uwb_rf_scope_build()` assigns that anchor layer nine, with the gateway at layer zero. Adjacent-layer forwarding then needs at least nine radio hops, but `MESH_NETWORK_MAX_HOPS` is eight. The build should reject that unreachable preset or reconcile the two meanings explicitly; it is not a valid eight-hop qualification topology. This is a source-confirmed bound, not a flashed test.

The new local ROUTE_SOLICIT design is also incomplete at the application boundary: the post-wake classifier explicitly accepts broadcast ROUTE_REQ but not ROUTE_SOLICIT; ordinary application delivery calls the gateway-only solicit handler, and the anchor reply builder has no application caller. The current C5 bank instead invokes the existing correlated legacy route-request path on missing routes. That distinction prevents claiming either that the new local recovery protocol works or that all existing route recovery is absent. See the rechecked source discussion in [the earlier real-hop audit](../real-hop-audit-2026-09-05.md); its initial fabricated-direct-success finding was superseded by subsequent code.

The required physical-hop case is `gateway — parent — child` with the child unable to hear either the gateway or clicker, the parent initially asleep, route loss during retained delivery, independent scan phases, and competing siblings. Add complete-frame RX and collision constraints to the application scenario before hardware qualification. F10 matters particularly in this case.

## What changes at 30 nodes

Thirty anchors fit the declared 50-anchor membership and 64-bit survey masks. The node budget also includes 18 clickers, but that is a capacity declaration rather than a simultaneous-load proof. Parent candidates are bounded to three and network depth to eight; 30 nodes does not imply support for a 30-node chain. Important bottlenecks remain the four-report C5 bank, small ingress/report queues, five command-result credits, and six BLE stream slots sharing 1,756 payload bytes.

Survey has at most 100 pairs per firmware batch; the GUI owns additional batches. Thirty anchors have 435 possible unordered pairs. Current firmware permits degree up to 49; a GUI degree-four plan is a common selection policy, not the firmware capacity limit. Sparse assigned slots also increase neighbor/signal records even when only 30 anchors remain, which is why the F1 sparse-roster case matters.

The plan probe uses a complete mutual-hearing bitmap so all chosen pairs conflict and serialize. Assigned hop counts are input stress cases, not proof that a complete RF clique naturally has depth eight:

| Anchors | Requested pairs | Maximum input depth | Result / planned self-stop |
| --- | ---: | ---: | --- |
| 30 | 60, degree-four selection | 1 | Accepted; 139.150 s |
| 30 | 60, degree-four selection | 3 | Accepted; 372.730 s |
| 30 | 60, degree-four selection | 8 | Accepted; 1,227.930 s |
| 30 | 100 | 3 | Accepted; 610.730 s |
| 30 | 100 | 8 | `PROTO_ERR_NO_SPACE`; exceeds the 1,800 s cap |

The 100-wave depth-eight execution alone is `102 × 19.70 s = 2,009.4 s`. Rejection is bounded and correct for the current cap; host batching must consider estimated execution time as well as pair count. This is a supported-input limitation, not memory corruption. Neighbor discovery separately costs 30 seconds for compact slots and up to 50 seconds when the highest occupied slot is 49, before control and result drains.

Existing mixed-50-node tests seed route advertisements directly into core state and serialize RF with arranged RX windows. They establish capacity/encoding properties but do not compose current HIA, survey, C5 bank, BLE credit and radio ownership under dense hidden-terminal contention. Required remaining scenarios are: 30-node cold enumeration with independent wake phases; sparse 30-of-50 assignments; a narrow relay bridge with local-click priority; full BLE byte/slot pressure; loss/restart in every protocol phase followed by a fresh operation; and simultaneous sibling traffic through an eight-hop path. Existing hardware models must retain full-frame overlap, collision, SPI/BLE/workqueue delays and explicit failure on missing routes/capacity.

The GUI event queue is unbounded and `_drain_events()` drains until empty (`tools/gateway_gui/app.py:237,2247`). Sustained producer load can postpone Tk timers; an uncaught handler exception also skips rescheduling because only `queue.Empty` is caught. These are source-level long-run recovery risks, not observed 30-node UI failures. Bound each drain by work/time while preserving receive timestamps for command deadlines, and put recovery around the callback boundary rather than hiding failed semantic apply.

## Power priorities

The clicker already uses retained radio sleep, Bluetooth shutdown, SPI suspension and pin parking. Preserve that architecture and its transactional wake/idle recovery. The detailed [power audit](power-audit.md) separates safe implementation opportunities from timing/hardware decisions.

1. Fix divider cleanup (F9), then replace indefinite 5 ms held-button release polling with race-safe release-edge handling. Preserve debounce, gesture ordering, queued presses and long-press behavior.
2. Avoid diagnostic formatting and recurring stack/RTT work when no qualification capture is active. Current production presets still force stack diagnostics, and warning-level Zephyr logging does not gate typed RTT formatting. Retain fatal evidence and a way to qualify the actual deployed configuration.
3. Measure whether clicker peripheral shutdown can happen before the unchanged two-second terminal LED hold, and whether anchors benefit from explicit SPI/pin parking during proven idle gaps. Ownership and wake latency must remain correct.
4. IRQ-assisted long RX waits could let the MCU sleep instead of polling SPI every 50 microseconds. The source does not establish a connected DWM IRQ, so this needs hardware confirmation and separate timing proof; it is not a global replacement of precise polling with millisecond sleeps.

Battery LED duty is configured at 0.5% for clickers and 1% for anchors. Reducing it changes visible indication/sample freshness, and clicker watchdog service currently shares that periodic work. Wake cadence, train duration, active click-parent listening, courtesy scanning, and LED timing are behavioral choices, not free reductions. No current, charge-per-click or battery-life improvement was measured or claimed.

## Validation and limits

| Area traced | Existing recovery to preserve | Principal remaining audit concern |
| --- | --- | --- |
| Click, self-test and gesture handling | Serialized gesture/attempt ownership, bounded action gates, transactional idle and wake recovery | Divider cleanup; indefinite release polling; charge spent during failure/retry paths |
| DWM3000 and radio ownership | Slow/fast SPI ordering, retained sleep configuration, receive-abort and explicit lease release | Physical wake coverage; polling cost; application-level loops that prevent release |
| Enumeration and durable assignment | Bounded roster capacity, exact table commitment, persistent infrequent configuration | Fresh enumeration conflicting with old survey ownership after gateway restart |
| Survey control, ranging and raw result lanes | Explicit self-stop, cleanup owner, richer control identity, bounded plan rejection | F1/F2/F7 raw-lane defects; publication and successor recovery |
| Routing and report forwarding | Correlated existing route requests, bounded parent/ancestry/TTL checks, exact bank admission | Bank age and local priority; incomplete new solicit application path |
| BLE command/results and GUI | Bounded firmware admission, host receipts for supported classes, GUI command ownership | F4/F5/F8; unbounded GUI drain and missing active-survey reconciliation |
| Watchdog, diagnostics and storage | Clicker idle checkpoint, bounded diagnostic writer, configuration stored separately from runtime queues | Keep watchdog service when reducing periodic power work; do not move retry state into NVS |

The matrix records the lifecycle paths reviewed, not a proof that every possible failure in each subsystem was exercised.

- Fresh native Debug configure/build and complete CTest: **201/201 passed** in 26.03 seconds, including all 165 `mesh_integration` and 157 `hardware_models` tests (labels overlap).
- GUI unittest suite: **282/282 passed** in 17.032 seconds.
- Clang static analysis: 126 native-compilable translation units/configurations, no analyzer compile failures after correcting invocation. Diagnostics in two files were triaged: a roster-output null-path warning contradicted by validation/capacity guards, and logging-disabled dead stores. Neither established a new recovery bug.
- Added audit probes independently reproduce F1–F4, F6–F8 and planner bounds; source traces establish F5/F9/F10. Each application/extracted-function probe records its limited assumptions separately. Passing existing tests does not cancel these findings.
- No full Zephyr role rebuild, fresh RAM/stack margin measurement, hardware flashing, current measurement, physical 30-node test, or multi-day soak was performed. Native seams fake clocks, locks and hardware; they prove the exercised branch/state invariants, not complete runtime scheduling.

The initial integration run selected a global `west` and failed three production-seam builds because it could not resolve this workspace's build extension. Using the project virtualenv on PATH fixed those three; the final full 201-test run used that corrected environment. This was a test-launch problem, not a firmware failure.

Reproduction commands and evidence hashes are in [probes/README.md](probes/README.md). The largest structural issue is ownership split between the C5 bank, legacy relay core, survey lanes and BLE publication. First establish bounded admission/retirement invariants at those seams; additional buffers or a broad rewrite would make these failures harder to reason about.

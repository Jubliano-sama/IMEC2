# UWB/BLE Viable Changes Implementation Spec

## 1. Purpose

This document specifies the concrete firmware and protocol changes to implement:

1. Keep all wake/contact behavior on UWB channel 5.
2. Move mesh payload traffic to UWB channel 9 only after peers have already established contact and negotiated bounded channel-9 events.
3. Replace per-sample ranging slots with one shared 200 ms channel-5 responder burst window.
4. Run DS-TWR without STS.
5. Always collect rich diagnostics for every valid ranging exchange, while measuring them at safe points that do not break delayed TX timing.

The target is lower click latency and lower clicker energy by reducing idle waiting in the ranging phase, while increasing the value of every captured distance sample for later compensation-model training.

## 2. Non-goals
Do not use BLE to wake anchors, authorize ranging, transport reports, or replace UWB routing.

Do not use channel 9 as a wake channel. Channel 9 is a scheduled mesh payload lane only.

Do not use STS diagnostics or STS quality logic in no-STS ranging sessions.

Do not make rich diagnostics optional or sampled every Nth exchange. Rich diagnostics are required per valid ranging exchange. Payload truncation/fragmentation is allowed, but the firmware must record what was captured, truncated, dropped, or unavailable.

## 3. Radio ownership model

The DWM3000 is a single radio. Firmware must never assume simultaneous channel-5 and channel-9 operation. All channel switching must be explicit, bounded, and observable.

Priority order:

1. Active channel-5 click service: accepted wake epoch, discovery listen/reply, range schedule handling, shared burst responder window, or clicker burst ranging.
2. Required channel-5 quick wake scan.
3. Channel-5 route discovery/contact/refresh.
4. Negotiated channel-9 mesh event.
5. Idle/retained sleep.

Channel-9 work must be preempted or deferred whenever a higher-priority channel-5 activity is due. A channel-9 event may be skipped. A channel-5 click wake scan may not be skipped merely to finish mesh.

Track counters for channel switches, PLL-ready failures, late returns to channel 5, mesh deferrals, channel-9 event misses, and click preemptions.

## 4. Channel 5 is the only wake/contact lane

Channel 5 remains the only lane for initial contact and wake-up. This includes:

- Clicker long-preamble wake claims.
- Clicker discovery and anchor discovery replies.
- Range schedule admission.
- Shared burst DS-TWR ranging.
- Mesh-related wake-up/contact attempts.
- Route discovery broadcasts.
- Route replies when no valid negotiated channel-9 timing exists.
- Route refresh when channel-9 supervision expires.
- Recovery after missed channel-9 events or stale peer timing.

Channel 9 must not be used for blind discovery, blind wake, wake claims, or route recovery from an unknown timing state. If a peer does not know when to listen on channel 9, the system must go back to channel 5 contact.

## 5. BLE-inspired negotiated channel-9 mesh events

After neighbors have established or refreshed route/contact state on channel 5, they may negotiate channel-9 mesh events. These events are conceptually similar to BLE connection events: both peers know when the next event starts, how long it lasts, how much guard time is needed, and when the schedule should be considered stale.

### 5.1 Per-route timing state

Add per-route or per-next-hop mesh timing state:

- `mesh_channel = 9`
- `event_interval_ms`
- `event_window_ms`
- `next_event_time_ms`
- `event_counter`
- `guard_ms`
- `peer_clock_skew_estimate_ppm` or equivalent drift guard
- `max_missed_events`
- `missed_event_count`
- `supervision_timeout_ms`
- `last_successful_ch9_event_ms`
- `fallback_required`

A route may be usable for forwarding only when both route freshness and channel-9 timing freshness are valid. If timing is stale, route/contact refresh happens on channel 5.

### 5.2 Negotiation messages

Implement either new messages or TLVs carried by existing route messages:

- `MESH_EVENT_PROPOSE`: proposes interval, window, first event time, guard, channel, and supervision timeout.
- `MESH_EVENT_ACCEPT`: accepts or clips the proposal.
- `MESH_EVENT_UPDATE`: adjusts cadence, normally when data is pending or the gateway wants longer windows.
- `MESH_EVENT_END` or stale timeout behavior: ends scheduled channel-9 use.

The gateway may propose longer or more frequent channel-9 events because it is USB powered. Battery anchors must keep channel-9 windows short and must always reserve time for channel-5 scans.

### 5.3 Mandatory room for channel-5 wake scan

Before starting any channel-9 event, the anchor must compute whether it can return to channel 5 in time for the next required wake scan. The event must be clipped or skipped if:

- It overlaps an active click epoch.
- It overlaps a scheduled discovery listen/reply.
- It overlaps a ranging responder window.
- It would miss the next required channel-5 quick wake scan.
- It leaves insufficient guard for retune, PLL readiness, RX setup, and software jitter.

A simple guard rule is acceptable for v1:

```text
ch9_event_end + retune_guard_ms <= next_required_ch5_scan_start
```

If this cannot be satisfied, defer mesh until the next negotiated event or refresh route/contact on channel 5.

### 5.4 Mesh payload behavior

Use channel 9 for mesh payloads after negotiation:

- Click reports.
- Heartbeats.
- Command results.
- Gateway ACKs.
- Survey data.
- Routed data packets.

Use fragmentation for large report payloads. Do not lengthen channel-9 RX windows indefinitely; split across connection events.

## 6. Shared channel-5 burst ranging

Replace the current interpretation of a ranging slot as one sample opportunity. The ranging phase uses one shared 200 ms responder burst window in which selected anchors stay ready and the clicker runs back-to-back DS-TWR exchanges round-robin.

### 6.1 Schedule format

Extend `UWB_RANGE_SCHEDULE` or add `UWB_RANGE_BURST_SCHEDULE` with:

- `channel = 5`
- `sts_mode = disabled`
- `burst_window_ms`
- `exchange_stride_us`
- `max_exchanges`
- `selected_anchor_count`
- selected anchor short addresses and full IDs if needed
- round-robin order
- `min_successful_unique_anchors` default 4 for normal clicks
- `min_samples_after_4_unique` or equivalent stop rule
- `diagnostics_required = true`
- mode flag: normal click vs diagnostic/self-test

Validation:

- `burst_window_ms >= 200`
- `exchange_stride_us >= configured_min_exchange_stride_us`
- default `configured_min_exchange_stride_us = 7000` until measured on hardware
- schedule must fit click budget and responder listen bounds
- normal click schedule is invalid when fewer than four eligible anchors were discovered

### 6.2 Clicker flow

1. Button press starts normal click budget.
2. Clicker performs channel-5 wake train and discovery.
3. If fewer than four eligible anchors reply, do not range in normal click mode; retry or fail according to the existing budget.
4. Select anchors and send burst schedule.
5. Run no-STS DS-TWR exchanges back-to-back in round-robin order.
6. Stop when either:
   - at least four unique anchors have successful ranges and the configured minimum sample condition is met, or
   - the burst window expires, or
   - click budget/radio ownership fails.
7. After each valid exchange, ensure clicker diagnostics are captured and transmitted to the anchor using the diagnostic path.

### 6.3 Anchor flow

1. Accepted channel-5 wake claim creates ownership epoch.
2. Anchor replies in its discovery slot if the selected clicker/event wins arbitration.
3. Anchor validates the burst schedule.
4. All selected anchors open the same continuous channel-5 responder window.
5. An anchor responds only to a matching addressed `POLL` for the accepted clicker/event/round.
6. Wrong-target `POLL`s are ignored while the anchor remains in the same responder window.
7. A successful addressed exchange is recorded as one sample.
8. The anchor collects rich diagnostics at the safe point described below.
9. After the window ends, the anchor queues reports for negotiated channel-9 mesh delivery.

## 7. No-STS DS-TWR

Configure ranging with STS disabled. Use Ipatov/pre-STS timestamp and diagnostic paths.

Rules:

- Do not call STS quality APIs for no-STS sessions.
- Do not include STS fields in no-STS reports. Mark them absent; do not encode zero as a fake value.
- `RANGE_STS_QUALITY_FAIL` must never be emitted for no-STS sessions.
- Use `RANGE_RX_TIMEOUT`, `RANGE_RX_ERROR`, `RANGE_BAD_FRAME`, `RANGE_DELAYED_TX_MISSED`, or `RANGE_TIMING_INVALID` as appropriate.
- Existing diagnostic vs normal-click flag consistency remains mandatory.

## 8. Rich diagnostics are mandatory per valid exchange

Rich diagnostics must be collected for every valid ranging exchange. The implementation should not sample only every Nth exchange and should not only collect diagnostics in a special training mode.

The phrase “rich diagnostics per exchange” does not mean reading diagnostics after every packet. It means collecting the richest useful diagnostic record for each completed addressed DS-TWR exchange at safe points.

### 8.1 Clicker diagnostics timing

The clicker receives `RESP`, then must send `FINAL` on time. Do not perform long diagnostic reads before `FINAL` if they risk missing delayed TX.

Clicker timing-critical `FINAL` fields:

- `poll_tx_ts_clicker`
- `resp_rx_ts_clicker`
- `final_tx_ts_clicker_planned`
- `response_seq_seen`
- clock offset from received `RESP`
- carrier integrator from received `RESP`
- delayed-TX late flag and timing margin
- burst ID, sample index, and round index

After `FINAL`, the clicker must collect and transmit rich diagnostics for the `RESP` observation using `UWB_CLICKER_DIAG` or equivalent. If register persistence requires reading some diagnostics immediately after `RESP`, then the configured exchange stride/final delay must be large enough to make that safe. Do not silently skip diagnostics to preserve an overly aggressive stride. The clicker only has to send diagnostics on its final round robin round that fits in the budget. It only has to go to one anchor and receive one acknowledgement. It doesnt matter which anchor this is.

Clicker rich diagnostic fields:

- actual final TX timestamp
- Ipatov RX timestamp/source metadata for `RESP`
- Ipatov RX status and POA
- Ipatov peak, power, F1, F2, F3, first-path index, accumulator count
- compact accumulator diagnostics
- RSSI/channel power and first-path power
- DGC decision/debug fields
- clock offset, carrier integrator, and xtal offset estimate
- PLL status
- event-counter deltas
- IC temperature and voltage
- MCU battery/temperature if available
- IRQ and processing timing

### 8.2 Anchor diagnostics timing

Anchors should not attempt rich diagnostic reads after every packet. In particular, reading rich diagnostics after `POLL` must not delay the scheduled `RESP`.

For every valid addressed exchange, the anchor must collect rich anchor-side diagnostics after receiving the first valid `FINAL` for that exchange. This confirms that the exchange belongs to the selected clicker/event and that the anchor is no longer racing the `RESP` delayed-TX deadline.

Anchor rich diagnostic fields:

- poll RX timestamp and final RX timestamp
- response TX timestamp
- final-frame Ipatov RX timestamp/source metadata
- final-frame Ipatov RX status and POA
- final-frame Ipatov peak, power, F1, F2, F3, first-path index, accumulator count
- compact accumulator diagnostics
- RSSI/channel power and first-path power
- DGC decision/debug fields
- clock offset/carrier integrator estimates for received clicker frames where available
- PLL status
- event-counter deltas
- IC temperature and voltage
- MCU battery/temperature if available
- IRQ and processing timing
- SPI speed/configuration
- optional raw CIR window, fragmented/truncated as required

If compact `POLL` diagnostics can be captured without threatening the `RESP` deadline, include them. Otherwise, the mandatory rich anchor diagnostic point is after the valid `FINAL`.

### 8.3 Diagnostic transport

`UWB_CLICKER_DIAG` is sent clicker-to-anchor after `FINAL`. Dropping or corrupting this frame must not invalidate the already completed range, but it must be reflected in diagnostics status.

Do not allow unbounded diagnostic payloads to starve the next scheduled exchange. If the configured diagnostic payload cannot fit the configured exchange stride, increase the stride or fragment; do not silently omit rich diagnostics.

## 9. Reporting over channel 9

After the burst window ends, anchors queue reports. Reports are delivered over negotiated channel-9 mesh events.

Each per-sample report record should combine:

- DS-TWR timestamps and derived distance
- clicker-provided `RESP` diagnostics
- anchor-side diagnostics measured after valid `FINAL`
- sample index and round index
- sequence start timestamp
- channel/data-rate/preamble/PAC/no-STS PHY configuration
- TX power/PG delay/antenna-delay calibration identifiers
- diagnostic status flags: present, missing, truncated, failed, unavailable

Reports must use skippable TLVs/subrecords so older parsers can ignore new diagnostics. Fragmentation and gateway ACK handling remain required.

Track:

- diagnostics bytes captured
- diagnostics bytes transmitted
- diagnostics bytes truncated
- diagnostics frames dropped
- report fragment count
- channel-9 report latency
- gateway ACK latency

## 10. Protocol additions

Recommended new or extended protocol elements:

### 10.1 Ranging

- `UWB_RANGE_BURST_SCHEDULE`, or extended `UWB_RANGE_SCHEDULE`
- `UWB_CLICKER_DIAG`
- diagnostic status flags for each exchange
- burst/window/round/sample identifiers
- no-STS PHY config identifier

### 10.2 Mesh scheduling

- `MESH_EVENT_PROPOSE`
- `MESH_EVENT_ACCEPT`
- `MESH_EVENT_UPDATE`
- `MESH_EVENT_STATUS` or counters in heartbeat

These can be standalone messages or TLVs inside existing route/control messages if that is simpler.

### 10.3 Diagnostic TLVs/subrecords

Add skippable TLVs or nested subrecords for:

- Ipatov timing/status/POA
- accumulator summary
- RSSI/channel power and first-path power
- DGC/debug state
- clock/carrier/xtal offset
- PLL status
- event counters
- temp/voltage
- firmware timing
- CIR window metadata and bytes

## 11. Tests

Implement unit, simulation, and hardware-facing instrumentation tests.

Required tests:

1. Channel 5 is the only wake/contact lane, including mesh-related wake/contact and route refresh.
2. Channel 9 cannot be used until channel-5 contact/route negotiation has established peer timing.
3. Channel-9 event scheduler clips/skips events that would overlap a required channel-5 quick scan.
4. Channel-9 mesh is preempted by channel-5 click service.
5. Missed channel-9 supervision falls back to channel-5 route/contact refresh.
6. Burst schedule validation rejects invalid windows, too-short stride, wrong mode flags, stale event IDs, and normal-click schedules with fewer than four discovered anchors.
7. Four-anchor simulation completes multiple round-robin no-STS DS-TWR exchanges inside one 200 ms shared responder window.
8. Wrong-target `POLL`s do not end an anchor responder window.
9. No STS API/status/quality path is used in no-STS sessions.
10. `RANGE_STS_QUALITY_FAIL` is impossible in no-STS sessions.
11. Clicker `FINAL` contains only timing-critical fields and compact metadata.
13. Anchor rich diagnostics are collected after first valid `FINAL` for every valid exchange.
14. Diagnostics are not read after every packet by default and do not break delayed TX or the next exchange.
15. Diagnostics encode/decode, truncate, and fragment correctly.
16. Reports combine clicker and anchor diagnostics and deliver over negotiated channel-9 mesh events with gateway ACK handling.
17. Instrumentation exposes click latency, UWB awake time, burst duration, exchange stride, diagnostics volume, channel-9 latency, and channel-5 preemption counts.

## 12. Implementation order

1. Add no-STS configuration guards and status behavior.
2. Add shared channel-5 burst schedule validation and simulated burst ranging.
3. Add clicker and anchor rich diagnostic capture paths at safe timing points.
4. Add diagnostic frame/report encoding, fragmentation, and counters.
5. Add negotiated channel-9 mesh event scheduling.
6. Move mesh payload delivery to channel 9 while keeping all wake/contact/refresh on channel 5.
7. Add hardware instrumentation and tune `exchange_stride_us`, diagnostic payload limits, and channel-9 guard values.

## 13. Firmware implementation status, 2026-05-18

The current firmware implements the v1 shape described above with these fixed defaults:

- Channel 5 remains the wake/contact/ranging lane. Route discovery, route replies without valid channel-9 timing, timing refresh, and stale-event recovery return to channel 5.
- Channel 9 is used for mesh payloads only after channel-5 contact and negotiated mesh event timing. Event timing carries interval, window, next event time, event counter, guard, skew, max missed events, and supervision timeout.
- Normal click ranging uses one shared 200 ms channel-5 responder burst, a 7 ms minimum exchange stride, no STS, diagnostics required, and at most 6 scheduled anchors.
- Normal clicks with fewer than four eligible discovery replies do not start ranging. Diagnostic/self-test schedules may range fewer anchors.
- The compact `UWB_CLICKER_DIAG` frame carries clicker-side post-`FINAL` diagnostics to the anchor. Anchor-side diagnostics are captured after a valid `FINAL`.
- Reports carry burst, stride, click latency, UWB awake time, diagnostic byte counts, fragment counts, channel-9 report latency, gateway ACK latency, PHY ID, clicker diagnostic bytes, and anchor diagnostic bytes.
- Heartbeat/status instrumentation includes mesh channel switches, PLL-ready failures, late channel-5 returns, mesh deferrals, channel-9 event misses, channel-5 preemptions, and channel-9 report latency.

The implementation is covered by native tests for schedule validation, no-STS status behavior, diagnostic encoding and fragmentation, channel-9 timing supervision, channel-5 preemption, channel-5 scan guards, channel-9 report transmission, gateway ACK timing requirements, and normal-click minimum-anchor enforcement. Hardware tuning remains necessary for the exact diagnostic byte budget, exchange-stride margin, channel-9 guard, and measured RF reliability.

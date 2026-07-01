# Mesh Route Test Debug Memory

## 2026-07-01 - RX Arrival Offset

RTT logs with `DBG_CH9_RX_EXPECTED delta=...` show expected channel-9 packets are observed about 44 ms after the planned RX slot start. Current samples:

- `gateway_rtt_only_20260701.typescript`: n=77, p50=43 ms, min=16 ms, max=56 ms.
- `gateway_rtt_with_ble_20260701.typescript`: n=73, p50=41 ms, min=13 ms, max=53 ms.
- `transmitter_rtt_after_tx_offset_20260701.typescript`: n=82, p50=44 ms, min=43 ms, max=45 ms.

This timestamp is currently taken after `dwm3000_driver_receive_frame_continuous()` returns and the frame has been delivered to app code, so it is a receive-complete/host-observed timestamp rather than a precise RF preamble/SFD arrival timestamp. Before shrinking the temporary 40 ms mesh-test TX offset, add or use a lower-level timestamp if available, or log TX planned/start and RX complete together to estimate true arrival margin.

## 2026-07-01 14:59 CEST - Transmitter ACK-Miss Test

Probe was on the transmitter for `transmitter_test_20260701-145553.typescript`. The transmitter sent 27 channel-9 batches, received 15 gateway ACKs, and timed out/requeued 12 times. There were zero `DBG_CH9_TX_BATCH_SLOT_FULL` events, so this run is not the old producer-cadence/slot-capacity symptom.

Successful ACK receptions had hardware receive timing of `rx_since_enable_uus=32363..35414` with median about `34307 us`; host-observed ACK delivery was `35..37 ms` after RX arm. On failed cycles the transmitter armed the ACK RX window and got no frames (`DBG_CH9_RX_EMPTY` / `DBG_CH9_RX_MISS frames=0`) before timing out. This localizes the failure to "no RF ACK arrives at transmitter" rather than "transmitter rejects malformed ACK"; gateway-side RTT or BLE logs are needed to distinguish gateway missing the data packet from gateway receiving data but not sending/scheduling ACK.

## 2026-07-01 15:07 CEST - Gateway RX Timing Suspect

The transmitter RTT sequence shows failed data transmissions are strongly correlated with `DBG_CH9_TX_SEND_AT now-start=40 ms`, while successful transmissions are often `+42..46 ms`. Before the patch, channel-9 RX retuned/configured during the guard but then waited until `channel9_plan.start_ms` before calling `dwm3000_driver_receive_frame_continuous_timed()`. That means the receiver was not actually in RX during the guard, and any RX-enable latency made a `start+40 ms` sender marginal. Route-test channel-9 RX now starts continuous RX immediately after mesh-payload configuration, i.e. from the guard period through `end + late_guard`.

## 2026-07-01 15:16 CEST - Gateway CH5 Scan Eats CH9 Guard

Gateway RTT after early-RX patch (`gateway_early_rx_20260701-150711.typescript`) showed 68 selected CH9 RX slots, 55 frames, and 13 empty slots. More importantly, most gateway RX workers were already late: `DBG_CH9_RX_SLOT now-start` was often `+38..41 ms`, and `DBG_CH9_RX_ARMED now-start` was often `+57..60 ms`. Successful frames were then received around host `+62..65 ms`, meaning the receiver was only barely catching the packet. Root cause is scheduling: with an active CH9 connection but no selected CH9 event, the gateway still ran ordinary channel-5 scans that could overlap the next CH9 guard/start. The scheduler now treats anchors and gateways the same for route-test CH5 gap scans: it clips/skips CH5 scanning near the next required CH9 activity, and it does not schedule useless local-TX CH9 slots when no ACK batch is pending.

## 2026-07-01 15:19 CEST - Gateway CH9 Timing Fixed, TX Reflash Needed

After the CH5-gap scheduler fix, `gateway_ch5_gap_fix_20260701-151121.typescript` shows gateway CH9 RX selection at `slot_now-start=-20..-18 ms` and RX armed at `armed-start=-1..1 ms`. Gateway ACK slots also transmit at `send-start=0..1 ms`. This confirms the gateway-side late RX/late ACK scheduling bug is fixed. The same run had only 7 received CH9 data frames and 26 empty CH9 RX slots; because the transmitter was flashed before the scheduler fix, the next fair test needs the transmitter rebuilt/flashed with the same timing code before interpreting remaining empty gateway RX slots as RF loss.

## 2026-07-01 15:22 CEST - ACK TX Was Too Early After Scheduler Fix

Transmitter RTT after both devices had the scheduler fix (`transmitter_ack_miss_20260701-151629.typescript`) showed data TX at `start+42..43 ms`, ACK RX slots selected at `start-20 ms`, and ACK RX armed at about `start-2 ms`, but every ACK RX slot was empty. The prior gateway capture showed ACK TX had moved to `send-start=0..1 ms`, so the ACK could begin too close to receiver enable. Route-test channel-9 gateway ACKs now use a separate `20 ms` TX offset while data payloads keep their `40 ms` offset.

## 2026-07-01 15:25 CEST - Use One Global CH9 TX Offset

The separate data/ACK offsets were only a diagnostic split and are not the intended protocol shape. Channel-9 data and ACK TX use the same retune/send path, so route-test firmware now uses one global channel-9 TX offset for all mesh-payload channel transmissions. The current value is `20 ms`, chosen because the fixed RX scheduler arms near slot start (`about -2..+1 ms`) and this leaves margin while preserving most of the 100 ms payload window.

## 2026-07-01 15:30 CEST - Gateway CH5 Preempt Starved Event ACCEPT

After Curie's gateway preempt changes, transmitter RTT (`transmitter_ack_10s_20260701-152443.typescript`) showed repeated route replies followed by `MSG_MESH_EVENT_PROPOSE` and `DBG_EVENT_ACCEPT_TIMEOUT`, with no channel-9 data phase. The likely app-layer bug was the gateway preempt scanner rescheduling channel-5 RX with `0 ms` delay even after a successful channel-5 frame, which can delay queued `mesh_rx_work` handling of route requests/event proposals on the same route work queue. Gateway preempt scan now yields `10 ms` after a successful channel-5 receive, while timeout windows still reschedule immediately.

## 2026-07-01 15:34 CEST - ACK Batch Bypassed Global CH9 TX Offset

Gateway RTT after the preempt-yield fix (`gateway_test_20260701-152927.typescript`) confirmed channel-5 negotiation now completes: the gateway receives `MSG_MESH_EVENT_PROPOSE`, sends `MSG_MESH_EVENT_ACCEPT`, receives channel-9 data frames, and sends gateway ACKs. However, `DBG_CH9_ACK_SLOT` followed by `DBG_CH9_TX_SEND_AT` still showed `send-start=0`, because `mesh_send_pending_ch9_ack_batch()` manually assigned `ack.earliest_tx_ms = plan->start_ms` and bypassed `mesh_ch9_slot_send_start_ms()`. ACK batches now use the same global route-test channel-9 TX offset helper as all other mesh-payload transmissions.

## 2026-07-01 15:37 CEST - 90s Transmitter ACK Monitor

Transmitter RTT `transmitter_90s_20260701-153439.typescript` showed 83 `DBG_GATEWAY_ACK_RX` events and zero `DBG_CH9_TX_ACK_TIMEOUT` events over the capture. ACK RX timing is now tight and consistent: transmitter arms ACK RX at about `start-2 ms`; gateway ACKs arrive with hardware `rx_since_enable_uus=21168..23118` and host delta `22..23 ms`, matching the shared `20 ms` channel-9 TX offset. One `DBG_CH9_TX_BATCH_SLOT_FULL` occurred for packet/seq `203` because the TX worker reached the slot after its end (`now=677850`, `end=677842`), but the packet stayed queued, was sent in the next TX slot, and was ACKed. Remaining concern is scheduling jitter: data TX uses a `20 ms` target, but actual send offsets ranged from `20` to `103 ms` when the worker was late; this did not cause ACK loss in this capture but should be optimized before multi-packet-per-slot testing.

## 2026-07-01 15:26 CEST - Gateway Has Two Upstream Route-Test Slots

The mesh-route-test gateway is intentionally not a normal relay with one upstream and one downstream channel-9 link. It never initiates route requests toward anchors in this bench, so its two channel-9 timing entries are treated as inbound/upstream slots. A useful channel-5 wake claim or route request now preempts gateway channel-9 activity and keeps scanning channel 5 until the route is rejected, the preempt window expires, or an event ACCEPT is sent and the inbound channel-9 timing is installed. When both inbound slots are already occupied, the gateway ignores new wake claims and drops new route requests instead of spending channel-5 airtime on a route it cannot accept; existing peers may still refresh their timing.

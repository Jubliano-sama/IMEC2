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

## 2026-07-01 15:47 CEST - Gateway Reset CH5 Discovery Miss

Gateway RTT `gateway_after_reset_ch5_20260701-154714.typescript` reproduced the "eventually works" channel-5 startup failure after an explicit gateway reset. The gateway was not blind: it saw many channel-5 wake claims from `0x3333333333333301`, received and handled `MSG_ROUTE_REQ` at about `5.038 s`, then transmitted a single `MSG_ROUTE_REPLY` at about `5.165 s` after the old fixed `100 ms` route-reply delay. The transmitter resumed wake-claim trains by about `5.191 s`, and the actual `MSG_MESH_EVENT_PROPOSE` did not arrive until about `8.424 s`. This localizes the cold-start channel-5 issue to a fragile single-shot route-reply/listen timing exchange, not gateway channel-5 validation. The route-test control path now uses a shorter `20 ms` gateway reply guard, sends a two-frame route-reply train, lowers wake-to-route to `40 ms`, and keeps an `80 ms` reply-to-event guard so the gateway finishes the reply train before the waker proposes the channel-9 event.

## 2026-07-01 15:57 CEST - Route Reply Captured But Retry Still Fired

Transmitter RTT `transmitter_after_reset_ch5_reply_train_20260701-155743.typescript` after the shorter guard/reply-train patch still showed two wake trains before channel-9 event setup. The first wake train did receive `DBG_ROUTE_REPLY_RX_FRAME` and `DBG_ROUTE_REPLY_RX`, but no `DBG_ROUTE_READY` followed before a second wake train started. Code inspection found `mesh_request_route()` always called `mesh_schedule_route_waiting_retry()` after `mesh_listen_for_route_reply()`, even when that listener captured and queued a route reply for `mesh_rx_work`. This made the route-wait retry race the queued route-reply processing, causing an unnecessary second discovery. The listener now reports whether a route reply was captured, and route discovery logs `DBG_ROUTE_REPLY_HANDOFF` and waits for queued RX processing instead of scheduling another immediate discovery retry when the reply was captured.

## 2026-07-01 16:01 CEST - Reply Handoff Still Had Early Reply And Stale Timeout

Transmitter RTT `transmitter_after_reset_route_reply_handoff_20260701-160132.typescript` still needed three wake trains before channel-9 setup, but then had `44` `DBG_GATEWAY_ACK_RX` and zero `DBG_CH9_TX_ACK_TIMEOUT`, so steady channel-9 was healthy. The first route attempt did not capture a route reply at all, suggesting the `20 ms` gateway route-reply guard was too aggressive for the transmitter to finish route-request TX and retune into channel-5 RX. The second route attempt did capture a route reply and logged `DBG_ROUTE_REPLY_HANDOFF`, but a third wake train started immediately, indicating an already-armed route timeout was still firing after handoff. The gateway reply guard is now `50 ms` with a second reply `25 ms` later, and the route handoff path cancels stale `mesh_tx_timeout_work` when no channel-9 ACK wait or active relay TX exists.

## 2026-07-01 16:08 CEST - Report TX Raced Queued Route Reply

Transmitter RTT `gateway_brief_after_reply_50ms_20260701-160658.typescript` showed the first route reply was received (`DBG_ROUTE_REPLY_RX`), handed off (`DBG_ROUTE_REPLY_HANDOFF`), and stale timeout cancellation ran (`DBG_ROUTE_REPLY_CANCEL_RETRY`), but `report_tx_work_handler()` immediately logged `DBG_REPORT_QUEUE_TO_ROUTE_WAIT` and started another route request before queued RX processing produced `DBG_ROUTE_READY`. This means the remaining second wake train was local worker ordering: report TX checked for channel-9 before the queued route reply had updated the route table. Route-test report TX now backs off when `mesh_rx_msgq` has queued control frames or when a route-waiting packet already owns discovery, logging `DBG_REPORT_WORK_BUSY_RX` or `DBG_REPORT_WORK_BUSY_ROUTE` instead of starting another route discovery.

## 2026-07-01 16:13 CEST - RX Queue Gate Was Too Narrow

Transmitter RTT `transmitter_after_reset_rx_queue_gate_20260701-161310.typescript` still showed two wake trains. The first captured route reply logged `DBG_ROUTE_REPLY_HANDOFF` and `DBG_ROUTE_REPLY_CANCEL_RETRY`, but `DBG_REPORT_QUEUE_TO_ROUTE_WAIT` still appeared immediately afterward and neither `DBG_REPORT_WORK_BUSY_RX` nor `DBG_REPORT_WORK_BUSY_ROUTE` fired. That means `report_tx_work_handler()` could enter during the small interval where the route reply was already captured but not yet represented by either queued RX visibility or `mesh_route_waiting_tx_active()`. The route-test transmitter now uses an explicit route-reply handoff flag with a `250 ms` deadline; report TX logs `DBG_REPORT_WORK_BUSY_HANDOFF` and backs off while that flag is active, then clears it on `DBG_ROUTE_READY`, route-wait clear/drop, or expiry.

## 2026-07-01 16:25 CEST - Gateway Channel-5 Was Still Segmented

Code inspection after `transmitter_after_reset_handoff_flag_20260701-161626.typescript` found the gateway mesh-test channel-5 scanner was not faithful to the intended model. Before any channel-9 connection it used normal gateway mesh RX slices (`50 ms` RX, `2 ms` idle). After a wake claim it used `100 ms` preempt slices and, after successful frames, a `10 ms` yield before the next slice. That can explain a first `MSG_ROUTE_REQ` landing in a receive gap even though the wake train was detected. The gateway mesh-test RX loop now treats channel 5 as the default continuous mode: no channel-9 connection uses a long immediately rearmed channel-5 receive; active channel-9 connections only interrupt channel 5 for scheduled channel-9 ACK/RX slots; the channel-5 gap between slots consumes the whole available gap instead of a capped `100 ms` segment. A valid wake claim or route request still holds the gateway in the channel-5 route/contact state until route rejection, timeout, or event ACCEPT/channel-9 timing install.

## 2026-07-01 16:30 CEST - First Route Reply Works, Route-Wait Worker Still Retried

After flashing the gateway continuous-channel-5 patch, transmitter RTT `transmitter_after_gateway_continuous_ch5_20260701-162819.typescript` showed the first route request now captured a route reply immediately (`DBG_ROUTE_REPLY_RX_FRAME` / `DBG_ROUTE_REPLY_RX` at lines 108-109). That proves the gateway channel-5 receive gap was a real first-contact bug. The same capture still showed a second wake train: after `DBG_ROUTE_REPLY_HANDOFF` and `DBG_ROUTE_REPLY_CANCEL_RETRY`, `DBG_REPORT_QUEUE_TO_ROUTE_WAIT` fired and then `DBG_CH9_UNAVAIL` started another route request before queued route-reply processing reached `DBG_ROUTE_READY`. The remaining issue is local transmitter worker ordering, not gateway RF contact. `mesh_try_route_waiting_tx()` now backs off while the route-reply handoff flag is active or channel-5 control frames are queued, logging `DBG_ROUTE_WAIT_RX_HANDOFF`, so stale route-wait retries cannot outrun queued route-reply processing.

## 2026-07-01 16:32 CEST - Single Wake Train Validated

After flashing the transmitter with the route-wait handoff guard, RTT `transmitter_after_route_wait_handoff_20260701-1630.typescript` validated the cold-start direct gateway path. Exact marker counts were one `DBG_WAKE_TRAIN_CONFIG_OK`, one `DBG_WAKE_TRAIN_FIRST_SEND_OK`, one actual `DBG_ROUTE_REPLY_RX`, one `DBG_EVENT_ACCEPT_RX`, `28` `DBG_MESH_TEST_SEND_OK`, `28` `DBG_GATEWAY_ACK_RX`, and zero `DBG_CH9_TX_ACK_TIMEOUT`. `DBG_ROUTE_WAIT_RX_HANDOFF` appeared three times during the queued route-reply/event-control handoff and prevented the stale route-wait path from starting a second wake train. Remaining `DBG_CH9_TX_BATCH_SLOT_FULL` markers are expected sender pacing: the transmitter held unsent packets for later TX slots instead of overrunning the 100 ms channel-9 window.

## 2026-07-01 16:40 CEST - Received ACK Timing Is Stable, Misses Track Late TX Starts

ACK timing audit across `transmitter_after_route_wait_handoff_20260701-1630.typescript`, `transmitter_repeat_1_20260701-163344.typescript`, `transmitter_repeat_2_20260701-163422.typescript`, `transmitter_repeat_3_20260701-163459.typescript`, and `transmitter_ack_timing_20260701-163758.typescript` found 144 successful channel-9 ACK receptions with `host_delta` from scheduled RX slot start tightly bounded at `4..7 ms` (`median=5 ms`, `p90=7 ms`, `stdev=0.71 ms`). The successful ACKs are therefore not arriving too early, too late, or with large jitter. The remaining intermittent ACK misses correlate with transmitter data packets being sent very late in the TX slot: examples include `DBG_CH9_TX_BATCH_TARGET offset=90`, `102`, `106`, and `109 ms`, followed by `DBG_CH9_RX_EMPTY` / `DBG_CH9_RX_MISS`. The next fix should make channel-9 TX slot-fit more conservative so late-start packets are deferred instead of transmitted near or past the end of the 100 ms slot.

## 2026-07-01 16:50 CEST - Conservative Channel-9 Fit Guard Removes Late TX ACK Misses

The channel-9 TX fit guard now models radio configuration time before send by using an effective send start of `max(scheduled_txstart, now + 25 ms config guard)` and then requiring estimated airtime plus the existing end-of-slot trailer to fit before the scheduled slot end. This keeps normal early workers sending at `start + 20 ms`, but defers late workers instead of allowing transmissions near or past the slot end. Three transmitter RTT validations after flashing (`transmitter_fit_guard_1_20260701-164731.typescript`, `transmitter_fit_guard_2_20260701-164808.typescript`, and `transmitter_fit_guard_3_20260701-164846.typescript`) each showed one wake train, one route reply, one event accept, `28` send successes, `28` gateway ACKs, and zero ACK timeouts. Late target candidates still appeared (`max offset 97..99 ms`), but they were logged as `DBG_CH9_TX_BATCH_SLOT_FULL` / `DBG_CH9_TX_BATCH_MISSED_SLOT` and deferred instead of transmitted. The current synthetic generator still produced only one ready packet per TX batch (`max_sent_per_batch=1`); the batch loop continues to support up to eight queued packets per slot when backlog exists and each packet passes the conservative fit check.

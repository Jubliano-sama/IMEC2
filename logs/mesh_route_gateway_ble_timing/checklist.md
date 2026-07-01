# Mesh Route Test Checklist

## 2026-07-01

- [ ] Optimize channel-9 RX arrival time immediately after event negotiation.
  - Current RTT evidence shows expected packets are observed about 44 ms after RX slot start.
  - Verify whether this is true packet start time or receive-complete/host-processing timestamp.
  - Reduce or remove the temporary mesh-test data TX offset only after measuring actual arrival relative to the guard-to-main-window transition.
  - Measurement under test: `DBG_CH9_RX_TIMING` reports RX arm delta from slot start, hardware RX timestamp estimate relative to slot start, host receive delta, frame length, and quality.
  - `transmitter_measurement_20260701-182438.typescript` measured ACK RX hardware arrival about 3.7-5.1 ms after slot start with host receive reporting 6-7 ms. Need the matching gateway measurement build flashed to measure transmitter data arrival at the gateway.
  - `transmitter_stack_measurement_20260701-182800.typescript` measured ACK RX hardware arrival about 3.1-4.6 ms after slot start with host receive reporting 5-7 ms.
- [x] Test multiple packets in one channel-9 TX slot.
  - Fill the queue intentionally and confirm sender only transmits packets that fit before slot expiry.
  - Confirm the receiver accepts multiple packets in one RX slot and returns one ACK batch containing all received IDs/seqs in the next TX slot.
  - Preserve ACK tracking: only require ACKs for packets actually transmitted.
  - Validated with `transmitter_burst_ack_ids_20260701-170028.typescript`: queue depth reached 8, batches sent up to 3 packets, one ACK packet carried up to 3 packet IDs, and the transmitter completed up to 3 pending ACKs from that one ACK with zero timeouts.
- [ ] Optimize same-slot channel-9 throughput toward the theoretical 8-packet target.
  - Current burst testing is limited by per-frame TX/config completion time, not by ACK matching.
  - Measured same-slot batches currently top out at 3 packets in the 100 ms slot.
  - Patch under test: configure channel 9 once per TX slot and send queued frames back-to-back, with the configured-slot fit counting only explicit inter-frame wait plus estimated airtime/trailer.
  - Session/destination are no longer batch boundaries; ready packets may share a slot when they use the same selected channel-9 next hop. ACK payloads now include parallel session and sequence lists for mixed-session acknowledgement.
  - `check_after_mixed_session_20260701-180211.typescript` improved large 963-byte frame slots to `max_sent=4`; distribution was 18 two-packet, 16 three-packet, and 17 four-packet slots with no ACK timeouts or queue-full markers.
  - Measurement under test: `DBG_CH9_TX_BATCH_FIT`, `DBG_CH9_TX_DONE`, `DBG_CH9_TX_BATCH_FRAME`, and `DBG_CH9_TX_BATCH_STOP` report fit estimates, actual send duration, remaining slot time, and early-stop causes.
  - `transmitter_measurement_20260701-182438.typescript` measured 963-byte frame sends at 14-15 ms. Four-frame slots ended with about 8 ms remaining; the fifth frame was correctly deferred because fit required about 18 ms.
  - `transmitter_stack_measurement_20260701-182800.typescript` repeated the result under stack diagnostics: 79 TX batches, max four 963-byte packets per slot, and fifth-packet deferrals remained legitimate fit failures.
- [x] Reduce gateway RAM after measured stack headroom.
  - `gateway_stack_20260701-180826.typescript` observed active gateway channel-9 ACK load with no stack faults.
  - Implemented gateway mesh-route-test `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=4096`; the transmitter preset still overrides to 16384.
  - Validated with `gateway_stack_reduced_syswq_20260701-181802.typescript`: `sysworkq` peaked at `used=496 free=3600 size=4096` while the gateway logged `258` channel-9 RX markers and `102` ACK batches.
  - Do not reduce `MESH_ROUTE_WORKQUEUE_STACK_SIZE` yet: `mesh_route` peaked at `used=2872 free=1224 size=4096` while ACK batches were active.
- [x] Force sustained backlog without report-queue overflow.
  - `transmitter_large_payload_20260701-173204.typescript` used 900-byte synthetic payloads and headroom-capped producer top-ups.
  - Queue depth stayed at or below 8, `DBG_REPORT_QUEUE_FULL=0`, ACKs were accepted while 6-7 packets remained queued, and there were zero ACK timeouts.

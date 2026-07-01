# Mesh Route Test Checklist

## 2026-07-01

- [ ] Optimize channel-9 RX arrival time immediately after event negotiation.
  - Current RTT evidence shows expected packets are observed about 44 ms after RX slot start.
  - Verify whether this is true packet start time or receive-complete/host-processing timestamp.
  - Reduce or remove the temporary mesh-test data TX offset only after measuring actual arrival relative to the guard-to-main-window transition.
- [ ] Lower synthetic transmit cadence after RX arrival timing is optimized.
  - Current 1000 ms cadence beats against the 880 ms direct TX opportunity cadence and causes periodic skipped opportunities.
- [ ] Test multiple packets in one channel-9 TX slot.
  - Fill the queue intentionally and confirm sender only transmits packets that fit before slot expiry.
  - Confirm the receiver accepts multiple packets in one RX slot and returns one ACK batch containing all received IDs/seqs in the next TX slot.
  - Preserve ACK tracking: only require ACKs for packets actually transmitted.

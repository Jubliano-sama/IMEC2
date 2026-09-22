# Three-anchor prototype qualification — 2026-09-05

Both requested topologies passed enumeration, survey and ten-click delivery on the final whole-burst reservation / PHY-error restoration source. No firmware behavior or timing constants were changed during this three-anchor run.

| Topology | Enumerated depths | Survey | Completed clicks | Reports / exact host receipts |
|---|---|---|---|---|
| F1F1D | A=1, B=2, C=2 | 3 neighbors, 3 pairs, 15/15 samples, no partial flags | 10/10 | 30/30 |
| F2F1D | A=1, B=2, C=3 | 3 neighbors, 2 visible pairs, 10/10 samples, no partial flags | 10/10 | 30/30 |

The clicker uses the normal three-anchor minimum and four-anchor maximum. Its fixed 400 ms burst shares twelve exchanges across the three selected anchors. F1F1D returned four samples in each of thirty reports; F2F1D returned four in twenty-eight reports and three in two reports. Both runs completed all clicks with ret=0 and delivered a report from every anchor for every click. There was no claim of lossless individual RF exchanges.

F1F1D shows an actual three-frame C5 batch from B to A (sent=0x7, acked=0x7) and a two-frame batch from A to the gateway (sent=0x3, acked=0x3). F2F1D RTT shows C's report entering B with previous=C and then A with previous=B; the host receipts prove delivery through the gateway. Gateway RTT is unavailable because its probe is on C. Forced-hop coverage is enforced RF-scope isolation, not physical-distance proof. F2F1D's two-edge graph does not determine a complete triangle or production geometry.

Evidence: f1f1d-powercycle-survey.log, f1f1d-clicks-summary.json and their RTT folders; f2f1d-survey.log, f2f1d-clicks-summary.json and their RTT folders. bench-notes.json records the live FICR/probe identities. Four role configurations built successfully; RAM margins are direct anchor 6,336 bytes, either forced anchor 6,400, clicker 33,328 and the unchanged gateway 5,408. Six focused RF-scope/click-chain/handoff tests passed; the prior mesh_integration and hardware_models gates remain applicable to the unchanged source.

Setup recovery: C initially rebooted on role-bound clicker storage. Its 24 KiB storage partition was backed up (SHA-256 f3e78de97e922f5faf7d64dbd508ea220a9175aa12f3868222d6286992f21c66), confirmed to contain role=1 records for its physical ID, and erased for the authorized anchor migration. No other board's storage was erased. Interrupting the initial survey left a BlueZ connection open; explicit disconnect restored advertising. A later BLE reboot command returned TIMEOUT, and the user power-cycled the gateway before the successful runs. That interrupted-operation recovery path remains unqualified.

The bench is left in F2F1D: A direct, B forced-1, C forced-2, plus the normal three-anchor clicker with RTT gesture injection enabled. These are prototype checks, not long-duration or reset-during-custody qualification. Follow-up source fixes remain local and uncommitted; origin/master remains the requested intermediate checkpoint d602e3faa.

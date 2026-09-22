# Production roles deployed — 2026-09-05

Commit `107e72d866f5d32443c04191220f20d56211fc77` was pushed to `origin/master`; live remote and local hashes matched. Production images were freshly built from that commit, without RTT click injection, the two-anchor overlay, synthetic transmission, or forced-hop restrictions.

| Probe | Production role | Hardware node |
|---|---|---|
| E46070D247233537 | clicker (former gateway connection / forced-hop test board) | 0xea35f5b508104aae |
| E4645C15CB365D30 | anchor | 0x0f6d3a3bdac0f858 |
| E46070D247394D36 | anchor | 0x9699122bd60a64e3 |
| E4645C15CB0F3B37 | anchor (previous test clicker) | 0xc1c2306a5138ab2d |

All four `west flash` operations succeeded at 4 MHz with sector erase. Live FICR reads matched the selected cohort. The two changing-role storage partitions were backed up first. The original same-board clicker storage backup was restored on E46070D247233537 and read back exactly; E4645C15CB0F3B37 received an empty anchor partition after backup. The other anchors retained their storage. No gateway firmware was flashed.

RTT confirmed all four production presets, successful node initialization and normal retained idle on the clicker. A fresh all-direct enumeration and survey passed: three anchors, three pairs, fifteen of fifteen samples, no partial flags. Production clicker RTT injection is disabled; no fresh physical-button click was exercised in this deployment. Earlier click qualification is in the committed cleanup evidence.

RAM margin is 6,336 bytes for the anchor and 33,456 bytes for the clicker. `production-builds.json`, `cohort.json`, `storage-migration.json`, flash logs and the survey summary preserve the deployment details. Unrelated Obsidian workspace and capture-ledger changes remain untouched.

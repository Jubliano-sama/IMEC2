# Three additional production anchors

All three boards were programmed concurrently at 4 MHz using west flash --skip-rebuild. Full image readback passed on every board. Programming and readback took 22.08 seconds, with 17.24 seconds of overlap between all three flash processes.

| Probe | FICR | Node ID | Final role |
|---|---|---|---|
| `E46070D247233537` | `0xeccb411ad01c0d43` | `0xa586045991524e41` | `mesh_anchor` |
| `E4645C15CB0F3B37` | `0xf92c40599992d2d9` | `0xb061051ad8dc91db` | `mesh_anchor` |
| `E4645C15CB365D30` | `0x592f7f0631d1c602` | `0x10623a45709f8500` | `mesh_anchor` |

Previous clicker-role storage was backed up and double-read verified, then storage sectors 0x7a000-0x80000 were initialized for the anchor role.

Startup RTT passed on all three: a single observed boot, completed anchor initialization, repeated Channel-5 scanning and no fatal/assertion/reset loop during the 25-second capture.

The unchanged image from commit e65885c6b61fb034b4e4915627e49bc27dc5c8dd was reused. Its existing gates passed 190 mesh_integration and 182 hardware_models tests in the first batch. Current incremental build reported no work; the HEX hash matched exactly. Static RAM headroom remains 5,760 bytes.

HEX SHA-256: `9ffb981f7467bc5c7e0f252902373732830a28eec71dd65cd708c3fdc1c6e204`. Exact readback: 464,840 bytes per board.

Enumeration, survey and physical-click delivery were not exercised.

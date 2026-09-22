# Four additional production anchors

All four boards were programmed concurrently at 4 MHz using west flash --skip-rebuild. Full image readback passed on every board. Programming and readback for the batch took 21.89 seconds; all four programming processes overlapped for 16.84 seconds.

| Probe | FICR | Node ID | Final role |
|---|---|---|---|
| `E46070D247233537` | `0x989f3e5219ebd3ae` | `0xd1d27b1158a590ac` | `mesh_anchor` |
| `E46070D247394D36` | `0x64feb35d48cef126` | `0x2db3f61e0980b224` | `mesh_anchor` |
| `E4645C15CB0F3B37` | `0xbeb265a9c0f6547a` | `0xf7ff20ea81b81778` | `mesh_anchor` |
| `E4645C15CB365D30` | `0x725e11427ca1ee89` | `0x3b1354013defad8b` | `mesh_anchor` |

Every board is distinct from the preceding four-board batch. Previous clicker-role storage was backed up and double-read verified, then storage sectors 0x7a000-0x80000 were initialized for the anchor role.

Startup RTT passed on all four: a single observed boot, completed anchor initialization, repeated Channel-5 scanning and no fatal/assertion/reset loop during the 25-second capture.

The unchanged image from commit e65885c6b61fb034b4e4915627e49bc27dc5c8dd was reused. Its existing gates passed 190 mesh_integration and 182 hardware_models tests in the preceding batch. Current incremental build reported no work; the HEX hash matched exactly. Static RAM headroom remains 5,760 bytes.

HEX SHA-256: `9ffb981f7467bc5c7e0f252902373732830a28eec71dd65cd708c3fdc1c6e204`. Exact readback: 464,840 bytes per board.

Enumeration, survey and physical-click delivery were not exercised.

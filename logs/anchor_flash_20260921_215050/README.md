# Four production anchors flashed

Image: `mesh_anchor`, commit `e65885c6b61fb034b4e4915627e49bc27dc5c8dd`.

All four boards passed exact image readback (464,840 bytes each) and 25-second startup RTT capture, with one boot, completed anchor initialization and repeated Channel-5 scanning. No fatal/assertion/reset loop was observed.

| Probe | Node ID | Final role |
|---|---|---|
| `E46070D247233537` | `0xfd6e9f4b704834f4` | `mesh_anchor` |
| `E46070D247394D36` | `0x2a2174ad70f39af5` | `mesh_anchor` |
| `E4645C15CB0F3B37` | `0x24652941fceb8683` | `mesh_anchor` |
| `E4645C15CB365D30` | `0xcfce33d3d33dc4c5` | `mesh_anchor` |

The previous clicker-role storage was backed up twice before migration, then the six storage sectors at 0x7a000-0x80000 were erased and verified. Firmware was programmed with west flash at 4 MHz using normal sector erase.

Checks passed: 190 mesh_integration tests and 182 hardware_models tests, run sequentially. Static RAM: 125,312 / 131,072 bytes, leaving 5,760 bytes.

Enumeration, survey and physical-click delivery were not exercised by this flash task.

HEX SHA-256: `9ffb981f7467bc5c7e0f252902373732830a28eec71dd65cd708c3fdc1c6e204`.

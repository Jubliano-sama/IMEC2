# Final two production anchors completed

Both boards passed full image readback and 25-second startup capture with a single boot, completed anchor initialization, repeated Channel-5 scanning and no observed fatal/assertion/reset loop.

| Final programming probe | FICR | Node ID | Role |
|---|---|---|---|
| `E4645C15CB365D30` | `0xe99e94d01e918e9b` | `0xa0d3d1935fdfcd99` | `mesh_anchor` |
| `E4645C15CB365D30` | `0xddc6576302758373` | `0x948b1220433bc071` | `mesh_anchor` |

The second board initially used probe E46070D247233537, which intermittently lost SWD communication before programming. The user moved it to E4645C15CB365D30 and its cable. The connection failed once after the swap, then attached successfully at 4 MHz. Its FICR identity was rechecked before migration and readback. The final two boards were completed sequentially because of connection failures.

Both prior clicker storage partitions were backed up and double-read verified before migration, then the six sectors at 0x7a000-0x80000 were erased and verified. Programming used west flash at 4 MHz with normal sector erase.

Unchanged production image SHA-256: `9ffb981f7467bc5c7e0f252902373732830a28eec71dd65cd708c3fdc1c6e204`; 464,840 image bytes verified on each board. Existing 190 mesh_integration and 182 hardware_models checks apply to this exact image; RAM headroom is 5,760 bytes.

Enumeration, survey and click delivery were not exercised.

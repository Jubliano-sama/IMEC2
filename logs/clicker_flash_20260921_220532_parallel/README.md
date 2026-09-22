# Three additional production clickers completed

All three protected boards were unlocked with a full-chip erase explicitly authorized by the user. Original data could not be read or backed up due to access protection. Subsequent flashing, readback and RTT sessions disabled automatic unlocking.

| Probe | FICR | Node ID | Final role |
|---|---|---|---|
| `E46070D247394D36` | `0xb0605de7eb7f21cd` | `0xf92d18a4aa3162cf` | `mesh_clicker` |
| `E4645C15CB0F3B37` | `0xa7c8b90c9bf88acc` | `0xee85fc4fdab6c9ce` | `mesh_clicker` |
| `E4645C15CB365D30` | `0x4b140123a2e5568a` | `0x02594460e3ab1588` | `mesh_clicker` |

All three were programmed concurrently at 4 MHz using west flash. Unlock, programming and full readback completed in 20.24 seconds. Every board passed exact image readback and 25-second startup capture with one boot, successful node communication initialization and the retained-idle marker, with no observed fatal/assertion/reset loop. Physical button, ranging and report delivery were not exercised.

The unchanged production mesh_clicker image from source commit e65885c6b61fb034b4e4915627e49bc27dc5c8dd was reused. Incremental build reported no work. RAM headroom: 31,856 bytes. Existing session tests passed 190 mesh_integration and 182 hardware_models checks against unchanged firmware source.

HEX SHA-256: `8733d894b8be53b8ea9a72d10d352b06f2b3179cb9552b76144e0eb206fb9fc4`. Readback verified 435,432 bytes per board.

Combined with the preceding clicker batch, six distinct boards have passed programming/readback and startup verification.

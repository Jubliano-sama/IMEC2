# Three additional production clickers completed

Continued the approved erase-and-flash workflow for three more access-protected boards. Full-chip unlock erased their original contents, which protection prevented backing up. All subsequent flashing, readback and RTT sessions disabled automatic unlocking.

| Probe | FICR | Node ID | Final role |
|---|---|---|---|
| `E46070D247394D36` | `0xe05517bda628dce6` | `0xa91852fee7669fe4` | `mesh_clicker` |
| `E4645C15CB0F3B37` | `0x1ab9ae846d358193` | `0x53f4ebc72c7bc291` | `mesh_clicker` |
| `E4645C15CB365D30` | `0x40dbef975afd22e3` | `0x0996aad41bb361e1` | `mesh_clicker` |

All three were programmed concurrently at 4 MHz using west flash. Unlock, programming and full readback completed in 19.90 seconds. Every board passed exact image readback and 25-second startup capture with one boot, successful node communication initialization and the retained-idle marker, with no observed fatal/assertion/reset loop. Physical button, ranging and report delivery were not exercised.

The unchanged production mesh_clicker image from source commit e65885c6b61fb034b4e4915627e49bc27dc5c8dd was reused. Incremental build reported no work. RAM headroom: 31,856 bytes. Existing session tests passed 190 mesh_integration and 182 hardware_models checks against unchanged firmware source.

HEX SHA-256: `8733d894b8be53b8ea9a72d10d352b06f2b3179cb9552b76144e0eb206fb9fc4`. Readback verified 435,432 bytes per board.

Combined with the preceding clicker batches, nine distinct boards have passed programming/readback and startup verification.

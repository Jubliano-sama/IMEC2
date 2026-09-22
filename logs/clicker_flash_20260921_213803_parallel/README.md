# Three production clickers completed

The requested four-board batch was reduced to the three reachable boards at the user's instruction. E46070D247233537 was not programmed because its SWD connection was unavailable.

| Probe | FICR | Node ID | Final role |
|---|---|---|---|
| `E46070D247394D36` | `0x26dc39f01da1f06d` | `0x6f917cb35cefb36f` | `mesh_clicker` |
| `E4645C15CB0F3B37` | `0xa378b0f6495e09ac` | `0xea35f5b508104aae` | `mesh_clicker` |
| `E4645C15CB365D30` | `0xecfd972519999c1c` | `0xa5b0d26658d7df1e` | `mesh_clicker` |

All three were programmed concurrently using west flash at 4 MHz with normal sector erase and auto_unlock=False. Full image readback passed for every board. Startup RTT over 25 seconds showed one boot per board, the production clicker preset, successful node communication initialization and the retained-idle local-debug marker, with no observed fatal/assertion/reset loop. Physical button/ranging/report delivery was not exercised.

**Initial preflight side effect:** pyOCD defaults auto_unlock=True. It automatically mass-erased the access-protected boards on probes 4D36 and 5D30 during initial connection before any backup could be taken. Their prior contents were not preserved. The blank post-unlock images were saved; these are not backups of the originals. All later preflight, flashing, readback and RTT sessions explicitly disabled automatic unlock.

The existing clicker on probe 3B37 had readable same-role storage; it was backed up and preserved using sector erase.

Build: production mesh_clicker at source commit e65885c6b61fb034b4e4915627e49bc27dc5c8dd. RAM 99,216 / 131,072 bytes, leaving 31,856 bytes. The 190 mesh_integration and 182 hardware_models tests previously passed in this session against the unchanged firmware source. The clicker hardware build completed successfully.

HEX SHA-256: `8733d894b8be53b8ea9a72d10d352b06f2b3179cb9552b76144e0eb206fb9fc4`. Readback verified 435,432 bytes per board.

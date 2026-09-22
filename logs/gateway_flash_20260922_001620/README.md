# Production gateway flash

Probe `E4645C15CB365D30`, FICR `0xeefc9670907f2ef7`, gateway node `0x9999888877776666`: programmed with the production `mesh_gateway` preset using west flash at 4 MHz, with automatic unlocking disabled.

Existing gateway-role storage was double-read backed up and preserved through normal sector erase. No role migration or storage erase was needed.

Full readback verified 479,682 image bytes. The 25-second RTT capture showed one boot, the gateway preset, successful BLE initialization and active advertising, with no observed fatal/assertion/reset loop. A passive BLE scan observed IMEC Mesh Test Gateway at E0:85:31:10:C4:17; the RTT independently proves this board started advertising.

Source commit: e65885c6b61fb034b4e4915627e49bc27dc5c8dd. Build RAM: 126,144 / 131,072 bytes, leaving 4,928 bytes. Existing session checks passed 190 mesh_integration and 182 hardware_models tests against the unchanged firmware source; the gateway hardware build completed successfully.

HEX SHA-256: `06347ee9a94a58b2f31d6d9fef04c67fcf41c97f08e16bb1ebf6694dd9dcc379`.

Enumeration, survey and click-delivery qualification were not performed during this flash.

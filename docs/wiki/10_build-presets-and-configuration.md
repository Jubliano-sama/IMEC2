<!-- PAGE_ID: imec2-10-build-and-configuration -->

[Wiki home](README.md) / Build Presets, Configuration, and Repository Boundaries

<details>
<summary>📚 Relevant source files</summary>

The following files were used as context for generating this wiki page:

- [AGENTS.md:3-122](../../AGENTS.md#L3-L122)
- [CODEMAP.md:62-116](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/CODEMAP.md#L62-L116)
- [CMakeLists.txt:1-77](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L1-L77)
- [CMakeLists.txt:1-364](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L1-L364)
- [Kconfig:1-38](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/Kconfig#L1-L38)
- [prj.conf:1-32](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/prj.conf#L1-L32)
- [prj-clicker.conf:1-12](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/prj-clicker.conf#L1-L12)
- [prj-gateway.conf:1-43](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/prj-gateway.conf#L1-L43)
- [mesh-clicker.conf:1-8](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/conf/mesh-clicker.conf#L1-L8)
- [mesh-anchor.conf:1-4](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/conf/mesh-anchor.conf#L1-L4)
- [app.overlay:1-90](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/app.overlay#L1-L90)
- [app_config.h:20-102](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_config.h#L20-L102)

</details>

# Build Presets, Configuration, and Repository Boundaries

The participant's click becomes trustworthy research data only when each board runs the exact artifact intended for its place in the story. This page shows how to build those artifacts while keeping product behavior, bench experiments, and imported dependencies separate.

> **Related pages:** [Product roles](01_product-roles-and-firmware-lines.md) · [Gateway and host tools](09_gateway-host-tools-and-observability.md) · [Verified deployment](11_verified-deployment-and-qualification.md) · [Testing and release evidence](12_testing-simulation-and-release-evidence.md)

---

<!-- BEGIN:AUTOGEN imec2-10-build-and-configuration-workspace-boundaries -->
## Workspace and Dependency Boundaries

IMEC2 is a west workspace, but the product-owned implementation is concentrated under `firmware/`. Shared, hardware-independent interfaces and logic live in `firmware/include/` and `firmware/src/`; Zephyr integration, GPIO, BLE, the DWM3000 port, board configuration, and role orchestration live in `firmware/app/` ([AGENTS.md:5-12](../../AGENTS.md#L5-L12), [CODEMAP.md:79-114](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/CODEMAP.md#L79-L114)). That boundary lets the same protocol, routing, timing, persistence, and survey logic run in native tests before it is linked into a board image.

| Area | Ownership | Working rule |
|---|---|---|
| `firmware/include/`, `firmware/src/`, `firmware/tests/` | IMEC2 | Put portable behavior here and exercise it with native CMake/CTest. |
| `firmware/app/` | IMEC2 | Keep Zephyr, device-tree, GPIO, BLE, SPI, DWM3000, and role orchestration here. |
| `Documentation/` | IMEC2 | Keep architecture and behavioral contracts aligned with intentional behavior changes. |
| `zephyr/`, `nrf/`, `nrfxlib/`, `modules/`, `bootloader/` | Imported west dependencies | Do not edit unless a task explicitly targets an imported dependency. |
| `dwm3000 examples and sdk/` | External DWM3000 reference | Treat it as external; the app points `DWM3000_SDK_DIR` at this tree rather than taking ownership of it ([CMakeLists.txt:97-100](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L97-L100)). |
| `archive/old-dw1000-impl/` | Historical reference | Read if useful, but do not modify it. |

The board contract is also repository-owned: `app.overlay` removes the development-kit aliases and assigns the click button, two RGB status LEDs, DWM3000 control lines, and battery signals to the project hardware ([app.overlay:6-43](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/app.overlay#L6-L43), [app.overlay:45-90](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/app.overlay#L45-L90)).

Sources: [AGENTS.md:3-12](../../AGENTS.md#L3-L12), [CODEMAP.md:62-116](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/CODEMAP.md#L62-L116), [CMakeLists.txt:97-100](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L97-L100), [app.overlay:6-90](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/app.overlay#L6-L90)
<!-- END:AUTOGEN imec2-10-build-and-configuration-workspace-boundaries -->

---

<!-- BEGIN:AUTOGEN imec2-10-build-and-configuration-native-build -->
## Native Core Build

Create the repository-local Python environment before invoking west or the Zephyr scripts. The documented setup deliberately uses a workspace-local uv cache and installs both Zephyr and nRF requirements into `.venv` ([AGENTS.md:14-21](../../AGENTS.md#L14-L21)).

```sh
UV_CACHE_DIR=$PWD/.uv-cache uv venv --clear .venv
UV_CACHE_DIR=$PWD/.uv-cache uv pip install --python .venv/bin/python \
  -r zephyr/scripts/requirements.txt \
  -r nrf/scripts/requirements.txt
```

The native build is the shortest path from a behavior change to evidence:

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
```

The root firmware CMake project fixes C11 without compiler extensions, builds the platform-independent modules into `core`, exposes `firmware/include/`, enables warnings as errors for GCC and Clang, and registers its executables with CTest ([CMakeLists.txt:1-55](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L1-L55)). This build validates portable behavior; it does not compile the device tree, Zephyr drivers, or role-specific board runtime.

Sources: [AGENTS.md:14-29](../../AGENTS.md#L14-L29), [CMakeLists.txt:1-55](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L1-L55)
<!-- END:AUTOGEN imec2-10-build-and-configuration-native-build -->

---

<!-- BEGIN:AUTOGEN imec2-10-build-and-configuration-exact-role-presets -->
## Exact Zephyr Role Presets

Build the production-candidate line by exact preset name. These three presets are the source of truth for current product behavior: the sleeping participant clicker, the common hardware-identified anchor, and the mesh-root gateway with its PC BLE edge ([AGENTS.md:31-47](../../AGENTS.md#L31-L47)).

```sh
.venv/bin/west build --no-sysbuild -s firmware/app \
  -b nrf52833dk/nrf52833 --build-dir build/mesh-clicker \
  -- -DIMEC_BUILD_PRESET=mesh_clicker

.venv/bin/west build --no-sysbuild -s firmware/app \
  -b nrf52833dk/nrf52833 --build-dir build/mesh-anchor \
  -- -DIMEC_BUILD_PRESET=mesh_anchor

.venv/bin/west build --no-sysbuild -s firmware/app \
  -b nrf52833dk/nrf52833 --build-dir build/mesh-gateway \
  -- -DIMEC_BUILD_PRESET=mesh_gateway
```

Those commands are the repository's documented exact builds ([AGENTS.md:62-72](../../AGENTS.md#L62-L72)). The preset is more than a label: `firmware/app/CMakeLists.txt` maps it to a role, optional variant flags, identity policy, role fragment, and stack diagnostics; an unknown preset fails configuration ([CMakeLists.txt:149-215](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L149-L215)).

Configuration composes in layers, so change the owner of a setting instead of adding a late override in an unrelated file:

| Layer | What it owns |
|---|---|
| CMake preset | Selects `FIRMWARE_ROLE`, variant flags, identity behavior, and the preset-specific `.conf` fragment. |
| Base Kconfig fragment | `prj.conf` owns the shared peripheral, logging, workqueue, watchdog, power-management, and no-UART baseline; gateway uses `prj-gateway.conf`, while clicker adds `prj-clicker.conf` ([CMakeLists.txt:223-228](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L223-L228), [prj.conf:1-32](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/prj.conf#L1-L32)). |
| Role and preset fragments | Clicker enables its courtesy BLE and retained idle policy; gateway enables connected BLE GATT; `mesh-clicker.conf` limits timing-sensitive logging and enables its communication queue; `mesh-anchor.conf` reserves the measured main-stack margin ([prj-clicker.conf:1-12](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/prj-clicker.conf#L1-L12), [prj-gateway.conf:22-43](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/prj-gateway.conf#L22-L43), [mesh-clicker.conf:1-8](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/conf/mesh-clicker.conf#L1-L8), [mesh-anchor.conf:1-4](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/conf/mesh-anchor.conf#L1-L4)). |
| Generated fragments | Anchor and gateway builds receive flash/NVS settings; route-test names and stack diagnostics are generated or appended before Zephyr configuration begins ([CMakeLists.txt:230-243](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L230-L243), [CMakeLists.txt:293-343](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L293-L343)). |
| Device tree | `app.overlay` owns physical pins and hardware aliases. |
| App-local compile-time contract | `app_config.h` maps numeric roles, device and gateway identity, network ID, and build metadata into the application ([app_config.h:20-102](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_config.h#L20-L102)). It also fixes gateway BLE queue and retry pacing, including a 2 ms base retry and a 128 ms maximum delay ([app_config.h:123-130](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_config.h#L123-L130)); the application refuses a build if that cap is lower than the base delay ([main.c:153-158](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/main.c#L153-L158)). |

The `mesh_anchor` preset deliberately clears any fixed `DEVICE_ID` and enables the hardware-derived anchor identity, so every production anchor can run the same artifact while retaining a stable physical identity ([CMakeLists.txt:198-213](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L198-L213)).

Sources: [AGENTS.md:31-72](../../AGENTS.md#L31-L72), [CMakeLists.txt:149-243](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L149-L243), [CMakeLists.txt:293-364](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L293-L364), [app_config.h:20-102](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_config.h#L20-L102), [app_config.h:123-130](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_config.h#L123-L130), [main.c:153-158](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/main.c#L153-L158)
<!-- END:AUTOGEN imec2-10-build-and-configuration-exact-role-presets -->

---

<!-- BEGIN:AUTOGEN imec2-10-build-and-configuration-nonproduction-configuration -->
## Nonproduction Configuration

The remaining presets answer narrower engineering questions; they do not redefine the product story.

| Line | Use it for | Do not treat it as |
|---|---|---|
| `mesh_transmitter` | Synthetic route, retry, and load traffic that may choose a direct gateway hop. | A production anchor. |
| `mesh_transmitter_forcedhop` | Relay regression where a forced intermediate hop is the behavior under test. | Proof from the generic transmitter; that preset may deliver directly ([AGENTS.md:74-83](../../AGENTS.md#L74-L83)). |
| `ml_clicker`, `ml_anchor_1` through `ml_anchor_8` | Distance-offset training and validation capture; each ML anchor preset receives a deterministic ID and slot ([AGENTS.md:108-116](../../AGENTS.md#L108-L116), [CMakeLists.txt:149-169](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L149-L169)). | Production clicker or anchor behavior. |
| `gateway_ble_connectivity_test` | An isolated gateway BLE connectivity smoke image, optionally configured as a passive range scanner. | The mesh gateway's complete UWB-to-host path ([CMakeLists.txt:145-148](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L145-L148), [CMakeLists.txt:245-260](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L245-L260)). |
| `tag_stage*`, `anchor_stage*`, `gateway_stage3_highdebug` | Staged hardware bring-up with stage and role fragments layered over `high-debug.conf`. | A production candidate; high-debug changes runtime observability and staged behavior ([CMakeLists.txt:108-145](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L108-L145), [CMakeLists.txt:268-291](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L268-L291)). |
| `FIRMWARE_ROLE=clicker|anchor|gateway` | Legacy compatibility and bounded regression builds. | The current connected-routing runtime contract ([AGENTS.md:85-106](../../AGENTS.md#L85-L106)). |

Always record the exact preset and build directory in test evidence. A label such as “anchor build” is ambiguous because it could mean the production `mesh_anchor`, an ML collector, a forced-hop traffic source, a high-debug stage, or the legacy direct role; those images make different promises and require different interpretation.

Sources: [AGENTS.md:49-116](../../AGENTS.md#L49-L116), [CMakeLists.txt:108-215](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L108-L215), [CMakeLists.txt:245-291](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L245-L291)
<!-- END:AUTOGEN imec2-10-build-and-configuration-nonproduction-configuration -->

---

← **Previous:** [Gateway, Host Tools, and Observability](09_gateway-host-tools-and-observability.md)

**Next:** [Verified Mesh Deployment and Hardware Qualification](11_verified-deployment-and-qualification.md) →

**Related:** [Product Roles and Firmware Lines](01_product-roles-and-firmware-lines.md) · [Testing, Simulation, and Release Evidence](12_testing-simulation-and-release-evidence.md) · [Start from the user story](README.md)

<!-- PAGE_ID: imec2-10-build-and-configuration -->

[Wiki home](README.md) / Build Presets, Configuration, and Repository Boundaries

<details>
<summary>📚 Historical generation context (f6594e4)</summary>

The following files were used as context for generating this wiki page:

- [AGENTS.md:3-123](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/AGENTS.md#L3-L123)
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

The repository has one product-owned firmware tree inside a larger west workspace. Portable protocols and state live in `firmware/include/` and `firmware/src/`; native tests live in `firmware/tests/`; and Zephyr, GPIO, BLE, settings, SPI, DWM3000, and role orchestration stay in `firmware/app/` ([AGENTS.md:36-47](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/AGENTS.md#L36-L47), [CODEMAP.md:79-115](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/CODEMAP.md#L79-L115)). That split is the test boundary: portable behavior can run under host CMake before the hardware adapter is linked.

| Area | Repository rule | Consequence |
|---|---|---|
| `firmware/include/`, `firmware/src/`, `firmware/tests/` | Own portable contracts, state, and native evidence. | Hardware-independent behavior should not depend on Zephyr headers or devices. |
| `firmware/app/` | Own Zephyr and board integration. | GPIO, BLE, settings, SPI, DWM3000, workqueues, and role adapters stay here. |
| `firmware/scripts/`, `.github/workflows/` | Own executable repository and release gates. | A prose rule is incomplete until the corresponding check fails closed. |
| `Documentation/` | Own current contracts and operator procedures. | Intentional behavior changes update code, tests, and the relevant contract together. |
| `zephyr/`, `nrf/`, `modules/`, `nrfxlib/`, `bootloader/`, the DWM3000 vendor tree | Imported dependencies. | Do not edit them unless the task explicitly targets that dependency; the app references the vendor SDK through `DWM3000_SDK_DIR` ([CMakeLists.txt:104-106](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/CMakeLists.txt#L104-L106)). |
| `archive/old-dw1000-impl/` | Historical reference. | It may explain history but is not current behavior and must not be modified. |

Before work changes any of these surfaces, `agent_preflight.py` receives the planned paths and operations and returns the applicable current rules and references; the append-only issue ledger is context, while `AGENT_CURRENT_ISSUES.json` is the curated present-tense overlay ([AGENTS.md:9-34](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/AGENTS.md#L9-L34)). This avoids treating an old fix as an active constraint.

The accepted architecture reset rejects a big-bang product or protocol rewrite. It freezes growth in the current orchestration monoliths, then replaces the gateway survey, radio admission, delivery custody, and textual-fragment owners one at a time behind the existing contracts ([Architecture Reset Plan.md:8-20](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/Documentation/Architecture%20Reset%20Plan.md#L8-L20), [Architecture Reset Plan.md:112-133](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/Documentation/Architecture%20Reset%20Plan.md#L112-L133)). New C files default to 2,500 lines, existing oversized files and include fragments are frozen at recorded ceilings, and the composed `app_anchor.c`, `app_mesh_report.c`, `dwm3000_driver.c`, and `mesh_relay.c` totals may not grow ([architecture_boundaries.json:8-15](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/architecture_boundaries.json#L8-L15), [architecture_boundaries.json:39-80](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/architecture_boundaries.json#L39-L80)). The checker loads those ceilings from an immutable ancestor commit rather than trusting the candidate checkout, so squashing, rebasing, pruning, or reconstructing that policy object fails closed ([check_architecture_boundaries.py:42-45](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/check_architecture_boundaries.py#L42-L45), [check_architecture_boundaries.py:866-929](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/check_architecture_boundaries.py#L866-L929)). These are no-growth limits, not approval of the current architecture.

Sources: [AGENTS.md:9-47](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/AGENTS.md#L9-L47), [AGENTS.md:160-185](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/AGENTS.md#L160-L185), [Architecture Reset Plan.md:8-64](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/Documentation/Architecture%20Reset%20Plan.md#L8-L64), [CODEMAP.md:62-115](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/CODEMAP.md#L62-L115), [architecture_boundaries.json:8-80](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/architecture_boundaries.json#L8-L80), [check_architecture_boundaries.py:42-45](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/check_architecture_boundaries.py#L42-L45), [check_architecture_boundaries.py:866-929](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/check_architecture_boundaries.py#L866-L929)
<!-- END:AUTOGEN imec2-10-build-and-configuration-workspace-boundaries -->

---

<!-- BEGIN:AUTOGEN imec2-10-build-and-configuration-native-build -->
## Native Core Build

Create the repository-local Python environment when it is absent. It includes the Zephyr, nRF, and native-test requirements used by the one verification entrypoint ([Development and Deployment Guide.md:30-39](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/Documentation/Development%20and%20Deployment%20Guide.md#L30-L39)).

```sh
UV_CACHE_DIR=$PWD/.uv-cache uv venv --clear .venv
UV_CACHE_DIR=$PWD/.uv-cache uv pip install --python .venv/bin/python \
  -r zephyr/scripts/requirements.txt \
  -r nrf/scripts/requirements.txt \
  -r firmware/tests/requirements-native.txt
```

Use the repository verifier for final native evidence:

```sh
.venv/bin/python firmware/scripts/verify_changes.py
```

It first runs repository-truth, architecture-boundary, agent-guidance, deployment-policy, and negative self-tests. It then configures a fresh Debug build, builds it, runs the complete CTest suite, and executes the deterministic 500-seed busy-line stress gate ([verify_changes.py:35-68](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verify_changes.py#L35-L68), [verify_changes.py:195-273](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verify_changes.py#L195-L273)). `--sanitizers` enables ASan and UBSan in a separate build, while `--checks-only` is the fast documentation/source-policy path and is not firmware-behavior qualification ([verify_changes.py:412-473](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verify_changes.py#L412-L473), [verify_changes.py:476-541](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verify_changes.py#L476-L541)).

Every run fingerprints committed, dirty, and nonignored untracked source state, then executes from a no-hardlink Git snapshot guarded against writes ([verification_inputs.py:201-246](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L201-L246), [verification_inputs.py:476-563](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L476-L563)). Zephyr matrices additionally require the exact 52-project lock ([west_projects.lock.json:3-316](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/west_projects.lock.json#L3-L316)), clean dependency trees without concealing index flags or influential ignored files, and no ambient module, toolchain, CMake, or compiler-search overrides ([verification_inputs.py:119-160](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L119-L160), [verification_inputs.py:984-1076](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L984-L1076), [verification_inputs.py:1186-1237](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L1186-L1237)). The pinned `.west/config`, frozen manifest, and live manifest are read through one bounded regular-file reader, the live manifest must be byte-identical to the snapshot, and the metadata plus resolved project set are checked again after the matrix ([verification_inputs.py:667-703](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L667-L703), [verification_inputs.py:706-883](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L706-L883), [verification_inputs.py:1369-1488](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L1369-L1488)). This makes a green result belong to one stable application and dependency graph instead of whichever files happened to be visible during the run. It still does not content-attest the host tools selected through `PATH` or the CMake package registry, so container-grade reproducibility needs a separately pinned toolchain image.

The native project itself fixes C11 without extensions, enables ASan/UBSan only through its explicit option, builds the portable modules into `core`, and treats GCC/Clang warnings as errors ([CMakeLists.txt:1-25](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/CMakeLists.txt#L1-L25), [CMakeLists.txt:27-70](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/CMakeLists.txt#L27-L70)). CI runs both the normal and sanitizer verifier paths from fresh checkouts and retains deterministic replay artifacts when either fails ([firmware-verification.yml:9-38](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/.github/workflows/firmware-verification.yml#L9-L38)).

Sources: [Development and Deployment Guide.md:30-70](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/Documentation/Development%20and%20Deployment%20Guide.md#L30-L70), [verify_changes.py:35-273](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verify_changes.py#L35-L273), [verify_changes.py:476-588](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verify_changes.py#L476-L588), [verification_inputs.py:119-160](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L119-L160), [verification_inputs.py:201-246](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L201-L246), [verification_inputs.py:476-563](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L476-L563), [verification_inputs.py:667-883](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L667-L883), [verification_inputs.py:984-1076](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L984-L1076), [verification_inputs.py:1186-1488](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verification_inputs.py#L1186-L1488), [firmware-verification.yml:9-38](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/.github/workflows/firmware-verification.yml#L9-L38)
<!-- END:AUTOGEN imec2-10-build-and-configuration-native-build -->

---

<!-- BEGIN:AUTOGEN imec2-10-build-and-configuration-exact-role-presets -->
## Exact Zephyr Role Presets

Build the production-candidate line by exact preset name. The three current product artifacts are `mesh_clicker`, `mesh_anchor`, and `mesh_gateway`; generic role labels are insufficient because other presets intentionally compile different behavior ([AGENTS.md:49-60](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/AGENTS.md#L49-L60)).

```sh
.venv/bin/west build --pristine=always --no-sysbuild -s firmware/app \
  -b nrf52833dk/nrf52833 --build-dir build/mesh-clicker \
  -- -DIMEC_BUILD_PRESET=mesh_clicker

.venv/bin/west build --pristine=always --no-sysbuild -s firmware/app \
  -b nrf52833dk/nrf52833 --build-dir build/mesh-anchor \
  -- -DIMEC_BUILD_PRESET=mesh_anchor

.venv/bin/west build --pristine=always --no-sysbuild -s firmware/app \
  -b nrf52833dk/nrf52833 --build-dir build/mesh-gateway \
  -- -DIMEC_BUILD_PRESET=mesh_gateway
```

These are the documented direct-iteration commands; the final Zephyr-facing gate should normally invoke `verify_changes.py --exact-roles --compatibility-builds` so it starts pristine, runs each exact role's static stack verifier, and executes the real Zephyr NVS persistence binary on `native_sim/native/64` ([Development and Deployment Guide.md:72-87](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/Documentation/Development%20and%20Deployment%20Guide.md#L72-L87), [verify_changes.py:290-358](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verify_changes.py#L290-L358)).

| Layer | What it owns |
|---|---|
| CMake preset mapping | Selects role, variant flags, identity policy, preset `.conf`, stack diagnostics, and whether the artifact is deployable. Unknown presets fail configuration ([CMakeLists.txt:161-235](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/CMakeLists.txt#L161-L235)). |
| Base and role Kconfig | `prj.conf` owns the common runtime; clicker appends `prj-clicker.conf`; gateway replaces the base with `prj-gateway.conf`; anchor and gateway builds receive generated flash/NVS configuration ([CMakeLists.txt:241-261](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/CMakeLists.txt#L241-L261)). |
| Preset fragments | `mesh-clicker.conf` bounds timing-sensitive logs and enables its communication queue, while `mesh-anchor.conf` reserves the compiler-measured main-stack margin ([mesh-clicker.conf:1-8](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/conf/mesh-clicker.conf#L1-L8), [mesh-anchor.conf:1-4](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/conf/mesh-anchor.conf#L1-L4)). |
| Compile-time app contract | CMake emits numeric `DEVICE_ROLE`; `app_config.h` maps role, device identity, network identity, build metadata, and bounded queue/timing constants into the application ([CMakeLists.txt:393-403](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/CMakeLists.txt#L393-L403), [app_config.h:21-103](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/src/app_config.h#L21-L103)). |
| Build identity and stack evidence | The artifact embeds preset, git, timestamp, board, and stack build identity, and every app object emits stack-usage and IPA call-graph evidence ([CMakeLists.txt:539-559](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/CMakeLists.txt#L539-L559)). |

`mesh_anchor` clears any fixed device ID and enables FICR-derived hardware identity, so one exact artifact can serve every production anchor while logical order remains assigned data ([CMakeLists.txt:212-230](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/CMakeLists.txt#L212-L230)).

Sources: [Development and Deployment Guide.md:72-92](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/Documentation/Development%20and%20Deployment%20Guide.md#L72-L92), [CMakeLists.txt:161-261](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/CMakeLists.txt#L161-L261), [verify_changes.py:290-358](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verify_changes.py#L290-L358)
<!-- END:AUTOGEN imec2-10-build-and-configuration-exact-role-presets -->

---

<!-- BEGIN:AUTOGEN imec2-10-build-and-configuration-nonproduction-configuration -->
## Nonproduction Configuration

The compatibility matrix keeps supported test, collection, and legacy surfaces compiling without promoting them into production.

| Line | Current purpose | Required distinction |
|---|---|---|
| `mesh_transmitter` | Powered synthetic route and load traffic. | It may choose a direct gateway path, so it cannot qualify anchor relay behavior. |
| `mesh_transmitter_forcedhop` | Powered traffic source for forced-relay retry, ACK, and preemption work. | This is the required relay qualification source, but it is still not deployed anchor firmware ([AGENTS.md:49-56](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/AGENTS.md#L49-L56)). |
| `ml_clicker` | BLE-controlled range-offset collection clicker. | It owns the collection PC link and bounds its one-notification-in-flight transport to four ACL TX buffers and a 2 KiB log ring ([ml-clicker.conf:1-21](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/conf/ml-clicker.conf#L1-L21)). |
| `ml_anchor_1` through `ml_anchor_8` | Deterministic UWB collection anchors with distinct IDs and discovery slots. | ML anchors do not carry the removed BLE debug service; their collection path is UWB ([CMakeLists.txt:172-182](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/CMakeLists.txt#L172-L182), [ml-anchor.conf:1-7](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/conf/ml-anchor.conf#L1-L7)). |
| `gateway_ble_connectivity_test` and staged high-debug presets | Isolated BLE checks or bounded hardware bring-up. | They deliberately alter or omit product runtime surfaces and do not define production behavior ([CMakeLists.txt:115-160](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/app/CMakeLists.txt#L115-L160)). |
| `FIRMWARE_ROLE=clicker|anchor|gateway` | Legacy compatibility regression. | These builds must remain compilable, but they are not the connected-routing contract ([Development and Deployment Guide.md:19-28](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/Documentation/Development%20and%20Deployment%20Guide.md#L19-L28)). |

`verify_changes.py --compatibility-builds` compiles both transmitters, `ml_clicker`, the first and last ML anchor slots, and all three generic legacy roles from pristine build directories ([verify_changes.py:69-77](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verify_changes.py#L69-L77), [verify_changes.py:361-409](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verify_changes.py#L361-L409)). CI runs that matrix beside exact-role and Zephyr persistence builds, which catches role-gating and RAM regressions that native seams cannot compile ([firmware-verification.yml:40-68](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/.github/workflows/firmware-verification.yml#L40-L68)). Passing it preserves compatibility only; production eligibility remains limited to the three exact mesh presets.

Sources: [Development and Deployment Guide.md:19-28](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/Documentation/Development%20and%20Deployment%20Guide.md#L19-L28), [verify_changes.py:69-77](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verify_changes.py#L69-L77), [verify_changes.py:361-409](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/firmware/scripts/verify_changes.py#L361-L409), [firmware-verification.yml:40-68](https://github.com/Jubliano-sama/IMEC2/blob/fda75299b34b5ec207fedd387f48e3d2652b5aea/.github/workflows/firmware-verification.yml#L40-L68)
<!-- END:AUTOGEN imec2-10-build-and-configuration-nonproduction-configuration -->

---

← **Previous:** [Gateway, Host Tools, and Observability](09_gateway-host-tools-and-observability.md)

**Next:** [Verified Mesh Deployment and Hardware Qualification](11_verified-deployment-and-qualification.md) →

**Related:** [Product Roles and Firmware Lines](01_product-roles-and-firmware-lines.md) · [Testing, Simulation, and Release Evidence](12_testing-simulation-and-release-evidence.md) · [Start from the user story](README.md)

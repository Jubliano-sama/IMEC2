# CODEMAP — Project Navigation Guide

This document is the primary map for understanding and navigating the IMEC2 codebase. It is written to be useful for both humans and LLMs.

**First actions for any task:**
1. Read `AGENTS.md` (rules, role semantics, required tests, "do not edit" zones).
2. Read the **entire `AGENT_KNOWN_ISSUES.md`** (mandatory before adding anything new, refactoring, or file ops — contains one-line tool issues and bug lessons from prior agents).
3. Read the relevant section of `Documentation/Mesh Connected Routing Contract.md` (authoritative behavioral contract).
4. Use this CODEMAP to find the right files.

---

## Project Intent (for Agents)

The system exists to support **experience sampling research** in instrumented office environments (the Living Vitality Hub). It collects subjective in-the-moment user feedback via clickers and correlates it with objective environmental sensor data.

Core requirements that drive design:
- Precise timestamps + automatic passive UWB localization.
- Extremely high robustness: no silent stalls, no dropped data, no false results or bad fallbacks. Must support multi-month reliable deployments.
- Recently added: Automated anchor self-setup using only inter-anchor distances + approximate radio radius to solve network geometry.

Everything (UWB wake strategy, channel 5 preemption, mesh routing priorities, low-duty operation, etc.) is in service of trustworthy, low-friction data collection.

When making decisions, default to "does this increase or decrease long-term robustness and data integrity?"

---

## 1. High-Level Architecture

The system is **UWB-owned**:

- **Channel 5 (control/preemption lane)**: Wake trains, discovery, ranging (DS-TWR), click claims. Has strict priority. Anchors must keep regular receive windows available.
- **Channel 9 (mesh lane)**: Reliable connected-routing mesh for click reports, transit payloads, hop/gateway ACKs, route discovery.
- **BLE**:
  - Clicker-to-clicker courtesy hints only (non-connected, channel 37).
  - Gateway-to-PC edge only (connected GATT with COBS-framed packets).
- No UWB traffic goes over BLE.

Core invariants live in the [Mesh Connected Routing Contract](Documentation/Mesh Connected Routing Contract.md).

### Roles & Build Presets (Production vs Everything Else)

**Production-candidate line (use these by default):**

| Preset                | Role     | Notes |
|-----------------------|----------|-------|
| `mesh_clicker`        | Clicker  | Normal battery device. Sleeps, wakes on button, ranges, reports via mesh. |
| `mesh_anchor`         | Anchor   | One artifact for every production anchor. Ranges clicks, relays mesh, prioritizes own reports, and persists gateway-assigned discovery order. |
| `mesh_gateway`        | Gateway  | Mesh root + connected BLE GATT to PC. Highest priority commands. |

**Other lines (do not treat as production):**

- `mesh_transmitter*` — Synthetic load generators for testing mesh.
- `ml_*` — Data collection builds for ML distance-offset model.
- Legacy `FIRMWARE_ROLE=clicker|anchor|gateway` and `stage*` / `high-debug` — Regression + bring-up only.
- Various smoke / power-profile / range-test apps under `firmware/*_test/`.

**Rule**: Always state the exact preset when discussing or flashing hardware. Generic role names are ambiguous.

---

## 2. Directory Structure (Focused View)

```
IMEC2/                          # West workspace root
├── README.md                   # This high-level entry point (new)
├── CODEMAP.md                  # You are here
├── AGENTS.md                   # Rules + detailed build/test instructions
├── archive/                    # Historical material (do not edit)
│   ├── old-dw1000-impl/        # Old DW1000 reference code
│   └── README.md
├── Documentation/              # Architecture, contracts, audits
│   ├── Mesh Connected Routing Contract.md   # ← Authoritative behavioral contract
│   ├── UWB+BLE Architecture 0.6.6.1.md      # Current architecture
│   ├── UWB+BLE Protocols and Strategies 0.3.12.3.md
│   ├── HARDWARE_BRINGUP_DEBUG.md
│   ├── ... (various notes & requirements)
│   └── ...                     # Superseded versions remain in Git history
├── firmware/                   # All project-specific code
│   ├── README.md               # Detailed implementation + bring-up checklist
│   ├── include/                # Public headers for the pure core
│   ├── src/                    # Platform-independent C core (native testable)
│   │   ├── protocol.c          # Packet/TLV/COBS encoding
│   │   ├── mesh*.c             # Mesh timing, relay, runtime, preemption
│   │   ├── route.c
│   │   ├── uwb*.c / uwb_session.c   # Wake, discovery, ranging sessions
│   │   ├── report.c, status.c, survey.c, ...
│   │   └── ...
│   ├── sim/                    # Discrete-event radio + runtime simulator
│   │   ├── mesh_sim.{c,h}
│   │   └── (used by mesh_integration tests)
│   ├── app/                    # Zephyr application
│   │   ├── src/                # Role logic, hardware glue, policies
│   │   │   ├── main.c
│   │   │   ├── app_mesh_*.c    # Coordinator, preemption, reports, ch9 ack, c5 priority, ...
│   │   │   ├── app_anchor.c / app_clicker.c
│   │   │   ├── dwm3000_*.c     # Driver + port (status-polled, no IRQ)
│   │   │   └── ...
│   │   ├── conf/               # Build preset .conf files (mesh-*.conf etc.)
│   │   ├── app.overlay         # Pin mapping
│   │   └── tests/              # Zephyr component tests
│   ├── tests/                  # Native C tests + mesh integration simulator tests
│   │   └── mesh_integration/   # High-fidelity discrete-event scenarios (mandatory gate)
│   └── *_test/                 # Standalone hardware smoke / power / range apps
│
├── manifest/                   # west.yml (pulls nRF Connect SDK)
├── dwm3000 examples and sdk/   # External reference (kept, used by some tests via DWM3000_SDK_DIR)
├── zephyr/, nrf/, nrfxlib/, modules/, bootloader/   # Imported SDKs (do not edit)
└── vendor/                     # (currently minimal)
```

**Key separation**:
- `firmware/src/` + `include/` = pure logic, can be built and tested with plain CMake + ctest.
- `firmware/app/` = everything that touches Zephyr, GPIO, BLE, the actual DWM3000 driver/port, and role orchestration.

**Code health expectation**: Agents are instructed to proactively *suggest* refactoring or splitting files when they judge them to be getting unwieldy (see AGENTS.md "Proactive Refactoring" section). They will propose a plan but will not perform the changes without your explicit confirmation. Historical examples of successful splits include breaking the original large `main.c` into many small `app_*.c` modules and extracting `app_mesh_coordinator.c`.

---

## 3. Navigation by Concern

### Packets, TLVs, Protocol Basics
- `firmware/include/protocol.h`
- `firmware/src/protocol.c`
- Tests: `firmware/tests/test_protocol.c`

### Mesh Routing, Relay, ACKs, Preemption, Timing
- Core: `firmware/src/mesh.c`, `mesh_relay.c`, `mesh_runtime.c`, `mesh_preemption.c`, `route.c`
- App glue & policy: `firmware/app/src/app_mesh_*` (coordinator, report, ch9 ack, c5 priority, handoff, etc.)
- Contract: `Documentation/Mesh Connected Routing Contract.md`
- Simulator exercising it: `firmware/tests/mesh_integration/`

### UWB Wake / Discovery / Ranging (DS-TWR)
- `firmware/include/uwb*.h`, `firmware/src/uwb*.c`, `uwb_session.c`
- App: `app_clicker.c`, `app_anchor.c`, `app_wake_train_politeness.c`
- Low-duty scan and claim logic in anchors.

### DWM3000 Hardware Interaction
- Pure model: `firmware/src/dwm3000_runtime.c` + `dwm3000_timing.c` (also used by simulator)
- Real driver: `firmware/app/src/dwm3000_driver.c`, `dwm3000_port.c`, `dwm3000_sdk_port.c`
- Important: Status polling (no IRQ line), retained sleep, slow/fast SPI ordering.

### Zephyr App Orchestration & Radio Coordination
- Entry: `firmware/app/src/main.c`
- Central decision maker: `app_mesh_coordinator.c` + `app_mesh_report.c` (mesh work allowed? click priority? etc.)
- Role files: `app_clicker.c`, `app_anchor.c`, `app_gateway_ble.c`

### Gateway BLE Transport
- Core: `firmware/src/gateway_ble_transport.c`
- App: `app_gateway_ble*.c`
- Simulator model: `firmware/tests/mesh_integration/test_gateway_ble_transport_model.c`

### Tests & Simulation
- Native unit tests: `firmware/tests/test_*.c`
- High-fidelity integration: `firmware/tests/mesh_integration/` (labeled `mesh_integration`)
- Hardware models: dwm3000, BLE transport, stack budget (labeled `hardware_models`)
- Zephyr component tests: `firmware/app/tests/`

Run focused:
```sh
ctest --test-dir firmware/build -L mesh_integration --output-on-failure
ctest --test-dir firmware/build -L hardware_models --output-on-failure
```

### Documentation Hierarchy (Current Versions Only)
- Contract (highest authority for mesh behavior): `Mesh Connected Routing Contract.md`
- Architecture: `UWB+BLE Architecture 0.6.6.1.md`
- Protocols: `UWB+BLE Protocols and Strategies 0.3.12.3.md`
- Machine-readable current set: `Documentation/CURRENT.json`
- Bring-up: `HARDWARE_BRINGUP_DEBUG.md`
- Superseded versions remain available in Git history.

---

## 4. Common Tasks & Where to Look

**Add or change mesh routing / retry / priority behavior**
→ Read Contract first. Edit `firmware/src/mesh*.c` + `route.c`. Add or update test in `firmware/tests/mesh_integration/`. Update AGENTS if contract invariants change.

**Work on click wake / discovery / ranging**
→ `firmware/src/uwb_session.c`, `uwb.c`, app clicker/anchor files, wake politeness.

**Touch DWM3000 SPI / sleep / timing**
→ `dwm3000_runtime.c` (model) and driver/port files. The runtime model enforces SPI rate ordering and retained config.

**Add a new build preset or role variant**
→ `firmware/app/conf/`, `firmware/app/CMakeLists.txt`, `app_config.h`, and role files. Document in AGENTS.md.

**Investigate a failing mesh_integration test**
→ The test file itself + `firmware/sim/mesh_sim.c`. The simulator drives the real core modules directly.

**Understand power / duty cycle assumptions**
→ Architecture 0.6.6.1 + `firmware/README.md` (the big table) + `app_radio_low_power_policy.h`.

---

## 5. Important Files (Cheat Sheet)

| Concern                        | Key Files |
|--------------------------------|-----------|
| Authoritative rules            | `AGENTS.md` |
| Behavioral contract            | `Documentation/Mesh Connected Routing Contract.md` |
| Core packet logic              | `firmware/src/protocol.c`, `include/protocol.h` |
| Mesh state & relay             | `firmware/src/mesh_relay.c`, `mesh_runtime.c` |
| App radio coordination         | `firmware/app/src/app_mesh_coordinator.c`, `app_mesh_report.c` |
| DWM3000 model (sim + tests)    | `firmware/src/dwm3000_runtime.c` |
| Real DWM3000 port              | `firmware/app/src/dwm3000_*.c` |
| Simulator                      | `firmware/sim/mesh_sim.{c,h}` |
| Production build entry         | `firmware/app/CMakeLists.txt` + `conf/mesh-*.conf` |
| Hardware pinout                | `firmware/app/app.overlay` |

---

## 6. What Not to Touch / Special Zones

- Imported trees (`zephyr/`, `nrf/`, `nrfxlib/`, `modules/`, `bootloader/`).
- `dwm3000 examples and sdk/` (external reference; some tests pull the decadriver from it via `DWM3000_SDK_DIR`).
- `archive/` contents.
- Anything that would contradict the Mesh Contract without explicit approval.

---

## 7. Build & Test Quick Reference (see AGENTS.md for full details)

Native:
```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
```

Production candidates:
```sh
.venv/bin/west build ... -DIMEC_BUILD_PRESET=mesh_clicker
.venv/bin/west build ... -DIMEC_BUILD_PRESET=mesh_anchor
.venv/bin/west build ... -DIMEC_BUILD_PRESET=mesh_gateway
```

Verified deployment (example):
```sh
.venv/bin/python firmware/scripts/flash_verified_mesh.py \
  --build-dir build/mesh-clicker \
  --probe-id <probe-id> --stage-only

.venv/bin/python firmware/scripts/capture_stack_evidence.py \
  --build-dir build/mesh-clicker \
  --probe-id <probe-id> \
  --output-dir logs/stack-evidence \
  --duration-seconds 300

.venv/bin/python firmware/scripts/flash_verified_mesh.py \
  --build-dir build/mesh-clicker \
  --hardware-manifest logs/stack-evidence/mesh-clicker-<capture-id>.json \
  --probe-id <probe-id>
```

The production mesh presets use only the verified wrapper, which fixes pyOCD
at 4 MHz. `--stage-only` is the transaction's sole programming step; the board
then runs as many qualification and regression workloads as needed without
another flash. Promotion verifies the current code sectors against the staged
artifact, permits expected NVS drift, and consumes one successful capture
without programming again. A failed qualification leaves the durable
`awaiting_qualification` transaction and staged image available for diagnosis
and retry. Bench and legacy images retain their explicitly documented
direct-flash exceptions in `AGENTS.md`.

---

This map is intentionally high-signal and low on implementation trivia. For deep implementation status and hardware checklists, see `firmware/README.md`.

When in doubt: **AGENTS.md → Contract → CODEMAP → specific source**.

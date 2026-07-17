<!-- PAGE_ID: imec2-01-product-roles -->

[← Start Here](README.md) / Product Roles and Firmware Lines

<details>
<summary>📚 Relevant source files</summary>

The following files were used as context for generating this wiki page:

- [AGENTS.md:31-122](../../AGENTS.md#L31-L122)
- [CODEMAP.md:28-116](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/CODEMAP.md#L28-L116)
- [README.md:35-99](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/README.md#L35-L99)
- [CMakeLists.txt:145-433](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L145-L433)
- [app_config.h:20-84](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_config.h#L20-L84)
- [main.c:431-538](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/main.c#L431-L538)
- [mesh-clicker.conf:1-8](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/conf/mesh-clicker.conf#L1-L8)
- [mesh-anchor.conf:1-4](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/conf/mesh-anchor.conf#L1-L4)
- [role-gateway.conf:1](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/conf/role-gateway.conf#L1)

</details>

# Product Roles and Firmware Lines

> **Related Pages:** [Start Here](README.md) · [One Click, End to End](02_one-click-end-to-end.md) · [Build Presets and Configuration](10_build-presets-and-configuration.md) · [Verified Deployment](11_verified-deployment-and-qualification.md)

The [user story](README.md#the-research-story) starts with one participant pressing a clicker. This page names the three product devices that carry that event toward the research system, then draws a firm boundary around images that exist only for tests, data collection, or bring-up.

---

<!-- BEGIN:AUTOGEN imec2-01-product-roles-production-candidate-line -->
## The Production-Candidate Line

The connected-routing `mesh_` line is the current source of truth for product behavior. Its prefix is transitional: `mesh_clicker`, `mesh_anchor`, and `mesh_gateway` are the production-candidate artifacts, while the similarly named transmitter presets are bench tools ([AGENTS.md:31-53](../../AGENTS.md#L31-L53)).

| Exact preset | Product device | Meaning in the participant journey |
|---|---|---|
| `mesh_clicker` | Clicker | The participant's battery device sleeps normally, wakes for a physical click and range sequence, then sends the event through the connected-routing path; it is not a continuous traffic source ([AGENTS.md:38-40](../../AGENTS.md#L38-L40)). |
| `mesh_anchor` | Anchor | One common production image identifies each physical anchor from hardware, ranges local clicks, relays traffic, and gives its own click reports priority over transit work ([AGENTS.md:41-45](../../AGENTS.md#L41-L45)). |
| `mesh_gateway` | Gateway | The mesh root owns the connected BLE edge to the PC and the highest-priority gateway commands ([AGENTS.md:46-47](../../AGENTS.md#L46-L47)). |

Use the exact preset name whenever you build, discuss, or qualify hardware. A generic label such as “anchor firmware” is ambiguous because production, ML, transmitter, staged-debug, and legacy images can all compile with an anchor role; the repository therefore requires the preset and probe-to-board mapping to be verified before flashing ([AGENTS.md:62-64](../../AGENTS.md#L62-L64)). The exact commands and configuration layers are on [Build Presets and Configuration](10_build-presets-and-configuration.md).

Sources: [AGENTS.md:31-64](../../AGENTS.md#L31-L64), [CODEMAP.md:41-58](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/CODEMAP.md#L41-L58)
<!-- END:AUTOGEN imec2-01-product-roles-production-candidate-line -->

---

<!-- BEGIN:AUTOGEN imec2-01-product-roles-role-ownership -->
## Role Ownership and Boundaries

One click crosses three ownership boundaries. The clicker owns the participant action and starts the bounded radio interaction; anchors turn that interaction into spatial evidence and carry reports; the gateway terminates the UWB mesh and exposes accepted records to the PC.

### Clicker: participant intent

The clicker owns the physical button and the transition between low-power idle and active click work. At startup, its role branch starts the click work queue, initializes the button, submits a retained boot action when present, or returns the device to retained/System OFF idle ([main.c:433-447](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/main.c#L433-L447)). Follow the event itself on [One Click, End to End](02_one-click-end-to-end.md).

### Anchor: ranging and relay

Every production anchor runs the same `mesh_anchor` artifact. The build explicitly clears any compiled device-ID override and enables hardware-derived identity, so logical discovery and ranging order can be assigned separately instead of creating one firmware image per installed anchor ([CMakeLists.txt:198-213](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L198-L213)). At runtime, the anchor starts its role orchestration and, in the mesh line, leaves idle Channel 5 reception with the low-duty scan owner while retaining mesh and report responsibilities ([main.c:453-504](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/main.c#L453-L504)). The identity and logical-assignment lifecycle continues on [Anchor Identity, Discovery, and Assignment](06_anchor-identity-discovery-and-assignment.md).

### Gateway: mesh root and PC edge

The gateway starts gateway-role radio state, initializes the BLE link, starts route refresh, and opens UWB mesh reception; its final startup log describes the resulting role as a reactive mesh root with a BLE packet/log link ([main.c:505-539](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/main.c#L505-L539)). BLE is the gateway-to-PC edge, while UWB remains the system's radio owner; UWB traffic is not tunneled over BLE ([CODEMAP.md:28-39](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/CODEMAP.md#L28-L39)). Host-side views and evidence are covered by [Gateway, Host Tools, and Observability](09_gateway-host-tools-and-observability.md).

Sources: [main.c:433-539](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/main.c#L433-L539), [CMakeLists.txt:198-213](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L198-L213), [CODEMAP.md:28-39](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/CODEMAP.md#L28-L39)
<!-- END:AUTOGEN imec2-01-product-roles-role-ownership -->

---

<!-- BEGIN:AUTOGEN imec2-01-product-roles-nonproduction-lines -->
## Bench, ML, Debug, and Legacy Lines

These images are useful, but none defines the deployed product contract. Keep the category attached to the artifact name so evidence from a synthetic or instrumented build is never reported as production behavior.

| Line | Intended scope | Boundary to preserve |
|---|---|---|
| `mesh_transmitter`, `mesh_transmitter_forcedhop` | Powered synthetic traffic for route, retry, preemption, and relay regression | CMake compiles them with an anchor role but separately enables the continuous transmitter build, so they are traffic generators rather than production anchors ([CMakeLists.txt:182-197](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L182-L197)). Use `mesh_transmitter_forcedhop` when a test must prove a relay hop; the generic transmitter may choose a direct gateway route ([AGENTS.md:74-83](../../AGENTS.md#L74-L83)). |
| `ml_clicker`, `ml_anchor_1` … `ml_anchor_8` | Distance-offset training and validation data collection | ML anchor presets intentionally assign deterministic IDs and slots, which is a collection convenience and not the production anchor identity/assignment model ([CMakeLists.txt:149-169](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L149-L169)). |
| `tag_stage*`, `anchor_stage*`, `gateway_stage*`, high-debug profiles | Bounded hardware bring-up and staged diagnosis | These presets select explicit stages and role fragments for observation; they remain compatibility and bring-up images rather than the source of new mainline behavior ([CMakeLists.txt:101-148](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L101-L148), [AGENTS.md:57-60](../../AGENTS.md#L57-L60)). |
| `gateway_ble_connectivity_test`, standalone `*_test/` apps | BLE, smoke, power-profile, or range-focused bench work | The connectivity preset deliberately omits the DWM3000 and mesh, while standalone test apps isolate a subsystem; neither demonstrates the product's end-to-end path ([README.md:95-99](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/README.md#L95-L99)). |
| Direct `FIRMWARE_ROLE=clicker\|anchor\|gateway` builds | Legacy compatibility and regression | Keep them building, but do not use them to infer the connected-routing runtime contract ([AGENTS.md:85-100](../../AGENTS.md#L85-L100)). |

The deployment boundary follows the same split: direct `west flash` is not supported for the three production-candidate presets, which must pass through the repository's verified wrapper ([AGENTS.md:118-122](../../AGENTS.md#L118-L122)). Bench and legacy exceptions are deliberately narrower and are explained on [Verified Deployment](11_verified-deployment-and-qualification.md).

Sources: [AGENTS.md:49-122](../../AGENTS.md#L49-L122), [CMakeLists.txt:101-213](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L101-L213), [README.md:93-99](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/README.md#L93-L99)
<!-- END:AUTOGEN imec2-01-product-roles-nonproduction-lines -->

---

<!-- BEGIN:AUTOGEN imec2-01-product-roles-code-boundaries -->
## Core Logic and Zephyr Boundaries

The repository separates portable behavior from hardware orchestration. Shared headers live in `firmware/include/`, platform-independent C modules live in `firmware/src/` and have native tests, while `firmware/app/` owns Zephyr, the board overlay, DWM3000 integration, BLE, GPIO, power, and role runtime ([AGENTS.md:3-12](../../AGENTS.md#L3-L12), [README.md:35-47](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/README.md#L35-L47)).

An exact preset composes those layers rather than selecting a separate codebase. CMake maps `clicker`, `anchor`, and `gateway` to numeric `DEVICE_ROLE` values, compiles the focused `app_*` orchestration modules, and links shared native communication, transaction, stack-diagnostic, and watchdog modules into the Zephyr target ([CMakeLists.txt:354-433](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L354-L433)). `app_config.h` gives those role values a common private definition and selects hardware-derived identity when the production anchor option is enabled ([app_config.h:20-25](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_config.h#L20-L25), [app_config.h:62-75](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_config.h#L62-L75)).

Preset-specific configuration then applies bounded hardware choices: the mesh clicker enables its dedicated communication work queue, the mesh anchor reserves its measured main-stack margin, and the gateway role fragment selects the gateway Kconfig role ([mesh-clicker.conf:1-8](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/conf/mesh-clicker.conf#L1-L8), [mesh-anchor.conf:1-4](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/conf/mesh-anchor.conf#L1-L4), [role-gateway.conf:1](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/conf/role-gateway.conf#L1)). Gateway BLE timing follows the same fail-fast pattern: the app configuration sets a 2 ms base notification retry and a 128 ms maximum, and the build rejects any configuration where the maximum cannot cover the base delay ([app_config.h:126-130](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_config.h#L126-L130), [main.c:153-158](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/main.c#L153-L158)).

Continue through the system in the order the participant's data travels:

1. [One Click, End to End](02_one-click-end-to-end.md) follows wake, ranging, report creation, delivery, and host arrival.
2. [Protocol, Packets, and Data Contracts](04_protocol-packets-and-data-contracts.md) defines the identities and envelopes shared across roles.
3. [Connected Routing and Reliable Delivery](05_connected-routing-and-reliable-delivery.md) explains route, custody, priority, retry, and acknowledgement behavior.
4. [Build Presets and Configuration](10_build-presets-and-configuration.md) turns these boundaries into exact build artifacts.

Sources: [AGENTS.md:3-12](../../AGENTS.md#L3-L12), [CMakeLists.txt:354-433](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/CMakeLists.txt#L354-L433), [app_config.h:20-130](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_config.h#L20-L130), [main.c:153-158](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/main.c#L153-L158), [README.md:35-47](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/README.md#L35-L47)
<!-- END:AUTOGEN imec2-01-product-roles-code-boundaries -->

---

[← Previous: Start Here](README.md) · [Next: One Click, End to End →](02_one-click-end-to-end.md)

**Related:** [Build Presets and Configuration](10_build-presets-and-configuration.md) · [Verified Deployment and Qualification](11_verified-deployment-and-qualification.md)

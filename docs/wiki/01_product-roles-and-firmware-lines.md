<!-- PAGE_ID: imec2-01-product-roles -->

[← Start Here](README.md) / Product Roles and Firmware Lines

<details>
<summary>📚 Historical generation context (f6594e4)</summary>

The following files were used as context for generating this wiki page:

- [AGENTS.md:36-74](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/AGENTS.md#L36-L74)
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

The connected-routing `mesh_` line is the current source of truth for product behavior. Its prefix is transitional: the only production-candidate presets are `mesh_clicker`, `mesh_anchor`, and `mesh_gateway`; transmitter, ML, generic-role, staged-debug, and other bring-up images remain outside that product line ([AGENTS.md:49-56](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/AGENTS.md#L49-L56)).

| Exact preset | Product device | Meaning in the participant journey |
|---|---|---|
| `mesh_clicker` | Clicker | The participant's battery device normally sleeps, wakes on the physical button, ranges, and reports through the mesh ([CODEMAP.md:43-49](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/CODEMAP.md#L43-L49)). |
| `mesh_anchor` | Anchor | One common artifact derives its identity from nRF FICR, persists gateway-assigned discovery order, ranges local clicks, relays traffic, and selects local reports ahead of transit without deleting accepted transit custody ([README.md:71-79](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/README.md#L71-L79), [CODEMAP.md:47-49](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/CODEMAP.md#L47-L49)). |
| `mesh_gateway` | Gateway | The mesh root owns the connected BLE GATT edge to the PC and the highest-priority gateway commands ([CODEMAP.md:47-49](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/CODEMAP.md#L47-L49)). |

The build system makes these distinctions executable. It marks only the three product presets deployable, enables hardware-derived identity for the production anchor, and leaves `mesh_anchor_forcedhop` outside the deployable set even though it shares the anchor role ([CMakeLists.txt:161-231](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L161-L231)). Use the exact preset, artifact, probe ID, and probe-to-board mapping whenever you build or qualify hardware, because a generic label such as “anchor firmware” does not identify which behavior is running ([AGENTS.md:58-60](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/AGENTS.md#L58-L60)). The exact commands and configuration layers are on [Build Presets and Configuration](10_build-presets-and-configuration.md).

Sources: [AGENTS.md:49-60](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/AGENTS.md#L49-L60), [CODEMAP.md:41-58](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/CODEMAP.md#L41-L58), [README.md:56-79](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/README.md#L56-L79), [CMakeLists.txt:161-231](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L161-L231)
<!-- END:AUTOGEN imec2-01-product-roles-production-candidate-line -->

---

<!-- BEGIN:AUTOGEN imec2-01-product-roles-role-ownership -->
## Role Ownership and Boundaries

One click crosses three device boundaries. The clicker owns the participant action and the bounded ranging session, anchors turn that interaction into spatial evidence and retain report custody, and the gateway terminates mesh delivery at the PC edge.

### Clicker: participant intent

The clicker owns the physical button and the transition between low-power idle and active click work. Its normal startup branch starts the click work queue, initializes the button, dispatches a retained boot action when present, or returns the device to retained/System OFF idle ([main.c:534-553](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/src/main.c#L534-L553)). Follow the event itself on [One Click, End to End](02_one-click-end-to-end.md).

### Anchor: ranging and relay

Every production anchor runs the same `mesh_anchor` artifact. The preset clears a compiled device-ID override and enables hardware-derived identity, so installed anchors can share one image while the gateway assigns and persists their logical discovery order ([CMakeLists.txt:212-230](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L212-L230), [README.md:75-79](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/README.md#L75-L79)). At runtime, the production mesh anchor starts the anchor role and leaves idle channel-5 reception with the low-duty scan owner, while bench transmitters explicitly bypass that receive ownership ([main.c:555-606](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/src/main.c#L555-L606)). The identity and assignment lifecycle continues on [Anchor Identity, Discovery, and Assignment](06_anchor-identity-discovery-and-assignment.md).

### Gateway: mesh root and PC edge

The gateway starts gateway-role state, initializes the BLE PC link, and opens UWB mesh reception; its final startup state is a reactive mesh root with a BLE packet/log edge ([main.c:607-640](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/src/main.c#L607-L640)). BLE remains limited to clicker courtesy and the gateway-to-PC edge, while channel 5 and channel 9 carry the UWB protocol; UWB traffic is never tunneled over BLE ([CODEMAP.md:28-39](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/CODEMAP.md#L28-L39)). Host-side views and evidence are covered by [Gateway, Host Tools, and Observability](09_gateway-host-tools-and-observability.md).

Sources: [main.c:534-640](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/src/main.c#L534-L640), [CMakeLists.txt:212-230](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L212-L230), [README.md:75-79](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/README.md#L75-L79), [CODEMAP.md:28-49](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/CODEMAP.md#L28-L49)
<!-- END:AUTOGEN imec2-01-product-roles-role-ownership -->

---

<!-- BEGIN:AUTOGEN imec2-01-product-roles-nonproduction-lines -->
## Bench, ML, Debug, and Legacy Lines

These images are useful, but none defines the deployed product contract. Keep the category attached to the artifact name so evidence from a synthetic or instrumented build is never reported as production behavior.

| Line | Intended scope | Boundary to preserve |
|---|---|---|
| `mesh_transmitter`, `mesh_transmitter_forcedhop` | Powered synthetic traffic for route, retry, preemption, and relay regression | CMake compiles an anchor role but separately enables the continuous transmitter worker and a fixed synthetic identity, so these are traffic generators rather than production anchors ([CMakeLists.txt:196-211](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L196-L211)). Use `mesh_transmitter_forcedhop` when a test must prove a relay hop, because the generic transmitter may take a direct gateway path ([AGENTS.md:51-56](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/AGENTS.md#L51-L56)). |
| `mesh_anchor_forcedhop` | Forced-route anchor bench image | It shares the anchor configuration and hardware identity path but is deliberately not marked deployable, so evidence from it cannot stand in for `mesh_anchor` deployment evidence ([CMakeLists.txt:212-231](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L212-L231)). |
| `ml_clicker`, `ml_anchor_1` … `ml_anchor_8` | Distance-offset training and validation collection | ML anchors intentionally receive deterministic IDs and slots, which is a collection convenience rather than the production hardware-identity and assignment model ([CMakeLists.txt:161-182](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L161-L182)). |
| `tag_stage*`, `anchor_stage*`, `gateway_stage*`, high-debug profiles | Bounded bring-up and staged diagnosis | These presets select explicit stages and role fragments for observation; the repository classifies them as test or compatibility builds, not current product behavior ([CMakeLists.txt:108-160](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L108-L160), [AGENTS.md:51-54](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/AGENTS.md#L51-L54)). |
| `gateway_ble_connectivity_test`, standalone `*_test/` apps | BLE, smoke, power-profile, or range-focused bench work | The connectivity preset isolates the gateway BLE path, while standalone test apps isolate individual subsystems; neither proves the production end-to-end path ([README.md:114-118](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/README.md#L114-L118)). |
| Direct `FIRMWARE_ROLE=clicker\|anchor\|gateway` builds | Compatibility and regression | Generic role builds are explicitly outside the three-preset product line, so they must not be used to infer the connected-routing runtime contract ([AGENTS.md:51-54](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/AGENTS.md#L51-L54)). |

The deployment boundary follows the same split. Production mesh deployment is a transactional wrapper-only sequence—stage the exact artifact, capture qualification from that running artifact, then promote with that capture—while direct flashing is forbidden for the three production presets ([AGENTS.md:62-67](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/AGENTS.md#L62-L67)). Bench and compatibility exceptions are deliberately narrower and are explained on [Verified Deployment](11_verified-deployment-and-qualification.md).

Sources: [AGENTS.md:49-67](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/AGENTS.md#L49-L67), [CMakeLists.txt:108-231](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L108-L231), [README.md:112-118](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/README.md#L112-L118)
<!-- END:AUTOGEN imec2-01-product-roles-nonproduction-lines -->

---

<!-- BEGIN:AUTOGEN imec2-01-product-roles-code-boundaries -->
## Core Logic and Zephyr Boundaries

The repository separates portable behavior from hardware orchestration. Hardware-independent protocol and state logic belongs in `firmware/src/` behind headers in `firmware/include/`; Zephyr, GPIO, BLE, settings, SPI, DWM3000 integration, power, and role orchestration belong in `firmware/app/` ([AGENTS.md:36-45](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/AGENTS.md#L36-L45), [CODEMAP.md:79-114](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/CODEMAP.md#L79-L114)).

An exact preset composes those layers rather than selecting a separate codebase. CMake maps the three runtime roles to numeric `DEVICE_ROLE` values, compiles the focused `app_*` orchestration modules, and links shared communication, transaction, stack-diagnostic, watchdog, protocol, routing, report, and survey modules into the Zephyr target ([CMakeLists.txt:393-475](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L393-L475), [CMakeLists.txt:487-524](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L487-L524)). The app-private configuration defines the same role values and resolves `DEVICE_ID` from hardware only when the production-anchor build option is active ([app_config.h:21-27](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/src/app_config.h#L21-L27), [app_config.h:63-77](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/src/app_config.h#L63-L77)).

Preset-specific fragments then apply bounded hardware choices: the mesh clicker enables timing-safe logging and a dedicated communication queue, the mesh anchor reserves a measured main-stack margin, and the gateway role fragment selects the gateway Kconfig role ([mesh-clicker.conf:1-8](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/conf/mesh-clicker.conf#L1-L8), [mesh-anchor.conf:1-4](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/conf/mesh-anchor.conf#L1-L4), [role-gateway.conf:1](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/conf/role-gateway.conf#L1)). Exact builds also embed preset, Git, timestamp, board, and stack-diagnostic identity, while compiler stack-usage records and IPA call graphs supply the root-to-function evidence required by the deployment verifier ([CMakeLists.txt:532-559](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L532-L559)).

Continue through the system in the order the participant's data travels:

1. [One Click, End to End](02_one-click-end-to-end.md) follows wake, ranging, report creation, delivery, and host arrival.
2. [Protocol, Packets, and Data Contracts](04_protocol-packets-and-data-contracts.md) defines the identities and envelopes shared across roles.
3. [Connected Routing and Reliable Delivery](05_connected-routing-and-reliable-delivery.md) explains route, custody, priority, retry, and acknowledgement behavior.
4. [Build Presets and Configuration](10_build-presets-and-configuration.md) turns these boundaries into exact build artifacts.

Sources: [AGENTS.md:36-45](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/AGENTS.md#L36-L45), [CODEMAP.md:79-114](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/CODEMAP.md#L79-L114), [CMakeLists.txt:393-559](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/CMakeLists.txt#L393-L559), [app_config.h:21-77](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/src/app_config.h#L21-L77), [mesh-clicker.conf:1-8](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/conf/mesh-clicker.conf#L1-L8), [mesh-anchor.conf:1-4](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/conf/mesh-anchor.conf#L1-L4), [role-gateway.conf:1](https://github.com/Jubliano-sama/IMEC2/blob/af15a7eb59b1ca8f75464506ffa97f980ffbfef7/firmware/app/conf/role-gateway.conf#L1)
<!-- END:AUTOGEN imec2-01-product-roles-code-boundaries -->

---

[← Previous: Start Here](README.md) · [Next: One Click, End to End →](02_one-click-end-to-end.md)

**Related:** [Build Presets and Configuration](10_build-presets-and-configuration.md) · [Verified Deployment and Qualification](11_verified-deployment-and-qualification.md)

<!-- PAGE_ID: imec2-09-host-tools-and-observability -->

[← Start Here](README.md) / Gateway, Host Tools, and Observability

<details>
<summary>📚 Relevant source files</summary>

The following files were used as context for generating this wiki page:

- [Gateway BLE Streaming.md:3-110](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20BLE%20Streaming.md#L3-L110)
- [Gateway Command Observability.md:5-200](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Command%20Observability.md#L5-L200)
- [app_gateway_ble.c:653-740](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L653-L740)
- [app_gateway_ble.c:2408-2473](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2408-L2473)
- [app_gateway_ble_stream.c:72-143](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble_stream.c#L72-L143)
- [app_gateway_ble_stream.c:351-583](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble_stream.c#L351-L583)
- [app_mesh_report_delivery.inc:2613-2698](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_mesh_report_delivery.inc#L2613-L2698)
- [app_gateway_command_observability.c:287-445](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_command_observability.c#L287-L445)
- [README.md:20-172](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/README.md#L20-L172)
- [app.py:399-435](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/app.py#L399-L435)
- [app.py:795-930](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/app.py#L795-L930)
- [protocol.py:867-961](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/protocol.py#L867-L961)
- [ble_transport.py:145-239](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/ble_transport.py#L145-L239)
- [mesh_ble_route_monitor.py:329-472](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/mesh_ble_route_monitor.py#L329-L472)
- [mesh_ble_route_monitor.py:623-794](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/mesh_ble_route_monitor.py#L623-L794)
- [capture_stack_evidence.py:61-128](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/capture_stack_evidence.py#L61-L128)
- [verify_stack_evidence.py:1072-1205](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/verify_stack_evidence.py#L1072-L1205)

</details>

# Gateway, Host Tools, and Observability

After a participant click has crossed the [connected mesh](05_connected-routing-and-reliable-delivery.md), the gateway becomes the boundary between protocol-owned research data and host-visible evidence. This page follows that boundary into the desktop GUI, headless capture tools, command timelines, and RTT qualification without treating a BLE write, a log line, or a quiet screen as proof that the underlying operation succeeded.

> **Related pages:** [Data Custody, Persistence, and Recovery](08_data-custody-persistence-and-recovery.md) · [Build Presets, Configuration, and Repository Boundaries](10_build-presets-and-configuration.md) · [Verified Mesh Deployment and Hardware Qualification](11_verified-deployment-and-qualification.md) · [Hardware Bring-Up and Troubleshooting](13_hardware-bring-up-and-troubleshooting.md)

---

<!-- BEGIN:AUTOGEN imec2-09-host-tools-and-observability-gateway-edge -->
## The Gateway-to-PC Edge

The production-candidate [gateway](01_product-roles-and-firmware-lines.md) exposes one GATT service with three explicit boundaries: a notify characteristic for gateway-to-host records, a write/write-without-response characteristic for host-to-gateway packets, and a read-only characteristic containing the firmware `DEVICE_ID`. Enabling notifications also replays retained command observability and resumes stream draining; inbound ATT chunks are accumulated until the serial-frame delimiter before workqueue processing ([app_gateway_ble.c:2416-2481](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2416-L2481), [app_gateway_ble.c:3126-3171](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L3126-L3171)). The desktop client checks all required UUIDs, reads that identity, subscribes before declaring itself connected, and splits a complete COBS-framed command into ordered write-without-response chunks sized for the characteristic ([ble_transport.py:145-185](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/ble_transport.py#L145-L185), [ble_transport.py:213-239](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/ble_transport.py#L213-L239)).

Outbound BLE is selective. Clicks, command results, surveys, diagnostics, and heartbeats are eligible; routing requests, hop acknowledgements, gateway ACKs/EACKs, event negotiation, and other mesh-control traffic stay off the normal host stream ([Gateway BLE Streaming.md:17-32](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20BLE%20Streaming.md#L17-L32)). Each record has a 40-byte header with packet identity, source and destination, gateway queue age, payload length, and a payload CRC, so the host can reassemble one record across ATT notification chunks and reject corruption ([Gateway BLE Streaming.md:69-94](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20BLE%20Streaming.md#L69-L94), [protocol.py:867-911](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/protocol.py#L867-L911)).

The stream has bounded RAM and explicit pressure behavior. Clicks have the highest ordinary priority, results and surveys follow, then diagnostics and status; a higher-priority arrival may evict a lower-priority queued record, while non-truncatable records that still do not fit fail admission ([Gateway BLE Streaming.md:34-67](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20BLE%20Streaming.md#L34-L67), [app_gateway_ble_stream.c:351-423](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble_stream.c#L351-L423)). For messages whose gateway ACK means semantic delivery, the firmware reserves host-stream capacity before changing protocol state. A full BLE queue leaves the radio packet unaccepted and retryable; only a semantically accepted new record with committed BLE custody can proceed to gateway-delivery commit and ACK, while rejected or stale input is not acknowledged ([app_mesh_report_delivery.inc:2613-2698](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_mesh_report_delivery.inc#L2613-L2698)).

Controller or notify-credit refusal now leaves the selected chunk and stream record in custody and schedules a capped exponential retry. It does not disconnect the PC after a fixed failure count. Pressure samples continue to expose queue, custody, credit, retry, and drain depth, while warnings are sampled at power-of-two failure counts so a long outage remains visible without a tight log loop ([app_gateway_ble.c:2335-2348](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2335-L2348), [app_gateway_ble.c:2774-2817](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2774-L2817)). A successful callback resets the failure counter and advances or retires the record only after the submitted chunk, and eventually the complete frame, finishes ([app_gateway_ble.c:2597-2664](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2597-L2664)).

That distinction defines the boundary: BLE transports host commands and selected observations, while UWB still owns mesh RF delivery. Ordinary stream loss can hide diagnostic or status visibility without changing route or custody state, but semantically gated research records do not receive false gateway success merely because their RF frame arrived ([Gateway BLE Streaming.md:6-15](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20BLE%20Streaming.md#L6-L15)).

Sources: [app_gateway_ble.c:2335-2348](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2335-L2348), [app_gateway_ble.c:2416-2481](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2416-L2481), [app_gateway_ble.c:2597-2664](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2597-L2664), [app_gateway_ble.c:2774-2817](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2774-L2817), [Gateway BLE Streaming.md:6-94](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20BLE%20Streaming.md#L6-L94), [app_mesh_report_delivery.inc:2613-2698](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_mesh_report_delivery.inc#L2613-L2698)
<!-- END:AUTOGEN imec2-09-host-tools-and-observability-gateway-edge -->

---

<!-- BEGIN:AUTOGEN imec2-09-host-tools-and-observability-gui-and-geometry -->
## Gateway GUI and Geometry Views

Run the desktop console from the repository root:

```sh
.venv/bin/python -m tools.gateway_gui
```

The GUI scans for IMEC devices, verifies the service, reads the gateway identity, and subscribes to notifications before enabling command controls. It clears identity on disconnect and refuses a contradictory gateway-local identity instead of deriving device identity from the BLE address ([README.md:20-38](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/README.md#L20-L38), [app.py:895-930](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/app.py#L895-L930)). The main workspace pairs a live packet table and transport activity log with inspectors for overview fields, aligned range samples, CIR windows, diagnostics, all TLVs, and raw bytes, while the diagnostics mixin adds the mesh-command and geometry workflows ([app.py:399-435](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/app.py#L399-L435)).

The command panel deliberately exposes only the three validated gateway workflows:

| Workflow | What to inspect |
| --- | --- |
| **Anchor Survey Discovery** | Discovery reports, the correlated command timeline, pair results, then the generated anchor geometry. |
| **Here I Am** | The terminal route-refresh result and its typed lifecycle rather than the completed BLE write. |
| **Assign discovery slots** | Per-anchor claims and slots, assignment-table publication, acknowledgements, and the terminal assigned-anchor count. |

Those controls use command-specific destinations and TLVs; there is no arbitrary command composer because firmware validation and single-command tracking do not promise arbitrary combinations ([README.md:90-113](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/README.md#L90-L113)). A completed ATT write remains transport completion only: the UI reports that the command outcome is still pending rather than upgrading the write to protocol success ([app.py:853-862](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/app.py#L853-L862)). A temporarily quiet live table is likewise inconclusive: the gateway retains a refused notification and retries it with capped backoff instead of forcing a disconnect after a fixed failure count ([app_gateway_ble.c:2774-2817](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2774-L2817)).

The geometry path continues the [anchor self-setup story](07_anchor-self-setup-survey-and-geometry.md). `Anchor Geometry` consumes successful survey-pair distances; failed pairs become visibility evidence only after terminal telemetry proves the scheduled opportunities were observed, and the selected solver does not silently fall back after failure. `Click Location` then groups ranges by protocol session, event sequence, and clicker identity against the current geometry generation, leaving stale, duplicate, unknown-anchor, invalid, or collinear inputs unsolved ([README.md:152-166](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/README.md#L152-L166)).

For headless checks, use `.venv/bin/python -m tools.gateway_gui.ble_smoke [BLE_ADDRESS]` to require identity-read and subscription success, or use `tools/mesh_ble_route_monitor.py` for a bounded, scriptable stream capture. The smoke check proves the GATT edge is usable; it does not prove that a later UWB command or mesh delivery succeeds ([README.md:55-88](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/README.md#L55-L88)).

Sources: [README.md:20-172](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/README.md#L20-L172), [app.py:399-435](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/app.py#L399-L435), [app.py:853-930](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/gateway_gui/app.py#L853-L930), [app_gateway_ble.c:2774-2817](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2774-L2817)
<!-- END:AUTOGEN imec2-09-host-tools-and-observability-gui-and-geometry -->

---

<!-- BEGIN:AUTOGEN imec2-09-host-tools-and-observability-command-telemetry -->
## Command and Lifecycle Telemetry

Typed `MSG_GATEWAY_COMMAND_EVENT` records turn enumeration, survey, and route refresh into timelines without replacing their existing command-result or survey-result packets. Every fixed 78-byte event carries the command kind and stage, status and reason, host correlation ID, original host session and sequence, gateway operation sequence, monotonically increasing event sequence, involved anchors or pair, previous hop, counts, hop count, and slot ([Gateway Command Observability.md:70-108](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Command%20Observability.md#L70-L108)).

Correlate a command by `(command_kind, correlation_id, host_session_id, host_seq)`, use `gateway_sequence` to distinguish the assignment epoch or survey ID within that request, and sort by `event_seq`. Replayed snapshots keep their original event sequence and replace the same timeline position, so reconnect does not invent another operation ([Gateway Command Observability.md:143-159](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Command%20Observability.md#L143-L159)).

The evidence levels matter:

| Observation | What it proves | What it does not prove |
| --- | --- | --- |
| BLE write completion | The host submitted every ATT chunk. | Gateway command acceptance, RF dispatch, or terminal success. |
| Valid stream header and payload CRC | The selected host record was reassembled intact. | That every RF or low-priority observability event appeared on BLE. |
| Accepted, queued, retry, or pair-stage event | The gateway reached that correlated lifecycle stage. | A successful terminal outcome. |
| Terminal event or matching terminal `COMMAND_RESULT` | The gateway declared the correlated operation terminal with explicit status and reason. | Remote attestation of the target or proof outside the stated protocol boundary. |
| Gateway ACK for a semantically gated record | The gateway protocol owner accepted the record after reserving or committing required host custody. | That an operator has already inspected or exported it. |

Terminal command events receive click-level priority and are retained until their complete stream record is acknowledged by the BLE callback. Disconnect, CCC-off, exhausted notification credit, transient submit failure, and queue pressure leave records pending for replay; sequence gaps count as irreversible loss only when `lost_event_count` also advances ([Gateway Command Observability.md:31-61](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Command%20Observability.md#L31-L61)). The implementation retains a bounded terminal backlog, preserves snapshots after transient enqueue refusal, and marks replayed snapshots explicitly ([app_gateway_command_observability.c:287-367](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_command_observability.c#L287-L367), [app_gateway_command_observability.c:369-427](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_command_observability.c#L369-L427)). Transient notification refusal now keeps that custody and schedules capped exponential retries; it no longer turns the eighth failure into a forced disconnect. The pressure sample records the current retry depth, and warning logs are emitted only at power-of-two failure counts ([app_gateway_ble.c:2774-2817](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2774-L2817)).

Backpressure may coalesce non-terminal progress to the latest safe snapshot, but it does not change an in-flight radio deadline or survey decision; pair orchestration pauses only at a safe between-pair boundary when telemetry lacks custody ([Gateway Command Observability.md:183-200](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Command%20Observability.md#L183-L200)). The GUI should therefore decide success from the authoritative terminal record, never from silence, a transport write, or the absence of an error ([Gateway Command Observability.md:44-51](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Command%20Observability.md#L44-L51)).

Sources: [Gateway Command Observability.md:18-68](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Command%20Observability.md#L18-L68), [Gateway Command Observability.md:70-200](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Command%20Observability.md#L70-L200), [app_gateway_command_observability.c:287-427](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_command_observability.c#L287-L427), [app_gateway_ble.c:2774-2817](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2774-L2817)
<!-- END:AUTOGEN imec2-09-host-tools-and-observability-command-telemetry -->

---

<!-- BEGIN:AUTOGEN imec2-09-host-tools-and-observability-rtt-and-capture-tools -->
## RTT, Route Monitoring, and Durable Captures

Use each host evidence path for the question it can answer:

| Tool or artifact | Best use | Evidence limit |
| --- | --- | --- |
| Gateway GUI | Interactive packet, command, CIR, survey, and geometry inspection. | Selective host-delivery view, not a complete RF trace. |
| BLE route monitor | Headless packet sequencing, hop/retry inspection, gaps, and repeatable captures. | Observes only records the gateway stream emitted and the host received. |
| Append-only JSONL | Machine-readable host arrival records for later correlation. | A missing record can mean filtering, pressure, disconnect, decode failure, or upstream absence. |
| Ordinary RTT transcript | Target-local boot, route, ACK, stack, and failure markers. | Marker presence is meaningful only under a validator that defines required milestones and rejects fatal or repeated-boot traces. |
| Schema-3 typed RTT capture | Exact-artifact qualification for [verified deployment](11_verified-deployment-and-qualification.md). | Strong local provenance, not cryptographic remote probe attestation. |

The BLE route monitor derives a sequence class from packet IDs, mesh event counters, event sequence, requested sequence, or the packet sequence. It tracks the high-water mark independently for each `(source_id, sequence_class)`, reports missing ranges, resets, and out-of-order arrivals, and deliberately does not move the high-water mark backward for a late record ([mesh_ble_route_monitor.py:329-397](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/mesh_ble_route_monitor.py#L329-L397)). That makes a gap actionable without confusing interleaved node-local streams. A temporary pause is not itself a gap: notify refusal retains the current record behind capped exponential retry, while pressure diagnostics expose zero credit, retry depth, and queue depth ([app_gateway_ble.c:2774-2817](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2774-L2817)).

For a durable headless capture, pass `--jsonl-file PATH`; the monitor opens it with append semantics, writes each JSON object completely, and optionally calls `fsync` for every record with `--jsonl-fsync`. Capture write failures increment `capture_errors`, appear in the final summary, and make the process exit nonzero ([mesh_ble_route_monitor.py:436-472](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/mesh_ble_route_monitor.py#L436-L472), [mesh_ble_route_monitor.py:623-754](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/mesh_ble_route_monitor.py#L623-L754), [mesh_ble_route_monitor.py:757-794](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/mesh_ble_route_monitor.py#L757-L794)). `--jsonl-fsync` improves survival across host power loss at the cost of one synchronous flush per packet.

Formal RTT qualification uses the repository capture tool, which runs `pyocd rtt -t nrf52833 -M pre-reset -u <probe> --up-channel-id 0` under a TTY-providing `script` wrapper and a bounded timeout. It rejects an empty transcript, then binds the exact ELF and HEX hashes, preset, target build identity, probe ID, transcript hash, fixed command, and UTC window into the manifest before verifying it ([capture_stack_evidence.py:61-128](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/capture_stack_evidence.py#L61-L128)). This is why an ad hoc redirected RTT log and a qualification capture are different artifacts.

The verifier requires correlated `RUN_BEGIN`, `SAMPLE_BEGIN`, stack rows, sample completion, and `RUN_END` records. It matches source, destination, session, sequence, message type, workload owner, and run identity while recording queue, custody, credit, retry, and drain state; unmatched, overlapping, unterminated, marker-only, or policy-incomplete evidence fails closed ([verify_stack_evidence.py:1072-1168](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/verify_stack_evidence.py#L1072-L1168), [verify_stack_evidence.py:1171-1205](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/verify_stack_evidence.py#L1171-L1205)).

When correlating host and target evidence, prefer protocol identity over wall-clock proximity: use source/destination, session, packet sequence, event sequence, and the command correlation tuple. Host elapsed and Unix arrival times explain when the PC observed a record, while target uptime and typed run identity explain where firmware was; neither clock alone proves they describe the same operation ([mesh_ble_route_monitor.py:436-460](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/mesh_ble_route_monitor.py#L436-L460), [Gateway Command Observability.md:143-159](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Command%20Observability.md#L143-L159), [verify_stack_evidence.py:1094-1127](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/verify_stack_evidence.py#L1094-L1127)).

Sources: [mesh_ble_route_monitor.py:329-472](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/mesh_ble_route_monitor.py#L329-L472), [mesh_ble_route_monitor.py:623-794](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/tools/mesh_ble_route_monitor.py#L623-L794), [capture_stack_evidence.py:61-128](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/capture_stack_evidence.py#L61-L128), [verify_stack_evidence.py:1072-1205](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/verify_stack_evidence.py#L1072-L1205), [app_gateway_ble.c:2774-2817](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_ble.c#L2774-L2817)
<!-- END:AUTOGEN imec2-09-host-tools-and-observability-rtt-and-capture-tools -->

---

## Continue the story

[← Previous: Data Custody, Persistence, and Recovery](08_data-custody-persistence-and-recovery.md) · [Next: Build Presets, Configuration, and Repository Boundaries →](10_build-presets-and-configuration.md)

Related: [Verified Mesh Deployment and Hardware Qualification](11_verified-deployment-and-qualification.md) · [Hardware Bring-Up and Troubleshooting](13_hardware-bring-up-and-troubleshooting.md) · [One Click, End to End](02_one-click-end-to-end.md) · [Anchor Self-Setup: Survey and Geometry](07_anchor-self-setup-survey-and-geometry.md)

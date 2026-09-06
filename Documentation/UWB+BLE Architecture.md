# UWB and BLE implementation map

Current production-candidate map, 2026-09-06. The [Mesh contract](<Mesh Connected Routing Contract.md>) owns radio/protocol behavior and [C5 delivery](<Channel 5 Delivery Protocol.md>) owns report custody. This overview replaces the older two-channel scheduling description.

| Component | Responsibility | Implementation entry points |
|---|---|---|
| Battery clicker | Button gesture, wake arbitration, discovery/schedule, DS-TWR initiation, local feedback and low-power return | `firmware/app/src/app_clicker.c`, native UWB session modules |
| Anchor | Stable hardware identity, assigned discovery slot, low-duty scan, click ranging, report/relay custody, exclusive survey execution | `app_anchor.c` and included anchor modules, `app_survey.c` |
| Gateway | UWB root, command lifecycle, enumeration/survey coordination and bounded BLE admission | gateway application modules, `app_survey.c`, gateway BLE stream modules |
| Desktop GUI | BLE connection, command orchestration, roster/pair queues, deduplication, CIR decoding, geometry/NLOS solving and visualization | `tools/gateway_gui/` |
| Native core | Wire validation, route selection, bounded custody/response lanes, timing arithmetic and testable state transitions | `firmware/src/`, `firmware/include/` |
| Hardware port | Single DWM3000 ownership, SPI/reset/wake, bounded status polling and proven parking | DWM3000 driver/port and radio coordinator |

## Radio and ownership

One physical DW3000 handles Channel-5 wake/contact, discovery, control, ranging and report delivery with operation-specific PHY settings. Production delivery does not require the old Channel-9 connection cadence. Legacy C9 modules/tests remain for compatibility and regression work; inspect call paths before attributing them to a production operation.

The anchor scans continuously within its acquisition window, then sleeps according to the shared scan contract. Wake traffic must provide a complete decodable frame at every allowed scan phase. No hardware SNIFF or pulsed acquisition is used. Active survey ownership excludes all unrelated commands and clicks across receive, queued execution and result drain; the gateway/GUI enforce the same operation boundary.

Ordinary click acquisition and range execution hold one radio owner through their scheduled work. Report custody can wait independently in RAM. Bounded workqueues and radio admission serialize the hardware; the [state-machine proposal](IMEC2_Firmware_State_Machine_Design.md) is not an implemented replacement dispatcher.

## Enumeration and survey

Enumeration establishes current parents and an authoritative stable-slot TABLE. Normal assignment is durable; survey enumeration requests temporary RAM-only assignment. TABLE completion is a bounded propagation boundary rather than an ACK_CONFIRM quorum.

A survey consumes the exact enumeration receive handoff, collects slot-ordered presence records and accepts committed PLAN batches. Compact neighbor/range response bundles use their own depth/slot lane and digest-bound ACKs. The GUI owns the full pair backlog; firmware owns a bounded current batch. This is separate from ordinary report-bank delivery and from the historical pair PREPARE/START machinery.

Geometry and CIR processing belong on the host. The current GUI's NLOS-aware solver and layout editor produce relative 2D geometry; external product requirements for 3D coordinates and workplace registration remain distinct from that implementation.

## BLE and storage

Connected BLE GATT carries binary commands, command lifecycle telemetry and packet output between gateway and GUI. A completed BLE write proves transport completion only. Exact command terminals, survey results and host-visible click records provide their own evidence. BLE admission is bounded by both record capacity and payload storage, so backpressure must preserve the producing owner.

The GUI is the gateway's external RAM. Pair orchestration, graph solving, deduplication and diagnostic reassembly should stay there when they do not need firmware ownership. Firmware RAM holds hot retries, receipts and queues; infrequent configuration such as normal discovery assignment can use NVS. New flash writers require a bounded write-rate argument rather than treating NVS as a larger RAM bank.

## Power and verification

The CPU, clocks, SPI, GPIO/LED states and regulators contribute alongside DW3000 RX/TX/idle/sleep. A scan-duty ratio is only one component of a power model. The status-polled driver restores 32 MHz SPI after its 2 MHz wake handshake, and bounded parking must prove the intended state before releasing radio ownership.

Use [firmware build instructions](../firmware/README.md), [GUI workflows](../tools/gateway_gui/README.md) and the mandatory [agent test gates](../AGENTS.md). Treat current-model timing, measured RF gaps, flash/readback, enumeration, survey and click delivery as separate evidence. Current bench work has no power instrument, so current draw and battery life remain unmeasured.

#internship #imec #architecture #documentation #UWB #BLE

Version: 0.6.6.1

Previous version: [[UWB+BLE Architecture 0.6.6]] Design rationale: [[UWB+BLE Design Story 0.1]] Component selection: [[Selecting a UWB and BLE Chip]], [[05-03-2026 Internship]]

## Changelog

### 2026-07-18 - 0.6.6.1

- Moved Here-I-Am sequencing out of gateway firmware: there is no startup or
  periodic refresh, and the GUI now owns one correlated
  `Here-I-Am -> successful terminal -> frozen target command` operation.
- Added the versioned RAM-only host operation policy for assignment timing,
  randomized survey discovery, pair reruns, and concurrent-lane limits.
- Replaced hop-prioritized assignment and all-or-nothing table completion with
  equal randomized responses and explicit ACKed-subset completion.
- Replaced the reserved-horizon survey discovery with one simple continuous
  one-to-four-round announce/listen window; one-way and partial reachability are
  useful inputs.
- Added disjoint-neighborhood concurrent pair batches, a shared future GO, and
  the separate 1.15-second local responder window. Hop-aware mesh control
  timeouts remain independent failure ceilings.
- Made anchor data scheduling local-first without erasing transit custody, and
  retained upstream and gateway-downlink routes until repeated terminal
  delivery evidence proves them unusable.
- Consolidated survey planning, lane lifecycle, GO custody, and cleanup behind
  focused state owners. See [[Mesh Connected Routing Contract]] for normative
  invariants and [[Mesh Connected Routing Walkthrough]] for the current
  plain-English timing and validation flow.

This patch section supersedes conflicting timing, retry, serial-survey,
destructive-preemption, and automatic-refresh statements inherited below from
0.6.6. The remaining inherited material is retained for unchanged architecture
and historical rationale; the connected-routing contract and walkthrough are
the implementation-accurate source for the changed mesh behavior.

### 2026-07-10 - 0.6.6

- Removed the gateway BLE debug-log lane and its fixed buffers. Connected BLE
  now carries only PC commands, packet reports, and gateway identity; firmware
  diagnostics use RTT.

### 2026-06-19 - 0.6.5

- Promoted the Stage 1 validated anchor wake scan to the normal target: 380 ms scan interval with a 5 ms RX slice.
- Replaced the old 1% scan guard language with a calibrated 13,000 us/s RX-duty budget and a separate conservative awake-time estimate.
- Updated the anchor battery estimate for the 7.67 ms conservative awake slice and removed the 300 ms continuous-RX debug estimate as the default Stage 1 reference.
- Increased the scheduled DS-TWR exchange stride from 30 ms to 33 ms after high-sample ML collection showed the extra spacing eliminates the observed responder timeouts.

### 2026-06-19 - 0.6.4

- Updated the anchor power estimate to the current 380 ms scan interval and 3 ms normal RX window.
- Added a separate Stage 1 bring-up estimate for the reliable 300 ms anchor RX debug window, marking it as non-production power behavior.
- Removed the old standalone periodic anchor mesh-RX idle row from the baseline estimate; channel-9 mesh receive is negotiated work after channel-5 contact, not a separate idle channel-5 listener.
- Updated status-polling and battery-life estimates for the new scan windows.

### 2026-06-18 - 0.6.3

- Replaced the gateway app-side USB serial PC link with a connected BLE GATT gateway link.
- Split gateway Bluetooth traffic into binary packet notifications/writes and a separate debug-log notification stream.
- Stopped gateway BLE primary-channel advertising on channels 37-39 after the PC connection is accepted, restarting it only after disconnect.
- Clarified that BLE still does not carry UWB wake, ranging, or mesh relay traffic; the connected BLE link is only the gateway-to-PC edge.

### 2026-06-18 - 0.6.2

- Returned the normal anchor idle wake scan to DWM3000 DEEPSLEEP and tightened the interval to 396 ms so a full scan period, including wake/settle/RX overhead, stays inside the 400 ms wake train.
- Updated the anchor idle duty model to 0.996% DWM3000 awake time: about 9183 us/s for wake scans plus 778 us/s for periodic UWB mesh receive.

### 2026-06-17 - 0.6.1

- Moved the implemented main firmware radio assumptions to long-range mode: 850 kbps, 4096-symbol preamble, PAC32, 4073-symbol SFD timeout, no STS, and maximum configured DWM3000 TX power.
- Changed normal-click range scheduling to select at most four anchors, use one continuous 400 ms responder burst, and reserve a 30 ms DS-TWR exchange stride.
- Split the DS-TWR reply-delay discussion into provisional short-range and long-range presets. The current long-range main firmware selects `UWB_RANGE_REPLY_DELAY_LONG_RANGE_UUS = 8000`; the lower `UWB_RANGE_REPLY_DELAY_SHORT_RANGE_UUS = 2750` candidate remains available for recalibration work.

### 2026-06-15 - 0.6

Version 0.6 closes the 0.5.x architecture line and records the current implemented system as a UWB-owned click, range, and mesh design with BLE reduced to clicker-to-clicker courtesy only. During 0.5.x the document was reorganized into a full system reference: stable system facts, shared hardware, click path, mesh and gateway behavior, power, and comparison against the earlier BLE-gated plan. The click path moved from a four-anchor expectation to a three-anchor acceptance threshold because the server-side solver accepts three distances, while schedules may still select up to six anchors and fill one continuous 200 ms responder burst with round-robin samples. The radio model was tightened around the actual DWM3000 behavior: 850 kbps, a 1024-symbol preamble, PAC8, a 1025-symbol SFD timeout, no STS, a 75 mA active-current estimate, and bounded `SYS_STATUS` polling instead of a direct IRQ line.

The mesh architecture also changed materially. Routes are now treated as valid until replaced, explicitly cleared, or proven stale by delivery failure; channel-9 timing freshness is handled separately. Shared mesh packets carry a saturating packet-age field so receivers can compensate for relay delay without gateway-synchronized clocks. Reliable delivery now includes hop-progress ACKs, bounded gateway-ACK retransmissions, capped route rediscovery with exponential backoff, and a gateway-originated force-rediscovery command. Survey setup moved away from route-state reachability and fixed provisioned slots. Gateways now start measured UWB survey discovery, send the slot count, and anchors derive reply/probe slots with `hash(anchor_id) % discovery_slot_count`; the default survey setup uses six slots unless overridden. Together, these changes make the architecture describe the firmware path that exists now: deterministic enough to avoid floods, flexible enough for different anchor counts, and explicit about the radio and timing assumptions that still need bench validation.

### 2026-06-15 - 0.5.56

- Removed the build-time/provisioned anchor discovery slot from the runtime model.
- Changed click and survey discovery slots to `hash(anchor_id) % discovery_slot_count`, where the sender/gateway provides the slot count in the discovery request.
- Changed gateway survey discovery to use six slots by default, with optional `DISCOVERY_SLOT_COUNT` override, so setup does not wait through unfilled 50-slot epochs unless requested.

### 2026-06-15 - 0.5.55

- Added mesh hop-progress ACK behavior: each forwarded ACK-required packet can extend the original sender's gateway-ACK wait while the packet is still moving downstream.
- Capped route rediscovery to five attempts per target with exponential 250/500/1000/2000/4000 ms backoff plus jitter.
- Clarified retry accounting: a selected candidate gets two retransmissions after the original send, then the third missing gateway ACK invalidates that candidate.
- Added the gateway-originated forced rediscovery command behavior, which invalidates known routes after the anchor attempts its command result.

### 2026-06-15 - 0.5.54

- Replaced gateway time-sync architecture with the shared packet-age field carried by every mesh packet.
- Updated anchor self-distance survey setup to use measured UWB survey discovery probes instead of route-state reachability.
- Added deterministic UWB survey discovery slots and deterministic per-anchor mesh report slots to prevent a discovery-report flood.
- Clarified that survey discovery is priority setup work and that the current DW3000 survey discovery PHY uses the lowest exposed data rate in this tree, 850 kbps, with the 1024-symbol long-preamble mode.

### 2026-06-12 - 0.5.53

- Replaced the BLE courtesy detection probability table with the reproducible single-radio simulation in `Documentation/scripts/ble_courtesy_probability.py`.
- Required full 1 ms advertising-event containment inside the receiver's effective scan RX window after subtracting that receiver's own local advertising TX blackouts.
- Documented that scan timing is fixed from scan start; the corrected simulation adds no random scan phase.

### 2026-06-12 - 0.5.52

- Clarified that BLE courtesy advertising and passive scanning are simultaneous host/controller roles, not simultaneous BLE RF TX/RX.
- Marked the courtesy detection Monte Carlo table as an optimistic first-order estimate because local advertising TX should be treated as scan-RX blackout in a strict single-radio model.
- Added that controller traces or a stricter scheduler model should calibrate the courtesy interception probability before treating it as proven.

### 2026-06-01 - 0.5.51

- Updated the DWM3000 hardware assumption: the IRQ pin is not directly available to the MCU.
- Replaced the board-level `irq-gpios` requirement with bounded `SYS_STATUS` polling over SPI for TX/RX completion.
- Marked `P0.02` unused for DWM3000 IRQ and shifted DS-TWR validation from IRQ-edge timing to status-poll timing.
- Added the MCU/SPI power implication of status polling: DWM3000 awake windows stay unchanged, but the MCU remains active during bounded UWB waits.

### 2026-05-29 - 0.5.50

- Changed the normal-click anchor threshold from four anchors to three because the server-side solver accepts three anchor distances.
- Clarified that normal-click range release applies only when one or two eligible discovery replies arrive; zero replies do not send release.
- Clarified that normal-click schedules may start from three discovery replies and still select up to six anchors.
- Clarified that burst acceptance requires at least three unique `RANGE_OK` anchors from the same click event and burst identity.
- Made `BURST_ID` mandatory for normal-click report grouping so retry bursts are not combined.
- Aligned wake-claim arbitration wording with the firmware rule: freshness first, then higher attempt, lower priority, lower clicker, lower click event for different events.
- Normalized `UWB_RANGE_REPLY_DELAY_UUS = 900` wording as the DWM/DW3000 delayed-TX unit.

### 2026-05-18 - 0.5.49

- Updated BLE courtesy deferral to use the higher-priority peer's advertised finish wait and allow up to three BLE deferrals.

### 2026-05-18 - 0.5.48

- Clarified that channel-9 event timing adjusts from received packets and closes only when supervision expires.
- Clarified decoded UWB gate waits, BLE-window edge sampling, gate restart behavior, and retry-only randomized contention.

### 2026-05-18 - 0.5.47

- Updated runtime-flow references to firmware state machines 0.1.39 after the full flowchart accuracy audit.
- Updated protocol references to protocols 0.2.44.

### 2026-05-18 - 0.5.46

- Updated runtime-flow references to firmware state machines 0.1.38 after the scheduled anchor ranging flowchart correction.
- Updated protocol references to protocols 0.2.43.

### 2026-05-18 - 0.5.45

- Switched the 1024-symbol UWB configuration to the bundled Qorvo preset shape: PAC8 with a 1025-symbol SFD timeout.
- Updated cross-references to protocols 0.2.42 and firmware state machines 0.1.37.

### 2026-05-18 - 0.5.44

- Corrected the implemented UWB PHY to a 1024-symbol preamble across wake, ranging, route contact, and channel-9 mesh payload. Version 0.5.45 applies the Qorvo preset PAC and SFD timeout.
- Documented the lower DWM3000 first-path sensitivity threshold (`IP_CONFIG_LO.IP_NTM=12`) and its expected outlier tradeoff.
- Updated cross-references to protocols 0.2.41 and firmware state machines 0.1.36.

### 2026-05-18 - 0.5.43

- Refactored the architecture document into a top-to-bottom system reference: stable design facts, hardware, click path, mesh/reporting, power, comparison, and firmware boundaries.
- Kept the hardware BOM in the architecture document and moved explanatory text around it so it reads as a hardware-plus-firmware system document, not a software-only design note.
- Added the then-implemented DWM3000 IRQ pin mapping, documented the range-release courtesy frame for insufficient discovery replies, and clarified that normal-click burst ranging starts only after at least four anchors replied. Version 0.5.51 supersedes the IRQ pin mapping with status polling.
- Updated route freshness wording: routes remain usable until replacement or delivery failure proves them stale; channel-9 event timing can still expire independently.
- Updated power budgets to use 75 mA for UWB RX/TX and rewrote the previous BLE-gated comparison around actual design differences instead of an unsupported fixed BLE duration.

### 2026-05-18 - 0.5.42

- Reorganized the document so hardware, click behavior, mesh behavior, operations, and power budgets each live in one nested section.
- Moved related click-flow details together: BLE courtesy, channel-5 wake/discovery, shared burst ranging, no-STS DS-TWR timing, diagnostics, and self-test behavior.
- Rewrote dense mesh-routing and reliability sections in plainer terms while keeping the implemented channel-5/channel-9 behavior and timing values.
- Kept the 200 ms shared responder burst, 6-anchor schedule limit, 75 ms BLE courtesy window, and updated power numbers from version 0.5.41.

Older changes are in [[UWB+BLE Architecture 0.5.47]].

## Reader Map

This document explains the whole clicker-anchor-gateway system: hardware, radio behavior, routing, power, and the tradeoff against the older BLE-gated concept. Exact packet fields are in [[UWB+BLE Protocols and Strategies 0.3.12.3]]. Current runtime flow is in [[Mesh Connected Routing Walkthrough]].

Read it in this order:

1. **System Facts** gives the constants the later sections depend on.
2. **Hardware Platform** records the electronics, BOM, power rail, and MCU pinout.
3. **Click Path** explains a normal click from button press to distance samples.
4. **Mesh, Gateway, and Reports** explains how anchors deliver data after ranging.
5. **Power and Battery** shows the current 75 mA UWB power model.
6. **Previous BLE-Gated Design Comparison** explains why the current design exists.

## System Facts

These facts are used throughout the document. They are listed first so later sections do not depend on hidden lower-level context.

| Topic | Current design |
| --- | --- |
| Main radio | DWM3000 UWB. Firmware uses channel 5 for wake, discovery, route contact, and ranging. |
| Payload mesh lane | DWM3000 UWB channel 9, only after channel-5 contact and channel-9 event timing are negotiated. |
| BLE use | Clicker-to-clicker courtesy plus the connected gateway-to-PC link. BLE does not wake anchors, authorize ranging, or carry UWB mesh relay traffic. |
| Normal click requirement | At least three eligible anchors must reply to discovery before burst ranging starts. |
| Schedule size | Up to four anchors can be scheduled in one normal-click burst. |
| Ranging burst | One continuous 400 ms channel-5 responder window shared by the selected anchors. |
| UWB PHY | 850 kbps, 4096-symbol preamble, PAC32, 4073-symbol SFD timeout, STS disabled, maximum configured DWM3000 TX power. Channel 5 is used for wake, discovery, route contact, and ranging; channel 9 uses the same PHY for negotiated payload events. |
| Ranging timing | Fixed equal DWM/DW3000 delayed-TX response/final delay. The current long-range main firmware selects `UWB_RANGE_REPLY_DELAY_LONG_RANGE_UUS = 8000`; the short-range candidate is `UWB_RANGE_REPLY_DELAY_SHORT_RANGE_UUS = 2750`. Both values need recalibration before being treated as final. The scheduled exchange stride is 33 ms. |
| First-path sensitivity | DWM3000 `IP_CONFIG_LO.IP_NTM=12`, the lower recommended first-path threshold. This may increase outliers, so sample-level outlier rejection remains required. |
| UWB active current model | 75 mA for RX/TX power estimates. Radio reset/configuration is still modeled separately at 20 mA where noted. |
| DWM3000 completion detection | No DWM3000 IRQ pin is directly available to the MCU. Firmware does not configure `irq-gpios`; it polls `SYS_STATUS` over SPI every 50 us while waiting for bounded TX/RX events. |
| Route freshness | Routes do not expire by age alone. They remain usable until replaced, explicitly cleared, or a delivery failure proves the route stale. |
| Channel-9 timing freshness | Event timing is separate from route knowledge. Received channel-9 packets adjust local timing; supervision expiry closes the timing entry and requires channel-5 contact refresh. |
| Mesh packet age | Every shared mesh packet carries a saturating millisecond age since original creation. Relays add queue/relay time before forwarding so receivers can compensate for mesh delay without gateway-synchronized anchor clocks. |

### Roles

| Role | What it owns | What it does not own |
| --- | --- | --- |
| Clicker | Button wake, click/self-test gesture handling, UWB wake claims, discovery, range schedule, DS-TWR initiation, local LED result | Question of the Day, office map, anchor placement, report storage |
| Anchor | Low-duty UWB wake scan, click admission, hash-derived discovery reply slot, responder burst, report queue, mesh relay, heartbeat/status, survey samples | Server-side event grouping, final position solve, question schedule |
| Gateway anchor | Same hardware as an anchor plus a connected BLE PC link, mesh root behavior, command routing, gateway ACKs, packet-age-aware survey orchestration | Distance solving and application storage |

### Traffic Lanes

| Lane | Channel | Used for | Must yield to |
| --- | --- | --- | --- |
| Click/contact lane | UWB channel 5 | Clicker wake claims, discovery, schedule admission, shared ranging burst, route discovery, route contact refresh | Nothing lower priority |
| Payload lane | UWB channel 9 | Reports, heartbeats, command results, gateway ACKs, survey data, routed packets | Active click service, required channel-5 wake scans, route/contact refresh |
| Courtesy side lane | BLE channel 37 | Short clicker-to-clicker priority hint before UWB wake | UWB wake train start |
| Gateway PC lane | Connected BLE GATT | PC command input, gateway packet output, and gateway identity | UWB work keeps priority over host servicing |

### Radio Scheduling

The DWM3000 is one radio. Firmware never assumes channel 5 and channel 9 can run at the same time. When work conflicts, priority is:

1. Active channel-5 click service: accepted wake epoch, discovery, schedule handling, or the shared responder burst.
2. Required quick channel-5 wake scan.
3. Channel-5 route contact or route timing refresh.
4. Negotiated channel-9 mesh payload event.
5. Retained sleep.

Channel-9 events may be clipped, skipped, or retried later. Channel-5 wake/contact work is not skipped merely to finish payload traffic.

## Hardware Platform

### Shared Electronics

Both clickers and anchors use the same main electronics:

- DWM3000 UWB module.
- ANNA-B402 module with nRF52833 MCU and BLE.
- TL3301 button on clickers.
- BQ24090 charger and LiPo/Li-Ion battery input.
- USB-C for charging, firmware upload, and bootloader recovery. Gateway runtime PC communication uses Bluetooth.
- Two RGB LEDs for local status.

### Power Rails

Both the clicker and anchor share the same power regulation topology:

```text
[LiPo/Li-Ion Battery] -> [Charger] <- [USB-C Input]
 3.2-4.2 V CC/CV at up to 1 A
 |
 v
[LDO]
 3.0 V regulated output
 150 mA max, Iq < 1 uA
 Dropout < 0.2 V, operates down to 3.2 V battery
 |
 +-> ANNA-B402-00B / nRF52833 VDD = 3.0 V
 +-> DWM3000 VDD3V3 = 3.0 V
```

The LiPo's low internal impedance can supply the 75 mA DWM3000 RX/TX pulses without the buffer capacitor that was needed in the older coin-cell concept. The charger provides USB over-voltage protection, selectable charge current, and status pins for charging/power-good reporting.

### Hardware Bill of Materials

| Name | Description | Designator | Quantity | Supplier Order Qty 1 | Availability | Manufacturer 1 | Manufacturer Part Number 1 | Supplier | Supplier Unit Price 1 | Supplier Subtotal 1 | Total |
|-------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------|----------|----------------------|--------------|-----------------------------|----------------------------|----------|-----------------------------|---------------------|---------|
| C0402C105K9PACTU | CAP CER 1UF 6.3V X5R 0402 | C1, C2, C3, C4, C7, C8, C9, C13 | 8 | 448 | 841700 | Yageo Group | C0402C105K9PACTU | Mouser | 80-C0402C105K9P | 0.00427 | 1.91 |
| C0603C220J1GACAUTO | CAP CER 22PF 100V C0G/NP0 0603 | C5, C6 | 2 | 112 | 77592 | Yageo Group | C0603C220J1GACAUTO | DigiKey | 399-15987-1-ND | 0.03014 | 3.38 |
| CC0805KKX7R7BB475 | CAP CER 4.7UF 16V X7R 0805 | C11 | 1 | 56 | 25273 | Yageo Group | CC0805KKX7R7BB475 | Mouser | 603-CC805KKX7R7BB475 | 0.14855 | 8.32 |
| NRVB130LSFT1G | NRVB130LSFT1G Rectifier Diode, Schottky, 1 Phase, 1 Element, 1A, 30V V(RRM), Silicon | D1 | 1 | 56 | 3499 | onsemi | NRVB130LSFT1G | Mouser | 863-NRVB130LSFT1G | 0.24673 | 13.82 |
| ESD241B1W0201E6327XTSA1 | TVS DIODE 3.3VWM 6VC WLL-2-3 | D2 | 1 | 56 | 3373 | Infineon | ESD241B1W0201E6327XTSA1 | Newark | ESD241B1W0201E6327XTSA1 | 0.03757 | 2.1 |
| TSW-102-07-F-S | CONN HEADER VERT 2POS 2.54MM | J1 | 1 | 56 | 100677 | Samtec | TSW-102-07-F-S | DigiKey | SAM10844-ND | 0.09391 | 5.26 |
| SM08B-SURS-TF(LF)(SN) | CONN HEADER SMD R/A 8POS 0.8MM | J2 | 1 | 56 | 7860 | JST | SM08B-SURS-TF(LF)(SN) | Farnell | SM08B-SURS-TF(LF)(SN) | 0.965 | 54.04 |
| TSW-104-23-T-S | CONN HEADER VERT 4POS 2.54MM | J3 | 1 | 56 | 1237 | Samtec | TSW-104-23-T-S | Mouser | 200-TSW10423TS | 0.22368 | 12.53 |
| USB4110-GF-A | USB-C (USB TYPE-C) 2.0 receptacle, 24 positions with 16+8 dummy positions | J4 | 1 | 56 | 109246 | Global Connector Technology | USB4110-GF-A | DigiKey | 2073-USB4110-GF-A-1-ND | 0.82012 | 45.93 |
| APF3236LSEEZGKQBKC | LED RGB CLEAR 6SMD | LED4, LED5 | 2 | 112 | 20537 | Kingbright | APF3236LSEEZGKQBKC | DigiKey | 754-1969-1-ND | 0.40297 | 45.13 |
| DWM3000TR13 | DWM3000 module; firmware uses channel 5 for wake/contact/ranging and channel 9 for negotiated mesh payload events | MD1 | 1 | 56 | 5596 | Qorvo | DWM3000TR13 | Mouser | 772-DWM3000TR13 | 17.24 | 965.29 |
| IRLML6401TRPBF | MOSFET P-CH 12V 4.3A SOT-23 | Q1, Q2 | 2 | 112 | 3943 | Infineon | IRLML6401TRPBF | DigiKey | IRLML6401PBFDKR-ND | 0.15052 | 16.86 |
| MCS0402MD1000DE000 | RES SMD 1.2K OHM 1% 1/10W 0402 | R1, R6, R7, R11, R17, R22, R23 | 7 | 392 | 17935 | Vishay | MCS0402MD1000DE000 | DigiKey | MCS0402-100-MDCT-ND | 0.07325 | 28.71 |
| AC0402JR-0727RL | RES 27Ω ±5% 0.063W 0402 | R2, R4 | 2 | 112 | 17885 | Yageo Group | AC0402JR-0727RL | Avnet | 58AK8894 | 0.00342 | 0.38248 |
| MCS0402PD1001DE500 | RES SMD 1.2K OHM 1% 1/10W 0402 | R3, R8, R9, R14, R24 | 5 | 280 | 5198 | Vishay | MCS0402PD1001DE500 | Mouser | 594-MCS0402PD1001DP5 | 0.10928 | 30.6 |
| CRGCQ0402J10K | CRGCQ 0402 10K 5% | R5 | 1 | 56 | 444124 | TE Connectivity | CRGCQ0402J10K | DigiKey | A130054CT-ND | 0.01144 | 0.64066 |
| AF0402FR-07470RL | Anti-sulfurated chip resistor, 470Ω 1% 100ppm/°C 0402 | R10, R19 | 2 | 112 | 104788 | Yageo Group | AF0402FR-07470RL | Mouser | 603-AF0402FR-07470RL | 0.00598 | 0.66934 |
| CRG0402F5K1 | | R12, R13 | 2 | 112 | 38339 | TE Connectivity | CRG0402F5K1 | DigiKey | 1712-CRG0402F5K1CT-ND | 0.01153 | 1.29 |
| CRGCQ0402F6K8 | CRGCQ 0402 6K8 1% | R15 | 1 | 56 | 17842 | TE Connectivity | CRGCQ0402F6K8 | Newark | CRGCQ0402F6K8 | 0.00171 | 0.09562 |
| CRCW0402100KJNTD | | R18 | 1 | 56 | 25156 | Vishay | CRCW0402100KJNTD | Mouser | 71-CRCW0402J-100K | 0.0333 | 1.86 |
| CRGCQ0603J680K | CRGCQ0603 680KΩ 5% ±100ppm/°C 75V | R20, R21 | 2 | 112 | 4509 | TE Connectivity | CRGCQ0603J680K | DigiKey | A130108CT-ND | 0.00973 | 1.09 |
| TL3301AF260QG | Tactile Switch, SPST-NO, 50 mA, 12 V, -20 to 70 degC, 4-Pin SMD, RoHS, Tape and Reel | SW1 | 1 | 56 | 34821 | E-Switch | TL3301AF260QG | DigiKey | EG2527CT-ND | 0.23632 | 13.23 |
| 5003 | Test Point, Orange, Height 4.6 mm, Tail Length 3 mm, 1-Pin THD, RoHS | TP_VCC, TP_VUSB | 2 | 112 | 59826 | Keystone | 5003 | DigiKey | 36-5003-ND | 0.1642 | 18.39 |
| BQ24090DGQR | 1 A single-cell Li-Ion battery charger with 10 K ohm NTC, 10-pin MSOP-PowerPAD | U2 | 1 | 56 | 4558 | Texas Instruments | BQ24090DGQR | Mouser | 595-BQ24090DGQR | 0.70349 | 39.4 |
| AP7354-30W5-7 | IC REG LINEAR 3V 150MA SOT-25 | U3 | 1 | 56 | 2730 | Diodes Inc. | AP7354-30W5-7 | Newark | AP7354-30W5-7 | 0.1938 | 10.85 |
| ANNA-B402-OPENCPU | u-blox ANNA-B402 Bluetooth 5 LE module | U4 | 1 | 56 | 10464 | u-blox | ANNA-B402-00B | Mouser | 377-ANNA-B402-00B | 5.04 | 282.08 |
| TPD4E05U06QDQARQ1 | TVS DIODE 5.5V 14V 10USON | U5 | 1 | 56 | 104604 | Texas Instruments | TPD4E05U06QDQARQ1 | DigiKey | 296-40696-1-ND | 0.61129 | 34.23 |
| FC-12M_32.7680KA-A5 | 32.768 kHz crystal | Y1 | 1 | 56 | 7793 | Epson | FC-12M 32.7680KA-A5 | DigiKey | 114-FC-12M32.7680KA-A5CT-ND | 0.44361 | 24.84 |

### MCU Pinout

| MCU Pin | Connected to | Notes |
| --- | --- | --- |
| P0.02 | Unused for DWM3000 IRQ | The DWM3000 IRQ pin is not routed directly to the MCU; overlay has no `irq-gpios` |
| P0.03 | DWM3000 CS | Chip select; used with nRF52833 SPIM3 |
| P0.16 | TX | JTAG only |
| P0.31 | DWM3000 RST | Pull low to reset, otherwise high |
| P0.30 | DWM3000 WakeUP pin | |
| P0.11 | DWM3000 SPICLK | nRF52833 SPIM3 SCK, runtime target 32 MHz |
| P0.12 | DWM3000 SPIMISO | nRF52833 SPIM3 MISO |
| P1.00 | DWM3000 SPIMOSI | nRF52833 SPIM3 MOSI |
| P0.04 | RX | JTAG only |
| USBDM / USBDP | USB C Port | Firmware upload and bootloader recovery |
| P0.17 | RGB BLUE Output 1 | |
| P0.20 | RGB RED Output 1 | |
| P0.14 | RGB GREEN Output 1 | |
| P0.13 | RGB BLUE Output 2 | |
| P1.01 | RGB GREEN Output 2 | |
| P0.08 | RGB RED Output 2 | |
| P0.26 | Clicker Button | Use internal pull-up |
| P0.15 | BAT_PG | Charger power-good status, use internal pull-up |
| P0.06 | BAT_CHG | Charger charge status, use internal pull-up |
| P0.29 | HALF_BAT_V | Half-battery voltage sense |
| P0.07 | BAT_ADC_MOSFET | High-side P-channel MOSFET, drive low to enable divider |

## Click Path

### Normal Click Flow

The clicker discovers anchors on every press. Moving, adding, or removing anchors does not require clicker configuration.

```text
t=0 ms      Button press wakes clicker
t=0-75+ ms  Clicker samples decoded UWB gate traffic at the start and end of the BLE courtesy window
t≈gate      After the decoded gate is quiet, clicker sends repeated UWB wake claims for 400 ms
t≈scan      Anchors that accept a valid claim create one ownership epoch
t≈wake      Clicker sends discovery and listens for hash-slot anchor replies
t≈reply     If at least three anchors replied, clicker sends the selected-anchor range schedule
t≈release   If one or two anchors replied, clicker releases those anchors back to low-duty scan
t≈range     Scheduled anchors run no-STS DS-TWR inside one shared 400 ms responder burst
t<15 s      Click succeeds after 3 unique anchors range, or retries up to 6 attempts
```

Discovery replies prove that anchors are present; they are not distance measurements. A normal click only ranges after at least three eligible anchors reply. The schedule may include up to four anchors. All selected anchors share the same continuous 400 ms channel-5 responder burst while the clicker runs addressed exchanges with a 33 ms minimum exchange stride.

If zero anchors reply, the clicker does not start burst ranging and sends no release. If one or two anchors reply, the clicker sends a compact range-release frame to those anchors before retrying or failing. That frame tells the anchors that this attempt did not reach the normal-click minimum and that they can return to low-duty scan instead of waiting for a schedule.

Retries after no discovery replies, after an insufficient-discovery release, and after an incomplete burst all use the same low-power retry path: the clicker sleeps through the fixed retry base delay and randomized contention bucket before the next wake phase. Incomplete burst retries keep the same click event and nonce, advance `attempt_index`, and use a fresh attempt-local `priority_id`.

A failing anchor cannot end the shared burst for the others, and three successful anchors do not stop the burst early. Wrong-target polls are ignored while the responder burst remains open. Wake-claim timing fields are bounded, and anchors use the same freshness and priority rules throughout the attempt.

### Politeness and BLE Courtesy

BLE courtesy is only a clicker-to-clicker hint during UWB politeness. It does not wake anchors, authorize ranging, carry reports, or replace UWB routing.

The advertisement carries `network_id`, full `clicker_id`, `click_event_id`, `attempt_index`, `priority_id`, and a peer-finish wait. Courtesy stays active for at least 75 ms before the UWB wake train. The clicker decodes a UWB politeness sample at the beginning and end of that BLE window; if the first sample finds relevant UWB traffic, BLE courtesy stops immediately and the UWB wait/restart path takes over. If a clicker hears a higher-priority peer, it sleeps for the advertised peer-finish wait and reruns the gate. After three courtesy deferrals, BLE courtesy is disabled for that attempt so only decoded UWB traffic can hold the click path.

Courtesy uses BLE advertising channel 37 only. The clicker disables advertising channels 38 and 39 and asks the SoftDevice Controller to scan only channel 37. If that setup fails, firmware skips BLE courtesy for the attempt and relies on UWB politeness.

UWB gate traffic is decoded before it can hold the clicker. Wrong-network, malformed, self-originated wake/discovery, and UWB mesh frames are ignored. Same-network peer wake claims wait for their advertised remaining claimed duration; same-network schedules or range traffic wait for the bounded active window. When that wait ends, the clicker restarts UWB politeness and requires quiet samples again. A clear gate does not add randomized contention; it starts the wake train. Randomized contention is retry-only collision desynchronization before the next gate: after the fixed 150 ms retry base delay, attempt 2 chooses one of 32 12 ms slots (0-372 ms) and attempt 3+ chooses one of 64 slots (0-756 ms). If two clickers collide and both retry, matching again is 1/32 on the first retry and 1/64 afterward; if one starts earlier, the other can hear it through the decoded politeness poll.

The BLE interval uses the fastest legal BLE 5.x non-connectable range on the target: 20.0 to 20.625 ms plus the controller advertising delay. Passive scanning uses 20 ms windows every 25 ms. BLE stops before the UWB wake train begins.

Advertising and scanning are enabled together at the host/controller level, but this is not a full-duplex BLE RF model. The nRF BLE controller schedules one physical channel-37 activity at a time. A local advertising TX event is not scan RX time and should be treated as a receive blackout when estimating whether a peer advertisement is intercepted.

### Ranging Radio Settings

Click ranging uses:

- UWB channel 5.
- 850 kbps.
- Preamble length 4096.
- PAC32.
- SFD timeout 4073.
- STS disabled.
- First-path threshold `IP_CONFIG_LO.IP_NTM=12`.
- Fixed equal `UWB_RANGE_REPLY_DELAY_UUS` DWM/DW3000 delayed-TX response/final delay, selected from provisional short-range and long-range presets.
- Maximum configured DWM3000 TX power.
- SPI at 2 MHz for reset/init and 32 MHz at runtime.

Channel 9 is not used for wake, discovery, route refresh, or ranging. It is only used for negotiated mesh payload events after channel-5 contact exists.

### One DS-TWR Exchange

The responder response delay and initiator final delay are both fixed to the selected `UWB_RANGE_REPLY_DELAY_UUS`, the DWM/DW3000 delayed-TX unit. Schedules using another delay are rejected. The current long-range main firmware selects `UWB_RANGE_REPLY_DELAY_LONG_RANGE_UUS = 8000`; `UWB_RANGE_REPLY_DELAY_SHORT_RANGE_UUS = 2750` is kept as the provisional lower-delay candidate. Both values still need recalibration against the final PHY, SPI/logging load, and stage-one traffic. The response frame that the clicker receives is 8 bytes longer than the poll received by the anchor, so the shorter path waits instead of making the two reply delays differ.

Scheduled receive legs use a zero preamble-detect timeout. That disables the separate preamble hunt timeout while keeping the receive bounded by the frame-wait timeout. Free-running receive paths, including wake and mesh scans, keep a nonzero preamble timeout so a missed preamble does not consume the whole software window.

| Phase | Description | Time |
| --- | --- | --- |
| 1. POLL TX | SPI write, radio start, POLL on-air at 850 kbps | ~700 us |
| 2. Wait for RESPONSE | RX enable, turnaround, and response frame | ~2200 us |
| 3. Process RESPONSE | Read timestamps and frame, validate, build FINAL | ~250 us |
| 4. FINAL TX | Delayed TX, FINAL on-air, read TX timestamp | ~1700 us |
| 5. Wait for REPORT | RX enable, fixed selected `UWB_RANGE_REPLY_DELAY_UUS` DWM/DW3000 delayed-TX unit, responder compute, REPORT on-air | bounded by the delayed-TX unit plus on-air/read time |
| 6. Read REPORT | Read and parse report frame | ~100 us |
| **Total** | | **~21000 us practical, scheduled with a 33 ms stride** |

### Diagnostics After Ranging

The clicker does not delay the scheduled `FINAL` for long diagnostic reads. It sends compact clicker-side diagnostics after `FINAL` with `UWB_CLICKER_DIAG`. Anchors collect anchor-side diagnostics after the first valid `FINAL`, when the response delayed-TX deadline is no longer at risk. Missing or truncated diagnostics do not invalidate a completed range, but reports record what was captured, transmitted, truncated, dropped, or unavailable.

### Self-Test and Status Indication

#### Button Gestures

| Gesture | Meaning |
| --- | --- |
| Short press | Normal click event |
| Long press | Arm self-test mode |
| Short press within 3 seconds after long press | Run self-test |
| Long press timeout | Return to idle without reporting a click |

Default timing values:

- Short press: button released before 1.5 seconds.
- Long press: button held for at least 1.5 seconds.
- Self-test arming window: 3 seconds after long press release.
- Debounce: 50 ms minimum stable button state before accepting an edge.

#### Self-Test Checks

Self-test verifies:

1. MCU wake from button interrupt.
2. RGB LED drive.
3. Battery voltage and charger status pins.
4. DWM3000 wake/init and diagnostic UWB wake/discovery.
5. At least one diagnostic discovery reply.
6. At least one diagnostic range exchange.
7. Return to low-power idle.

Self-test traffic uses diagnostic flags and never sets the normal click flag. Anchors may forward diagnostic events for maintenance logs, but the server must not count them as user clicks.

#### LED Patterns

| Pattern | Meaning |
| --- | --- |
| Blue pulse | Self-test armed; waiting for confirming short press |
| Blue chase | Self-test running |
| Green solid for 2 seconds | Self-test passed or click accepted |
| Amber blink once | Low battery warning |
| Amber slow blink | Charging |
| Green slow blink | Charged and idle on dock |
| Red blink code repeated 3 times | Self-test failure, click failure, or module failure |

Self-test failure codes:

| Red blinks | Failure |
| --- | --- |
| 1 | Battery/status read failed |
| 2 | UWB wake/discovery setup failed |
| 3 | DWM3000 wake/init failed |
| 4 | No anchor replied to diagnostic discovery |
| 5 | UWB diagnostic range failed |
| 6 | Internal firmware state error |

Click failure codes:

| Red blinks | Failure |
| --- | --- |
| 1 | No anchor heard during UWB discovery |
| 2 | Fewer than the required anchors ranged successfully |

## Mesh, Gateway, and Reports

### Channel Use

Channel 5 is the contact lane:

- Clicker wake claims.
- Discovery and discovery replies.
- Range schedule admission.
- Shared burst ranging.
- Mesh route discovery.
- Route replies when channel-9 timing is missing.
- Route/contact refresh after missing or expired channel-9 timing.

Channel 9 is the payload lane:

- Click reports.
- Heartbeats.
- Command results.
- Gateway ACKs.
- Survey data.
- Routed packets.

Channel 9 is used only after channel-5 contact exists and both peers have usable event timing. Event timing can go stale independently of the route; received channel-9 packets adjust the local next-window estimate, and only supervision expiry closes the timing entry and sends payload delivery back through channel-5 contact refresh.

### Channel-9 Mesh Events

A channel-9 event is a short scheduled payload window between two neighbors. The timing state is per next hop:

- Event interval.
- Event window.
- Next event time.
- Event counter.
- Guard time for retune, PLL readiness, RX setup, and software jitter.
- Clock-skew guard.
- Maximum missed events.
- Supervision timeout.

Event timing is negotiated after channel-5 contact. One side proposes a window, the peer accepts or clips it, and then both sides treat the next hop as channel-9-ready. When a packet is received in a channel-9 window, the receiver shifts its next local event from that observed arrival. Missed windows keep advancing on channel 9 until the supervision timeout closes the timing entry.

Before entering channel 9, firmware checks for active click service, discovery, responder bursts, and required quick channel-5 wake scans. If a conflict exists, mesh is deferred or skipped. Heartbeat/status telemetry exposes channel switches, PLL-ready failures, late channel-5 returns, mesh deferrals, missed channel-9 events, channel-5 preemptions, and channel-9 report latency.

### Routes in Plain Terms

Routes are learned only when a packet needs to move. A sender without a usable path sends a route request on channel 5. Relays remember how to get back to the requester. The target sends a reply back along that path. Each node stores only the next hop it should use, not a full topology map.

The gateway is the normal root. For gateway-bound traffic, the best route is primarily the one with fewer hops. Link quality only chooses between routes with the same hop count.

| RSSI | Link quality |
| --- | --- |
| ≤ -100 dBm | 1 |
| -99 to -41 dBm | `RSSI + 100`, range 1-59 |
| ≥ -40 dBm | 100 |

Route cost:

```text
cost = hop_count * 100 + (100 - link_quality)
```

Lower cost wins. A direct gateway route with weak quality still beats a one-relay route because one extra hop adds 100 cost points. When two routes tie, firmware chooses better link quality, then fewer hops, then newer observation time, then lower next-hop ID.

For multi-hop paths, the stored quality is the weakest link in the path. If gateway-to-A is 72 and A-to-B is 45, B stores path quality 45.

### Reliable Delivery

Every important gateway-bound packet asks for an end-to-end gateway ACK. Reports, survey results, heartbeats, and command results are considered delivered only when that ACK returns.

| Parameter | Value |
| --- | --- |
| Clicker UWB wake train | 400 ms |
| Anchor normal wake scan interval/window | 380 ms / 5 ms RX |
| Anchor Stage 1 rxproof wake scan interval/window | 380 ms / 5 ms RX, matching the normal target |
| Anchor channel-9 mesh RX | Scheduled after channel-5 contact; no standalone idle channel-5 mesh listener |
| Gateway UWB mesh RX window/idle | 50 ms / 2 ms idle |
| Gateway ACK timeout | 2 s base window; matching hop ACKs reset the window while downstream progress continues |
| Max retries | Two retransmits on the selected candidate; the third missing gateway ACK invalidates that candidate |
| Retry backoff | 100 ms, 250 ms, then 500 ms for alternate-route retry, each with jitter |
| Route rediscovery budget | Five requests per target with 250/500/1000/2000/4000 ms exponential backoff plus jitter |
| Route freshness | Usable until replaced, explicitly cleared, or a delivery failure proves it stale |
| Duplicate suppression window | 60 s per message identity |
| Gateway command-result timeout | 12 s for one outstanding gateway command |

Routes are considered usable until there is evidence they are wrong. Age alone does not remove a route. A route becomes stale when a newer route epoch replaces it, a command clears it, repeated missing gateway ACKs exhaust the selected upstream path, a downlink command times out, or a UWB send failure proves the selected next hop cannot be used. If another candidate exists, firmware switches routes after randomized backoff. If no route remains, the packet returns to the queue and bounded route discovery starts again.

Gateway ACK remains the only delivery confirmation. Hop ACKs are progress telemetry: a downstream relay sends one after it forwards an ACK-required packet, and the original sender resets its 2 s gateway-ACK wait when the hop ACK matches the pending packet. If hop ACKs stop and the gateway ACK still does not arrive, normal retry and route invalidation resumes. Packet age is refreshed at the gateway-ACK timeout and again before retransmission, so retransmitted packets include queue time, ACK wait time, and retry backoff time.

Rediscovery attempts are counted per target. A successful route reply, command-installed route, or gateway ACK resets the rediscovery budget. A candidate invalidated by missing ACKs may become selected again later if rediscovery advertises that same next hop in the current route epoch and it wins the route-cost comparison.

`CMD_FORCE_REDISCOVERY` is gateway-originated maintenance. The target anchor first attempts to send the command result through the current route, then invalidates upstream, downlink, and channel-9 route timing state and starts bounded rediscovery toward the gateway.

A relay that is already waiting for one gateway-bound confirmation drops new packets that would require forwarding; the previous sender will retry if needed.

Duplicate packet identities stay in a 60 s cache. If a duplicate arrives while the relay is idle and has a route, it can be forwarded again without local double-delivery. If the relay is busy or has no route, it drops the duplicate.

### Click Report Delivery

Anchor-side range results are queued during the responder burst. Each normal-click report carries distance, quality, range status, sample data, clicker identity, anchor identity, click event, timestamps, round indices, and the burst identity. The first normal-click report packet for a scheduled burst must include `BURST_ID`; fragments for the same measurement keep the same anchor, clicker, event, and burst grouping context. If samples or diagnostics do not fit in one UWB mesh frame, the anchor queues additional fragments for the same anchor, clicker, event, and burst.

After the DWM3000 returns to retained sleep, the anchor drains reports one at a time. Reports prefer negotiated channel-9 events. If channel-9 timing is missing or stale, the sender refreshes contact on channel 5 first. Each report waits for a gateway ACK before it is considered delivered.

### Click Priority Over Mesh

Anchor click handling has priority over relay traffic. During wake scanning, valid wake claims are decoded before mesh frames. The anchor cancels active mesh forwarding and clears pending mesh receive work only after a claim passes network, channel, flag, freshness, and ownership checks. Foreign or rejected claims do not interrupt mesh work. Already-built local click reports stay queued for later delivery.

### Gateway Commands

The gateway sends commands through the same mesh envelope and downlink route table. The v1 command classes are:

| Command class | Purpose |
| --- | --- |
| Ping/status | Verify an anchor is alive and report firmware, uptime, battery, route, and radio status |
| LED/status pattern | Trigger a visible setup/testing pattern |
| Route management | Set, clear, or request route information |
| Heartbeat control | Start or stop periodic anchor health reports |
| Survey control | Start survey discovery, abort active survey work, and schedule anchor-to-anchor distance measurements |

The target returns a `COMMAND_RESULT`. Unsupported commands must return `UNSUPPORTED_COMMAND`.

The gateway keeps one outstanding command at a time. A second PC command received over the connected BLE gateway link is rejected with `COMMAND_BUSY` until the first command resolves by matching result, route failure, or the 12 s command-result timeout.

### Packet Age Instead Of Gateway Time Sync

The current firmware does not maintain a gateway-synchronized anchor clock. Each shared mesh packet has an always-present millisecond age field. Sources create packets with age zero; queues, relays, and retransmissions add elapsed local time before sending. The value saturates at `UINT32_MAX` and is carried through every mesh packet class.

Reports, survey samples, status responses, and heartbeats carry local uptime/event timestamps. The gateway can combine those local timestamps with packet age when it needs to understand how long ago a message was created. Survey discovery uses this directly: anchors subtract packet age from the gateway-provided discovery start delay so a multi-hop broadcast still starts all reachable anchors near the same hash-slotted discovery epoch.

## Survey and Server Handling

### Anchor Self-Distance Survey

The gateway uses surveys to collect anchor-to-anchor distances for off-device placement solving. Firmware only measures and reports distances; solving the geometry is server-side/off-site work.

Survey flow:

1. Gateway creates a `survey_id` and floods `SURVEY_DISCOVERY_START` through the mesh with a future start delay, slot length, and slot count.
2. Anchors subtract packet age from the start delay, preempt ordinary click/mesh work, and join the same hash-slotted UWB discovery epoch.
3. Each anchor transmits one compact `SURVEY_DISCOVERY_PROBE` in slot `hash(anchor_id) % slot_count` while all other anchors listen.
4. Anchors queue `SURVEY_DISCOVERY_REPORT` packets that list the peers heard, signal level, and quality metadata.
5. Discovery reports use post-discovery mesh report slots based on the same hash-derived slot, so the gateway receives a report train instead of a flood.
6. Gateway builds a graph of reachable anchor pairs from measured UWB discovery reports.
7. Gateway prepares one unordered anchor pair at a time.
8. After both anchors acknowledge readiness, the gateway starts exactly `n` measurements.
9. The pair runs measurements on the dedicated survey worker.
10. Each sample is reported to the gateway as diagnostic survey data.
11. Gateway schedules the next pair or aborts.

The current runtime uses a 2 s future start delay, six 40 ms survey discovery slots by default, and accepts an optional gateway command `DISCOVERY_SLOT_COUNT` override up to the protocol maximum of 50 slots. Each anchor's discovery report is then delayed by `discovery_duration + anchor_slot * survey_report_mesh_slot_ms`; the report slot is longer than the gateway ACK timeout to reduce self-inflicted mesh flooding. With the local DW3000 SDK, survey discovery uses the lowest exposed data rate, 850 kbps, with the long 4096-symbol no-STS wake PHY. If later hardware validation supports a different survey-only PHY, that PHY should keep the lowest supported data rate and longest validated preamble while keeping slots guarded by airtime/build-time checks.

During a survey pair run, command and mesh work stay available. A survey abort is recorded immediately and the worker exits at the next sample boundary or bounded responder-listen check.

Survey pair result messages include:

- `survey_id`.
- Initiator anchor ID.
- Responder anchor ID.
- Sample index.
- Requested sample count.
- Distance in millimeters.
- Quality/status code.
- Diagnostic flag.

Survey traffic must never count as click events.

### Server Event Handling

The server receives independent anchor reports for the same click event:

1. Group normal-click reports by clicker, click event, and burst identity.
2. Wait for a configurable window to allow mesh relay delay.
3. Finalize the event with all valid distance measurements from the same burst identity.
4. Compute `(x, y)` position from at least three anchor distances, known anchor coordinates, and any other selected inputs.
5. Store the event with the active Question of the Day.

The server must not silently combine failed partial attempts from different retry bursts. If `BURST_ID` is missing for a normal click, the server needs an equally strong attempt-isolation mechanism before it may accept a three-anchor solve.

Clickers and normal anchors do not know the Question of the Day. The server owns that schedule. Any display at the clicker station is a separate system.

## Power and Battery

All UWB RX/TX energy below uses 75 mA. This is deliberately conservative for the active radio windows and is separate from the 20 mA reset/configuration row.

### Clicker Budget

Assumptions:

- 50 clicks/day.
- Two quiet 2 ms UWB politeness samples in the normal quiet case.
- 75 ms minimum BLE courtesy scan/advertise during normal-click politeness.
- 400 ms UWB wake-claim train.
- UWB discovery/reply/schedule exchange.
- Up to 4 scheduled anchors.
- One shared 400 ms channel-5 ranging burst.
- Retry and contention backoff are mostly sleep time.

#### Per-Click Energy

| Phase | Duration | Current | Charge (uA·s) |
| --- | --- | --- | --- |
| MCU wake from deep sleep | 1 ms | 5 mA | 5 |
| UWB sampled politeness | 2 × (2.67 ms startup/PLL + 2 ms RX) = 9.34 ms active | 75 mA | 701 |
| BLE courtesy scan/advertise | 75 ms wall time, controller-scheduled 20/25 ms passive scan duty plus about three 1 ms TX events; local TX is not RX time | 5 mA active | 315 |
| UWB wake train | 400 ms | 75 mA | 30,000 |
| UWB discovery/reply/schedule | ~80 ms | 75 mA | 6,000 |
| UWB radio wake/reset/configure | 10 ms | 20 mA | 200 |
| UWB shared TWR burst | One 400 ms channel-5 burst covering up to 4 scheduled anchors | 75 mA | 30,000 |
| MCU processing + LED blink | 50 ms | 10 mA | 500 |
| **Total per click** | **~1.05 s wall time in the normal quiet case** | | **69,971 uA·s** |

69,971 uA·s is about 0.0194 mAh per click. Decoded same-network UWB waits are mostly sleep until the advertised or scheduled window clears, followed by a restarted politeness gate. A full 500 ms no-clear politeness window is still sampled, not continuous RX, and costs far less than a continuous 500 ms listen at 75 mA.

#### Daily Energy

| Component | Calculation | mAh/day |
| --- | --- | --- |
| Active clicks | 50 × 0.0194 mAh | 0.97 |
| Deep sleep, MCU plus UWB | 2.86 uA × 24h | 0.069 |
| **Daily total** | | **1.04** |
| With ~3× safety margin | | **3.12** |

| Battery | Capacity | Estimated Life |
| --- | --- | --- |
| LiPo 85 mAh | 85 mAh | ~27 days |

### Anchor Budget

Assumptions:

- Low-duty DWM3000 wake scanning: 380 ms interval, 5 ms RX window, plus 2.5 ms startup and 0.17 ms PLL overhead.
- Stage 1 rxproof uses the same 380 ms interval and 5 ms RX window. Higher-duty debug overrides are bench-only and must be estimated from their configured interval/RX values.
- Anchor channel-9 mesh receive is scheduled after channel-5 wake/contact negotiation and is not counted as a separate always-on idle baseline.
- 1000 selected ranging events/day.
- 1000 queued click-report mesh deliveries/day.

#### Daily Consumption

| Component | Calculation | mAh/day | % of total |
| --- | --- | --- | --- |
| UWB wake scan baseline | 7.67 ms awake / 387.67 ms period = 1.978%; 1709.4 s/day × 75 mA | 35.61 | 72.6% |
| UWB discovery/schedule control | 1000 × (15 ms claim collection + 108 ms discovery listen + 20 ms reply TX + 80 ms schedule RX) × 75 mA | 4.65 | 9.5% |
| UWB responder bursts | 1000 × 400 ms shared responder window × 75 mA | 8.33 | 17.0% |
| UWB mesh report TX | 1000 × (2.67 ms startup/PLL + 20 ms TX timeout) × 75 mA | 0.47 | 1.0% |
| **Daily total** | Estimated radio budget before margin | **49.06** | **100%** |
| **With 1.5× safety margin** | 49.06 mAh/day × 1.5 | **73.59** | |

The Stage 1 rxproof build now uses the same 380 ms / 5 ms scan setting as the normal anchor. Any future continuous-RX or over-budget debug profile is useful for proving the wake/discovery/range protocol path, but it must not be used for production battery sizing.

A route miss that adds one extra `ROUTE_REQ` transmit costs another 0.47 mAh per 1000 reports. A day where every report starts with route discovery is about 49.54 mAh before margin, or 74.31 mAh/day with the same margin.

#### Battery Life

Assumptions:

- Load: 73.59 mAh/day after 1.5× safety margin for the normal 5 ms scan estimate.
- Battery: 18650 Li-Ion, 3000 mAh nominal.
- Efficiency: 0.85 usable capacity.

| Number of Batteries | Total Usable Capacity | Est. Days | Est. Months | Est. Years |
| --- | --- | --- | --- | --- |
| 1 × 18650 | 2,550 mAh | 34.7 | 1.1 | 0.09 |
| 2 × 18650 | 5,100 mAh | 69.3 | 2.3 | 0.19 |
| 3 × 18650 | 7,650 mAh | 104.0 | 3.5 | 0.28 |

Low-duty UWB wake scanning is the dominant anchor idle cost. The current 5 ms RX setting is about 1.29% RX-window duty over the full wake/sleep cycle, or 1.32% against the configured 380 ms sleep interval. It is about 1.98% conservative DWM3000 awake-time duty once startup and PLL time are charged at the same 75 mA estimate. Active route/report traffic adds about 13.45 mAh/day under the 1000 selected-events/day assumption. Higher-duty Stage 1 debug scans must not be treated as the production anchor setting.

The IRQ-free runtime does not change the configured DWM3000 awake windows in the table above. It adds MCU and SPI energy while firmware waits for UWB TX/RX completion because the nRF polls `SYS_STATUS` every 50 us instead of sleeping until a DWM3000 IRQ edge. The additional daily cost is approximately `MCU_active_current_mA * status_polled_seconds_per_day / 3600`. For the normal 5 ms periodic anchor scan baseline, the conservative awake-time model is about 1709 status-polled seconds/day, so a 4-6 mA MCU active-current delta adds roughly 1.90-2.85 mAh/day before margin. Higher-duty debug profiles must be measured directly rather than extrapolated into production. Hardware power validation should measure this separately from the DWM3000 RX/TX current.

### Previous BLE-Gated Design Comparison

The older BLE-gated concept is useful as a contrast, but it should not be summarized by inventing one fixed BLE exchange duration. The important difference is which radio owns admission and how long a selected anchor can be forced to keep UWB awake.

| Question | Previous BLE-gated concept | Current UWB-gated plus BLE courtesy |
| --- | --- | --- |
| Who wakes anchors? | BLE request/ready behavior decided when UWB should start. | UWB wake claims are the admission authority. BLE is only a courtesy hint between clickers. |
| What proves the selected clicker? | BLE and later UWB state both had to agree. | One UWB identity chain covers wake, discovery, schedule, range, and report identity. |
| What happens when too few anchors reply? | The old design did not have the current range-release courtesy frame. | The clicker releases any anchors that did reply, then retries or fails. |
| How long can a selected anchor sit in UWB? | The risk is any long first-poll or post-range responder wait. Every extra 1 s of UWB awake at 75 mA costs 0.0208 mAh per selected event, or 20.8 mAh/day at 1000 events. | The normal selected-anchor responder window is bounded to one 400 ms burst, which costs 0.00833 mAh per selected event, or 8.33 mAh/day at 1000 events. |
| How are reports delivered? | Operational routing/reporting needed a separate design path. | The same UWB system owns ranging reports, heartbeats, commands, ACKs, and surveys. |

The current design moves complexity into deterministic UWB admission and routing so the anchor's high-current UWB windows are short and explainable. BLE remains useful, but only as collision-reducing courtesy before UWB starts.

### Reliability Notes

The current wake path is designed for timing coverage first. The 400 ms advertised wake-claim train is longer than the 399.67 ms anchor scan period, so every anchor should get at least one chance to hear a claim if RF conditions are good and no higher-priority work blocks the scan.

BLE courtesy reduces simultaneous-click collisions before UWB starts. The useful courtesy metric is "lower hears higher" because only the lower-priority clicker should defer.

The table below is generated by `Documentation/scripts/ble_courtesy_probability.py` with 1,000,000 Monte Carlo trials and seed `0x1a2b3c4d`. It uses the implemented BLE courtesy timing from `firmware/app/src/main.c`: scan starts before advertising, passive scan is fixed at 20 ms every 25 ms on channel 37, advertising uses 20.0-20.625 ms intervals plus 0-10 ms controller advertising delay, and the implemented courtesy window is 75 ms. The model treats the nRF BLE radio as single-event: each receiver's own channel-37 advertising event removes that 1 ms from scan RX. A peer is counted as heard only when the full 1 ms peer advertising event is contained inside the receiver's remaining scan RX time. The model does not add a random scan phase; the only scan-off time is the configured 5 ms gap in each 25 ms scan period plus local TX blackout.

| Courtesy window | Lower hears higher | At least one direction hears | Mutual detection |
| ---: | ---: | ---: | ---: |
| 10 ms | 27.6% | 48.2% | 6.9% |
| 15 ms | 42.9% | 67.6% | 18.0% |
| 20 ms | 58.1% | 81.6% | 34.5% |
| 25 ms | 58.1% | 81.6% | 34.5% |
| 27 ms | 61.1% | 83.8% | 38.4% |
| 30 ms | 70.2% | 89.6% | 50.7% |
| 40 ms | 76.5% | 93.6% | 59.5% |
| 50 ms | 80.6% | 95.3% | 65.8% |
| 60 ms | 86.3% | 97.7% | 74.9% |
| **75 ms** | **88.8%** | **98.5%** | **79.0%** |
| 90 ms | 92.0% | 99.2% | 84.7% |
| 100 ms | 93.3% | 99.5% | 87.1% |
| 125 ms | 96.0% | 99.8% | 92.1% |
| 150 ms | 97.6% | 99.9% | 95.2% |
| 200 ms | 99.1% | 100.0% | 98.2% |

The implemented value is 75 ms. In the corrected single-radio model, the directionally useful "lower hears higher" probability is 88.8%, not the previous 96.7% first-order overlap estimate. Misses remain safe because courtesy is advisory; a miss falls back to UWB politeness and retry-side contention rather than blocking the click.

Normal click acceptance requires three unique anchors. With more anchors in range, the click can tolerate individual anchor misses. With fewer than three eligible discovery replies, a normal click does not range; if one or two anchors replied, the clicker sends range release before retrying or failing according to the click budget.

### Tradeoffs

| Architecture | Strengths | Costs and risks |
| --- | --- | --- |
| Previous BLE-gated | Low-cost BLE scanning, mature BLE receiver behavior, BLE request advertisement covers the 1 s scan phase, DWM can stay asleep until BLE request | High anchor event energy from long UWB responder deadlines, BLE discovery is not the ranging identity authority, weak coordination with hidden/multiple clickers, operational mesh/routing needs another transport path, old responder can burn 120 s of UWB listen time if no addressed poll arrives |
| Current UWB-gated plus BLE courtesy | One protocol family owns wake, discovery, ranging, reports, and mesh; deterministic wake-train overlap; CRC and full identity checks before state creation; selected-clicker-only ranging; bounded scheduled windows; idle UWB duty is bounded by the calibrated RX-duty budget; BLE courtesy reduces simultaneous-click collisions before UWB TX | Requires status-poll timing validation on hardware, UWB wake-scan link budget must be validated on hardware, idle UWB scan dominates battery budget, BLE courtesy depends on nRF scan-channel-map support and is advisory only, active mesh traffic must be tuned against click priority |

## Firmware Boundaries

Both clicker and anchor firmware run on the same ANNA-B402-00B plus DWM3000 hardware. The configured role decides whether the device behaves as a clicker, anchor, or gateway. Firmware uses Zephyr through nRF Connect SDK.

Keep hardware-independent behavior in `firmware/src` and role/runtime code in `firmware/app`. Protocol field definitions belong in [[UWB+BLE Protocols and Strategies 0.3.12.3]], and current runtime flow belongs in [[Mesh Connected Routing Walkthrough]].

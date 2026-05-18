#internship #imec #architecture #documentation #UWB #BLE

Version: 0.5.45

Previous version: [[UWB+BLE Architecture 0.5.44]] Design rationale: [[UWB+BLE Design Story 0.1]] Component selection: [[Selecting a UWB and BLE Chip]], [[05-03-2026 Internship]]

## Changelog

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
- Added the implemented DWM3000 IRQ pin mapping, documented the range-release courtesy frame for insufficient discovery replies, and clarified that normal-click burst ranging starts only after at least four anchors replied.
- Updated route freshness wording: routes remain usable until replacement or delivery failure proves them stale; channel-9 event timing can still expire independently.
- Updated power budgets to use 75 mA for UWB RX/TX and rewrote the previous BLE-gated comparison around actual design differences instead of an unsupported fixed BLE duration.

### 2026-05-18 - 0.5.42

- Reorganized the document so hardware, click behavior, mesh behavior, operations, and power budgets each live in one nested section.
- Moved related click-flow details together: BLE courtesy, channel-5 wake/discovery, shared burst ranging, no-STS DS-TWR timing, diagnostics, and self-test behavior.
- Rewrote dense mesh-routing and reliability sections in plainer terms while keeping the implemented channel-5/channel-9 behavior and timing values.
- Kept the 200 ms shared responder burst, 6-anchor schedule limit, 75 ms BLE courtesy window, and updated power numbers from version 0.5.41.

Older changes are in [[UWB+BLE Architecture 0.5.44]].

## Reader Map

This document explains the whole clicker-anchor-gateway system: hardware, radio behavior, routing, power, and the tradeoff against the older BLE-gated concept. Exact packet fields are in [[UWB+BLE Protocols and Strategies 0.2.42]]. Runtime flow charts are in [[Firmware State Machines 0.1.37]].

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
| BLE use | Clicker-to-clicker courtesy only. BLE does not wake anchors, authorize ranging, or carry reports. |
| Normal click requirement | At least four eligible anchors must reply to discovery before burst ranging starts. |
| Schedule size | Up to six anchors can be scheduled in one normal-click burst. |
| Ranging burst | One continuous 200 ms channel-5 responder window shared by the selected anchors. |
| UWB PHY | 850 kbps, 1024-symbol preamble, PAC8, STS disabled. Channel 5 is used for wake, discovery, route contact, and ranging; channel 9 uses the same PHY for negotiated payload events. |
| Ranging timing | Fixed 900 uus response/final delay. |
| First-path sensitivity | DWM3000 `IP_CONFIG_LO.IP_NTM=12`, the lower recommended first-path threshold. This may increase outliers, so sample-level outlier rejection remains required. |
| UWB active current model | 75 mA for RX/TX power estimates. Radio reset/configuration is still modeled separately at 20 mA where noted. |
| DWM3000 IRQ | Required. The board overlay maps `irq-gpios` to `P0.02`. |
| Route freshness | Routes do not expire by age alone. They remain usable until replaced, explicitly cleared, or a delivery failure proves the route stale. |
| Channel-9 timing freshness | Event timing is separate from route knowledge. Timing can expire or miss supervision and require channel-5 contact refresh. |

### Roles

| Role | What it owns | What it does not own |
| --- | --- | --- |
| Clicker | Button wake, click/self-test gesture handling, UWB wake claims, discovery, range schedule, DS-TWR initiation, local LED result | Question of the Day, office map, anchor placement, report storage |
| Anchor | Low-duty UWB wake scan, click admission, deterministic discovery reply slot, responder burst, report queue, mesh relay, heartbeat/status, survey samples | Server-side event grouping, final position solve, question schedule |
| Gateway anchor | Same hardware as an anchor plus USB serial, mesh root behavior, command routing, gateway ACKs, time sync, survey orchestration | Distance solving and application storage |

### Traffic Lanes

| Lane | Channel | Used for | Must yield to |
| --- | --- | --- | --- |
| Click/contact lane | UWB channel 5 | Clicker wake claims, discovery, schedule admission, shared ranging burst, route discovery, route contact refresh | Nothing lower priority |
| Payload lane | UWB channel 9 | Reports, heartbeats, command results, gateway ACKs, survey data, routed packets | Active click service, required channel-5 wake scans, route/contact refresh |
| Courtesy side lane | BLE channel 37 | Short clicker-to-clicker priority hint before UWB wake | UWB wake train start |

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
- USB-C for charging, firmware upload, gateway serial, and debug.
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
| P0.02 | DWM3000 IRQ | Required GPIO interrupt input; overlay uses `irq-gpios = <&gpio0 2 GPIO_ACTIVE_HIGH>` |
| P0.03 | DWM3000 CS | Chip select; used with nRF52833 SPIM3 |
| P0.16 | TX | JTAG only |
| P0.31 | DWM3000 RST | Pull low to reset, otherwise high |
| P0.30 | DWM3000 WakeUP pin | |
| P0.11 | DWM3000 SPICLK | nRF52833 SPIM3 SCK, runtime target 32 MHz |
| P0.12 | DWM3000 SPIMISO | nRF52833 SPIM3 MISO |
| P1.00 | DWM3000 SPIMOSI | nRF52833 SPIM3 MOSI |
| P0.04 | RX | JTAG only |
| USBDM / USBDP | USB C Port | Firmware upload, gateway integration, debug output |
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
t=0-75 ms   Clicker listens for UWB quietness and sends/scans BLE courtesy priority
t=75 ms     Clicker sends repeated long-preamble UWB wake claims for 430 ms
t≈scan      Anchors that accept a valid claim create one ownership epoch
t≈wake      Clicker sends discovery and listens for static-slot anchor replies
t≈reply     If at least four anchors replied, clicker sends the selected-anchor range schedule
t≈release   If one to three anchors replied, clicker releases those anchors back to low-duty scan
t≈range     Scheduled anchors run no-STS DS-TWR inside one shared 200 ms responder burst
t<15 s      Click succeeds after 4 unique anchors range, or retries up to 6 attempts
```

Discovery replies prove that anchors are present; they are not distance measurements. A normal click only ranges after at least four eligible anchors reply. The schedule may include up to six anchors. All selected anchors share the same continuous 200 ms channel-5 responder burst while the clicker runs addressed exchanges with a 7 ms minimum exchange stride.

If fewer than four anchors reply, the clicker does not start burst ranging. When at least one anchor did reply, the clicker sends a compact range-release frame to those anchors before retrying or failing. That frame tells the anchors that this attempt did not reach the normal-click minimum and that they can return to low-duty scan instead of waiting for a schedule.

A failing anchor cannot end the shared burst for the others. Wrong-target polls are ignored while the responder burst remains open. Wake-claim timing fields are bounded, and anchors use the same freshness and priority rules throughout the attempt.

### Politeness and BLE Courtesy

BLE courtesy is only a clicker-to-clicker hint during UWB politeness. It does not wake anchors, authorize ranging, carry reports, or replace UWB routing.

The advertisement carries `network_id`, full `clicker_id`, `click_event_id`, `attempt_index`, and `priority_id`. Courtesy stays active for at least 75 ms before the UWB wake train. If a clicker hears a higher-priority peer, it defers that attempt for 75 ms and reruns the gate. After two courtesy deferrals, it proceeds so BLE cannot starve the click path.

Courtesy uses BLE advertising channel 37 only. The clicker disables advertising channels 38 and 39 and asks the SoftDevice Controller to scan only channel 37. If that setup fails, firmware skips BLE courtesy for the attempt and uses UWB politeness plus randomized contention.

The BLE interval uses the fastest legal BLE 5.x non-connectable range on the target: 20.0 to 20.625 ms plus the controller advertising delay. Passive scanning uses 20 ms windows every 25 ms. BLE stops before the UWB wake train begins.

### Ranging Radio Settings

Click ranging uses:

- UWB channel 5.
- 850 kbps.
- Preamble length 1024.
- PAC8.
- STS disabled.
- First-path threshold `IP_CONFIG_LO.IP_NTM=12`.
- Fixed 900 uus response/final delay.
- SPI at 2 MHz for reset/init and 32 MHz at runtime.

Channel 9 is not used for wake, discovery, route refresh, or ranging. It is only used for negotiated mesh payload events after channel-5 contact exists.

### One DS-TWR Exchange

The responder response delay and initiator final delay are both fixed at 900 uus. Schedules using another delay are rejected. The response frame that the clicker receives is 8 bytes longer than the poll received by the anchor, so the shorter path waits instead of making the two reply delays differ.

Scheduled receive legs use a zero preamble-detect timeout. That disables the separate preamble hunt timeout while keeping the receive bounded by the frame-wait timeout. Free-running receive paths, including wake and mesh scans, keep a nonzero preamble timeout so a missed preamble does not consume the whole software window.

| Phase | Description | Time |
| --- | --- | --- |
| 1. POLL TX | SPI write, radio start, POLL on-air at 850 kbps | ~700 us |
| 2. Wait for RESPONSE | RX enable, turnaround, and response frame | ~2200 us |
| 3. Process RESPONSE | Read timestamps and frame, validate, build FINAL | ~250 us |
| 4. FINAL TX | Delayed TX, FINAL on-air, read TX timestamp | ~1700 us |
| 5. Wait for REPORT | RX enable, responder compute, REPORT on-air | ~900 us |
| 6. Read REPORT | Read and parse report frame | ~100 us |
| **Total** | | **~6000 us, about 6 ms** |

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
- Route/contact refresh after stale or missed channel-9 events.

Channel 9 is the payload lane:

- Click reports.
- Heartbeats.
- Command results.
- Gateway ACKs.
- Survey data.
- Routed packets.

Channel 9 is used only after channel-5 contact exists and both peers have usable event timing. Event timing can go stale independently of the route; when that happens, firmware refreshes contact on channel 5 before sending payload data.

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

Event timing is negotiated after channel-5 contact. One side proposes a window, the peer accepts or clips it, and then both sides treat the next hop as channel-9-ready. If events are missed too many times, timing expires and the node returns to channel 5 to refresh contact.

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
| Clicker UWB wake train | 430 ms |
| Anchor wake scan interval/window | 400 ms / 1 ms RX |
| Anchor UWB mesh RX interval/window | 6000 ms / 2 ms RX |
| Gateway UWB mesh RX window/idle | 50 ms / 2 ms idle |
| Gateway ACK timeout | 2 s |
| Max retries | 3 |
| Retry backoff | 100 ms, 250 ms, 500 ms |
| Route freshness | Usable until replaced, explicitly cleared, or a delivery failure proves it stale |
| Duplicate suppression window | 60 s per message identity |
| Gateway command-result timeout | 12 s for one outstanding gateway command |

Routes are considered usable until there is evidence they are wrong. Age alone does not remove a route. A route becomes stale when a newer route epoch replaces it, a command clears it, repeated missing gateway ACKs exhaust the selected upstream path, a downlink command times out, or a UWB send failure proves the selected next hop cannot be used. If another candidate exists, firmware switches routes. If no route remains, the packet returns to the queue and route discovery starts again.

A relay that is already waiting for one gateway-bound confirmation drops new packets that would require forwarding; the previous sender will retry if needed.

Duplicate packet identities stay in a 60 s cache. If a duplicate arrives while the relay is idle and has a route, it can be forwarded again without local double-delivery. If the relay is busy or has no route, it drops the duplicate.

### Click Report Delivery

Anchor-side range results are queued during the responder burst. Each report carries distance, quality, range status, sample data, and diagnostic metadata from both sides of the exchange when available. If samples or diagnostics do not fit in one UWB mesh frame, the anchor queues additional fragments for the same anchor, clicker, and event.

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
| Survey control | Start or abort anchor reachability and anchor-to-anchor distance measurements |

The target returns a `COMMAND_RESULT`. Unsupported commands must return `UNSUPPORTED_COMMAND`.

The gateway keeps one outstanding command at a time. A second USB command is rejected with `COMMAND_BUSY` until the first command resolves by matching result, route failure, or the 12 s command-result timeout.

### Gateway Time Sync

The gateway broadcasts time over UWB mesh once per hour. Anchors accept the broadcast without command results, store an offset from their own non-wrapping uptime to gateway uptime, and forward the broadcast like survey reachability traffic.

Click reports, diagnostic reports, survey samples, status responses, and heartbeats carry the gateway-synchronized timestamp plus the age of the last accepted sync. If an anchor has not synced, it reports local uptime and marks time sync stale.

The sync interval uses:

```text
max_interval_ms = allowed_drift_ms * 1,000,000 / oscillator_error_ppm
```

Using 500 ppm and a 60,000 ms drift target gives 120,000,000 ms, about 33.3 h. Hourly sync keeps drift to about 1.8 s at that bound.

## Survey and Server Handling

### Anchor Self-Distance Survey

The gateway uses surveys to collect anchor-to-anchor distances for off-device placement solving. Firmware only measures and reports distances; solving the geometry is server-side/off-site work.

Survey flow:

1. Gateway creates a `survey_id` and sends a mesh reachability request.
2. Anchors report which other anchors they can reach, with signal and quality metadata.
3. Gateway builds a graph of reachable anchor pairs.
4. Gateway prepares one unordered anchor pair at a time.
5. After both anchors acknowledge readiness, the gateway starts exactly `n` measurements.
6. The pair runs measurements on the dedicated survey worker.
7. Each sample is reported to the gateway as diagnostic survey data.
8. Gateway schedules the next pair or aborts.

During a survey pair run, command and mesh work stay available. A survey abort is recorded immediately and the worker exits at the next sample boundary or bounded responder-listen check.

Survey messages include:

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

1. Group reports by clicker and click event.
2. Wait for a configurable window to allow mesh relay delay.
3. Finalize the event with all received distance measurements.
4. Compute `(x, y)` position from at least three anchor distances, known anchor coordinates, and any other selected inputs.
5. Store the event with the active Question of the Day.

Clickers and normal anchors do not know the Question of the Day. The server owns that schedule. Any display at the clicker station is a separate system.

## Power and Battery

All UWB RX/TX energy below uses 75 mA. This is deliberately conservative for the active radio windows and is separate from the 20 mA reset/configuration row.

### Clicker Budget

Assumptions:

- 50 clicks/day.
- Two quiet 2 ms UWB politeness samples in the normal quiet case.
- 75 ms minimum BLE courtesy scan/advertise during normal-click politeness.
- 430 ms UWB wake-claim train.
- UWB discovery/reply/schedule exchange.
- Up to 6 scheduled anchors.
- One shared 200 ms channel-5 ranging burst.
- Retry and contention backoff are mostly sleep time.

#### Per-Click Energy

| Phase | Duration | Current | Charge (uA·s) |
| --- | --- | --- | --- |
| MCU wake from deep sleep | 1 ms | 5 mA | 5 |
| UWB sampled politeness | 2 × (2.67 ms startup/PLL + 2 ms RX) = 9.34 ms active | 75 mA | 701 |
| BLE courtesy scan/advertise | 75 ms wall time, 20/25 ms passive scan duty plus about three 1 ms TX events | 5 mA active | 315 |
| UWB wake train | 430 ms | 75 mA | 32,250 |
| UWB discovery/reply/schedule | ~80 ms | 75 mA | 6,000 |
| UWB radio wake/reset/configure | 10 ms | 20 mA | 200 |
| UWB shared TWR burst | One 200 ms channel-5 burst covering up to 6 scheduled anchors | 75 mA | 15,000 |
| MCU processing + LED blink | 50 ms | 10 mA | 500 |
| **Total per click** | **~0.85 s wall time in the normal quiet case** | | **54,971 uA·s** |

54,971 uA·s is about 0.0153 mAh per click. A worst-case 500 ms busy politeness wait uses about 2,452 uA·s of UWB receive energy plus about 2,100 uA·s for BLE courtesy. That is still far below a continuous 500 ms UWB listen at 75 mA, which would cost 37,500 uA·s.

#### Daily Energy

| Component | Calculation | mAh/day |
| --- | --- | --- |
| Active clicks | 50 × 0.0153 mAh | 0.76 |
| Deep sleep, MCU plus UWB | 2.86 uA × 24h | 0.069 |
| **Daily total** | | **0.83** |
| With ~3× safety margin | | **2.50** |

| Battery | Capacity | Estimated Life |
| --- | --- | --- |
| LiPo 85 mAh | 85 mAh | ~34 days |

### Anchor Budget

Assumptions:

- Low-duty DWM3000 wake scanning: 400 ms interval, 1 ms RX window, plus startup/PLL overhead.
- Periodic anchor mesh receive: 6000 ms interval, 2 ms RX window, plus startup/PLL overhead.
- 1000 selected ranging events/day.
- 1000 queued click-report mesh deliveries/day.

#### Daily Consumption

| Component | Calculation | mAh/day | % of total |
| --- | --- | --- | --- |
| UWB wake scan baseline | 3.67 ms awake / 403.67 ms period = 0.909%; 785.5 s/day × 75 mA | 16.36 | 65.1% |
| Periodic UWB mesh RX baseline | 4.67 ms awake / 6004.67 ms period = 0.0778%; 67.2 s/day × 75 mA | 1.40 | 5.6% |
| UWB discovery/schedule control | 1000 × (15 ms claim collection + 16 ms discovery listen + 20 ms reply TX + 80 ms schedule RX) × 75 mA | 2.73 | 10.9% |
| UWB responder bursts | 1000 × 200 ms shared responder window × 75 mA | 4.17 | 16.6% |
| UWB mesh report TX | 1000 × (2.67 ms startup/PLL + 20 ms TX timeout) × 75 mA | 0.47 | 1.9% |
| **Daily total** | Estimated radio budget before margin | **25.13** | **100%** |
| **With 1.5× safety margin** | 25.13 mAh/day × 1.5 | **37.70** | |

A route miss that adds one extra `ROUTE_REQ` transmit costs another 0.47 mAh per 1000 reports. A day where every report starts with route discovery is about 25.60 mAh before margin, or 38.40 mAh/day with the same margin.

#### Battery Life

Assumptions:

- Load: 37.70 mAh/day after 1.5× safety margin.
- Battery: 18650 Li-Ion, 3000 mAh nominal.
- Efficiency: 0.85 usable capacity.

| Number of Batteries | Total Usable Capacity | Est. Days | Est. Months | Est. Years |
| --- | --- | --- | --- | --- |
| 1 × 18650 | 2,550 mAh | 67.6 | 2.2 | 0.19 |
| 2 × 18650 | 5,100 mAh | 135.3 | 4.4 | 0.37 |
| 3 × 18650 | 7,650 mAh | 202.9 | 6.7 | 0.56 |

Low-duty UWB wake scanning is the dominant anchor cost. Normal idle scan plus periodic UWB mesh RX stays at about 0.987% DWM3000 awake time, just under the intended ~1% periodic idle target. Active route/report traffic adds about 7.37 mAh/day under the 1000 selected-events/day assumption.

### Previous BLE-Gated Design Comparison

The older BLE-gated concept is useful as a contrast, but it should not be summarized by inventing one fixed BLE exchange duration. The important difference is which radio owns admission and how long a selected anchor can be forced to keep UWB awake.

| Question | Previous BLE-gated concept | Current UWB-gated plus BLE courtesy |
| --- | --- | --- |
| Who wakes anchors? | BLE request/ready behavior decided when UWB should start. | UWB wake claims are the admission authority. BLE is only a courtesy hint between clickers. |
| What proves the selected clicker? | BLE and later UWB state both had to agree. | One UWB identity chain covers wake, discovery, schedule, range, and report identity. |
| What happens when too few anchors reply? | The old design did not have the current range-release courtesy frame. | The clicker releases any anchors that did reply, then retries or fails. |
| How long can a selected anchor sit in UWB? | The risk is any long first-poll or post-range responder wait. Every extra 1 s of UWB awake at 75 mA costs 0.0208 mAh per selected event, or 20.8 mAh/day at 1000 events. | The normal selected-anchor responder window is bounded to one 200 ms burst, which costs 0.00417 mAh per selected event, or 4.17 mAh/day at 1000 events. |
| How are reports delivered? | Operational routing/reporting needed a separate design path. | The same UWB system owns ranging reports, heartbeats, commands, ACKs, and surveys. |

The current design moves complexity into deterministic UWB admission and routing so the anchor's high-current UWB windows are short and explainable. BLE remains useful, but only as collision-reducing courtesy before UWB starts.

### Reliability Notes

The current wake path is designed for timing coverage first. The 430 ms wake-claim train is longer than the 403.67 ms anchor scan period, so every anchor should get at least one chance to hear a claim if RF conditions are good and no higher-priority work blocks the scan.

BLE courtesy reduces simultaneous-click collisions before UWB starts. The useful courtesy metric is "lower hears higher" because only the lower-priority clicker should defer.

| Courtesy window | Lower hears higher | At least one direction hears | Mutual detection |
| ---: | ---: | ---: | ---: |
| 10 ms | 27.4% | 47.3% | 7.5% |
| 15 ms | 41.2% | 65.4% | 16.9% |
| 20 ms | 54.8% | 79.6% | 29.9% |
| 25 ms | 69.0% | 90.3% | 47.6% |
| 27 ms | 74.9% | 93.7% | 56.0% |
| 30 ms | 83.8% | 97.3% | 70.0% |
| 40 ms | 88.5% | 98.7% | 78.3% |
| 50 ms | 91.9% | 99.3% | 84.5% |
| 60 ms | 94.8% | 99.7% | 89.9% |
| **75 ms** | **96.7%** | **99.9%** | **93.6%** |
| 90 ms | 98.1% | 100.0% | 96.3% |
| 100 ms | 98.7% | 100.0% | 97.4% |
| 125 ms | 99.5% | 100.0% | 99.0% |
| 150 ms | 99.8% | 100.0% | 99.6% |
| 200 ms | 100.0% | 100.0% | 99.9% |

The implemented value is 75 ms. It captures most of the detection benefit before the curve flattens, while adding less latency than 100-200 ms options. Courtesy is advisory, so misses fall back to UWB contention rather than blocking the click.

Normal click acceptance requires four unique anchors. With more anchors in range, the click can tolerate individual anchor misses. With fewer than four eligible discovery replies, a normal click does not range; if any anchors replied, the clicker sends range release before retrying or failing according to the click budget.

### Tradeoffs

| Architecture | Strengths | Costs and risks |
| --- | --- | --- |
| Previous BLE-gated | Low-cost BLE scanning, mature BLE receiver behavior, BLE request advertisement covers the 1 s scan phase, DWM can stay asleep until BLE request | High anchor event energy from long UWB responder deadlines, BLE discovery is not the ranging identity authority, weak coordination with hidden/multiple clickers, operational mesh/routing needs another transport path, old responder can burn 120 s of UWB listen time if no addressed poll arrives |
| Current UWB-gated plus BLE courtesy | One protocol family owns wake, discovery, ranging, reports, and mesh; deterministic wake-train overlap; CRC and full identity checks before state creation; selected-clicker-only ranging; bounded scheduled windows; idle UWB duty stays near 1%; BLE courtesy reduces simultaneous-click collisions before UWB TX | Requires reliable DWM3000 IRQ wiring, UWB wake-scan link budget must be validated on hardware, idle UWB scan dominates battery budget, BLE courtesy depends on nRF scan-channel-map support and is advisory only, active mesh traffic must be tuned against click priority |

## Firmware Boundaries

Both clicker and anchor firmware run on the same ANNA-B402-00B plus DWM3000 hardware. The configured role decides whether the device behaves as a clicker, anchor, or gateway. Firmware uses Zephyr through nRF Connect SDK.

Keep hardware-independent behavior in `firmware/src` and role/runtime code in `firmware/app`. Protocol field definitions belong in [[UWB+BLE Protocols and Strategies 0.2.42]], and detailed runtime diagrams belong in [[Firmware State Machines 0.1.37]].

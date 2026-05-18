#internship #imec #architecture #documentation #UWB #BLE

Version: 0.5.41

Previous version: [[UWB+BLE Architecture 0.5.40]] Design rationale: [[UWB+BLE Design Story 0.1]] Component selection: [[Selecting a UWB and BLE Chip]], [[05-03-2026 Internship]]

## Changelog

### 2026-05-18 - 0.5.41

- Align the architecture with the implemented channel ownership model: channel 5 is wake/contact/ranging, and channel 9 is negotiated mesh payload only.
- Update normal-click ranging to a shared 200 ms channel-5 burst with up to six scheduled anchors and no-STS DS-TWR.
- Document the BLE-inspired channel-9 event negotiation, preemption, fallback, and report delivery behavior.
- Refresh clicker and anchor power calculations for the 200 ms shared burst and six-anchor maximum.

Older changes are in [[UWB+BLE Architecture 0.5.40]].

## System Architecture

### Hardware Overview
| [[Physical Domain \| System]] / [[Constraints]] | Component Name | Additional Info |
| ----------------------------------------------- | --------------------- | --------------- |
| DP-0.2.1.1 | DWM3000 | UWB Chip |
| DP-02 / DP-03 | TL3301AF260QG | Button |
| DP-0.2 / DP-05 | ANNA-B402 | MCU w/ BLE |
| DP-04 | BQ24090 | Charging Chip |
| DP-04 / DP-05 | USB4110-GF-A | USB C Port |
| DP-04 / CON-10 | Renata ICP390831PR | Battery |
| DP-02 / CON-10 | EPSON FC-12M | Crystal |
| DP-06 | 2x APF3236LSEEZGKQBKC | RGB Status LEDs |

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
| USB4110-GF-A | USB-C (USB TYPE-C) USB 2.0 Receptacle Connector 24 (16+8 dummy) Posities Opbouwmontage, rechte hoek | J4 | 1 | 56 | 109246 | Global Connector Technology | USB4110-GF-A | DigiKey | 2073-USB4110-GF-A-1-ND | 0.82012 | 45.93 |
| APF3236LSEEZGKQBKC | LED RGB CLEAR 6SMD | LED4, LED5 | 2 | 112 | 20537 | Kingbright | APF3236LSEEZGKQBKC | DigiKey | 754-1969-1-ND | 0.40297 | 45.13 |
| DWM3000TR13 | DWM3000 module (factory default channel 9, 7.9872 GHz; firmware uses channel 5, 6.5 GHz at 850 kbps) SPI 2.5-3.6V 8 GPIO's | MD1 | 1 | 56 | 5596 | Qorvo | DWM3000TR13 | Mouser | 772-DWM3000TR13 | 17.24 | 965.29 |
| IRLML6401TRPBF | MOSFET P-CH 12V 4.3A SOT-23 | Q1, Q2 | 2 | 112 | 3943 | Infineon | IRLML6401TRPBF | DigiKey | IRLML6401PBFDKR-ND | 0.15052 | 16.86 |
| MCS0402MD1000DE000 | RES SMD 1.2K OHM 1% 1/10W 0402 | R1, R6, R7, R11, R17, R22, R23 | 7 | 392 | 17935 | Vishay | MCS0402MD1000DE000 | DigiKey | MCS0402-100-MDCT-ND | 0.07325 | 28.71 |
| AC0402JR-0727RL | RES 27Ω ±5% 0.063W 0402 | R2, R4 | 2 | 112 | 17885 | Yageo Group | AC0402JR-0727RL | Avnet | 58AK8894 | 0.00342 | 0.38248 |
| MCS0402PD1001DE500 | RES SMD 1.2K OHM 1% 1/10W 0402 | R3, R8, R9, R14, R24 | 5 | 280 | 5198 | Vishay | MCS0402PD1001DE500 | Mouser | 594-MCS0402PD1001DP5 | 0.10928 | 30.6 |
| CRGCQ0402J10K | CRGCQ 0402 10K 5% | R5 | 1 | 56 | 444124 | TE Connectivity | CRGCQ0402J10K | DigiKey | A130054CT-ND | 0.01144 | 0.64066 |
| AF0402FR-07470RL | Anti-Sulfurated Chip Resistors Automotive Grade 470Ω 1% 100ppm/°C 0402 | R10, R19 | 2 | 112 | 104788 | Yageo Group | AF0402FR-07470RL | Mouser | 603-AF0402FR-07470RL | 0.00598 | 0.66934 |
| CRG0402F5K1 | | R12, R13 | 2 | 112 | 38339 | TE Connectivity | CRG0402F5K1 | DigiKey | 1712-CRG0402F5K1CT-ND | 0.01153 | 1.29 |
| CRGCQ0402F6K8 | CRGCQ 0402 6K8 1% | R15 | 1 | 56 | 17842 | TE Connectivity | CRGCQ0402F6K8 | Newark | CRGCQ0402F6K8 | 0.00171 | 0.09562 |
| CRCW0402100KJNTD | | R18 | 1 | 56 | 25156 | Vishay | CRCW0402100KJNTD | Mouser | 71-CRCW0402J-100K | 0.0333 | 1.86 |
| CRGCQ0603J680K | CRGCQ0603 680KΩ 5% ±100ppm/°C 75V | R20, R21 | 2 | 112 | 4509 | TE Connectivity | CRGCQ0603J680K | DigiKey | A130108CT-ND | 0.00973 | 1.09 |
| TL3301AF260QG | Tactile Switch, SPST-NO, 50 mA, 12 V, -20 to 70 degC, 4-Pin SMD, RoHS, Tape and Reel | SW1 | 1 | 56 | 34821 | E-Switch | TL3301AF260QG | DigiKey | EG2527CT-ND | 0.23632 | 13.23 |
| 5003 | Test Point, Orange, Height 4.6 mm, Tail Length 3 mm, 1-Pin THD, RoHS | TP_VCC, TP_VUSB | 2 | 112 | 59826 | Keystone | 5003 | DigiKey | 36-5003-ND | 0.1642 | 18.39 |
| BQ24090DGQR | 1 A, Single Input, Single Cell Li-Ion Battery Charger with 10 K ohm NTC, 4.5 V, -40 to 150 degC, 10-pin MSOP-PowerPAD (DGQ), Green (RoHS & no Sb/Br) | U2 | 1 | 56 | 4558 | Texas Instruments | BQ24090DGQR | Mouser | 595-BQ24090DGQR | 0.70349 | 39.4 |
| AP7354-30W5-7 | IC REG LINEAR 3V 150MA SOT-25 | U3 | 1 | 56 | 2730 | Diodes Inc. | AP7354-30W5-7 | Newark | AP7354-30W5-7 | 0.1938 | 10.85 |
| ANNA-B402-OPENCPU | u-blox ANNA-B402 Bluetooth 5 LE module | U4 | 1 | 56 | 10464 | u-blox | ANNA-B402-00B | Mouser | 377-ANNA-B402-00B | 5.04 | 282.08 |
| TPD4E05U06QDQARQ1 | TVS DIODE 5.5V 14V 10USON | U5 | 1 | 56 | 104604 | Texas Instruments | TPD4E05U06QDQARQ1 | DigiKey | 296-40696-1-ND | 0.61129 | 34.23 |
| FC-12M_32.7680KA-A5 | | Y1 | 1 | 56 | 7793 | Epson | FC-12M 32.7680KA-A5 | DigiKey | 114-FC-12M32.7680KA-A5CT-ND | 0.44361 | 24.84 |

### Device Roles

#### Clicker (Tag)
- Carried by office workers, picked up from a station each morning.
- Single button input.
- Sleeps until button press (GPIO interrupt). No background activity.
- On press, the clicker checks whether the air is quiet, shares a short BLE courtesy hint with other clickers, sends a timed UWB wake train, discovers anchors, receives a burst range schedule, runs round-robin DS-TWR inside one shared responder window, and returns to sleep.
- After the click or self-test active window, the clicker stops UWB activity and returns the DWM3000 to retained sleep before returning to idle.
- Self-test mode is entered with a long press followed by a short press.
- In self-test mode, the clicker verifies local modules and sends a diagnostic "dud" ranging request. This request must not be counted as a user click.
- Identity: Permanent 64-bit device ID
- Does not know the current Question of the Day or office layout.

#### Anchor (Fixed Node)
- Mounted at known positions on the ceiling of the office.
- Runs low-duty long-preamble UWB wake scans for clicker wake claims.
- The DWM3000 is not an always-on receiver. Firmware parks the DWM3000 wake pin inactive at boot and wakes/configures the radio only for bounded UWB wake scans, click epochs, mesh windows, or survey windows.
- On clicker detection: accepts only a CRC-valid claim, locks one ownership epoch, arbitrates competing clickers, sends a discovery-slot reply, validates the range schedule, opens the shared responder burst window, queues any range result for mesh delivery, then puts the DWM3000 into retained sleep before draining queued reports.
- Relays data toward gateway via UWB mesh.
- Participates in routing protocol for self-organizing mesh.
- Reports battery level, route state, and diagnostics via periodic heartbeat requested by gateway.
- Responds to gateway commands, including status requests, route updates, diagnostics, and anchor-to-anchor distance survey commands.

#### Gateway Anchor
- Physically identical to other anchors, but connected to server via USB-C.
- Acts as the mesh root.
- Forwards all received data (own + relayed) to server over serial.
- May or may not perform ranging sequences itself. Depending on processing headroom.
- Issues mesh commands and receives command results plus end-to-end gateway acknowledgements.
- Schedules anchor-to-anchor distance surveys after anchors report which other anchors they can reach.

---

## Ranging Protocol (Per Click Event)
The clicker discovers anchors dynamically on each press. Moving, adding, or removing anchors requires no clicker configuration change.

```
t=0ms     Button press wakes clicker
t=0-75ms+ Clicker listens for UWB quietness and advertises/scans BLE courtesy priority
t=75ms    Clicker sends repeated long-preamble UWB wake claims for 430 ms
t≈scan    Anchors that accept a CRC-valid local claim create one ownership epoch
t≈wake    Clicker sends discovery and listens for static-slot anchor replies
t≈reply   Clicker sends the selected-anchor range schedule
t≈range   Clicker runs round-robin no-STS DS-TWR inside one shared 200 ms responder burst
t=<15s    Click succeeds after 4 unique anchors range, or retries up to 6 wake attempts
```

Discovery replies are presence-only and never count as range measurements. A normal click only starts ranging after at least four eligible anchors reply. The schedule may include up to six anchors. All selected anchors share the same continuous 200 ms channel-5 responder burst while the clicker runs addressed exchanges with a 7 ms minimum exchange stride. A failing anchor cannot end the shared burst for the others; wrong-target polls are ignored while the responder window remains active. Wake-claim timing fields are bounded, and anchors use the same clicker/event freshness and priority rules throughout one attempt. The exact frame names, byte layouts, and validation rules are in [[UWB+BLE Protocols and Strategies 0.2.39]]. Runtime flow charts are in [[Firmware State Machines 0.1.34]].

### BLE Courtesy Side Channel
BLE courtesy is a clicker-to-clicker hint that runs only during UWB politeness. The advertisement carries `network_id`, full `clicker_id`, `click_event_id`, `attempt_index`, and `priority_id`. It does not wake anchors and does not authorize ranging. The current implementation keeps courtesy active for at least 75 ms before UWB contention/wake. If a clicker hears a higher-precedence peer, it defers the same UWB attempt for 75 ms and reruns the gate; after two courtesy defers it proceeds so BLE cannot starve the UWB path.

The courtesy path deliberately uses only BLE advertising channel 37. The clicker disables advertising channels 38 and 39 and asks the SoftDevice Controller to scan only channel 37. This avoids the short-window failure mode where a scanner rotates to the desired advertising channel only after the politeness phase has already ended. If the controller scan-channel-map command fails, the firmware skips BLE courtesy for that attempt and uses UWB politeness plus randomized contention only.

The BLE interval respects the BLE 5.x non-connectable advertising limit on the nRF52833 target: 20.0 to 20.625 ms plus the controller advertising delay. Passive scanning uses 20 ms windows every 25 ms. The BLE radio is stopped before the UWB wake-claim train begins.

### DWM3000 Ranging Budget

Double-sided two-way ranging is the UWB distance exchange between one clicker and one selected anchor. The firmware uses DWM3000 channel 5, 850 kbps, preamble length 64, PAC8, and STS disabled for click ranging. Channel 9 is reserved for negotiated mesh payload events after channel-5 contact exists. SPI runs at 2 MHz during reset/init and 32 MHz at runtime. Exact ranging frame fields and sizes are defined in [[UWB+BLE Protocols and Strategies 0.2.39]].

The ranging reply timing policy is equality first, shortest possible common delay second. The responder response delay and initiator final delay are both programmed to the same fixed 900 uus value; schedules with any other reply delay are rejected. The initiator's received response is 8 B longer than the responder's received poll. At 850 kbps, that extra receive path is `ceil(8 B * 8 bits * 1e6 / 850e3) = 76 us`, so the shorter poll path intentionally waits instead of making the two reply times differ.

Scheduled receive legs use a zero preamble-detect timeout. On the DWM3000 this disables the separate preamble hunt timeout; the receive remains bounded by the configured frame-wait timeout because the expected responder, final, or report frame has a scheduled arrival window. Immediate and free-running receive paths, including wake and mesh scans, keep a nonzero preamble timeout so missed preambles do not burn the whole software window.

| Phase | Description | Time |
| --- | --- | --- |
| 1. POLL TX | SPI write + radio start + POLL on-air at 850 kbps | ~700 µs |
| 2. Wait for RESPONSE | 690 µs RX enable starts from POLL TX; turnaround + RESP on-air overlap with RX enable | ~2200 µs |
| 3. Process RESPONSE | Read timestamps (RX + poll TX) + read frame + validate + build FINAL | ~250 µs |
| 4. FINAL TX (delayed) | 900 uus delayed TX + FINAL on-air + read TX timestamp | ~1700 µs |
| 5. Wait for REPORT | RX enable + responder compute + REPORT on-air | ~900 µs |
| 6. Read REPORT | Read frame + parse | ~100 µs |
| **Total** | | **~6000 µs ≈ 6 ms** |

### DWM3000 Runtime Policy

- Reset and soft-reset SPI paths run at 2 MHz because DW3000 reset-clock access must stay at or below 7 MHz.
- Runtime SPI targets 32 MHz on nRF52833 SPIM3 after the radio reaches IDLE.
- The DWM3000 IRQ pin is required. Firmware waits on GPIO IRQ events for radio completion; there is no bounded SPI polling runtime mode. The current source overlay maps `irq-gpios` to `P0.02`.
- The default TX config writes `PGdly=0x34`, `TX_POWER=0xFDFDFDFD`, and `PGcount=0x0`. `TX_POWER` is a raw DW3000 register value; conducted output still needs hardware calibration.

---

## Status Indication and Clicker Self-Test

Status indication is part of the device behavior, not only a debug aid. A user must be able to test a clicker quickly without a computer, and the test result must not pollute normal click data.

### Button Interpretation

The clicker uses one button:

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

### Self-Test Sequence

Self-test performs the following checks in order:

1. MCU woke correctly from button interrupt.
2. RGB LEDs can be driven through the status indication module.
3. Battery voltage and charger status pins can be read.
4. DWM3000 can wake, initialize, and send a diagnostic UWB wake/discovery attempt.
5. At least one anchor replies with a diagnostic UWB discovery reply.
6. DWM3000 can perform at least one diagnostic UWB ranging exchange with a responding anchor.
7. The clicker returns the DWM3000 and MCU to the expected low-power idle state.

Self-test traffic uses diagnostic flags and never sets the normal click flag. Diagnostic ranging requests are "dud" events: anchors may forward them for maintenance logs, but the server must not include them in behavioral click counts.

### Status LED Standard

The two RGB LEDs show identical patterns unless a later enclosure design explicitly separates "system" and "radio" indicators.

| Pattern | Meaning |
| --- | --- |
| Blue pulse | Self-test armed; waiting for confirming short press |
| Blue chase | Self-test running |
| Green solid for 2 seconds | Self-test passed / click accepted |
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

Click failure codes (shown after a normal click that did not produce enough ranges):

| Red blinks | Failure |
| --- | --- |
| 1 | No anchor heard during UWB discovery |
| 2 | Insufficient successful ranges (fewer than required anchors) |

---

## UWB Mesh Routing
Anchors form a self-organizing UWB mesh using simple reactive routing. Each packet has a final destination, but each sender only chooses the next hop. v1 prioritizes explicit acknowledgements, click responsiveness, and debuggable delivery over throughput.

### UWB PHY and TX Power Policy

Clicker wake/discovery and mesh forwarding use DWM3000 UWB frames on the configured network ID. Wake/discovery uses the long-preamble PHY on channel 5 so sleeping anchors can catch claims during short scan windows. Ranging and mesh contact/refresh also use channel 5. Once peers have channel-5 contact and have negotiated timing, mesh data packets, gateway ACKs, commands, and reports are sent as UWB mesh frames to the selected next hop during bounded channel-9 events.

The UWB mesh frame carries `network_id`, previous-hop ID, next-hop ID, a shared protocol packet, and a frame CRC. Anchors and the gateway schedule periodic UWB mesh receive windows; anchors skip mesh RX when a click epoch or tracked mesh TX is active.

Anchors keep low-duty UWB wake scanning near the intended ~1% DWM3000 awake-time target. The current firmware uses a 400 ms scan interval and a 1 ms RX window because the measured Zephyr/DWM3000 wake path includes startup/PLL overhead before RX. Clicker wake trains last 430 ms so every anchor scan window overlaps a valid claim. After an anchor accepts a selected claim, it can return the DWM3000 to retained sleep until the selected discovery time and again between its scheduled poll slots. These retained-sleep intervals keep the ownership epoch continuous but are excluded from awake-time diagnostics.

The periodic idle duty guard covers wake scanning plus periodic anchor UWB mesh receive. Current anchor mesh receive is 2 ms every 6000 ms. The theoretical power model below also includes event-driven route/report traffic under the normalized workload. Runtime scan-duty commands are bounded by duty-cycle and wake-overlap guards: accepted scan intervals must still leave each 430 ms clicker wake train overlapping an anchor scan window. The theoretical tuning order is mesh receive interval/window first, then scan interval and clicker wake train together so wake overlap remains guaranteed.

UWB mesh receive is intentionally short on anchors so click wake claims can preempt relay work. The gateway uses longer UWB mesh RX windows because it is USB-powered.

### Mesh Packet Standard

Architecture treats mesh packets as reliable routed messages. Anchors forward toward the selected next hop, the gateway is the normal root, and important gateway-bound data waits for end-to-end confirmation before being considered delivered. The exact packet envelope, message IDs, and Type-Length-Value payload fields are defined in [[UWB+BLE Protocols and Strategies 0.2.39]].

### Channel-9 Mesh Events

Channel 9 is a payload lane, not a discovery lane. A node may only transmit payload traffic on channel 9 after channel-5 contact has established a route candidate and both peers have valid event timing. The timing state is per next hop: event interval, event window, next event time, event counter, retune guard, clock-skew guard, maximum missed events, and supervision timeout.

Event timing is negotiated with mesh event control packets after channel-5 route/contact refresh. A proposal carries the first event time, cadence, guard, window, channel, and supervision timeout. The peer accepts or clips the timing before either side treats the next hop as channel-9-ready. If an event is missed too many times, timing expires and the route falls back to channel-5 contact refresh.

Before entering channel 9, firmware checks whether the event would overlap an active click epoch, discovery work, a channel-5 responder burst, or a required quick channel-5 wake scan. If it would, the event is deferred, clipped, or skipped. Channel-5 wake/contact work preempts channel-9 mesh, and the heartbeat/status path exposes counters for channel switches, PLL-ready failures, late channel-5 returns, mesh deferrals, missed channel-9 events, channel-5 preemptions, and channel-9 report latency.

### Gateway Delivery Confirmation

Every gateway-bound packet that matters sets:

- `GATEWAY_ACK_REQUIRED`: the gateway must send an end-to-end gateway ACK back to the original sender.

Reports, survey results, heartbeats, and command results require gateway ACKs. The original sender treats gateway-bound traffic as delivered only after the end-to-end gateway ACK returns. Next-hop transfer is a bounded UWB mesh TX/RX attempt, normally during negotiated channel-9 mesh events after channel-5 contact exists. Gateway-originated commands complete when the target returns `COMMAND_RESULT`, or fail locally after the 12 s command-result timeout.

Default retry behavior:

| Parameter | Value |
| --- | --- |
| Clicker UWB wake train | 430 ms |
| Anchor wake scan interval/window | 400 ms / 1 ms RX |
| Anchor UWB mesh RX interval/window | 6000 ms / 2 ms RX |
| Gateway UWB mesh RX window/idle | 50 ms / 2 ms idle |
| Gateway ACK timeout | 2 s |
| Max retries | 3 |
| Retry backoff | 100 ms, 250 ms, 500 ms |
| Route freshness window | 30 s since route discovery, successful UWB send, or gateway ACK |
| Duplicate suppression window | 60 s per message identity |
| Gateway command-result timeout | 12 s for one outstanding gateway-originated command; sized for anchor mesh RX cadence, gateway ACK timeout, and margin |

### Link Quality Weighting

The `link_quality` metric is the UWB quality score for one mesh hop:

| RSSI | link_quality |
| --- | --- |
| ≤ **-100 dBm** | 1 (floor) |
| -99 .. **-41 dBm** | `RSSI + 100` (linear, range 1-59) |
| ≥ **-40 dBm** | 100 (ceiling) |

`link_quality=0` is reserved for unknown/not set, and values above 100 are rejected.

Route selection uses a single cost value. For gateway-bound paths, `hop_count` is the reported distance from the next hop back to the gateway. A gateway route reply starts at `hop_count=0`, and each relay forwards it one hop higher.

```
effective_cost = hop_count * 100 + (100 - link_quality)
```

Lower cost wins. Because the quality penalty is 0-99 and each extra reported hop adds 100, hop count is primary and quality only ranks candidates inside the same hop-count band.

Examples:

| Candidate | Cost | Result |
| --- | --- | --- |
| Direct gateway route, weak UWB quality: `hop_count=0`, `link_quality=1` | `0 * 100 + 99 = 99` | Still beats a relay path |
| Relay path that reports `hop_count=1`, perfect quality | `1 * 100 + 0 = 100` | Loses to the direct route above |
| Two candidates with `hop_count=1`, qualities 80 and 30 | Costs 120 and 170 | Quality 80 wins |

The same score is used upstream and downlink. If two routes have the same cost, the firmware chooses better link quality, then lower hop count, then newer observation, then lower next-hop ID.

**Quality propagation through the mesh** uses the weakest link in the path. A route request or reply starts with quality 100. Each receiver compares that reported path quality with the UWB quality of the local hop that just delivered the frame:

```
path_quality = min(reported_path_quality, local_link_quality)
```

The receiver stores and forwards that path quality. If gateway-to-A is 72 and A-to-B is 45, B stores 45. Another path with the same hop count and quality 60 wins because it has lower cost.

### Route Discovery and State

Routing is on-demand:

1. A sender with no usable route sends a UWB mesh `ROUTE_REQ` with `INITIATOR_ID` set to itself and `RESPONDER_ID` set to the target.
2. Each receiver stores a reverse breadcrumb to the initiator through the previous hop.
3. Relays rebroadcast the request with lower TTL, higher hop count, and weakest-link quality.
4. The target sends `ROUTE_REPLY` back through the reverse path.
5. Each receiver of the reply installs a route to the target through the previous hop.
6. The original sender transmits the waiting data packet inside a UWB mesh frame to the selected next hop.

For anchor reports and command results, the target is the configured gateway. For gateway-originated commands, the target is the anchor ID. Upstream and downlink candidates older than 30 s are expired before routing decisions. v1 still has one configured gateway root for normal operation.

### Route Failure Behavior

Gateway ACK packets update pending transmissions before normal packet forwarding decisions are made.

When an anchor sends a gateway-bound packet, the successful UWB mesh transmit refreshes the selected route age and the pending transmission waits for the end-to-end gateway ACK from the gateway.

**Gateway ACK failure** — if the gateway ACK deadline (2 s) expires, the anchor marks the selected route attempt as failed and chooses one of three actions:

| Condition | Action |
| --- | --- |
| failure_count < 3 | Retry the same candidate |
| failure_count ≥ 3, alternative exists | Invalidate the candidate, switch to the next best route |
| failure_count ≥ 3, no alternative | Invalidate, stop the pending transmission, report route discovery needed |

If an alternative candidate is selected, the pending transmission is retransmitted through the new next hop with `failure_count` reset to 0. If no alternative exists and the original packet was a queued click report, it goes back into the report queue and triggers a new `ROUTE_REQ`.

The report may have reached the gateway, but the return ACK was lost, so the sender retransmits rather than dropping the report.

**Success** — a successful UWB mesh transmit refreshes `last_seen_ms`; a matching gateway ACK also resets `failure_count` to 0. This keeps usable routes fresh without hiding repeated missing gateway confirmations.

Each node can track exactly one gateway-bound transmission waiting for confirmation. When a relay is busy with one of these transmissions, it drops other packets that would need forwarding or an immediate local response. The previous sender relies on its UWB mesh transmit result and higher-level gateway confirmation to repair the path if needed.

#### Duplicate Retry and Busy Relays

When a relay receives a directed-unicast retry whose identity (`msg_type`, `src_id`, `dst_id`, `session_id`, `seq`) already exists in its duplicate cache, it recognizes the retry as a repair attempt rather than a new packet:

- **Relay idle, route available** — the relay re-forwards the packet. The duplicate cache prevents double delivery to the target.
- **Relay busy (tx in flight) or no route** — the relay drops the packet.

Local or broadcast duplicates are not delivered twice.

#### Downlink Delivery and Gateway Command Serialization

Commands flow from the gateway toward a target anchor through a flat downlink directory learned from `ROUTE_REPLY` packets (`target_id -> next_hop_id`). The gateway does not maintain a full topology map and does not flood operational commands.

If no downlink entry exists, the gateway emits `ROUTE_REQ` in UWB mesh frames and keeps the USB command pending. When the target's `ROUTE_REPLY` returns, the command is sent over the selected UWB next hop. A command with a cached route relies on UWB mesh transmit status for next-hop transport and waits for `COMMAND_RESULT`; the 12 s command-result timeout remains the application-level limit.

The gateway serializes commands — it tracks exactly one outstanding command at a time. A second USB command is immediately rejected with `COMMAND_BUSY` until the first command is resolved. Resolution occurs when:

- A matching `COMMAND_RESULT` arrives from the target anchor,
- The cached next-hop entry is invalidated and discovery cannot complete before timeout, or
- The 12 s `GATEWAY_COMMAND_RESULT_TIMEOUT_MS` elapses.

A command result matches when its `src_id` equals the command's target anchor, `dst_id` equals the gateway, and both `session_id` and `seq` match the original command.

### Data Forwarding (every click)
Anchor-side UWB results are queued during the responder window. Each click distance report carries distance, quality, range status, sample data, and diagnostic metadata from both sides of the exchange when available. If all samples or diagnostics do not fit one UWB mesh frame, the remaining chunks are queued as additional report packets for the same anchor, clicker, and event sequence. After the DWM3000 returns to retained sleep, the anchor drains reports one at a time through the UWB mesh. Reports prefer negotiated channel-9 events. If channel-9 timing is missing or stale, the sender refreshes contact on channel 5 before payload delivery. Each report waits for a gateway ACK. If all retries exhaust and the upstream route is lost, the report returns to the queue and starts reactive route discovery.

### Mesh Relay Hop Latency

Each operational relay hop uses the selected UWB mesh next hop. Route discovery happens before the data packet is sent:

```
Route miss:
T=0ms      Sender emits a route request
T≈rx       Relays forward ROUTE_REQ; target returns ROUTE_REPLY
T≈reply    Sender receives ROUTE_REPLY and uses the selected next hop

Data hop:
T=0ms      Sender transmits a mesh data frame
T≈rx       Receiver decodes frame
T≈rx       Receiver forwards data to its next hop if needed
```

The first packet after a route miss pays the route-discovery cost. Once a route exists, each hop is bounded mainly by the UWB mesh TX/RX window cadence.

### Self-Healing
Route entries expire after 30 s without refresh. Repeated missing gateway ACKs can invalidate a selected upstream route and start new discovery. Anchors requeue undelivered reports and start a new `ROUTE_REQ`; the gateway keeps a pending command while discovery runs and reports `COMMAND_TIMEOUT` only if no matching command result arrives within 12 s. Duplicate identities expire after 60 s.

### Click Priority Over Mesh Work

Anchor click handling has priority over relay traffic. During wake scanning, valid wake claims are decoded before mesh frames. The anchor cancels active mesh forwarding and clears pending mesh receive work only after the claim is accepted for the configured network, channels, flags, and ownership epoch. Valid but rejected or foreign claims do not clear an existing epoch and do not interrupt mesh work. Already-built local click reports stay queued for later delivery. This prevents relay work from delaying discovery-slot replies or the selected clicker's scheduled UWB responder window while keeping unrelated UWB traffic from disrupting mesh delivery.

---

## Gateway Commands

The gateway issues commands through the same mesh envelope and downlink directory described above. The protocol reserves space for future commands, but v1 needs these command classes:

| Command class | Purpose |
| --- | --- |
| Ping/status | Verify an anchor is alive and report firmware, uptime, battery, route, and radio status |
| LED/status pattern | Trigger a visible indication on a specific anchor for installation/testing |
| Route management | Set, clear, or request route information |
| Heartbeat control | Start/stop periodic anchor health reports |
| Survey control | Start/abort anchor reachability and anchor-to-anchor distance measurements |

The target executes or rejects the command and returns a `COMMAND_RESULT`; that result uses the standard gateway ACK delivery path.

For status, an anchor reports role, uptime, gateway-synchronized time, sync age, UWB health, and a compact route summary. Detailed status fields and values are in [[UWB+BLE Protocols and Strategies 0.2.39]].

Heartbeat commands start or stop periodic anchor health reports. The default interval is 60 s, and accepted intervals range from 5 s to 1 h. Heartbeats wait behind click handling and other tracked mesh transmissions. The heartbeat packet shape is defined in [[UWB+BLE Protocols and Strategies 0.2.39]].

Unsupported commands must return `UNSUPPORTED_COMMAND`, not be silently ignored.

### Gateway Time Synchronization

The gateway periodically broadcasts time over UWB mesh. Anchors accept this broadcast without returning command results, store a local offset from their own non-wrapping uptime to gateway uptime, and forward the broadcast through the mesh like survey reachability broadcasts. This keeps sync independent of unique per-anchor production IDs and avoids a command-result storm when many anchors hear the same sync. The command fields are defined in [[UWB+BLE Protocols and Strategies 0.2.39]].

Click range reports, diagnostic range reports, survey pair sample results, status responses, and anchor heartbeats carry the gateway-synchronized timestamp for when the relevant measurement or status snapshot was taken. They also report how old the anchor's last sync was at that timestamp. If an anchor has not synced yet, it reports local uptime as the timestamp and marks time sync stale.

The clicker writes the round-robin round into every scheduled range exchange. Anchors validate that field against the accepted schedule and store it with the resulting sample.

Each anchor reports per-sample round indices and per-sample sequence start times to the gateway. The gateway groups samples by clicker, event, and round, then can average the anchor-side start times for that round even when one click contains several round robins. Anchors do not exchange start times with each other; they only report what they measured.

The required sync interval follows:

`max_interval_ms = allowed_drift_ms * 1,000,000 / oscillator_error_ppm`

Using a conservative 500 ppm bound and a 60,000 ms drift target gives:

`60,000 * 1,000,000 / 500 = 120,000,000 ms = 33.3 h`

Firmware broadcasts sync every 3,600,000 ms (1 h). At 500 ppm, one hour contributes about 1.8 s of drift; even missed syncs have more than a day of margin before drift reaches roughly one minute.

---

## Anchor Self-Distance Survey Protocol

Anchor self-distance measuring is used to automate setup. The firmware only measures distances and reports them. Off-site software processes the resulting distance network and computes anchor placement.

### Survey Flow

1. Gateway creates a `survey_id` and sends a UWB mesh reachability request.
2. Anchors exchange UWB survey reachability frames for the requested duration.
3. Each anchor reports which other anchors it can reach, with RSSI and link quality metadata.
4. Gateway builds a reachable graph from those reports.
5. Gateway schedules one unordered anchor pair at a time.
6. For each pair, the gateway sends prepare commands to both anchors.
7. After both anchors acknowledge readiness, the gateway sends a start command with `n`, the number of measurements to perform.
8. The participating anchors perform exactly `n` UWB range measurements on the dedicated survey worker.
9. Each sample is reported to the gateway as diagnostic survey data.
10. Gateway schedules the next pair or aborts the survey.

During a survey pair run, the system command/mesh work stays available. If the gateway sends a survey abort command, the target anchor records the abort request immediately and the survey worker exits at the next sample or bounded responder-listen check.

Each survey range sample has a unique protocol identity derived from the pair and sample index, so long surveys remain unambiguous even when compact radio sequence numbers wrap.

Each sample report also carries `TIMESTAMP_MS` and `TIME_SYNC_AGE_MS`, so downstream survey processing can place each individual ranging sequence on the gateway timebase rather than inferring timing from packet arrival order.

### Survey Message Requirements

Survey messages must include:

- `survey_id`
- initiator anchor ID
- responder anchor ID
- sample index
- requested sample count `n`
- distance in millimeters
- quality/status code
- diagnostic flag

Survey measurements must never be counted as click events. They are diagnostic infrastructure data.

### Scheduling Strategy

v1 uses sequential pair scheduling. Only one anchor pair is actively ranging at a time. This avoids UWB collisions and makes results easier to validate during installation.

The gateway may skip unreachable pairs based on the reachability graph. It may also repeat failed pairs later, but the firmware does not decide which pairs are useful for geometry solving.

---

## Server-Side Event Correlation
The server receives independent reports from multiple anchors for the same click event. Correlation logic:
1. Group incoming ranging reports by clicker and click event.
2. Wait for a configurable window to allow for worst case mesh relay delay.
3. Once the window closes, the event is finalized with all received distance measurements.
4. **Trilateration computes (x, y) position** from at least three anchor distances, known anchor coordinates, and any other selected inputs. _(Kenneth's scope)_
5. Event is stored with the active Question of the Day for that timestamp. The clicker does not need to know the question.

### Question Association
The clicker and normal anchors do not know the Question of the Day. The server maintains the question schedule and tags each click event with whatever question was active at that timestamp.

A display screen at the clicker station (separate system, e.g., e-ink + WiFi or a tablet) shows the current question. This is independent of the clicker and anchor hardware.

---
## MCU Pinout

| MCU Pin | Connected to | Extra Information |
| ------------- | ------------------ | --------------------------------------------------------------------------------- |
| P0.03 | DWM3000 CS | Chip Select, Pull LOW to select; used with nRF52833 SPIM3 |
| P0.16 | TX | only via JTAG |
| P0.31 | DWM3000 RST | Pull low to reset, otherwise high |
| P0.30 | DWM3000 WakeUP pin | |
| P0.11 | DWM3000 SPICLK | nRF52833 SPIM3 SCK, runtime target 32 MHz |
| P0.12 | DWM3000 SPIMISO | nRF52833 SPIM3 MISO |
| P1.00 | DWM3000 SPIMOSI | nRF52833 SPIM3 MOSI |
| P0.04 | RX | only via JTAG |
| USBDM / USBDP | USB C Port | Use for firmware upload + gateway integration + debug output |
| P0.17 | RGB BLUE Output 1 | |
| P0.20 | RGB RED Output 1 | |
| P0.14 | RGB GREEN Output 1 | |
| P0.13 | RGB BLUE Output 2 | |
| P1.01 | RGB GREEN Output 2 | |
| P0.08 | RGB RED Output 2 | |
| P0.26 | Clicker Button | Use internal pull-up |
| P0.15 | BAT_PG | Battery Charger IC Status Output, Use internal pull-up |
| P0.06 | BAT_CHG | Battery Charger IC Status Output, Use internal pull-up |
| P0.29 | HALF_BAT_V | Half the Battery Voltage, enable mosfet to read |
| P0.07 | BAT_ADC_MOSFET | High Side P channel mosfet, drive low to enable battery voltage resistive divider |

## Power Architecture

### Power Rail Design

Both the Clicker and Anchor share the same power regulation topology:

```
[LiPo/Li-Ion Battery] ──► [Charger] ◄── [USB-C Input]
 3.2–4.2V CC/CV @ up to 1A
 │
 ▼
[LDO]
 3.0V regulated output
 150 mA max, Iq < 1µA
 Dropout < 0.2V (operates down to 3.2V battery)
 │
 ├──► ANNA-B402-00B (nRF52833) VDD = 3.0V
 └──► DWM3000 VDD3V3 = 3.0V
```

**Key advantage over coin cell ([[UWB+BLE Architecture 0.1]]):** The LiPo's low internal impedance (max 350mΩ) can deliver the ~50 mA UWB TX pulses without the buffer capacitor that was mandatory with a CR2477. This simplifies the PCB layout and reduces BOM.

**BQ2409x features used:**
- Over-voltage protection (6.6V threshold) for USB-C safety
- Selectable charge current via ISET2 pin (85mA for clicker, 1000mA for anchor)
- Status indicators (CHG, PG pins)

### Clicker Power Budget
Assumptions: 50 clicks/day and current firmware timing from `main.c`: two quiet 2 ms UWB politeness samples in the normal quiet case, 25 ms quiet politeness sample period, 75 ms busy politeness sample period after activity, 500 ms maximum politeness wall-clock wait, 75 ms minimum BLE courtesy scan/advertise during normal-click politeness, randomized contention sleep before the wake train, 430 ms UWB wake-claim train, UWB discovery/reply/schedule exchange, up to 6 scheduled anchors, and one shared 200 ms channel-5 ranging burst. The clicker returns the DWM3000 to retained sleep after each active window. Retry and contention backoff are mostly sleep time, not UWB receive time.

#### Per-Click Energy

| Phase | Duration | Current | Charge (µA·s) |
| --------------------------------- | ------------------- | ------- | --------------- |
| MCU wake from deep sleep | 1 ms | 5 mA | 5 |
| UWB sampled politeness | 2 × (2.67 ms startup/PLL + 2 ms RX) = 9.34 ms active | 50 mA | 467 |
| BLE courtesy scan/advertise | 75 ms wall time, 20/25 ms passive scan duty plus about three 1 ms TX events | 5 mA active | 315 |
| UWB wake train | 430 ms | 50 mA | 21,500 |
| UWB discovery/reply/schedule | ~80 ms | 50 mA | 4,000 |
| UWB radio wake/reset/configure | 10 ms | 20 mA | 200 |
| UWB shared TWR burst | One 200 ms channel-5 burst covering up to 6 scheduled anchors | 50 mA | 10,000 |
| MCU processing + LED blink | 50 ms | 10 mA | 500 |
| **Total per click** | **~0.85 s wall time in the normal quiet case** | | **36,987 µA·s** |

Note: 36,987 µA·s = approximately 0.0103 mAh per click. A worst-case 500 ms busy sampled-politeness wait now uses about seven UWB samples, or `7 × (2.67 ms startup/PLL + 2 ms RX) × 50 mA = 1,635 µA·s = 0.00045 mAh` of UWB receive energy, plus roughly `500 ms × 80% × 5 mA + 20 × 1 ms × 5 mA = 2,100 µA·s = 0.00058 mAh` for BLE courtesy scan/advertise. That combined worst-case politeness energy remains far below `500 ms × 50 mA = 25,000 µA·s = 0.0069 mAh` for a continuous UWB listen.

#### Daily Energy

| Component | Calculation | mAh/day |
| --------------------------- | -------------- | --------- |
| Active clicks | 50 × 0.0103 mAh | 0.51 |
| Deep sleep (24h, MCU + UWB) | 2.86 µA × 24h | 0.069 |
| **Daily total** | | **0.58** |
| With ~3× safety margin | | **1.75** |

#### Battery Life Estimates (with 3× margin)

| Battery | Capacity | Estimated Life |
| ----------- | -------- | -------------- |
| LiPo 85 mAh | 85 mAh | ~49 days |

**Conclusion:** A 85 mAh LiPo is more than sufficient.

### Anchor Power Budget
Assumptions: low-duty DWM3000 wake scanning (`400 ms` interval, `1 ms` RX window plus 2.5 ms startup and 0.17 ms PLL overhead), periodic anchor UWB mesh receive (`6000 ms` interval, `2 ms` RX window plus the same startup/PLL overhead), 1000 selected ranging events/day (16 clickers × 50 clicks × 70% proximity, rounded up), and 1000 queued click-report mesh deliveries/day. A selected ranging event uses UWB discovery/schedule frames and scheduled range responder windows. Mesh route discovery and operational reports use UWB mesh frames.

This is a theoretical radio-dominant budget from the current firmware constants. Every line below is calculated from an explicit duration, current draw, and event-rate assumption.

#### Daily Consumption Breakdown

| **Component** | **Calculation** | **mAh/day** | **% of total** |
| ---------------------------- | --------------------------------- | ----------- | -------------- |
| UWB wake scan baseline | 3.67ms awake / 403.67ms period = 0.909%; 785.5s/day × 50mA | 10.91 | 65.1% |
| Periodic UWB mesh RX baseline | 4.67ms awake / 6004.67ms period = 0.0778%; 67.2s/day × 50mA | 0.93 | 5.5% |
| UWB discovery/schedule control | 1000 × (15ms claim collection + 16ms discovery listen + 20ms reply TX + 80ms schedule RX) × 50mA | 1.82 | 10.9% |
| UWB responder bursts | 1000 × 200ms shared responder window × 50mA | 2.78 | 16.6% |
| UWB mesh report TX | 1000 × (2.67ms startup/PLL + 20ms TX timeout) × 50mA | 0.31 | 1.8% |
| **Daily total** | Theoretical radio budget before margin | **16.76** | **100%** |
| **With 1.5× safety margin** | 16.76mAh/day × 1.5 | **25.14** | |

A route miss that adds one extra `ROUTE_REQ` transmit costs another 0.31mAh per 1000 reports. A worst-case "every report starts with route discovery" day is therefore about 17.07mAh before margin, or 25.60mAh/day with the same 1.5× margin.

#### Battery Life Estimates (with 1.5× margin, 0.85 derating)

**Assumptions:**
- **Load:** 25.14 mAh/day theoretical anchor radio budget after 1.5× safety margin.
- **Battery:** 18650 Li-Ion (3000 mAh nominal).
- **Configuration:** Batteries in parallel (capacity adds up).
- **Efficiency:** 0.85 (85% usable capacity).

| **Number of Batteries** | **Total Usable Capacity** | **Est. Days** | **Est. Months** | **Est. Years** |
| ----------------------- | ------------------------- | ------------- | --------------- | -------------- |
| **1 × 18650** | 2,550 mAh | 101.4 | 3.3 | 0.28 |
| **2 × 18650** | 5,100 mAh | 202.9 | 6.7 | 0.56 |
| **3 × 18650** | 7,650 mAh | 304.3 | 10.0 | 0.83 |

**Observation:** Low-duty UWB wake scanning is the dominant theoretical anchor budget. The current firmware keeps normal idle scan plus periodic UWB mesh RX at 0.987% DWM3000 awake time, just under the intended ~1% periodic idle target. Active route/report traffic is event-driven and adds about 4.91mAh/day under the 1000 selected-events/day assumption.

### Previous BLE-Gated Versus Current UWB-Gated Architecture

This comparison normalizes both designs to the same functional target: 50 clicker events/day, up to 6 scheduled anchors, one shared channel-5 ranging burst for the current design, and 1000 selected anchor events/day. It uses 50mA for active UWB and 6mA for active BLE scan/advertise. The previous prototype code used 10% BLE idle scanning, 1000ms request advertising, 2000ms continuous READY collection, a 120s first-poll responder deadline, and 2000ms post-range responder idle timeout. The 1000-exchange measurement loop in the prototype is not used because it was a test mode, not a fair production click workload; the table below is a theoretical production-workload calculation.

#### Theoretical Power Comparison

| Role / Design | Calculation | Theoretical Result |
| --- | --- | --- |
| Previous BLE-gated clicker | 3s BLE exchange × 6mA + 3.8s UWB awake × 50mA | 0.0578mAh/click; 2.89mAh/day at 50 clicks |
| Current UWB-gated clicker | Current clicker budget above | 0.0103mAh/click; 0.51mAh/day active-click energy |
| Previous BLE-gated anchor idle | 10% BLE scan × 6mA × 24h | 14.40mAh/day before any UWB ranging |
| Previous BLE-gated selected anchor event | 3s BLE coordination wait + 2 UWB samples + 2s post-range idle = 5.1s UWB awake × 50mA | 0.0708mAh/event; 70.83mAh/day at 1000 events |
| Previous BLE-gated anchor total | 14.40mAh BLE idle + 70.83mAh selected-event UWB | 85.23mAh/day, or 127.85mAh/day with 1.5× margin |
| Current UWB-gated anchor total | Calculated anchor budget above | 16.76mAh/day, or 25.14mAh/day with 1.5× margin |

The current UWB-gated anchor is theoretically about 5.1× lower power than the previous BLE-gated anchor under the normalized 1000 selected-events/day load. The difference is mainly the old responder behavior: after a BLE request it keeps the UWB radio available through a long first-poll deadline and a 2s post-range idle tail. In the current architecture the anchor uses bounded channel-5 responder bursts and negotiated channel-9 mesh events.

For battery sizing with the same 0.85 derated 18650 capacity, the previous BLE-gated anchor budget would last about 19.9 days on one cell, 39.9 days on two cells, and 59.8 days on three cells after the 1.5× margin. The current UWB-gated anchor estimate is about 101.4, 202.9, and 304.3 days respectively.

#### Theoretical Success Probability

Let `p_ble` be the probability that one BLE advertisement is decoded when the scanner is awake, `p_claim` the probability that one UWB wake claim is decoded during an overlapping wake scan, `p_discovery` the probability that a selected discovery reply is exchanged, and `p_twr` the probability that one range sample completes.

Previous BLE-gated wake has strong phase coverage because the 1000ms request advertisement spans the 1000ms anchor scan interval. Its request detection probability is therefore RF-loss dominated: `P_request = 1 - (1 - p_ble)^N_request`, where `N_request` is the number of request advertisements that land in the anchor scan window. READY collection is also RF-loss dominated because the clicker scans continuously for 2000ms: `P_ready = 1 - (1 - p_ble)^N_ready`. A normalized two-sample per-anchor success estimate is `P_anchor_old = P_request × P_ready × (1 - (1 - p_twr)^2)`.

Current UWB-gated wake has deterministic phase coverage because the 430ms wake-claim train is longer than the 403.67ms anchor scan period. Under perfect RF and no scheduling gaps, every anchor gets at least one chance to hear the claim. With RF loss, `P_claim = 1 - (1 - p_claim)^N_claim`, where `N_claim >= 1` by timing design. A selected per-anchor success estimate is `P_anchor_current = P_claim × p_discovery × (1 - (1 - p_twr)^2)`.

The current firmware adds collision desynchronization before and during the wake phase. During politeness, the clicker advertises and scans BLE courtesy priority on channel 37 only. If it hears a higher-precedence clicker, it waits 75ms and reruns the same attempt gate. Before each wake train, the clicker sleeps for a random 12ms slot: attempt 1 uses 16 slots (`0-180ms`), attempt 2 uses 32 slots (`0-372ms`), and attempt 3+ uses 64 slots (`0-756ms`). Retries add a 150ms base delay plus a new politeness/courtesy gate and the same randomized contention window for the next attempt. During a wake train, repeated wake claims use only sub-millisecond random jitter (`0-400us`) so exact simultaneous transmitters can drift apart without intentionally creating multi-millisecond holes in the long-preamble coverage. After an anchor receives a valid claim, it keeps a 15ms claim-collection window open so a nearly simultaneous higher-precedence claim can still be considered. For two clickers pressed at the same instant, the probability that they pick the same first-attempt contention slot is `1/16`; if they remain tied, the next retry slot match probability is `1/32`, then `1/64`.

Monte Carlo courtesy estimate, 300,000 trials per row: both clickers start together, both use one BLE channel, passive scan window is 20ms every 25ms, the first advertising event phase is uniformly distributed over one 20.0-30.625ms advertising period, later events use 20.0-20.625ms plus a uniform 0-10ms controller advertising delay, and a channel-37 advertising event occupies 1ms. The useful collision-avoidance metric is "lower hears higher" because only the lower-precedence clicker should defer.

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

The implemented value is 75ms for now. It captures most of the detection benefit before the curve flattens, while adding less latency than the 100-200ms options. This is advisory, so a miss falls back to UWB contention rather than blocking the click.

Normal click acceptance requires four unique anchors. With `A` anchors in range and equal per-anchor success probability `p_anchor`, one attempt succeeds with:

`P_attempt = sum(k=4..A) C(A,k) × p_anchor^k × (1 - p_anchor)^(A-k)`

With up to 6 attempts, the click succeeds with:

`P_click = 1 - (1 - P_attempt)^6`

The previous BLE-gated path can have excellent single-anchor wake probability in clean RF, but its probability model is weaker at the full system level because BLE request/READY discovery is decoupled from UWB ownership and scheduling. Multiple clickers, hidden devices, delayed responders, or stale READY advertisements can leave anchors awake for long periods or produce poorly coordinated UWB attempts. The current UWB-gated path has a higher per-wake energy cost than one BLE scan window, but every operational frame is bound to one selected clicker/event/anchor; anchors lock to one clicker/event epoch; and hidden clickers can cause retries without corrupting ranging.

#### Pros And Cons

| Architecture | Pros | Cons |
| --- | --- | --- |
| Previous BLE-gated | Low-cost BLE scanning, mature BLE receiver behavior, request advertisement covers the 1s scan phase, DWM can stay in deep sleep until BLE request | High anchor event energy from long UWB responder deadlines, BLE discovery is not the ranging identity authority, hidden/multiple clicker coordination is weak, operational mesh/routing needs another transport path, old responder can burn 120s of UWB listen time if a request is seen but no addressed poll arrives |
| Current UWB-gated plus BLE courtesy | One radio/protocol family owns wake, discovery, ranging, reports, and mesh; deterministic wake-train overlap; CRC and full identity checks before state creation; selected-clicker-only ranging; bounded scheduled windows; software idle UWB duty stays near 1%; BLE courtesy reduces exact simultaneous-click collisions before UWB TX | Requires reliable DWM3000 IRQ wiring, UWB wake-scan link budget must be validated on hardware, idle UWB scan dominates battery budget, BLE courtesy depends on nRF scan-channel-map support and is advisory only, active mesh traffic must be tuned against click priority |

---
## Firmware Architecture

Both clicker and anchor firmware run on the same ANNA-B402-00B plus DWM3000 hardware. The configured role decides whether the device behaves as a clicker, anchor, or gateway. Firmware uses Zephyr through nRF Connect SDK.

This document keeps the architecture-level responsibilities and tradeoffs. The detailed runtime diagrams are maintained in [[Firmware State Machines 0.1.34]], and exact protocol fields are maintained in [[UWB+BLE Protocols and Strategies 0.2.39]].

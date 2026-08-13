### Roles

| Role           | What it owns                                                                                                                                            | What it does not own                                                |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| Clicker        | Button wake, click/self-test gesture handling, UWB wake claims, discovery, range schedule, DS-TWR initiation, local LED result                          | Question of the Day, office map, anchor placement, report storage   |
| Anchor         | Low-duty UWB wake scan, click admission, hash-derived discovery reply slot, responder burst, report queue, mesh relay, heartbeat/status, survey samples | Server-side event grouping, final position solve, question schedule |
| Gateway anchor | Same hardware as an anchor plus a connected BLE PC link, mesh root behavior, command routing, gateway ACKs, packet-age-aware survey orchestration       | Distance solving and application storage                            |

The connected PC side is this repo's Python GUI (`tools/gateway_gui/`). It is the intended home for heavy host-side processing: distance solving, CIR reassembly, localization, and diagnostics are allowed and expected to live there rather than in gateway firmware.

### Traffic Lanes

| Lane               | Channel            | Used for                                                                                                         | Must yield to                                       |
| ------------------ | ------------------ | ---------------------------------------------------------------------------------------------------------------- | --------------------------------------------------- |
| Click/contact lane | UWB channel 5      | Clicker wake claims, discovery, schedule admission, shared ranging burst, route discovery, route contact refresh | Nothing lower priority                              |
| Payload lane       | UWB channel 9      | Reports, heartbeats, command results, gateway ACKs, survey data, routed packets                                  | Active click service, required channel-5 wake scans |
| Courtesy side lane | BLE channel 37     | Short clicker-to-clicker priority hint before UWB wake                                                           | UWB wake train start                                |
| Gateway PC lane    | Connected BLE GATT | PC command input, gateway packet output, gateway identity, and host-side processing in `tools/gateway_gui/`       | UWB work keeps priority over host servicing         |

### Radio Scheduling

The DWM3000 is one radio. Firmware never assumes channel 5 and channel 9 can run at the same time.

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

Discovery replies prove that anchors are present. A normal click only ranges after at least three eligible anchors reply. The schedule may include up to four anchors. All selected anchors share the same continuous 400 ms channel-5 responder burst while the clicker runs addressed exchanges.

If zero anchors reply, the clicker does not start burst ranging and sends no release. If one or two anchors reply, the clicker sends a compact range-release frame to those anchors before retrying or failing. That frame tells the anchors that this attempt did not reach the normal-click minimum and that they can return to low-duty scan instead of waiting for a schedule.

Retries after no discovery replies, after an insufficient-discovery release, and after an incomplete burst all use the same retry path: the clicker waits the fixed retry base delay and randomized contention bucket before the next wake phase. Incomplete burst retries keep the same click event and nonce, advance `attempt_index`, and use a fresh `priority_id`.

A failing anchor cannot end the shared burst for the others, and three successful anchors do not stop the burst early. Wrong-target polls are ignored while the responder burst remains open.

### Politeness and BLE Courtesy

BLE courtesy is only a clicker-to-clicker hint during UWB politeness.

The advertisement carries `network_id`, full `clicker_id`, `click_event_id`, `attempt_index`, `priority_id`, and a peer-finish wait. Courtesy stays active for at least 75 ms before the UWB wake train. The clicker decodes a UWB politeness sample at the beginning and end of that BLE window; if the first sample finds higher priority UWB traffic, BLE courtesy stops immediately and the UWB wait/restart path takes over.

Courtesy uses BLE advertising channel 37 only. The clicker disables advertising channels 38 and 39 and asks the SoftDevice Controller to scan only channel 37.

The BLE interval uses the fastest BLE 5.x non-connectable range on the target: 20.0 to 20.625 ms plus the controller advertising delay. Passive scanning uses 20 ms windows every 25 ms. BLE stops before the UWB wake train begins.

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

The responder response delay and initiator final delay are both fixed to the selected `UWB_RANGE_REPLY_DELAY_UUS`, the DWM/DW3000 delayed-TX unit. Schedules using another delay are rejected. The current long-range main firmware selects `UWB_RANGE_REPLY_DELAY_LONG_RANGE_UUS = 8000`; This value still needs recalibration against the final PHY, SPI/logging load, and stage-one traffic. The response frame that the clicker receives is 8 bytes longer than the poll received by the anchor, so the shorter path waits instead of making the two reply delays differ.

### Diagnostics After Ranging

The clicker sends compact clicker-side diagnostics after `FINAL` with `UWB_CLICKER_DIAG`. Anchors collect anchor-side diagnostics after the first valid `FINAL`, when the response delayed-TX deadline is no longer at risk. Missing or truncated diagnostics do not invalidate a completed range, but reports record what was captured, transmitted, truncated, dropped, or unavailable.

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

The implemented self-test verifies DWM3000 wake/init, diagnostic UWB
wake/discovery, at least one discovery reply, at least one diagnostic range,
report handoff, and return to low-power idle. The LEDs display the outcome, but
the firmware does not electrically verify the LED drive. It also does not yet
measure battery voltage or charger status, and an RTT-injected gesture exercises
the same action state machine without proving the physical button IRQ path.

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
- Routed packets..

### Channel-9 Mesh Events

See the [[Mesh Connected Routing Contract]] for details.

Reliable gateway delivery does not finish at BLE notification. The GUI first
validates and commits the retained stream record in bounded RAM, then returns an
exact identity-and-digest host receipt. Only that accepted receipt lets the
gateway acknowledge the mesh owner. The source retains its original immutable
record until the corresponding semantic ACK/ACK-confirmation exchange completes;
a BLE disconnect or missing receipt therefore causes replay rather than silent
loss. Best-effort telemetry is deliberately outside this receipt boundary.

### Click Priority Over Mesh

Anchor click handling has priority over relay traffic. During wake scanning, valid wake claims are decoded before mesh frames. The anchor cancels active mesh forwarding and clears pending mesh receive work only after a claim passes network, channel, flag, freshness, and ownership checks. Foreign or rejected claims do not interrupt mesh work. Already-built local click reports stay queued for later delivery.

### Packet Age Instead Of Gateway Time Sync

Each shared mesh packet has an always-present millisecond age field. Sources create packets with age zero; queues, relays, and retransmissions add elapsed local time before sending.

Reports, survey samples, status responses, and heartbeats carry local uptime/event timestamps. The gateway can combine those local timestamps with packet age when it needs to understand how long ago a message was created.

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


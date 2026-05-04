#internship #imec #architecture #documentation #UWB #BLE

Previous version: [[UWB+BLE Architecture 0.2] Design rationale: [[UWB+BLE Design Story 0.1]] Component selection: [[Selecting a UWB and BLE Chip]], [[05-03-2026 Internship]]

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
| DWM3000TR13 | DWM3000 module Freq: 7.9872 GHz 6.8Mbps SPI 2.5-3.6V 8 GPIO's | MD1 | 1 | 56 | 5596 | Qorvo | DWM3000TR13 | Mouser | 772-DWM3000TR13 | 17.24 | 965.29 |
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
- On press: BLE broadcast → discover anchors → UWB DS-TWR with all responders → sleep.
- Self-test mode is entered with a long press followed by a short press.
- In self-test mode, the clicker verifies local modules and sends a diagnostic "dud" ranging request. This request must not be counted as a user click.
- Identity: Permanent 64-bit device ID
- Knows nothing about QOTD, office configuration

#### Anchor (Fixed Node)
- Mounted at known positions on the ceiling of the office.
- Continuously scans for BLE advertisements from clickers (low duty cycle).
- The DWM3000 is not an always-on receiver. It stays idle/asleep until a valid BLE discovery request or gateway survey command schedules a UWB window.
- On clicker detection: advertises READY, wakes the UWB radio, opens a bounded DS-TWR responder window, then returns the DWM3000 to idle/standby.
- Relays data toward gateway via BLE mesh.
- Participates in routing protocol for self-organizing mesh.
- Reports battery level and connection data via periodic heartbeat requested by gateway.
- Responds to gateway commands, including status requests, route updates, diagnostics, and anchor-to-anchor distance survey commands.

#### Gateway Anchor
- Physically identical to other anchors, but connected to server via USB-C.
- Acts as the mesh root.
- Forwards all received data (own + relayed) to server over serial.
- May or may not perform ranging sequences itself. Depending on processing headroom.
- Issues mesh commands and receives both hop acknowledgements and end-to-end gateway acknowledgements.
- Schedules anchor-to-anchor distance surveys after anchors report which other anchors they can reach.

---

## Ranging Protocol (Per Click Event)
Based on proof-of-concept measurements ([[25-02-2026 Internship]], [[26-02-2026 Internship]], [[27-02-2026 Internship]]).

```
t=0ms Button press (GPIO interrupt wakes MCU)

t=1ms Clicker sends BLE ADV over 200ms
 Payload: clicker_eui, event_seq, "REQUEST_TO_RANGE"
 All anchors in BLE range may hear this

t=10ms+ Anchors that receive ADV:
 → Send BLE ADV response: anchor_id, uwb_short_addr, "READY"
 → Wake UWB radio from deep sleep/standby only after the BLE request
 → Open a bounded UWB responder window while READY remains visible

t=270ms Clicker closes fast MVP discovery window
 Builds temporary anchor list from responses received
 Deduplicates by anchor_id and sorts by reciprocal RSSI score
 (No pre-configuration — ranges with whoever showed up)
			
			Sequential UWB Two-Way Ranging (DS-TWR):
 → POLL Anchor A → RESP → FINAL → REPORT → distance_A (~5.75ms)
 → POLL Anchor B → RESP → FINAL → REPORT → distance_B (~5.75ms)
 → POLL Anchor C → RESP → FINAL → REPORT → distance_C (~5.75ms)
 → ... all discovered anchors

t=316ms Ranging complete (8 anchors best-case budget: 8 × 5.75ms ≈ 46ms)
 Clicker: all radios off, deep sleep
 Anchors: store result in buffer, UWB off

t=320ms LED blink / haptic confirms click registered

t=400ms Anchor READY/UWB window expires if no more polls arrive
```

### Measured TWR Exchange Breakdown (DW1000, ~5.75ms)

| Phase | Description | Time |
| --------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | ------------ |
| 1. POLL TX | Force off + clear + write buffer + TX_FCTRL + start + poll TX complete + read TX timestamp | ~400 µs |
| 2. Wait for RESPONSE | Responder detects POLL (~50µs) + SPI processing (~400µs) + 1600µs delay + RESPONSE air time (~157µs) + initiator detection (~50µs) | ~2250 µs |
| 3. Process RESPONSE | Read RX timestamp + read frame + validate + build FINAL | ~300 µs |
| 4. FINAL TX (delayed) | Schedule via tx_fast (~200µs) + wait for delay (~850µs) + FINAL air time (~162µs) + read TX timestamp | ~1500 µs |
| 5. Wait for REPORT | Responder detects FINAL (~50µs) + read frame (~100µs) + compute DS-TWR (~50µs) + send REPORT (~300µs) + REPORT air time (~153µs) + detection (~50µs) | ~700 µs |
| 6. Read REPORT | Read frame + parse + return | ~100 µs |
| **Total** | | **~5750 µs** |

**Note on DWM3000 migration:** The DWM3000 module uses the newer DWM3000 API. ~80% of firmware will port directly from the DWM1001 proof of concept. The ~20% requiring rewrite is the UWB driver layer. The DWM3000's faster SPI throughput and hardware acceleration should reduce the per-exchange time way below 5.75ms.

**DWM3000 SPI speed policy:** DWM3000 reset and soft-reset paths run at 2 MHz because the DW3000 API requires SPI at or below 7 MHz while the device is using its reset clock. Runtime traffic after the DWM3000 has initialized and reached IDLE targets 32 MHz on the nRF52833 SPIM3 controller. The DWM3000 examples note support up to 38 MHz, but SPIM3 is the nRF52833 controller that can meet the 32 MHz target; SPI1 is not sufficient because it is capped at 8 MHz.

**DWM3000 IRQ policy:** the current pinout does not connect the DWM3000 IRQ line. v1 therefore uses bounded SPI polling of DWM3000 status registers during scheduled UWB windows. This is acceptable because anchors only wake UWB after BLE has scheduled work; they must not spin in a permanent UWB receive loop.

Key property: the clicker discovers anchors dynamically. Moving, adding, or removing anchors requires zero firmware changes or configuration updates on any clicker.

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
4. BLE stack can advertise a diagnostic ranging request.
5. At least one anchor replies with a diagnostic READY response.
6. DWM3000 can wake, initialize, and perform at least one diagnostic UWB ranging exchange with a responding anchor.
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
| Red blink code repeated 3 times | Self-test or module failure |

Red failure codes:

| Red blinks | Failure |
| --- | --- |
| 1 | Battery/status read failed |
| 2 | BLE advertise or scan failed |
| 3 | DWM3000 wake/init failed |
| 4 | No anchor replied READY |
| 5 | UWB diagnostic range failed |
| 6 | Internal firmware state error |

---

## BLE Mesh Routing
The anchors form a self-organizing mesh network to relay data to the gateway. The intuitive model is: each packet has a final destination, but each sender only chooses the next neighbor to hand it to.

All mesh communication is symmetric at the protocol level. A sender must be able to know whether the next hop received the message. For gateway-bound traffic, the original sender must also be able to know whether the gateway ultimately received the message.

For v1, this explicit acknowledgement behavior is more important than maximizing throughput. It makes route debugging and test validation straightforward.

### Mesh Packet Standard

All routed packets use a versioned binary envelope. The detailed field list is documented in [[UWB+BLE Protocols and Strategies 0.1]], but each packet contains:

- protocol version
- message type
- flags
- 64-bit source ID
- 64-bit destination ID, where 0 means broadcast
- session ID
- sender-local sequence number
- TTL
- TLV payload
- CRC

`TLV` means Type-Length-Value. It allows new command payloads to be added without changing the shared IMEC packet envelope.

### Required Acknowledgements

Every routed packet that matters sets:

- `ACK_REQUESTED`: the next hop must reply with a hop ACK.

Every gateway-bound packet that matters also sets:

- `GATEWAY_ACK_REQUIRED`: the gateway must send an end-to-end gateway ACK back to the original sender.

This means reports, route status packets, survey results, heartbeats, and command results require gateway ACKs. Gateway-originated commands do not, because the gateway is already the sender. A command is complete when the target anchor returns `COMMAND_RESULT`.

For gateway-bound packets, the sender considers the message successful only after receiving the gateway ACK. A hop ACK only proves that the next relay received the message; it does not prove gateway delivery.

Default retry behavior:

| Parameter | Value |
| --- | --- |
| Hop ACK timeout | 150 ms |
| Gateway ACK timeout | 2 s |
| Max retries | 3 |
| Retry backoff | 100 ms, 250 ms, 500 ms plus jitter |

### Link Quality Weighting (optional enhancement)

```
effective_cost = hop_count + (1.0 - rssi_normalized)
```

This causes anchors to prefer fewer hops through strong links over more hops or weak (through-wall) links. Requires careful weighing of rssi.

### Route Discovery and State

The gateway is the mesh root. It periodically advertises a route beacon with hop count 0 and a route epoch. Anchors that hear this beacon store the gateway as a direct candidate route. Anchors then re-advertise their selected route with hop count +1, allowing other anchors to discover multi-hop paths.

Route discovery has two directions:

1. `ROUTE_ADV` flows outward from the gateway. It means: "I know a way to this gateway in this many hops."
2. `ROUTE_STATUS` flows inward from anchors. It means: "I am this anchor, this is my selected route to the gateway, and this is how you can reach me again."

Every relay that forwards `ROUTE_STATUS` stores a reverse entry for that anchor. The gateway therefore builds an anchor directory from route status packets instead of flooding commands.

Each anchor stores:

- selected next-hop anchor ID
- gateway ID
- hop count
- route epoch
- latest RSSI/link quality for the next hop
- last route advertisement time
- consecutive delivery failure count

The gateway stores a matching downlink directory:

- anchor ID
- selected next-hop anchor ID
- gateway ID
- hop count
- route epoch
- latest link quality
- last route status time

Route selection defaults to lowest hop count. If two routes have the same hop count, the anchor prefers the route with better recent link quality. A gateway route epoch change invalidates old routes, which lets the gateway force route rediscovery.

### Route Failure Behavior

An anchor marks its current route suspect when hop ACK or gateway ACK repeatedly fails. After three failed delivery attempts for the same route, the anchor tries another known route candidate. If no candidate exists, it enters route discovery mode and buffers non-expired diagnostic/click reports until a route is found or the report TTL expires.

### Data Forwarding (every click)
Anchor calculates ToF data, forwards it to `my_next_hop`, waits for a hop ACK, then waits for a gateway ACK. Intermediate anchors relay onward. Packets carry a TTL (decremented per hop, dropped at zero) to prevent loops during route convergence.

If hop ACK fails, the sender retries with backoff. If gateway ACK fails after the packet was hop-acknowledged, the sender may retry through the same route first, then attempt route rediscovery.

### Self-Healing
If an anchor dies, its neighbors stop hearing route advertisements. After a certain amount of missed transmissions, they invalidate that route and recalculate from whatever alternatives they hear. Traffic automatically reroutes.

---

## Gateway Commands

The gateway can issue commands to anchors through the same mesh envelope. v1 must implement a small command set, but the protocol reserves space for many future commands.

The gateway reaches an anchor by using the downlink directory learned from `ROUTE_STATUS`. If the directory says "Anchor A is behind Anchor B," the gateway sends the command to B with `dst_id=Anchor A`. B then performs the same lookup until the command reaches A.

Required v1 command classes:

| Command class | Purpose |
| --- | --- |
| Ping/status | Verify an anchor is alive and report firmware, uptime, battery, route, and radio status |
| LED/status pattern | Trigger a visible indication on a specific anchor for installation/testing |
| Route management | Set, clear, or request route information |
| Heartbeat control | Start/stop periodic anchor health reports |
| Survey control | Start/abort anchor reachability and anchor-to-anchor distance measurements |

Command responses use the same acknowledgement standard as other mesh messages:

1. The next hop sends a hop ACK.
2. The final command target executes or rejects the command and returns a command result.
3. The gateway sends an end-to-end gateway ACK for results it accepts.

Unsupported commands must return `UNSUPPORTED_COMMAND`, not be silently ignored.

---

## Anchor Self-Distance Survey Protocol

Anchor self-distance measuring is used to automate setup. The firmware only measures distances and reports them. Off-site software processes the resulting distance network and computes anchor placement.

### Survey Flow

1. Gateway creates a `survey_id` and broadcasts a reachability request.
2. Anchors advertise and scan survey beacons for the requested duration.
3. Each anchor reports which other anchors it can reach, with RSSI and link quality metadata.
4. Gateway builds a reachable graph from those reports.
5. Gateway schedules one unordered anchor pair at a time.
6. For each pair, the gateway sends prepare commands to both anchors.
7. After both anchors acknowledge readiness, the gateway sends a start command with `n`, the number of measurements to perform.
8. The initiator anchor performs exactly `n` DS-TWR measurements against the responder anchor.
9. Each sample is reported to the gateway as diagnostic survey data.
10. Gateway schedules the next pair or aborts the survey.

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
1. Group incoming ranging reports by `(clicker_uuid, event_seq)`.
2. Wait for a configurable window to allow for worst case mesh relay delay.
3. Once the window closes, the event is finalized with all received distance measurements.
4. **Trilateration computes (x, y) position** from ≥3 anchor distances + known anchor coordinates + Other data. _(Kenneth's scope)_
5. Event is stored with the active QOTD for that timestamp (server-side association, clicker doesn't need to know the question).

### QOTD Association
The clicker and normal anchors do not know the Question of the Day. The server maintains the QOTD schedule and simply tags each click event with whatever question was active at that timestamp.

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
 200 mA max, Iq < 1µA
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
Assumptions: 50 clicks/day, fast MVP event duration around 320ms, 46ms UWB active for 8 anchors, remainder BLE+MCU.

#### Per-Click Energy

| Phase | Duration | Current | Charge (µA·s) |
| --------------------------------- | ------------------- | ------- | --------------- |
| MCU wake from deep sleep | 1 ms | 5 mA | 5 |
| BLE ADV TX (3 channels) | 15 ms | 12 mA | 180 |
| BLE RX (collect anchor responses) | 150 ms | 9.3 mA | 1,395 |
| UWB radio wake from DEEP_SLEEP | 5 ms | 20 mA | 100 |
| UWB TWR × 8 anchors (DS-TWR) | 8 × 5.75 ms = 46 ms | 50 mA | 2,300 |
| MCU processing + LED blink | 50 ms | 10 mA | 500 |
| BLE idle during UWB phase | 46 ms | 3 mA | 138 |
| **Total per click** | **~367 ms** | | **4,618 µA·s** |

Note: 4618 µA·s = approximately 0.0013 mAh per click.

#### Daily Energy

| Component | Calculation | mAh/day |
| --------------------------- | -------------- | --------- |
| Active clicks | 50 × 0.0013 mAh | 0.065 |
| Deep sleep (24h, MCU + UWB) | 2.86 µA × 24h | 0.069 |
| **Daily total** | | **0.134** |
| With ~3× safety margin | | **1** |

#### Battery Life Estimates (with 3× margin)

| Battery | Capacity | Estimated Life |
| ----------- | -------- | -------------- |
| LiPo 85 mAh | 85 mAh | ~85 days |

**Conclusion:** A 85 mAh LiPo is more than sufficient.
### Anchor Power Budget
Assumptions: 10% BLE scan duty cycle, 1000 ranging events/day (16 clickers × 50 clicks × 70% proximity), 1000 mesh relay events/day.
#### Daily Consumption Breakdown 

| **Component** | **Calculation** | **mAh/day** | **% of total** |
| ---------------------------- | --------------------------------- | ----------- | -------------- |
| BLE scan baseline (10% duty) | 6.0 mA × 2.4h + 0.003 mA × 21.6h | 14.5 | 65% |
| UWB ranging windows | 1000 × 0.4s × 50mA = 20,000 mAs | 5.56 | 22% |
| BLE/MCU during ranging | 1000 × 0.4s × 6.0 mA = 2,400 mAs | 0.67 | 3% |
| BLE mesh relay | 1000 × 1.0s × 6.0 mA = 6,000 mAs | 1.67 | 8% |
| Route discovery + heartbeat | 288/day × 15ms × 12mA | 0.011 | <1% |
| **Daily total** | | **22.3** | |
| **With 1.5× safety margin** | | **33.5** | |
#### Battery Life Estimates (with 1.5× margin, 0.85 derating)

**Assumptions:**
- **Load:** 33.5 mAh/day (includes 1.5× safety margin).
- **Battery:** 18650 Li-Ion (3000 mAh nominal).
- **Configuration:** Batteries in parallel (capacity adds up).
- **Efficiency:** 0.85 (85% usable capacity).

| **Number of Batteries** | **Total Usable Capacity** | **Est. Days** | **Est. Months** | **Est. Years** |
| ----------------------- | ------------------------- | ------------- | --------------- | -------------- |
| **1 × 18650** | 2,550 mAh | **~76 days** | 2.5 months | 0.2 years |
| **2 × 18650** | 5,100 mAh | **~152 days** | 5.0 months | 0.4 years |
| **3 × 18650** | 7,650 mAh | **~228 days** | 7.5 months | 0.6 years |

**Observation:** BLE scanning dominates at 65%, reducing the scan duty cycle further is likely possible. Custom 2.4GHz protocols might optimize this further.

---
## Firmware Architecture

### Shared Codebase

Both Clicker and Anchor run on identical hardware (ANNA-B402-00B + DWM3000). The role is determined by a firmware configuration flag.

**RTOS:** Zephyr (via nRF Connect SDK)

### Clicker State Machine

```
[DEEP_SLEEP] ──GPIO IRQ──► [BLE_ADVERTISE] ──200ms──► [UWB_RANGE]
 │
 all anchors done
 │
 ▼
 [CONFIRM_LED]
 │
 ▼
 [DEEP_SLEEP]
```

```
 ┌─────── (Voltage OK) ───────-----┐
 │ │
 ▼ ▲
 [DEEP_SLEEP] ───── RTC (24h) ─────► [BAT_CHECK]
 │ │
 GPIO IRQ │ (Voltage Low)
 │ ▼
 │ [LOW_BAT_BLINK] ◄──┐
 │ │ │
 ▼ │ 2s Timer
 [BLE_ADVERTISE] ▼ │
 │ [SLEEP_SHORT] ─────┘
 200ms
 │
 ▼
 [UWB_RANGE]
 │
all anchors done
 │
 ▼
 [CONFIRM_LED]
 │
 ▼
 [DEEP_SLEEP]
```
### Anchor State Machine

```
[BLE_SCANNING]──►RX clicker ADV──►RX/TX TWR Exchange──►Mesh TX [TWR Result]
 ▲ │
 │ ▼
 └──────────────────────────────────────────────────--------- [BLE_SCANNING]

[BLE_SCANNING] ──► RX Mesh Relay ADV ──► Mesh TX ──► [BLE_SCANNING]

[BLE_SCANNING] ──► RX route request ADV ──► TX[ROUTE_ADV] ──► [BLE_SCANNING]
```

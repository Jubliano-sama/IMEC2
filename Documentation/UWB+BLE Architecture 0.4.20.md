#internship #imec #architecture #documentation #UWB #BLE

Version: 0.4.20

Previous version: [[UWB+BLE Architecture 0.4.19]] Design rationale: [[UWB+BLE Design Story 0.1]] Component selection: [[Selecting a UWB and BLE Chip]], [[05-03-2026 Internship]]

## Changelog

### 2026-05-04 - 0.4.20

- Rewrite the mesh architecture around simple reactive routing: `ROUTE_REQ`/`ROUTE_REPLY` use advertisements for path discovery, while operational mesh packets use BLE connections to the selected next hop.
- Document that anchors and the gateway keep their configured scan duty cycle active during mesh connection establishment and connection events.
- Document click priority over mesh relay work: anchors preempt active relay transmission and pending mesh RX when a valid click wake advertisement arrives.
- Document that oversized aggregated click sample lists are split across multiple connected report packets, while UWB received signal level appears only once per aggregate.

### 2026-05-04 - 0.4.19

- Update the click-to-ranging architecture to the robust wake/READY strategy: UWB politeness sniff, 330 ms clicker wake advertisements, 200 ms READY scan, deterministic anchor arbitration, 180 ms addressed READY advertisements, 500 ms UWB responder windows, and 4 unique anchor ranges required for click success.
- Update DS-TWR packet sizes and timing notes: timing-critical UWB frames now use compact 16-bit UWB short addresses, and reply delays must be equal first and shortest second.

### 2026-05-04 - 0.4.18

- Document that queued click distance reports include UWB received signal level from DWM3000 RX diagnostics.

### 2026-05-04 - 0.4.17

- Update BLE radio policy to SoftDevice Controller with LE 1M PHY extended advertising and 1M scanning; Coded PHY and 2M PHY controller support are disabled.

### 2026-05-02 - 0.4.16

- Remove duplicated ranging timelines, stale 200 ms clicker advertisement references, and repeated mesh retry/self-healing explanations while preserving the current timing and routing behavior.

### 2026-05-02 - 0.4.15

- Simplify relay busy behavior wording by focusing on the custody rule and removing redundant scan-callback ordering detail.

### 2026-05-02 - 0.4.14

- Clarify link-quality weighting: route selection is hop-count first, then RSSI-derived quality within the same hop-count band; quality propagation uses the weakest-link value along the advertised path.

### 2026-05-02 - 0.4.13

- Correct the clicker and anchor power budgets to match the current BLE/UWB timing: 120 ms discovery advertisements, 150 ms READY scans, 400 ms anchor responder windows, 120 ms mesh advertisements, and gateway route advertisements every 2 s.

### 2026-05-02 - 0.4.12

- Rewrite downlink delivery section: clarify that the gateway has a flat next-hop directory (not a full topology map), retry is limited to 3 attempts against the cached next-hop, and failure means immediate COMMAND_TIMEOUT with no active discovery or reroute request. Passive recovery only via ongoing ROUTE_ADV/ROUTE_STATUS exchange.
- Add cross-reference to retry flowcharts in [[Firmware State Machines and Status Report 0.1.1]].
- Update link to [[UWB+BLE Protocols and Strategies 0.1]].

### 2026-05-01 - 0.4.11

- Add theoretical DWM3000 DS-TWR exchange breakdown and per-click timeline based on firmware delay constants and frame sizes, alongside the existing DW1000 proof-of-concept measurements.
- Add mesh relay hop latency timeline.
- Clarify mesh advertisement wording: coded advertisements and mesh advertisements are the same LE Coded PHY transmissions; document how the 30-60 ms advertising interval distributes 2-4 events within each 120 ms active period.
- Document RSSI-to-link_quality mapping and mesh quality propagation (bottleneck principle).

### 2026-05-01 - 0.4.10

- Document queued anchor click-report delivery: anchors now buffer UWB range results during the responder window, put the DWM3000 into deep sleep when the window closes, then drain reports through the acknowledged BLE mesh. Reports that lose their route after retry exhaustion are requeued for later route recovery instead of being dropped immediately.
- Document clicker BLE shutdown after normal click and self-test active windows, so the clicker returns to a no-background-radio idle state.

### 2026-05-01 - 0.4.9

- Document low-power mesh refresh behavior: passive route freshness is 30 s, anchors throttle routine route status/advertisement refreshes to 20 s, the DWM3000 wake pin is parked inactive at boot, and the DWM3000 enters deep sleep after each UWB window.

### 2026-05-01 - 0.4.8

- Document duplicate retry custody behavior: directed duplicate packets are re-forwarded when the relay can take custody, while busy relays stay silent instead of sending a false hop ACK.

### 2026-05-01 - 0.4.7

- Document anchor status route telemetry: `CMD_GET_STATUS` responses now include selected route TLVs, or a missing-route reason when no upstream path is selected.

### 2026-05-01 - 0.4.6

- Document low-duty BLE mesh timing: coded advertisements now use a 30-60 ms interval, mesh advertisements last 120 ms, hop ACK replies wait 130 ms before transmitting to avoid half-duplex collisions, and the hop ACK timeout is 500 ms.

### 2026-05-01 - 0.4.5

- Document custody-ACK mesh behavior: a node sends a hop ACK only after it can accept local handling or forwarding, busy relays stay silent so senders retry, and gateway ACKs now use hop ACKs on the return path.

### 2026-05-01 - 0.4.4

- Document gateway command-result timeout tracking: a routed command now remains pending until the target returns `COMMAND_RESULT` or a 5 s timeout emits `COMMAND_TIMEOUT` over USB.

### 2026-05-01 - 0.4.3

- Document the current mesh reliability approach: 7 s route freshness expiry, weighted upstream/downlink route selection, alternate downlink retry, 60 s duplicate cache expiry, and USB timeout reporting for failed gateway commands.

### 2026-05-01 - 0.4.2

- Document concrete BLE transmit power and DWM3000 UWB RF settings used by the firmware.

### 2026-05-01 - 0.4.1

- Document BLE long-range policy: all firmware BLE traffic uses LE Coded PHY/S=8 intent with coded-only scanning and highest configured nRF52 BLE TX power.
- Document implemented RSSI/link-quality route weighting used by mesh route selection.

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
- After the click or self-test active window, the clicker stops advertising/scanning and disables the BLE runtime before returning to idle.
- Self-test mode is entered with a long press followed by a short press.
- In self-test mode, the clicker verifies local modules and sends a diagnostic "dud" ranging request. This request must not be counted as a user click.
- Identity: Permanent 64-bit device ID
- Knows nothing about QOTD, office configuration

#### Anchor (Fixed Node)
- Mounted at known positions on the ceiling of the office.
- Continuously scans for BLE advertisements from clickers (low duty cycle).
- The DWM3000 is not an always-on receiver. Firmware parks the DWM3000 wake pin inactive at boot, does not initialize the radio driver while idle, and only wakes/configures the radio after a valid BLE discovery request or gateway survey command schedules a UWB window.
- On clicker detection: advertises READY, wakes the UWB radio, opens a bounded DS-TWR responder window, queues any range result for mesh delivery, then puts the DWM3000 into deep sleep before draining queued reports.
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
The clicker discovers anchors dynamically on each press. Moving, adding, or removing anchors requires no clicker configuration change.

```
t=0ms     Button press wakes clicker
t=0-30ms+ Clicker listens for UWB quietness before advertising
t=30ms    Clicker advertises REQUEST_TO_RANGE for 330 ms on LE 1M PHY
t=30-360ms Anchors that hear it wake UWB, scan for competing clickers, and schedule READY
t=360ms   Clicker stops advertising and scans addressed READY replies for 200 ms
t=360ms+  Selected anchors advertise addressed READY for 180 ms
t=560ms   Clicker sorts READY anchors by BLE RSSI and ranges anchors sequentially
t=560ms+  Each selected anchor gives the clicker a 50 ms per-anchor DS-TWR window
t=<15s    Click succeeds after 4 unique anchors range, or retries up to 6 wake attempts
```

### DWM3000 DS-TWR Exchange Budget

The firmware configures the DWM3000 with channel 5, preamble length 64, PAC8, STS mode 1 with SDC, STS length 64, and 6.8 Mbps data rate. SPI runs at 2 MHz during reset/init then switches to 32 MHz on nRF52833 SPIM3. The UWB frame payload sizes are defined in `uwb.h`: POLL 13 B, RESPONSE 21 B, FINAL 25 B, and REPORT 21 B before the radio FCS. Full 64-bit clicker and anchor IDs are kept out of the timing-critical DS-TWR packets; BLE READY provides the selected anchor's 16-bit UWB short address.

DS-TWR reply timing policy is equality first, shortest possible common delay second. The responder response delay and initiator final delay are both programmed to the same 900 uus value. The initiator's received response is 8 B longer than the responder's received poll. At 6.8 Mbps, that extra receive path is `ceil(8 B * 8 bits * 1e6 / 6.8e6) = 10 us`, so the shorter poll path intentionally waits instead of making the two reply times differ.

| Phase | Description | Time |
| --- | --- | --- |
| 1. POLL TX | SPI write (27 B @ 32 MHz) + start + on-air + read TX timestamp | ~300 µs |
| 2. Wait for RESPONSE | 690 µs RX enable delay + 900 uus responder turnaround + RESP on-air | ~1200 µs |
| 3. Process RESPONSE | Read RX timestamp + read frame + validate + build FINAL | ~200 µs |
| 4. FINAL TX (delayed) | 900 uus delayed TX + FINAL on-air + read TX timestamp | ~1200 µs |
| 5. Wait for REPORT | 670 µs RX enable + RESPONDER compute + REPORT on-air | ~500 µs |
| 6. Read REPORT | Read frame + parse | ~100 µs |
| **Total** | | **~3500 µs ≈ 3.5 ms** |

### DWM3000 Runtime Policy

- Reset and soft-reset SPI paths run at 2 MHz because DW3000 reset-clock access must stay at or below 7 MHz.
- Runtime SPI targets 32 MHz on nRF52833 SPIM3 after the radio reaches IDLE.
- The DWM3000 IRQ line is not connected, so v1 uses bounded SPI polling only inside BLE-arbitrated UWB windows.
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
Anchors form a self-organizing BLE mesh using simple reactive routing. Each packet has a final destination, but each sender only chooses the next hop. v1 prioritizes explicit acknowledgements, click responsiveness, and debuggable delivery over throughput.

### BLE PHY and TX Power Policy

Clicker discovery and READY replies use non-connectable extended advertising through the SoftDevice Controller on LE 1M PHY. Reactive mesh path discovery uses connectable extended advertisements for `ROUTE_REQ` and `ROUTE_REPLY`. Once a path is found, mesh data packets, hop ACKs, gateway ACKs, commands, and reports are sent over a BLE connection to the selected next hop using the mesh GATT write characteristic.

Extended advertising disables the secondary 2M PHY with `BT_LE_ADV_OPT_NO_2M`, scanning uses LE 1M only, and the controller has `CONFIG_BT_CTLR_PHY_CODED=n` plus `CONFIG_BT_CTLR_PHY_2M=n`. The nRF52 controller is still configured for `CONFIG_BT_CTLR_TX_PWR_PLUS_8` (+8 dBm).

Anchors keep their click-discovery scan at the expected 300 ms interval and 30 ms window even while mesh connections are being created or are active. The gateway keeps full-duty mesh scanning. The firmware enables BLE central, peripheral, GATT client, and controller support for parallel scanning/initiation; connection creation parameters mirror the role scan cadence so connection events do not become a hidden scan-off state.

Route discovery advertisements run for 250 ms. This gives the peer time to receive a route reply and initiate a connection while the advertiser is still connectable.

### Mesh Packet Standard

All routed packets use a versioned binary envelope. The detailed field list is documented in [[UWB+BLE Protocols and Strategies 0.1.8]], but each packet contains:

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

- `ACK_REQUESTED`: the next hop must reply with a hop ACK only after it can accept custody of the packet for local handling or forwarding.

Every gateway-bound packet that matters also sets:

- `GATEWAY_ACK_REQUIRED`: the gateway must send an end-to-end gateway ACK back to the original sender.

Reports, survey results, heartbeats, and command results require gateway ACKs. A hop ACK only proves next-hop custody; the original sender treats gateway-bound traffic as delivered only after the end-to-end gateway ACK returns. Gateway-originated commands instead complete when the target returns `COMMAND_RESULT`, or fail locally after the 5 s command-result timeout.

Default retry behavior:

| Parameter | Value |
| --- | --- |
| Click/READY advertisement interval | 20 ms |
| Route discovery advertisement duration | 250 ms |
| Hop ACK reply delay | 0 ms; ACKs use the active/created BLE connection |
| Hop ACK timeout | 500 ms |
| Gateway ACK timeout | 2 s |
| Max retries | 3 |
| Retry backoff | 100 ms, 250 ms, 500 ms |
| Route freshness window | 30 s since route discovery or successful ACK refresh |
| Duplicate suppression window | 60 s per message identity |
| Gateway command-result timeout | 5 s for one outstanding gateway-originated command |

### Link Quality Weighting

The `link_quality` metric is the RSSI-derived score for one BLE mesh hop:

| RSSI | link_quality |
| --- | --- |
| ≤ **-100 dBm** | 1 (floor) |
| -99 .. **-41 dBm** | `RSSI + 100` (linear, range 1-59) |
| ≥ **-40 dBm** | 100 (ceiling) |

`link_quality=0` is reserved for unknown/not set, and values above 100 are rejected.

Route selection uses a single cost value. For gateway-bound paths, `hop_count` is the advertised distance from the next hop back to the gateway. A gateway route reply starts at `hop_count=0`, and each relay forwards it one hop higher.

```
effective_cost = hop_count * 100 + (100 - link_quality)
```

Lower cost wins. Because the quality penalty is 0-99 and each extra advertised hop adds 100, hop count is primary and quality only ranks candidates inside the same hop-count band.

Examples:

| Candidate | Cost | Result |
| --- | --- | --- |
| Direct gateway advertisement, weak RSSI: `hop_count=0`, `link_quality=1` | `0 * 100 + 99 = 99` | Still beats a relay path |
| Relay path that advertises `hop_count=1`, perfect quality | `1 * 100 + 0 = 100` | Loses to the direct route above |
| Two candidates with `hop_count=1`, qualities 80 and 30 | Costs 120 and 170 | Quality 80 wins |

The same score is used upstream and downlink. If two routes have the same cost, the firmware chooses better link quality, then lower hop count, then newer observation, then lower next-hop ID.

**Quality propagation through the mesh** uses the weakest link in the path. A route request or reply starts with quality 100. Each receiver compares that advertised path quality with the RSSI-derived quality of the local hop that just delivered the advertisement:

```
path_quality = min(advertised_path_quality, local_link_quality)
```

The receiver stores and forwards that path quality. If gateway-to-A is 72 and A-to-B is 45, B stores 45. Another path with the same hop count and quality 60 wins because it has lower cost.

### Route Discovery and State

Routing is on-demand:

1. A sender with no usable route advertises `ROUTE_REQ` with `INITIATOR_ID` set to itself and `RESPONDER_ID` set to the target.
2. Each receiver stores a reverse breadcrumb to the initiator through the previous hop.
3. Relays rebroadcast the request with lower TTL, higher hop count, and weakest-link quality.
4. The target sends `ROUTE_REPLY` back through the reverse path.
5. Each receiver of the reply installs a route to the target through the previous hop.
6. The original sender transmits the waiting data packet over a BLE connection to the selected next hop.

For anchor reports and command results, the target is the configured gateway. For gateway-originated commands, the target is the anchor ID. Upstream and downlink candidates older than 30 s are expired before routing decisions. v1 still has one configured gateway root for normal operation.

### Route Failure Behavior

Mesh ACK packets (`MSG_MESH_ACK` and `MSG_GATEWAY_ACK`) update pending transmissions and route freshness before normal packet custody decisions are made.

When an anchor sends a gateway-bound packet, the pending transmission enters a two-phase wait: first for the hop ACK from its next-hop relay, then for the end-to-end gateway ACK from the gateway.

**Hop ACK failure** — if the hop ACK deadline (500 ms + retry backoff) expires without an ACK, `route_record_failure()` increments the candidate's `failure_count`. The function returns one of three actions:

| Condition | Action |
| --- | --- |
| failure_count < 3 | Retry the same candidate |
| failure_count ≥ 3, alternative exists | Invalidate the candidate, switch to the next best route |
| failure_count ≥ 3, no alternative | Invalidate, stop the pending transmission, report route discovery needed |

If an alternative candidate is selected, the pending transmission is retransmitted through the new next hop with `failure_count` reset to 0. If no alternative exists and the original packet was a queued click report, it goes back into the report queue and triggers a new `ROUTE_REQ`.

**Gateway ACK failure** — if the hop ACK succeeds but the gateway ACK deadline (2 s) expires, the anchor records `ROUTE_FAILURE_GATEWAY_ACK` and follows the same retry logic. The report may have reached the gateway, but the return ACK was lost, so the sender retransmits rather than dropping the report.

**Success** — any successful hop ACK or gateway ACK calls `route_record_success()`, resetting `failure_count` to 0 and updating `last_seen_ms`. This keeps the route fresh and trusted.

Each node can track exactly one pending transmission at a time. When a relay is already awaiting a hop ACK or gateway ACK (`pending.state != TX_IDLE`), it cannot take custody of another packet that would require forwarding or a local response (`COMMAND_RESULT`, `GATEWAY_ACK`). The relay drops that packet silently: it sends no hop ACK and does not add the packet to the duplicate cache. The sender treats the silence as a missed ACK and retries later.

#### Duplicate Retry and Busy Relays

When a relay receives a directed-unicast retry whose identity (`msg_type`, `src_id`, `dst_id`, `session_id`, `seq`) already exists in its duplicate cache, it recognizes the retry as a repair attempt rather than a new packet:

- **Relay idle, route available** — the relay re-forwards the packet and sends a fresh hop ACK back to the previous hop. The duplicate cache prevents double delivery to the target.
- **Relay busy (tx in flight) or no route** — the relay drops the packet without a hop ACK, since it cannot honestly take custody.

For local or broadcast duplicates, the relay sends a fresh ACK if applicable but does not deliver the payload twice.

#### Downlink Delivery and Gateway Command Serialization

Commands flow from the gateway toward a target anchor through a flat downlink directory learned from `ROUTE_REPLY` packets (`target_id -> next_hop_id`). The gateway does not maintain a full topology map and does not flood operational commands.

If no downlink entry exists, the gateway advertises `ROUTE_REQ` and keeps the USB command pending. When the target's `ROUTE_REPLY` returns, the command is sent over the selected next-hop connection. For a command with a cached route, the gateway transmits to the cached next hop and retries missed hop ACKs up to three times with 100 ms, 250 ms, and 500 ms backoff. After three failures it invalidates the cached entry and starts discovery again while the 5 s command-result timeout remains the application-level limit.

The gateway serializes commands — it tracks exactly one outstanding command at a time. A second USB command is immediately rejected with `COMMAND_BUSY` until the first command is resolved. Resolution occurs when:

- A matching `COMMAND_RESULT` arrives from the target anchor,
- The cached next-hop entry is invalidated and discovery cannot complete before timeout, or
- The 5 s `GATEWAY_COMMAND_RESULT_TIMEOUT_MS` elapses.

A command result matches when its `src_id` equals the command's target anchor, `dst_id` equals the gateway, and both `session_id` and `seq` match the original command.

### Data Forwarding (every click)
Anchor-side UWB results are queued during the responder window. Each click distance report carries distance, quality, range status, and sample data. The first packet for an aggregated anchor-click measurement also carries the UWB received signal level in dBm. If all samples do not fit one connected mesh write, the remaining sample chunks are queued as additional report packets for the same anchor, clicker, and event sequence. After the DWM3000 returns to deep sleep, the anchor drains reports one at a time through the mesh. Each report waits for a hop ACK and then a gateway ACK. If all retries exhaust and the upstream route is lost, the report returns to the queue and starts reactive route discovery.

### Mesh Relay Hop Latency

Each operational relay hop uses the selected BLE connection. Discovery advertisements happen before the data packet is sent:

```
Route miss:
T=0ms      Sender advertises ROUTE_REQ for 250 ms
T≈scan     Relays forward ROUTE_REQ; target returns ROUTE_REPLY
T≈reply    Sender receives ROUTE_REPLY and opens/reuses next-hop connection

Data hop:
T=0ms      Sender writes mesh frame to next-hop GATT characteristic
T≈conn     Receiver decodes frame and accepts custody
T≈conn     Receiver writes MESH_ACK back over the connection
T≈conn     Receiver forwards data to its next hop if needed
```

The first packet after a route miss pays the advertisement discovery cost. Once a route and connection exist, each hop is bounded mainly by BLE connection scheduling plus GATT write handling. The 500 ms hop-ACK timeout remains conservative enough to cover low-duty scan discovery, connection establishment, and a missed connection event.

### Self-Healing
Route entries expire after 30 s without refresh. Active traffic can remove bad selected routes faster after three missed hop ACKs. Anchors requeue undelivered reports and start a new `ROUTE_REQ`; the gateway keeps a pending command while discovery runs and reports `COMMAND_TIMEOUT` only if no matching command result arrives within 5 s. Duplicate identities expire after 60 s.

### Click Priority Over Mesh Work

Anchor click handling has priority over relay traffic. In the scan parser, click wake advertisements are decoded before mesh discovery advertisements. When a valid wake request arrives, the anchor cancels active mesh forwarding, clears pending mesh RX work, and keeps any already-built local click report queued for later delivery. This prevents relay work from delaying READY advertising or the selected clicker's UWB responder window.

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

The target executes or rejects the command and returns a `COMMAND_RESULT`; that result uses the standard hop ACK plus gateway ACK delivery path.

For `CMD_GET_STATUS`, an anchor includes the normal role, uptime, and status fields plus mesh route telemetry. If a route is selected, the response includes `GATEWAY_ID`, `NEXT_HOP_ID`, `ROUTE_EPOCH`, `HOP_COUNT`, `QUALITY`, and `RETRY_COUNT`. If no upstream route is currently selected, the response includes `GATEWAY_ID` and `REASON=7`, where 7 maps to `PROTO_ERR_NOT_FOUND`.

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
Assumptions: 50 clicks/day, current firmware timing from `main.c`: 30 ms minimum UWB politeness sniff, 330 ms wake advertisement, 200 ms READY scan, up to 8 READY anchors, and one 50 ms DS-TWR retry window per selected anchor. The clicker shuts down BLE and returns the DWM3000 to deep sleep after the click window. Worst-case politeness can add up to 500 ms before BLE wake advertising.

#### Per-Click Energy

| Phase | Duration | Current | Charge (µA·s) |
| --------------------------------- | ------------------- | ------- | --------------- |
| MCU wake from deep sleep | 1 ms | 5 mA | 5 |
| UWB politeness sniff | 30 ms | 50 mA | 1,500 |
| BLE wake ADV window | 330 ms | 12 mA | 3,960 |
| BLE RX (collect addressed READY responses) | 200 ms | 9.3 mA | 1,860 |
| UWB radio wake/reset/configure | 10 ms | 20 mA | 200 |
| UWB TWR windows × 8 anchors | 8 × 50 ms = 400 ms | 50 mA | 20,000 |
| MCU processing + LED blink | 50 ms | 10 mA | 500 |
| **Total per click** | **~1.02 s** | | **28,025 µA·s** |

Note: 28,025 µA·s = approximately 0.0078 mAh per click. A worst-case 500 ms politeness wait adds roughly 0.0065 mAh per click.

#### Daily Energy

| Component | Calculation | mAh/day |
| --------------------------- | -------------- | --------- |
| Active clicks | 50 × 0.0078 mAh | 0.39 |
| Deep sleep (24h, MCU + UWB) | 2.86 µA × 24h | 0.069 |
| **Daily total** | | **0.46** |
| With ~3× safety margin | | **1.37** |

#### Battery Life Estimates (with 3× margin)

| Battery | Capacity | Estimated Life |
| ----------- | -------- | -------------- |
| LiPo 85 mAh | 85 mAh | ~60 days |

**Conclusion:** A 85 mAh LiPo is more than sufficient.

### Anchor Power Budget
Assumptions: 10% LE 1M BLE scan duty cycle (`300 ms` interval, `30 ms` window), 1000 ranging events/day (16 clickers × 50 clicks × 70% proximity, rounded up), and 1000 queued click-report mesh deliveries/day. A selected ranging event uses a 180 ms READY advertisement and a 500 ms UWB responder window. Mesh route discovery uses 250 ms connectable advertisements only when a route is missing; operational reports use BLE connection writes after the path is found. The connection-current numbers below are placeholders until hardware current is measured with the connection interval and scan duty active together.

#### Daily Consumption Breakdown 

| **Component** | **Calculation** | **mAh/day** | **% of total** |
| ---------------------------- | --------------------------------- | ----------- | -------------- |
| BLE scan baseline (10% duty) | 6.0 mA × 2.4h + 0.003 mA × 21.6h | 14.46 | 57% |
| Addressed READY advertisements | 1000 × 180ms × 12mA = 2,160 mAs | 0.60 | 2% |
| UWB responder windows | 1000 × 0.5s × 50mA = 25,000 mAs | 6.94 | 25% |
| BLE mesh route discovery worst case | 1000 × 250ms × 12mA = 3,000 mAs | 0.83 | 4% |
| BLE mesh connected report TX placeholder | 1000 × 30ms × 12mA = 360 mAs | 0.10 | <1% |
| **Daily total** | | **22.93** | |
| **With 1.5× safety margin** | | **34.40** | |

#### Battery Life Estimates (with 1.5× margin, 0.85 derating)

**Assumptions:**
- **Load:** 34.4 mAh/day (includes 1.5× safety margin).
- **Battery:** 18650 Li-Ion (3000 mAh nominal).
- **Configuration:** Batteries in parallel (capacity adds up).
- **Efficiency:** 0.85 (85% usable capacity).

| **Number of Batteries** | **Total Usable Capacity** | **Est. Days** | **Est. Months** | **Est. Years** |
| ----------------------- | ------------------------- | ------------- | --------------- | -------------- |
| **1 × 18650** | 2,550 mAh | **~74 days** | 2.5 months | 0.2 years |
| **2 × 18650** | 5,100 mAh | **~148 days** | 4.9 months | 0.4 years |
| **3 × 18650** | 7,650 mAh | **~222 days** | 7.4 months | 0.6 years |

**Observation:** BLE scanning remains the dominant anchor load. The next meaningful battery-life improvement is reducing scan duty cycle or making anchors wake from a scheduled sync rather than passive scanning.

---
## Firmware Architecture

### Shared Codebase

Both Clicker and Anchor run on identical hardware (ANNA-B402-00B + DWM3000). The role is determined by a firmware configuration flag.

**RTOS:** Zephyr (via nRF Connect SDK)

### Clicker State Machine

```
[DEEP_SLEEP]
 │
 ├──GPIO IRQ──► [UWB_POLITENESS] ──► [WAKE_ADV 330ms] ──► [READY_SCAN 200ms] ──► [UWB_RANGE]
 │                                                                         │
 ├──RTC──────► [BAT_CHECK] ──low battery──► [LOW_BAT_BLINK] ───────────────┤
 │                                                                         ▼
 └◄────────────────────────────── [CONFIRM_LED] ◄──── all anchors done ────┘
```

### Anchor State Machine

```
[BLE_SCANNING]──►RX clicker ADV──►READY + UWB WINDOW──►QUEUE TWR RESULT──►DWM3000 SLEEP
 ▲                                                                            │
 │                                                                            ▼
 └──────────────────────────────DRAIN REPORT QUEUE VIA MESH────────────────────┘

[BLE_SCANNING] ──► RX route discovery ADV ──► Forward route discovery ADV ──► [BLE_SCANNING]

[BLE_SCANNING] ──► RX mesh data on connection ──► Forward over next-hop connection ──► [BLE_SCANNING]

RX clicker ADV preempts mesh relay work; scanning duty stays active during mesh connections.
```

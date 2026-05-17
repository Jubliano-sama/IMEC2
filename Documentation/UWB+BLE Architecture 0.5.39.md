#internship #imec #architecture #documentation #UWB #BLE

Version: 0.5.39

Previous version: [[UWB+BLE Architecture 0.5.38]] Design rationale: [[UWB+BLE Design Story 0.1]] Component selection: [[Selecting a UWB and BLE Chip]], [[05-03-2026 Internship]]

## Changelog

### 2026-05-17 - 0.5.39

- Record the round-robin round as a clicker-transmitted DS-TWR range-header field, then echo and validate it through the exchange.
- Replace per-sample time offsets with direct per-sample anchor sequence start timestamps in gateway-bound click reports.
- Clarify that the gateway groups anchor reports by event and round, while anchors only report their own sequence start times and do not exchange those times with each other.
- Point routed-packet cross-references at Protocols 0.2.37 and state-flow references at Firmware State Machines 0.1.32.

### 2026-05-17 - 0.5.38

- Add per-sample click-report round indices and time offsets so each anchor range sample can be mapped to its round-robin round.
- Clarify that aggregated click-report chunks use the first sample's gateway-synchronized timestamp as the base for per-sample offsets.
- Point routed-packet cross-references at Protocols 0.2.36 and state-flow references at Firmware State Machines 0.1.31.

### 2026-05-17 - 0.5.37

- Set optimized DS-TWR delayed-RX preamble hunt timeout to zero while keeping frame-wait timeouts as the receive bound.
- Clarify that immediate/free-running RX paths keep a nonzero preamble timeout for low-duty wake and mesh scans.

### 2026-05-17 - 0.5.36

- Add gateway-initiated broadcast time synchronization so anchors align report timestamps to gateway uptime without depending on unique production IDs.
- Define the sync interval calculation: at a conservative 500 ppm oscillator bound, 60 s of allowed drift permits 33.3 h between syncs; firmware broadcasts sync hourly for margin.
- Clarify that click range and survey sample reports timestamp when the ranging exchange started using 64-bit gateway-synchronized `TIMESTAMP_MS`, with `TIME_SYNC_AGE_MS` marking freshness.
- Point routed-packet cross-references at Protocols 0.2.35 and state-flow references at Firmware State Machines 0.1.30.

### 2026-05-17 - 0.5.35

- Raise the Zephyr system workqueue stack budget for mesh relay RX, forwarding, ACK, and route-repair paths.
- Move long anchor survey pair runs to a dedicated survey worker so command handling, mesh RX, report draining, and abort commands remain responsive during high-sample surveys.
- Clarify that scheduled UWB listen deadlines are computed from 64-bit local uptime while 32-bit uptime remains a compact wire/status field.
- Keep deterministic anchor identity and discovery-slot assignment as a production assumption for this scale.

### 2026-05-17 - 0.5.34

- Add clicker-only BLE courtesy advertisements during UWB politeness to reduce exact simultaneous-click collisions.
- Use BLE channel 37 only for the courtesy path so short scans do not depend on BLE scan-channel rotation.
- Set the implemented BLE courtesy window to 75 ms before UWB contention/wake.
- Change competing-clicker arbitration to attempt-first precedence before priority ID and stable tie-breakers.
- Add a Monte Carlo estimate for two-clicker courtesy advertisement detection probability.

### 2026-05-17 - 0.5.33

- Set the implemented DWM3000 runtime PHY to 850 kbps for wake, discovery, ranging, and UWB mesh frames.
- Update DS-TWR timing notes for the 850 kbps frame-length delta and longer RX frame-wait timeouts.

### 2026-05-17 - 0.5.32

- Document the implemented sampled-politeness and randomized UWB contention strategy: 2 ms politeness samples every 25 ms, randomized 12 ms contention slots by attempt, 150 ms retry base delay, sub-millisecond wake-claim jitter, and 15 ms anchor claim collection.
- Recalculate the theoretical clicker and anchor power budgets from those implemented constants.
- Record that the board overlay now carries the real DWM3000 IRQ pin, `P0.02`.

### 2026-05-16 - 0.5.31

- Correct the routed-packet section cross-reference to Protocols 0.2.32 after the theoretical-power-only documentation bump.

### 2026-05-16 - 0.5.30

- Make the power budget explicitly theoretical-only: remove remaining measurement-dependent wording from the calculation sections, state the current/previous architecture assumptions, and point routed-packet cross-references at Protocols 0.2.31.

### 2026-05-16 - 0.5.29

- Point routed-packet cross-references at Protocols 0.2.30 and state-flow references at Firmware State Machines 0.1.28 after clarifying pre-DS-TWR abort completion when the required unique completed ranges already exist.

### 2026-05-16 - 0.5.28

- Point routed-packet cross-references at Protocols 0.2.29 and state-flow references at Firmware State Machines 0.1.27 after documenting self-test UWB mesh reporting and removing the remaining route-request advertising wording.

### 2026-05-16 - 0.5.27

- Replace earlier noncomputed anchor power rows with theoretical calculations from current firmware timing constants.
- Add a side-by-side theoretical power and success-probability comparison for the previous BLE-gated architecture versus the current UWB-gated architecture.
- Point routed-packet cross-references at Protocols 0.2.28.

### 2026-05-16 - 0.5.26

- Point routed-packet cross-references at Protocols 0.2.27 after clarifying the `DISCOVER` field summary.

### 2026-05-16 - 0.5.25

- Point routed-packet cross-references at Protocols 0.2.26 after correcting the gateway command wait diagram in Firmware State Machines 0.1.26.

### 2026-05-16 - 0.5.24

- Correct the self-healing route section to use the implemented 12 s gateway command-result timeout.
- Point routed-packet cross-references at Protocols 0.2.25.

### 2026-05-16 - 0.5.23

- Account for periodic anchor UWB mesh RX in the compile-time idle duty guard together with wake scanning; the current periodic mesh listen cadence is 2 ms every 6000 ms.
- Set the gateway command-result timeout to 12 s so one outstanding command can cover the anchor mesh RX cadence, gateway ACK timeout, and margin.
- Clarify that runtime `CMD_SET_SCAN_DUTY` changes are bounded by both duty-cycle and wake-overlap guards.
- Point routed-packet cross-references at Protocols 0.2.24 and state-flow references at Firmware State Machines 0.1.25.

### 2026-05-16 - 0.5.22

- Align the ranging schedule description with the firmware guard that requires at least 50 ms between scheduled DS-TWR polls, leaving room for the 40 ms response wait and guard margin.
- Point routed-packet cross-references at Protocols 0.2.23.

### 2026-05-16 - 0.5.21

- Clarify the duty-cycle scope: the firmware guard enforces the low-duty wake-scan baseline near the intended ~1% DWM3000 awake-time target, while periodic UWB mesh RX and active traffic are separately accounted in the theoretical power model.
- Point routed-packet cross-references at Protocols 0.2.22.

### 2026-05-16 - 0.5.20

- Remove the retained native legacy discovery advertising codecs from the current firmware architecture. Operational wake, discovery, schedule, ranging, and mesh traffic remain UWB-only; historical message IDs `0x01` and `0x02` are reserved but not built or decoded.
- Point routed-packet cross-references at Protocols 0.2.21.

### 2026-05-16 - 0.5.19

- Clarify that repeated `WAKE_CLAIM` frames advertise the remaining claimed duration from each frame, while the build-time guard covers the maximum first-frame click epoch.
- Point routed-packet cross-references at Protocols 0.2.20.

### 2026-05-16 - 0.5.18

- Document bounded `WAKE_CLAIM` timing fields and align the advertised click epoch duration with the current full scheduled range span.
- Clarify that accepted anchors can put the DWM3000 into retained sleep until the selected discovery time and between scheduled poll slots, with retained-sleep intervals excluded from awake-time diagnostics.
- Point routed-packet cross-references at Protocols 0.2.19.

### 2026-05-16 - 0.5.17

- Clarify that repeated `WAKE_CLAIM` frames for the same clicker/event/attempt/nonce/mode must keep the same arbitration priority; same-attempt priority drift is malformed and cannot refresh an anchor ownership epoch.
- Point routed-packet cross-references at Protocols 0.2.18.

### 2026-05-16 - 0.5.16

- Point routed-packet cross-references at Protocols 0.2.17 after making DS-TWR sequence `0` invalid in POLL/RESP/FINAL/REPORT exchanges and expected responder windows.

### 2026-05-16 - 0.5.15

- Point routed-packet cross-references at Protocols 0.2.16 after requiring survey DS-TWR session nonces to include the sample index, preventing identity reuse when long surveys wrap the compact 8-bit sequence.

### 2026-05-16 - 0.5.14

- Point routed-packet cross-references at Protocols 0.2.15 after clarifying that legacy BLE discovery message IDs are compatibility payloads only and are not emitted by the UWB-only firmware path.

### 2026-05-16 - 0.5.13

- Point routed-packet cross-references at Protocols 0.2.13 after documenting clicker pre-range abort handling in Firmware State Machines 0.1.24.
- Clarify that a clicker can abort a scheduled attempt before DS-TWR starts when the remaining click budget or radio access cannot safely start the next exchange; this is parent-flow retry handling, not a completed DS-TWR failure.

### 2026-05-16 - 0.5.12

- Define concrete UWB health meanings for `STATUS_BITS` in anchor status and heartbeat payloads. Anchors now expose whether wake scanning has run, wake decode failures occurred, claim collisions/lost arbitration occurred, DS-TWR failures occurred, timing rejections occurred, and UWB mesh frames were received.
- Point routed-packet cross-references at Protocols 0.2.12.

### 2026-05-16 - 0.5.11

- Remove the remaining generic broadcast POLL allowance from DS-TWR. Every POLL/RESP/FINAL/REPORT exchange must name one concrete responder ID and matching short address, including diagnostics, so the responder path cannot be driven by an unscheduled broadcast request.
- Point routed-packet cross-references at Protocols 0.2.11.

### 2026-05-16 - 0.5.10

- Bind DS-TWR POLL/RESP/FINAL/REPORT frames to the full 64-bit initiator and responder IDs in addition to short UWB addresses, `network_id`, event/session ID, nonce, sequence, and mode flags. This removes the lower-16-bit short-address collision weakness while preserving the scheduled, one-anchor-at-a-time ranging window.
- Point routed-packet cross-references at Protocols 0.2.10.

### 2026-05-16 - 0.5.9

- Document the implemented anchor heartbeat path: `CMD_START_HEARTBEAT` starts periodic `MSG_ANCHOR_HEARTBEAT` reports over UWB mesh with gateway ACK, `CMD_STOP_HEARTBEAT` cancels them, and heartbeat sends defer behind click epochs or active tracked mesh transmissions.
- Point routed-packet cross-references at Protocols 0.2.9.

### 2026-05-16 - 0.5.8

- Clean remaining high-level summary wording that still described the older BLE advertisement/READY flow. The implemented v1 path is UWB politeness, long-preamble wake claims, UWB discovery/schedule, serialized DS-TWR, and UWB mesh reports.
- Point routed-packet cross-references at Protocols 0.2.8.

### 2026-05-16 - 0.5.7

- Clarify wake-claim freshness: a newer retry attempt for the same clicker, event, nonce, and mode refreshes the anchor ownership epoch before ordinary competing-clicker priority arbitration; older attempts are stale.
- Clarify that timing-invalid completed DS-TWR exchanges keep their `RANGE_TIMING_INVALID` failure reason in report/survey payloads.
- Make the fixed DS-TWR reply-delay policy explicit: schedules are valid only with the firmware's 900 us equal reply delay.

### 2026-05-16 - 0.5.6

- Clarify that mesh preemption happens only after an anchor accepts a local `WAKE_CLAIM` into its ownership epoch. CRC-valid but rejected or foreign claims do not clear an existing epoch or interrupt mesh work.

### 2026-05-16 - 0.5.5

- Clean remaining current-section wording that implied advertising or connection-based routing. Survey reachability and reactive route discovery are described as UWB mesh traffic.

### 2026-05-16 - 0.5.4

- Make the DWM3000 IRQ path mandatory by design. Firmware requires `irq-gpios`, waits on GPIO IRQ events for radio completion, and intentionally fails Zephyr role builds until the final board IRQ pin is added to the overlay.

### 2026-05-16 - 0.5.3

- Replace BLE-gated wake and BLE mesh routing with UWB-gated wake, UWB discovery/schedule frames, selected-clicker-only DS-TWR, and UWB mesh relay frames.
- Assume the DWM3000 IRQ pin is available once the board pin is known. This assumption is now mandatory in firmware as of 0.5.4.
- Remove the Zephyr app Bluetooth stack dependency and keep legacy BLE discovery payloads only as native compatibility codecs.
- Update anchor duty-cycle assumptions to low-duty long-preamble UWB scan windows and document the current 400 ms scan interval, 1 ms RX window, and 430 ms clicker wake train deviation needed to preserve the intended ~1% DWM3000 awake-time target with measured wake/settle overhead.

### 2026-05-05 - 0.5.2

- Remove explicit mesh custody acknowledgement packets and their retry state. In the current UWB-only transport, UWB mesh TX/RX windows cover next-hop transfer, while gateway-bound traffic still waits for an end-to-end gateway ACK.

### 2026-05-05 - 0.5.1

- Add one first-path DWM3000 CIR accumulator sample to the first gateway-bound click range report packet, next to the once-per-aggregate UWB received signal level.

### 2026-05-05 - 0.5

Changes from version 0.4:

- **Ranging protocol**: Replaced the older BLE advertisement/READY flow with UWB politeness sniff, clicker-only BLE courtesy advertisements, repeated long-preamble `WAKE_CLAIM`, UWB discovery, `RANGE_SCHEDULE`, deterministic anchor arbitration, and serialized scheduled DS-TWR. Normal click requires 4 unique anchor ranges with up to 6 wake attempts within a 15 s budget. Failed DS-TWR exchanges are bounded so one failing anchor does not block timely attempts to others.
- **DS-TWR**: Replaced DW1000 measured exchange (~5.75 ms, 4-message poll/resp/final/report) with DWM3000 scheduled exchange using compact UWB short addresses plus full 64-bit initiator/responder IDs, STS mode 1 with SDC, preamble 64, PAC8, channel 5, and 850 kbps. Equal reply delays (900 uus) with shortest-common-delay second priority. Current DS-TWR frame sizes are POLL 41 B, RESPONSE 49 B, FINAL 53 B, REPORT 49 B before FCS.
- **Clicker self-test**: Added UWB politeness sniff, diagnostic UWB wake/discovery/schedule, and diagnostic UWB ranging with explicit LED failure codes (1–6 blinks). Self-test traffic uses separate diagnostic flags that never set the normal click flag.
- **Click failure indication**: Failed normal clicks now show a red blink code (1 = no anchor heard, 2 = insufficient ranges) instead of no indication.
- **UWB mesh routing**: Replaced periodic `ROUTE_ADV`/`ROUTE_STATUS` beaconing with on-demand `ROUTE_REQ`/`ROUTE_REPLY` discovery inside UWB mesh frames. Operational mesh packets and gateway ACKs travel over UWB frames, not BLE connections. Link quality weighting changed from floating-point RSSI normalization to integer `hop_count × 100 + (100 − quality)` with weakest-link quality propagation.
- **Mesh reliability**: Documented single-pending-TX custody rule, duplicate re-forward repair, 60 s duplicate cache, 30 s route freshness, and gateway command serialization with one outstanding command at a time.
- **Downlink delivery**: Gateway uses a flat next-hop directory from `ROUTE_REPLY` packets, not a full topology map. Failed downlink retries invalidate the cached entry and trigger rediscovery.
- **Click priority over mesh**: Anchors preempt active mesh forwarding and pending mesh RX only after accepting a local `WAKE_CLAIM` into the ownership epoch, keeping any already-built local click report queued for later delivery.
- **Report fragmentation**: Oversized aggregated click sample lists split across multiple UWB mesh report packets. UWB received signal level and one first-path CIR sample appear only once per aggregate.
- **DWM3000 runtime policy**: Documented channel 5, STS mode 1 with SDC, STS length 64, 850 kbps, SPI 2 MHz init / 32 MHz runtime on SPIM3. The driver requires `irq-gpios`, waits on GPIO IRQ events for radio completion, and parks the wake pin inactive at boot.
- **Hardware**: AP7354-30W5-7 LDO corrected to 150 mA max (was 200 mA). DWM3000 BOM description updated to note factory default channel 9 (7.9872 GHz) vs firmware channel 5 (6.5 GHz).
- **Timing corrections**: DS-TWR timing table rewritten to reflect DWM3000 firmware constants and overlap between RX-enable and turnaround delays. Poll TX timestamp read moved from Phase 1 to Phase 3 where it occurs in code. Removed imprecise "27 B @ 32 MHz" SPI estimate.
- **Anchor heartbeat**: Originally defined the protocol surface (`CMD_START_HEARTBEAT`, `CMD_STOP_HEARTBEAT`, `MSG_ANCHOR_HEARTBEAT`); firmware implementation is documented in 0.5.9 above.
- **Power budget**: Updated clicker budget for current firmware timing (sampled 2 ms politeness listens, 430 ms UWB wake train, UWB discovery/schedule, and 400 ms UWB range budget). Updated anchor budget for 400 ms low-duty UWB scan interval, 1 ms RX window, 15 ms claim collection, scheduled responder windows, and UWB mesh receive windows. Added gateway power budget.
- **State machines**: Added detailed Mermaid flowcharts for clicker normal-click end-to-end, UWB wake/discovery attempt, anchor arbitration, anchor UWB responder window, self-test, gateway command handling, mesh relay state machine, anchor upstream retry, gateway downlink retry, and initiator/responder DS-TWR.

### 2026-05-01 - 0.4

Initial documented version with DW1000 TWR measurements, periodic ROUTE_ADV/ROUTE_STATUS mesh, and 200 ms clicker advertisement.

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
- On press: sampled UWB politeness sniff plus clicker-only BLE courtesy advertisement/scan → randomized contention delay → long-preamble `WAKE_CLAIM` train → UWB discovery/schedule → serialized DS-TWR with selected responders → sleep.
- After the click or self-test active window, the clicker stops UWB activity and returns the DWM3000 to retained sleep before returning to idle.
- Self-test mode is entered with a long press followed by a short press.
- In self-test mode, the clicker verifies local modules and sends a diagnostic "dud" ranging request. This request must not be counted as a user click.
- Identity: Permanent 64-bit device ID
- Knows nothing about QOTD, office configuration

#### Anchor (Fixed Node)
- Mounted at known positions on the ceiling of the office.
- Runs low-duty long-preamble UWB wake scans for clicker `WAKE_CLAIM` frames.
- The DWM3000 is not an always-on receiver. Firmware parks the DWM3000 wake pin inactive at boot and wakes/configures the radio only for bounded UWB wake scans, click epochs, mesh windows, or survey windows.
- On clicker detection: accepts only a CRC-valid claim, locks one ownership epoch, arbitrates competing clickers, sends a discovery-slot reply, validates the range schedule, sleeps until selected polls, queues any range result for mesh delivery, then puts the DWM3000 into retained sleep before draining queued reports.
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

### Runtime Reliability Guards

Production firmware keeps system workqueue work bounded. Mesh receive, timeout, command, and report-drain work still use the Zephyr system workqueue, but that stack is sized for relay result buffers, packet forwarding, gateway ACKs, and driver/logging call frames. Long-running survey pair measurements run on a separate anchor survey worker so a 1000-sample responder survey cannot block command handling or the survey abort path.

Local UWB schedule deadlines use 64-bit uptime. Compact 32-bit uptime is still valid for status fields and wrap-safe protocol freshness helpers, but retained-sleep and listen deadlines are computed against non-wrapping local uptime so anchors keep honoring clicker discovery and scheduled ranging windows after long uptimes.

Device identities and anchor discovery slots remain deterministic deployment inputs. Production images must assign stable `DEVICE_ID`, `GATEWAY_ID`, and, where needed, `UWB_ANCHOR_SLOT` values during build/provisioning. The firmware keeps deterministic slots because anchors answer in scheduled UWB windows; removing fixed slot identity would add avoidable discovery overhead at the intended deployment scale.

---

## Ranging Protocol (Per Click Event)
The clicker discovers anchors dynamically on each press. Moving, adding, or removing anchors requires no clicker configuration change.

```
t=0ms     Button press wakes clicker
t=0-75ms+ Clicker listens for UWB quietness and advertises/scans BLE courtesy priority
t=30ms    Clicker sends repeated long-preamble WAKE_CLAIM frames for 430 ms
t≈scan    Anchors that accept a CRC-valid local claim create one ownership epoch
t≈wake    Clicker sends UWB DISCOVER and listens for static-slot DISCOVERY_REPLY frames
t≈reply   Clicker sends RANGE_SCHEDULE for selected anchors
t≈range   Clicker ranges one scheduled anchor at a time using DS-TWR
t=<15s    Click succeeds after 4 unique anchors range, or retries up to 6 wake attempts
```

Discovery replies are presence-only and never count as range measurements. Each anchor window is bounded so a failing anchor cannot block the others. The schedule poll spacing must be at least 50 ms, matching the current 40 ms clicker response wait plus guard margin. `WAKE_CLAIM` timing fields are bounded: wake-train and discovery-start offsets are at most 1000 ms, discovery cannot precede the wake-train end, and claimed duration is at most 2000 ms while covering both discovery and the scheduled range span. The current clicker advertises the remaining claimed duration from each repeated wake claim, so anchors that hear a later claim do not hold the epoch for the full first-frame wake-train duration. For the same clicker/event/nonce/mode, duplicate claims for the same attempt must keep the same priority, newer wake attempts replace older ownership attempts, and older attempts are rejected as stale. Competing clicker events arbitrate by higher attempt index first, then lower priority ID, lower clicker ID, and lower event ID.

### BLE Courtesy Side Channel

BLE courtesy is a clicker-to-clicker hint that runs only during UWB politeness. The advertisement carries `network_id`, full `clicker_id`, `click_event_id`, `attempt_index`, and `priority_id`. It does not wake anchors and does not authorize ranging. The current implementation keeps courtesy active for at least 75 ms before UWB contention/wake. If a clicker hears a higher-precedence peer, it defers the same UWB attempt for 75 ms and reruns the gate; after two courtesy defers it proceeds so BLE cannot starve the UWB path.

The courtesy path deliberately uses only BLE advertising channel 37. The clicker disables advertising channels 38 and 39 and asks the SoftDevice Controller to scan only channel 37. This avoids the short-window failure mode where a scanner rotates to the desired advertising channel only after the politeness phase has already ended. If the controller scan-channel-map command fails, the firmware skips BLE courtesy for that attempt and uses UWB politeness plus randomized contention only.

The BLE interval respects the BLE 5.x non-connectable advertising limit on the nRF52833 target: 20.0 to 20.625 ms plus the controller advertising delay. Passive scanning uses 20 ms windows every 25 ms. The BLE radio is stopped before the UWB wake-claim train begins.

### DWM3000 DS-TWR Exchange Budget

The firmware configures the DWM3000 with channel 5, preamble length 64, PAC8, STS mode 1 with SDC, STS length 64, and 850 kbps data rate for ranging and mesh. The long-preamble wake/discovery mode also uses 850 kbps so all operational UWB frames run at the same data rate. SPI runs at 2 MHz during reset/init then switches to 32 MHz on nRF52833 SPIM3. The UWB frame payload sizes are defined in `uwb.h`: POLL 41 B, RESPONSE 49 B, FINAL 53 B, and REPORT 49 B before the radio FCS. DS-TWR packets carry full 64-bit initiator and responder IDs in addition to compact short addresses, `network_id`, `session_nonce`, event/session ID, nonzero sequence, and flags. Broadcast POLL is not valid for this architecture; every exchange names one selected responder. This binds every exchange to one selected clicker/event/anchor without relying on lower-16-bit short-address uniqueness.

DS-TWR reply timing policy is equality first, shortest possible common delay second. The responder response delay and initiator final delay are both programmed to the same fixed 900 uus value; schedules with any other reply delay are rejected. The initiator's received response is 8 B longer than the responder's received poll. At 850 kbps, that extra receive path is `ceil(8 B * 8 bits * 1e6 / 850e3) = 76 us`, so the shorter poll path intentionally waits instead of making the two reply times differ.

The optimized DS-TWR legs that arm receive from a scheduled TX/RX offset use a zero preamble-detect timeout. On the DWM3000 this disables the separate preamble hunt timeout; the receive remains bounded by the configured frame-wait timeout because the expected responder, final, or report frame has a scheduled arrival window. Immediate and free-running receive paths, including wake and mesh scans, keep a nonzero preamble timeout so missed preambles do not burn the whole software window.

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

Clicker wake/discovery and mesh forwarding use DWM3000 UWB frames on the configured network ID. Wake/discovery uses the long-preamble PHY so sleeping anchors can catch claims during short scan windows; ranging and mesh packets use the normal channel-5 STS-SDC runtime PHY. Reactive mesh path discovery uses `ROUTE_REQ` and `ROUTE_REPLY` packets inside `UWB_MESH` frames. Once a path is found, mesh data packets, gateway ACKs, commands, and reports are sent as UWB mesh frames to the selected next hop.

The UWB mesh frame carries `network_id`, previous-hop ID, next-hop ID, a shared protocol packet, and a frame CRC. Anchors and the gateway schedule periodic UWB mesh receive windows; anchors skip mesh RX when a click epoch or tracked mesh TX is active.

Anchors keep low-duty UWB wake scanning near the intended ~1% DWM3000 awake-time target. The current firmware uses a 400 ms scan interval and a 1 ms RX window because the measured Zephyr/DWM3000 wake path includes startup/PLL overhead before RX. Clicker wake trains last 430 ms so every anchor scan window overlaps a valid claim. After an anchor accepts a selected claim, it can return the DWM3000 to retained sleep until the selected discovery time and again between its scheduled poll slots. These retained-sleep intervals keep the ownership epoch continuous but are excluded from awake-time diagnostics.

The periodic idle duty guard covers wake scanning plus periodic anchor UWB mesh RX. Current anchor mesh RX is 2 ms every 6000 ms. The theoretical power model below also includes event-driven route/report traffic under the normalized workload. Runtime `CMD_SET_SCAN_DUTY` commands are bounded by duty-cycle and wake-overlap guards: accepted scan intervals must still leave each 430 ms clicker wake train overlapping an anchor scan window. The theoretical tuning order is mesh RX interval/window first, then scan interval and clicker wake train together so wake overlap remains guaranteed.

UWB mesh receive is intentionally short on anchors so click wake claims can preempt relay work. The gateway uses longer UWB mesh RX windows because it is USB-powered.

### Mesh Packet Standard

All routed packets use a versioned binary envelope. The detailed field list is documented in [[UWB+BLE Protocols and Strategies 0.2.37]], but each packet contains:

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

### Gateway Delivery Confirmation

Every gateway-bound packet that matters sets:

- `GATEWAY_ACK_REQUIRED`: the gateway must send an end-to-end gateway ACK back to the original sender.

Reports, survey results, heartbeats, and command results require gateway ACKs. The original sender treats gateway-bound traffic as delivered only after the end-to-end gateway ACK returns. Next-hop transfer is a bounded UWB mesh TX/RX attempt. Gateway-originated commands complete when the target returns `COMMAND_RESULT`, or fail locally after the 12 s command-result timeout.

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

**Gateway ACK failure** — if the gateway ACK deadline (2 s) expires, the anchor records `ROUTE_FAILURE_GATEWAY_ACK`. `route_record_failure()` increments the candidate's `failure_count` and returns one of three actions:

| Condition | Action |
| --- | --- |
| failure_count < 3 | Retry the same candidate |
| failure_count ≥ 3, alternative exists | Invalidate the candidate, switch to the next best route |
| failure_count ≥ 3, no alternative | Invalidate, stop the pending transmission, report route discovery needed |

If an alternative candidate is selected, the pending transmission is retransmitted through the new next hop with `failure_count` reset to 0. If no alternative exists and the original packet was a queued click report, it goes back into the report queue and triggers a new `ROUTE_REQ`.

The report may have reached the gateway, but the return ACK was lost, so the sender retransmits rather than dropping the report.

**Success** — a successful UWB mesh transmit refreshes `last_seen_ms`; a matching gateway ACK also resets `failure_count` to 0. This keeps usable routes fresh without hiding repeated missing gateway confirmations.

Each node can track exactly one pending gateway-bound transmission at a time. When a relay is already awaiting a gateway ACK (`pending.state != TX_IDLE`), it cannot start another packet that would require forwarding or a local response (`COMMAND_RESULT`, `GATEWAY_ACK`). The relay drops that packet and does not add it to the duplicate cache. The previous sender relies on its UWB mesh transmit result and higher-level gateway confirmation to repair the path if needed.

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
Anchor-side UWB results are queued during the responder window. Each click distance report carries distance, quality, range status, and sample data. The first packet for an aggregated anchor-click measurement also carries the UWB received signal level in dBm and one raw first-path DWM3000 CIR accumulator sample. If all samples do not fit one UWB mesh frame, the remaining sample chunks are queued as additional report packets for the same anchor, clicker, and event sequence. After the DWM3000 returns to retained sleep, the anchor drains reports one at a time through the UWB mesh. Each report waits for a gateway ACK. If all retries exhaust and the upstream route is lost, the report returns to the queue and starts reactive route discovery.

### Mesh Relay Hop Latency

Each operational relay hop uses the selected UWB mesh next hop. Route discovery happens before the data packet is sent:

```
Route miss:
T=0ms      Sender emits UWB_MESH ROUTE_REQ
T≈rx       Relays forward ROUTE_REQ; target returns ROUTE_REPLY
T≈reply    Sender receives ROUTE_REPLY and uses the selected next hop

Data hop:
T=0ms      Sender transmits UWB_MESH data frame
T≈rx       Receiver decodes frame
T≈rx       Receiver forwards data to its next hop if needed
```

The first packet after a route miss pays the route-discovery cost. Once a route exists, each hop is bounded mainly by the UWB mesh TX/RX window cadence.

### Self-Healing
Route entries expire after 30 s without refresh. Repeated missing gateway ACKs can invalidate a selected upstream route and start new discovery. Anchors requeue undelivered reports and start a new `ROUTE_REQ`; the gateway keeps a pending command while discovery runs and reports `COMMAND_TIMEOUT` only if no matching command result arrives within 12 s. Duplicate identities expire after 60 s.

### Click Priority Over Mesh Work

Anchor click handling has priority over relay traffic. During wake scanning, CRC-valid `WAKE_CLAIM` frames are decoded before mesh frames. The anchor cancels active mesh forwarding and clears pending mesh RX work only after the claim is accepted for the configured network, channels, flags, and ownership epoch. CRC-valid but rejected or foreign claims do not clear an existing epoch and do not interrupt mesh work. Already-built local click reports stay queued for later delivery. This prevents relay work from delaying discovery-slot replies or the selected clicker's scheduled UWB responder window while keeping unrelated UWB traffic from disrupting mesh delivery.

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

For `CMD_GET_STATUS`, an anchor includes the normal role, uptime, gateway-synchronized timestamp, sync age, and status fields plus mesh route telemetry. `STATUS_BITS` summarizes UWB health from the anchor diagnostics counters and time-sync state: wake scanning active, wake decode failures, claim collisions/lost arbitration, DS-TWR failures, timing rejections, UWB mesh RX activity, time synced, and time sync stale. If a route is selected, the response includes `GATEWAY_ID`, `NEXT_HOP_ID`, `ROUTE_EPOCH`, `HOP_COUNT`, `QUALITY`, and `RETRY_COUNT`. If no upstream route is currently selected, the response includes `GATEWAY_ID` and `REASON=7`, where 7 maps to `PROTO_ERR_NOT_FOUND`.

`CMD_START_HEARTBEAT` starts periodic `MSG_ANCHOR_HEARTBEAT` packets from the target anchor to the gateway. The command may include `DURATION_MS` as the interval; if omitted, firmware uses 60 s. The accepted software range is 5 s to 1 h. Heartbeats carry the same role, battery, uptime, status, and route telemetry TLVs used by `CMD_GET_STATUS`, require gateway ACK, and are sent only when the anchor is not inside a selected click epoch or another tracked mesh transmission. `CMD_STOP_HEARTBEAT` cancels the periodic work.

Unsupported commands must return `UNSUPPORTED_COMMAND`, not be silently ignored.

### Gateway Time Synchronization

The gateway periodically broadcasts `CMD_SYNC_TIME` over UWB mesh with `dst_id=broadcast` and a 64-bit `TIMESTAMP_MS` equal to gateway uptime in milliseconds at send time. Anchors accept this broadcast without returning command results, store a local offset from their own non-wrapping uptime to gateway uptime, and forward the broadcast through the mesh like survey reachability broadcasts. This keeps sync independent of unique per-anchor production IDs and avoids a command-result storm when many anchors hear the same sync.

Click range reports, diagnostic range reports, survey pair sample results, `CMD_GET_STATUS` responses, and anchor heartbeats carry `TIMESTAMP_MS` as the gateway-synchronized timestamp for when the relevant measurement or status snapshot was taken. `TIME_SYNC_AGE_MS` reports how old the anchor's last sync was at that timestamp. If an anchor has not synced yet, it reports local uptime as `TIMESTAMP_MS`, sets `TIME_SYNC_AGE_MS` to `UINT32_MAX`, and marks time sync stale in `STATUS_BITS`.

The clicker writes the round-robin round into the DS-TWR range header for every scheduled poll. Anchors validate that field against the accepted schedule, echo it through the DS-TWR response/report path, and store it with the resulting sample.

Aggregated click reports also carry `RANGE_ROUND_INDICES` and `SEQUENCE_START_TIMESTAMPS_MS` arrays aligned with `DISTANCE_SAMPLES_MM`. For each report chunk, `TIMESTAMP_MS` is the gateway-synchronized start time of the first distance sample in that chunk, and `SEQUENCE_START_TIMESTAMPS_MS` carries the direct per-sample start timestamps from that anchor. The gateway groups samples by `(clicker_id, event_seq, round_index)` and can average the respective anchor start times for each round even when several round robins occur for one click. Anchors do not exchange start times with each other.

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
8. The participating anchors perform exactly `n` DS-TWR measurements on the dedicated survey worker.
9. Each sample is reported to the gateway as diagnostic survey data.
10. Gateway schedules the next pair or aborts the survey.

During a survey pair run, the system command/mesh work stays available. If the gateway sends `CMD_SURVEY_ABORT`, the target anchor records the abort request immediately and the survey worker exits at the next sample or bounded responder-listen check.

Each survey DS-TWR sample derives its session nonce from the pair identity and sample index. This keeps `(survey_id, nonce, sequence, initiator, responder)` unique even when long surveys wrap the 8-bit DS-TWR sequence value.

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
Assumptions: 50 clicks/day and current firmware timing from `main.c`: two quiet 2 ms UWB politeness samples in the normal quiet case, 25 ms quiet politeness sample period, 75 ms busy politeness sample period after activity, 500 ms maximum politeness wall-clock wait, 75 ms minimum BLE courtesy scan/advertise during normal-click politeness, randomized contention sleep before the wake train, 430 ms UWB wake-claim train, UWB discovery/reply/schedule exchange, up to 8 scheduled anchors, and two 50 ms-spaced DS-TWR samples per selected anchor. The clicker returns the DWM3000 to retained sleep after each active window. Retry and contention backoff are mostly sleep time, not UWB receive time.

#### Per-Click Energy

| Phase | Duration | Current | Charge (µA·s) |
| --------------------------------- | ------------------- | ------- | --------------- |
| MCU wake from deep sleep | 1 ms | 5 mA | 5 |
| UWB sampled politeness | 2 × (2.67 ms startup/PLL + 2 ms RX) = 9.34 ms active | 50 mA | 467 |
| BLE courtesy scan/advertise | 75 ms wall time, 20/25 ms passive scan duty plus about three 1 ms TX events | 5 mA active | 315 |
| UWB WAKE_CLAIM train | 430 ms | 50 mA | 21,500 |
| UWB discovery/reply/schedule | ~80 ms | 50 mA | 4,000 |
| UWB radio wake/reset/configure | 10 ms | 20 mA | 200 |
| UWB scheduled TWR samples | 8 anchors × 2 samples × 50 ms spacing = 800 ms | 50 mA | 40,000 |
| MCU processing + LED blink | 50 ms | 10 mA | 500 |
| **Total per click** | **~1.45 s wall time in the normal quiet case** | | **66,987 µA·s** |

Note: 66,987 µA·s = approximately 0.0186 mAh per click. A worst-case 500 ms busy sampled-politeness wait now uses about seven UWB samples, or `7 × (2.67 ms startup/PLL + 2 ms RX) × 50 mA = 1,635 µA·s = 0.00045 mAh` of UWB receive energy, plus roughly `500 ms × 80% × 5 mA + 20 × 1 ms × 5 mA = 2,100 µA·s = 0.00058 mAh` for BLE courtesy scan/advertise. That combined worst-case politeness energy remains far below `500 ms × 50 mA = 25,000 µA·s = 0.0069 mAh` for a continuous UWB listen.

#### Daily Energy

| Component | Calculation | mAh/day |
| --------------------------- | -------------- | --------- |
| Active clicks | 50 × 0.0186 mAh | 0.93 |
| Deep sleep (24h, MCU + UWB) | 2.86 µA × 24h | 0.069 |
| **Daily total** | | **1.00** |
| With ~3× safety margin | | **3.00** |

#### Battery Life Estimates (with 3× margin)

| Battery | Capacity | Estimated Life |
| ----------- | -------- | -------------- |
| LiPo 85 mAh | 85 mAh | ~28 days |

**Conclusion:** A 85 mAh LiPo is more than sufficient.

### Anchor Power Budget
Assumptions: low-duty DWM3000 wake scanning (`400 ms` interval, `1 ms` RX window plus 2.5 ms startup and 0.17 ms PLL overhead), periodic anchor UWB mesh RX (`6000 ms` interval, `2 ms` RX window plus the same startup/PLL overhead), 1000 selected ranging events/day (16 clickers × 50 clicks × 70% proximity, rounded up), and 1000 queued click-report mesh deliveries/day. A selected ranging event uses UWB discovery/schedule frames and scheduled DS-TWR responder windows. Mesh route discovery and operational reports use UWB mesh frames.

This is a theoretical radio-dominant budget from the current firmware constants. Every line below is calculated from an explicit duration, current draw, and event-rate assumption.

#### Daily Consumption Breakdown

| **Component** | **Calculation** | **mAh/day** | **% of total** |
| ---------------------------- | --------------------------------- | ----------- | -------------- |
| UWB wake scan baseline | 3.67ms awake / 403.67ms period = 0.909%; 785.5s/day × 50mA | 10.91 | 68.5% |
| Periodic UWB mesh RX baseline | 4.67ms awake / 6004.67ms period = 0.0778%; 67.2s/day × 50mA | 0.93 | 5.8% |
| UWB discovery/schedule control | 1000 × (15ms claim collection + 16ms discovery listen + 20ms reply TX + 80ms schedule RX) × 50mA | 1.82 | 11.4% |
| UWB responder windows | 1000 × (2 samples × 70ms scheduled listen window) × 50mA | 1.94 | 12.2% |
| UWB mesh report TX | 1000 × (2.67ms startup/PLL + 20ms TX timeout) × 50mA | 0.31 | 1.9% |
| **Daily total** | Theoretical radio budget before margin | **15.92** | **100%** |
| **With 1.5× safety margin** | 15.92mAh/day × 1.5 | **23.88** | |

A route miss that adds one extra `ROUTE_REQ` transmit costs another 0.31mAh per 1000 reports. A worst-case "every report starts with route discovery" day is therefore about 16.23mAh before margin, or 24.35mAh/day with the same 1.5× margin.

#### Battery Life Estimates (with 1.5× margin, 0.85 derating)

**Assumptions:**
- **Load:** 23.88 mAh/day theoretical anchor radio budget after 1.5× safety margin.
- **Battery:** 18650 Li-Ion (3000 mAh nominal).
- **Configuration:** Batteries in parallel (capacity adds up).
- **Efficiency:** 0.85 (85% usable capacity).

| **Number of Batteries** | **Total Usable Capacity** | **Est. Days** | **Est. Months** | **Est. Years** |
| ----------------------- | ------------------------- | ------------- | --------------- | -------------- |
| **1 × 18650** | 2,550 mAh | 106.8 | 3.5 | 0.29 |
| **2 × 18650** | 5,100 mAh | 213.6 | 7.0 | 0.59 |
| **3 × 18650** | 7,650 mAh | 320.4 | 10.5 | 0.88 |

**Observation:** Low-duty UWB wake scanning is the dominant theoretical anchor budget. The current firmware keeps normal idle scan plus periodic UWB mesh RX at 0.987% DWM3000 awake time, just under the intended ~1% periodic idle target. Active route/report traffic is event-driven and adds about 4.07mAh/day under the 1000 selected-events/day assumption.

### Previous BLE-Gated Versus Current UWB-Gated Architecture

This comparison normalizes both designs to the same functional target: 50 clicker events/day, up to 8 reachable anchors, 2 UWB samples per selected anchor, and 1000 selected anchor events/day. It uses 50mA for active UWB and 6mA for active BLE scan/advertise. The previous prototype code used 10% BLE idle scanning, 1000ms request advertising, 2000ms continuous READY collection, a 120s first-poll responder deadline, and 2000ms post-range responder idle timeout. The 1000-exchange measurement loop in the prototype is not used because it was a test mode, not a fair production click workload; the table below is a theoretical production-workload calculation.

#### Theoretical Power Comparison

| Role / Design | Calculation | Theoretical Result |
| --- | --- | --- |
| Previous BLE-gated clicker | 3s BLE exchange × 6mA + 3.8s UWB awake × 50mA | 0.0578mAh/click; 2.89mAh/day at 50 clicks |
| Current UWB-gated clicker | Current clicker budget above | 0.0186mAh/click; 0.93mAh/day active-click energy |
| Previous BLE-gated anchor idle | 10% BLE scan × 6mA × 24h | 14.40mAh/day before any UWB ranging |
| Previous BLE-gated selected anchor event | 3s BLE coordination wait + 2 UWB samples + 2s post-range idle = 5.1s UWB awake × 50mA | 0.0708mAh/event; 70.83mAh/day at 1000 events |
| Previous BLE-gated anchor total | 14.40mAh BLE idle + 70.83mAh selected-event UWB | 85.23mAh/day, or 127.85mAh/day with 1.5× margin |
| Current UWB-gated anchor total | Calculated anchor budget above | 15.92mAh/day, or 23.88mAh/day with 1.5× margin |

The current UWB-gated anchor is theoretically about 5.4× lower power than the previous BLE-gated anchor under the normalized 1000 selected-events/day load. The difference is mainly the old responder behavior: after a BLE request it keeps the UWB radio available through a long first-poll deadline and a 2s post-range idle tail. In the current architecture the anchor sleeps until scheduled polls and uses bounded UWB windows.

For battery sizing with the same 0.85 derated 18650 capacity, the previous BLE-gated anchor budget would last about 19.9 days on one cell, 39.9 days on two cells, and 59.8 days on three cells after the 1.5× margin. The current UWB-gated anchor estimate is about 106.8, 213.6, and 320.4 days respectively.

#### Theoretical Success Probability

Let `p_ble` be the probability that one BLE advertisement is decoded when the scanner is awake, `p_claim` the probability that one UWB `WAKE_CLAIM` is decoded during an overlapping wake scan, `p_discovery` the probability that a selected discovery reply is exchanged, and `p_twr` the probability that one DS-TWR sample completes.

Previous BLE-gated wake has strong phase coverage because the 1000ms request advertisement spans the 1000ms anchor scan interval. Its request detection probability is therefore RF-loss dominated: `P_request = 1 - (1 - p_ble)^N_request`, where `N_request` is the number of request advertisements that land in the anchor scan window. READY collection is also RF-loss dominated because the clicker scans continuously for 2000ms: `P_ready = 1 - (1 - p_ble)^N_ready`. A normalized two-sample per-anchor success estimate is `P_anchor_old = P_request × P_ready × (1 - (1 - p_twr)^2)`.

Current UWB-gated wake has deterministic phase coverage because the 430ms `WAKE_CLAIM` train is longer than the 403.67ms anchor scan period. Under perfect RF and no scheduling gaps, every anchor gets at least one chance to hear the claim. With RF loss, `P_claim = 1 - (1 - p_claim)^N_claim`, where `N_claim >= 1` by timing design. A selected per-anchor success estimate is `P_anchor_current = P_claim × p_discovery × (1 - (1 - p_twr)^2)`.

The current firmware adds collision desynchronization before and during the wake phase. During politeness, the clicker advertises and scans BLE courtesy priority on channel 37 only. If it hears a higher-precedence clicker, it waits 75ms and reruns the same attempt gate. Before each wake train, the clicker sleeps for a random 12ms slot: attempt 1 uses 16 slots (`0-180ms`), attempt 2 uses 32 slots (`0-372ms`), and attempt 3+ uses 64 slots (`0-756ms`). Retries add a 150ms base delay plus a new politeness/courtesy gate and the same randomized contention window for the next attempt. During a wake train, repeated `WAKE_CLAIM` frames use only sub-millisecond random jitter (`0-400us`) so exact simultaneous transmitters can drift apart without intentionally creating multi-millisecond holes in the long-preamble coverage. After an anchor receives a valid claim, it keeps a 15ms claim-collection window open so a nearly simultaneous higher-precedence claim can still be considered. For two clickers pressed at the same instant, the probability that they pick the same first-attempt contention slot is `1/16`; if they remain tied, the next retry slot match probability is `1/32`, then `1/64`.

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

The previous BLE-gated path can have excellent single-anchor wake probability in clean RF, but its probability model is weaker at the full system level because BLE request/READY discovery is decoupled from UWB ownership and scheduling. Multiple clickers, hidden devices, delayed responders, or stale READY advertisements can leave anchors awake for long periods or produce poorly coordinated UWB attempts. The current UWB-gated path has a higher per-wake energy cost than one BLE scan window, but every operational frame is network-, nonce-, event-, mode-, and selected-device-bound; anchors lock to one clicker/event epoch; and hidden clickers can cause retries without corrupting DS-TWR.

#### Pros And Cons

| Architecture | Pros | Cons |
| --- | --- | --- |
| Previous BLE-gated | Low-cost BLE scanning, mature BLE receiver behavior, request advertisement covers the 1s scan phase, DWM can stay in deep sleep until BLE request | High anchor event energy from long UWB responder deadlines, BLE discovery is not the ranging identity authority, hidden/multiple clicker coordination is weak, operational mesh/routing needs another transport path, old responder can burn 120s of UWB listen time if a request is seen but no addressed poll arrives |
| Current UWB-gated plus BLE courtesy | One radio/protocol family owns wake, discovery, ranging, reports, and mesh; deterministic wake-train overlap; CRC and full identity checks before state creation; selected-clicker-only DS-TWR; bounded scheduled windows; software idle UWB duty stays near 1%; BLE courtesy reduces exact simultaneous-click collisions before UWB TX | Requires reliable DWM3000 IRQ wiring, UWB wake-scan link budget must be validated on hardware, idle UWB scan dominates battery budget, BLE courtesy depends on nRF scan-channel-map support and is advisory only, active mesh traffic must be tuned against click priority |

---
## Firmware Architecture

### Shared Codebase

Both Clicker and Anchor run on identical hardware (ANNA-B402-00B + DWM3000). The role is determined by a firmware configuration flag.

**RTOS:** Zephyr (via nRF Connect SDK)

### Clicker State Machine

```
[DEEP_SLEEP]
 │
 ├──GPIO IRQ──► [UWB_POLITENESS] ──► [WAKE_CLAIM 430ms] ──► [DISCOVER/SCHEDULE] ──► [UWB_RANGE]
 │                                                                         │
 ├──RTC──────► [BAT_CHECK] ──low battery──► [LOW_BAT_BLINK] ───────────────┤
 │                                                                         ▼
 └◄────────────────────────────── [CONFIRM_LED] ◄──── all anchors done ────┘
```

### Anchor State Machine

```
[UWB_WAKE_SCAN]──►RX WAKE_CLAIM──►DISCOVERY + SCHEDULED UWB WINDOW──►QUEUE TWR RESULT──►DWM3000 SLEEP
 ▲                                                                            │
 │                                                                            ▼
 └──────────────────────────────DRAIN REPORT QUEUE VIA MESH────────────────────┘

[UWB_MESH_RX] ──► RX route discovery frame ──► Forward route discovery frame ──► [UWB_MESH_RX]

[UWB_MESH_RX] ──► RX mesh data frame ──► Forward over next-hop UWB frame ──► [UWB_MESH_RX]

Accepted WAKE_CLAIM preempts mesh relay work; anchors skip mesh RX while a click epoch is active.
```

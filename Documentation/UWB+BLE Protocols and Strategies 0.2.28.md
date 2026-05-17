#internship #imec #protocol #documentation #UWB #BLE

# UWB+BLE Protocols and Strategies

Version: 0.2.28

Previous version: [[UWB+BLE Protocols and Strategies 0.2.27]]

This document defines the v1 wire protocol: binary packet formats, message types, TLVs, and forwarding rules. System architecture, timing budgets, power estimates, and state machine flows are in [[UWB+BLE Architecture 0.5.27]]. Runtime behavior and decision flows are in [[Firmware State Machines 0.1.26]].

## Changelog

### 2026-05-16 - 0.2.28

- Point protocol cross-references at Architecture 0.5.27 after replacing placeholder power rows with theoretical calculations and adding the BLE-gated versus UWB-gated comparison.
- Clarify that `SELF_TEST_REPORT` is emitted by the clicker as diagnostic gateway-bound mesh traffic.

### 2026-05-16 - 0.2.27

- Add `discovery_slot_count` to the `DISCOVER` frame field summary and point protocol cross-references at Architecture 0.5.26.

### 2026-05-16 - 0.2.26

- Point protocol cross-references at Architecture 0.5.25 and Firmware State Machines 0.1.26 after correcting the gateway command wait diagram.

### 2026-05-16 - 0.2.25

- Correct the gateway command timeout wording to the implemented 12 s command-result wait.
- Point protocol cross-references at Architecture 0.5.24.

### 2026-05-16 - 0.2.24

- Clarify that `CMD_SET_SCAN_DUTY` is accepted only within the firmware duty-cycle and wake-overlap limits.
- Point protocol cross-references at Architecture 0.5.23 and Firmware State Machines 0.1.25.

### 2026-05-16 - 0.2.23

- Require `RANGE_SCHEDULE.poll_spacing_ms` to be at least 50 ms so a failed or late anchor exchange remains inside one scheduled slot and cannot block the following anchor.
- Point protocol cross-references at Architecture 0.5.22.

### 2026-05-16 - 0.2.22

- Point protocol cross-references at Architecture 0.5.21 after clarifying wake-scan versus UWB mesh RX duty-cycle accounting.

### 2026-05-16 - 0.2.21

- Retire the legacy discovery advertising payload codecs from the native firmware core. Message IDs `0x01` and `0x02` remain reserved historical values, but v1 firmware does not emit, decode, or test them.
- Point protocol cross-references at Architecture 0.5.20.

### 2026-05-16 - 0.2.20

- Clarify that each repeated `WAKE_CLAIM` advertises the remaining claimed duration from that frame: remaining wake train plus discovery, schedule reception, and the full scheduled range span.
- Point protocol cross-references at Architecture 0.5.19.

### 2026-05-16 - 0.2.19

- Bound `WAKE_CLAIM` timing fields so a CRC-valid but unreasonable claim cannot hold an anchor epoch for an unbounded sleep: wake train and discovery start offsets are limited to 1000 ms, claimed duration is limited to 2000 ms, discovery cannot precede wake-train end, and claimed duration must cover both.
- Point protocol cross-references at Architecture 0.5.18.

### 2026-05-16 - 0.2.18

- Require repeated `WAKE_CLAIM` frames for the same clicker/event/attempt/nonce/mode to keep the same `priority_id`; same-attempt priority drift is malformed and cannot refresh an anchor epoch.
- Point protocol cross-references at Architecture 0.5.17.

### 2026-05-16 - 0.2.17

- Require DS-TWR sequence `0` to be invalid in POLL/RESP/FINAL/REPORT exchanges; scheduled click and survey ranging use concrete nonzero sequence values, so no wildcard sequence can enter an expected responder window.
- Point protocol cross-references at Architecture 0.5.16.

### 2026-05-16 - 0.2.16

- Require anchor self-distance survey DS-TWR samples to derive the DS-TWR session nonce from the pair identity and sample index, so sample identities stay fresh even when the 8-bit DS-TWR sequence number wraps during long surveys.
- Point protocol cross-references at Architecture 0.5.15.

### 2026-05-16 - 0.2.15

- Tighten legacy BLE discovery wording so message IDs `0x01` and `0x02` are clearly retained compatibility payloads, not active wake, discovery, or mesh transport in the UWB-only firmware path.
- Point protocol cross-references at Architecture 0.5.14.

### 2026-05-16 - 0.2.14

- Clarify that a same clicker/event claim with a changed nonce or mode flag is malformed and cannot win priority arbitration; only matching network/clicker/event/nonce/mode retries use attempt freshness.

### 2026-05-16 - 0.2.13

- Point protocol cross-references at Architecture 0.5.13 and Firmware State Machines 0.1.24 after documenting clicker pre-range abort handling.
- Clarify that pre-range attempt aborts are outside DS-TWR result accounting; only completed DS-TWR exchanges produce range success or failure status.

### 2026-05-16 - 0.2.12

- Define the UWB health bit layout for `STATUS_BITS` in `CMD_GET_STATUS` and `MSG_ANCHOR_HEARTBEAT` payloads.
- Point protocol cross-references at Architecture 0.5.12.

### 2026-05-16 - 0.2.11

- Remove the remaining DS-TWR broadcast POLL allowance. POLL frames must carry one concrete responder ID and matching short address; diagnostics use the same selected-responder framing as normal clicks.
- Point protocol cross-references at Architecture 0.5.11.

### 2026-05-16 - 0.2.10

- Add full 64-bit `initiator_id` and `responder_id` fields to DS-TWR POLL/RESP/FINAL/REPORT headers. Frames still carry compact short addresses for the radio path, but full IDs are validated with `network_id`, event/session ID, nonce, sequence, and flags to avoid short-address collision ambiguity.
- Point protocol cross-references at Architecture 0.5.10.

### 2026-05-16 - 0.2.9

- Document `CMD_START_HEARTBEAT` interval handling: optional `DURATION_MS`, 60 s default, and 5 s to 1 h accepted firmware range. `MSG_ANCHOR_HEARTBEAT` uses the normal gateway ACK path over UWB mesh.
- Point protocol cross-references at Architecture 0.5.9.

### 2026-05-16 - 0.2.8

- Point protocol cross-references at Architecture 0.5.8 after cleaning the final BLE advertisement/READY wording from the implemented UWB-only flow.

### 2026-05-16 - 0.2.7

- Clarify that same clicker/event/nonce/mode wake retries use attempt freshness before priority arbitration: newer attempts refresh the epoch and older attempts are stale.
- Clarify that `RANGE_TIMING_INVALID` is valid in report and survey payloads so completed-but-rejected DS-TWR exchanges preserve their failure reason.
- Require `RANGE_SCHEDULE.reply_delay_us` to be the fixed firmware value, 900 us.

### 2026-05-16 - 0.2.6

- Clarify that click-priority mesh preemption follows local `WAKE_CLAIM` acceptance. CRC-valid claims that fail network, channel, flag, freshness, or arbitration checks do not preempt mesh work.

### 2026-05-16 - 0.2.5

- Update cross-references after the architecture and state-machine wording cleanup. Clarify that route discovery requests are sent inside UWB mesh frames, not advertised.

### 2026-05-16 - 0.2.4

- Point protocol cross-references at the mandatory-IRQ architecture version after making the DWM3000 IRQ path required by design.

### 2026-05-16 - 0.2.3

- Move operational wake, discovery, ranging schedules, and mesh relay transport to UWB frames. BLE discovery payloads remain documented only as legacy compatibility payloads and are not part of the operational mesh path.
- Add nonce/network-tagged `WAKE_CLAIM`, `DISCOVER`, `DISCOVERY_REPLY`, `RANGE_SCHEDULE`, and `UWB_MESH` frames, plus `RANGE_TIMING_INVALID`.
- Replace legacy GATT mesh transport language with UWB mesh frame forwarding and UWB route-listen windows.
- Remove the Zephyr app dependency on legacy BLE discovery payloads; the BLE codec remains native compatibility coverage only.
- Reserve legacy `ROUTE_ADV`/`ROUTE_STATUS` message values and reject `RANGE_SCHEDULE` entries whose per-anchor DS-TWR sequence range would wrap.
- Require scheduled DS-TWR POLL frames to target the selected anchor short address; broadcast POLL is not valid for scheduled ranging.
- Require UWB wake, discovery, schedule, and DS-TWR session frames to carry exactly one mode bit: `DIAGNOSTIC` or `COUNT_AS_CLICK`. Frames with neither or both are invalid.

### 2026-05-05 - 0.2.2

- Remove the explicit mesh custody acknowledgement packet, the request flag for it, and retry rules tied to that packet. In the current UWB-only transport, UWB mesh TX/RX windows cover next-hop transfer; `GATEWAY_ACK` remains the end-to-end confirmation for gateway-bound traffic.

### 2026-05-05 - 0.2.1

- Add the `UWB_CIR_SAMPLE` TLV to click range reports. It carries one raw DWM3000 Ipatov accumulator complex sample from the integer first-path index and is included only on the first packet of an aggregated or fragmented range report.

### 2026-05-05 - 0.2

- Rewrite as a concise wire-protocol reference. Remove duplicated architecture, timing, power, and state-machine content already covered by the architecture and state-machines documents. Consolidate the 0.1.x changelog into this entry.
- Correct the packet header byte layout, field sizes, and byte order to match the firmware encode/decode implementation.
- Correct the BLE advertisement field layout to match the firmware discovery codec.
- Remove stale references to 200 ms discovery advertisements, 120 ms mesh advertisements, obsolete acknowledgement delay, and periodic ROUTE_ADV beacons.
- Document the COMMAND_ID and COMMAND_STATUS enumerated values.
- Document the RANGE_STATUS enumerated values.

## Abbreviations

| Abbreviation | Meaning |
| --- | --- |
| ACK | Acknowledgement. Confirms gateway delivery. |
| BLE | Bluetooth Low Energy. Present in hardware naming and historical documents only; operational wake, discovery, ranging, and mesh transport are UWB-only in v1 firmware. |
| COBS | Consistent Overhead Byte Stuffing. Frames binary packets over USB serial. |
| CRC | Cyclic Redundancy Check. Detects corrupted packets. |
| DS-TWR | Double-Sided Two-Way Ranging. UWB distance measurement. |
| STS | Scrambled Timestamp Sequence. DWM3000 secure ranging feature. |
| TLV | Type-Length-Value. Extensible payload field format. |
| TTL | Time To Live. Hop limit that prevents infinite forwarding. |
| UWB | Ultra-Wideband. Used for distance measurements. |

## Packet Envelope

All mesh payloads and USB packets use the shared IMEC binary envelope. UWB mesh frames wrap one shared packet for over-the-air relay. UWB control/ranging frames use separate compact layouts.

### Byte Layout

All multi-byte fields are little-endian.

| Offset | Size | Field | Purpose |
| --- | ---: | --- | --- |
| 0 | 1 | Magic | `0xC1`. Rejects non-IMEC data. |
| 1 | 1 | Version | `0x01`. Protocol version. |
| 2 | 1 | Message type | Selects the message family and handler. |
| 3 | 1 | Flags | `GATEWAY_ACK_REQUIRED`, `GATEWAY_ACK`, `DIAGNOSTIC`, `COUNT_AS_CLICK`, etc. |
| 4 | 8 | Source ID | Globally identifies the sender. |
| 12 | 8 | Destination ID | Target device, or `0x0000000000000000` for broadcast. |
| 20 | 4 | Session ID | Groups packets for one click, command, or survey. |
| 24 | 2 | Sequence | Sender-local packet number. Detects retries and duplicates. |
| 26 | 1 | TTL | Forwarding depth limit. Each relay decrements by 1. |
| 27 | 1 | Payload length | Number of TLV bytes that follow. |
| 28 | variable | Payload | TLV fields for the message type. |
| 28 + len | 2 | CRC16 | CRC-16/CCITT-FALSE over bytes 0 through 27 + len. Little-endian. |

Total header is 28 bytes. Maximum payload is 255 bytes. Maximum packet is 28 + 255 + 2 = 285 bytes.

USB serial wraps this with COBS framing. UWB mesh transport wraps one shared packet in a `UWB_MESH` frame with `network_id`, previous-hop ID, next-hop ID, packet length, and CRC. The maximum UWB mesh frame is 312 bytes: 25 bytes of UWB mesh header, one 285-byte shared packet, and a 2-byte frame CRC.

## Message Types

Message types are one byte on the wire. The ranges are a readability convention, not priority.

| Range | Family | Purpose |
| --- | --- | --- |
| `0x01-0x0F` | Discovery and UWB control | Retired legacy discovery IDs plus active UWB wake/discovery/schedule/mesh control frames. |
| `0x10-0x1F` | UWB ranging | DS-TWR exchange frames. |
| `0x20-0x2F` | Device reports | Click reports, self-test reports, heartbeat/status. |
| `0x30-0x3F` | Mesh reliability/routing | Relay payloads, gateway ACKs, route discovery. |
| `0x40-0x4F` | Gateway commands | Extensible command and result messages. |
| `0x50-0x5F` | Anchor survey | Reachability and anchor-to-anchor distance measurements. |
| `0x7F` | Error | Common error response. |

| Value | Name | Direction |
| ---: | --- | --- |
| `0x01` | Reserved | Retired legacy discovery advertising ID; do not emit or decode in v1 firmware |
| `0x02` | Reserved | Retired legacy discovery advertising ID; do not emit or decode in v1 firmware |
| `0x08` | `UWB_WAKE_CLAIM` | Clicker → Anchors (UWB long-preamble wake frame) |
| `0x09` | `UWB_DISCOVER` | Clicker → Selected anchors (UWB discovery frame) |
| `0x0A` | `UWB_DISCOVERY_REPLY` | Anchor → Selected clicker (UWB discovery slot reply) |
| `0x0B` | `UWB_RANGE_SCHEDULE` | Clicker → Selected anchors (UWB range schedule) |
| `0x0C` | `UWB_MESH` | Any relay → next hop or broadcast (UWB mesh frame) |
| `0x10` | `UWB_POLL` | Clicker → Anchor (UWB) |
| `0x11` | `UWB_RESP` | Anchor → Clicker (UWB) |
| `0x12` | `UWB_FINAL` | Clicker → Anchor (UWB) |
| `0x13` | `UWB_REPORT` | Anchor → Clicker (UWB) |
| `0x20` | `CLICK_REPORT` | Anchor → Gateway (mesh) |
| `0x21` | `SELF_TEST_REPORT` | Clicker → Gateway (mesh) |
| `0x22` | `ANCHOR_HEARTBEAT` | Anchor → Gateway (mesh) |
| `0x30` | `MESH_DATA` | Any → Any (inside UWB mesh frame) |
| `0x31` | Reserved | Do not emit in v1 firmware |
| `0x32` | `GATEWAY_ACK` | Gateway → Sender (end-to-end ACK inside UWB mesh frame) |
| `0x33-0x34` | Reserved | Legacy `ROUTE_ADV`/`ROUTE_STATUS`; do not emit in v1 firmware |
| `0x35` | `ROUTE_REQ` | Any → Broadcast (inside UWB mesh frame) |
| `0x36` | `ROUTE_REPLY` | Target → Requester (inside UWB mesh frame) |
| `0x40` | `COMMAND` | Gateway → Anchor (mesh) |
| `0x41` | `COMMAND_RESULT` | Anchor → Gateway (mesh) |
| `0x50` | `SURVEY_REACH_REQ` | Gateway → Anchors (mesh) |
| `0x51` | `SURVEY_REACH_REPORT` | Anchor → Gateway (mesh) |
| `0x52` | `SURVEY_PAIR_PREPARE` | Gateway → Anchor (mesh) |
| `0x53` | `SURVEY_PAIR_RESULT` | Anchor → Gateway (mesh) |
| `0x7F` | `MSG_ERROR` | Any → Any |

## Flags

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | Reserved | Do not set in v1 firmware. |
| 1 | Reserved | Do not set in v1 firmware. |
| 2 | `GATEWAY_ACK_REQUIRED` | Gateway must send an end-to-end gateway ACK. |
| 3 | `GATEWAY_ACK` | This packet is a gateway ACK. |
| 4 | `DIAGNOSTIC` | Self-test traffic. Never counted as a real click. |
| 5 | `COUNT_AS_CLICK` | Normal click traffic. Mutually exclusive with `DIAGNOSTIC`. |
| 6 | `ERROR` | Packet carries an error indication. |

For UWB wake, discovery, range schedule, and DS-TWR frames, the flags byte must be exactly `DIAGNOSTIC` or exactly `COUNT_AS_CLICK`; no other packet flags are valid in these UWB session frames. The selected mode is part of the anchor ownership epoch and must match through `DISCOVER`, `RANGE_SCHEDULE`, `POLL`, `RESP`, `FINAL`, and `UWB_REPORT`.

## COMMAND_ID Values

| Value | Name | Purpose |
| ---: | --- | --- |
| `0x0001` | `CMD_PING` | Verify anchor is alive. |
| `0x0002` | `CMD_GET_STATUS` | Report role, uptime, battery, route, and radio status. |
| `0x0003` | `CMD_SET_LED_PATTERN` | Trigger a visible LED pattern on a specific anchor. |
| `0x0004` | `CMD_REBOOT` | Reboot the target anchor. |
| `0x0005` | `CMD_SET_ROLE` | Change the device role. |
| `0x0006` | `CMD_SET_ROUTE` | Set a route entry on the target. |
| `0x0007` | `CMD_CLEAR_ROUTE` | Clear a route entry on the target. |
| `0x0008` | `CMD_SET_SCAN_DUTY` | Change the low-duty UWB scan cadence when the requested interval remains within firmware duty-cycle and wake-overlap limits. |
| `0x0009` | `CMD_START_HEARTBEAT` | Start periodic anchor health reports. Optional `DURATION_MS` sets the interval; omitted means 60 s. Firmware accepts 5 s to 1 h. |
| `0x000A` | `CMD_STOP_HEARTBEAT` | Stop periodic anchor health reports. |
| `0x0100` | `CMD_SURVEY_REACHABILITY` | Start anchor reachability survey. |
| `0x0101` | `CMD_SURVEY_PREPARE_PAIR` | Prepare an anchor pair for UWB ranging. |
| `0x0102` | `CMD_SURVEY_START_PAIR` | Start UWB ranging for a prepared pair. |
| `0x0103` | `CMD_SURVEY_ABORT` | Abort the current survey. |
| `0x8000` | `CMD_VENDOR_BASE` | Base for vendor-specific commands. |

## COMMAND_STATUS Values

| Value | Name | Meaning |
| ---: | --- | --- |
| 0 | `COMMAND_OK` | Command succeeded. |
| 1 | `COMMAND_UNSUPPORTED_COMMAND` | Command ID not recognised. |
| 2 | `COMMAND_MALFORMED_PAYLOAD` | TLV payload could not be parsed. |
| 3 | `COMMAND_BUSY` | Only one outstanding command at a time. |
| 4 | `COMMAND_DENIED` | Command not allowed in current state. |
| 5 | `COMMAND_TIMEOUT` | No matching command result within 12 s. |
| 6 | `COMMAND_RADIO_ERROR` | UWB radio failure. |
| 7 | `COMMAND_INVALID_STATE` | Device not in the right state for this command. |
| 8 | `COMMAND_INTERNAL_ERROR` | Unexpected firmware error. |

## RANGE_STATUS Values

| Value | Name | Meaning |
| ---: | --- | --- |
| 0 | `RANGE_OK` | Successful range. |
| 1 | `RANGE_RX_TIMEOUT` | No response received within timeout. |
| 2 | `RANGE_RX_ERROR` | General receive error. |
| 3 | `RANGE_BAD_FRAME` | Frame decoded but failed validation. |
| 4 | `RANGE_WRONG_TARGET` | Frame addressed to a different device. |
| 5 | `RANGE_STS_QUALITY_FAIL` | STS quality check failed. |
| 6 | `RANGE_DELAYED_TX_MISSED` | Delayed TX start deadline was missed. |
| 7 | `RANGE_INTERNAL_ERROR` | Unexpected driver error. |
| 8 | `RANGE_TIMING_INVALID` | Equal reply-delay validation failed; discard the exchange. |

## TLVs

TLVs let command and report payloads grow without changing the fixed packet header. Unknown TLVs are skipped unless the specific message handler marks them as mandatory.

| ID | Name | Value size | Meaning |
| --- | --- | ---: | --- |
| `0x01` | `DEVICE_ROLE` | 1 | Clicker, anchor, or gateway. |
| `0x02` | `BATTERY_MV` | 2 | Battery voltage in millivolts. |
| `0x03` | `STATUS_BITS` | 4 | Health bitfield for module state, charging, and faults. Anchor UWB bits are defined below. |
| `0x04` | `ERROR_CODE` | 1 or 2 | Machine-readable failure reason. |
| `0x05` | `ERROR_DETAIL` | variable | Extra debug context. Not part of normal control flow. |
| `0x06` | `EVENT_SEQ` | 4 | Clicker-local event counter. Correlates anchor reports for one click or self-test. |
| `0x07` | `TIMESTAMP_MS` | 4 | Local millisecond timestamp. Not trusted as global time. |
| `0x08` | `RSSI_DBM` | 1 (i8) | Legacy signal strength hint for route quality. |
| `0x09` | `UWB_SHORT_ADDR` | 2 | Compact address used inside UWB frames in place of 64-bit IDs. |
| `0x0A` | `ANCHOR_ID` | 8 | Anchor that measured, relayed, or reported data. |
| `0x0B` | `CLICKER_ID` | 8 | Clicker that originated a click or self-test. |
| `0x0C` | `DISTANCE_MM` | 4 (i32) | Millimeter distance. Averaged for aggregated click ranges. Negative = invalid. |
| `0x0D` | `QUALITY` | 1 | Normalized 1-100 range quality score. |
| `0x0E` | `SAMPLE_INDEX` | 2 | First sample index in this packet for fragmented reports. |
| `0x0F` | `SAMPLE_COUNT` | 2 | Total measurement count for the whole aggregate. |
| `0x10` | `COMMAND_ID` | 2 | Selects the gateway command being executed. |
| `0x11` | `COMMAND_STATUS` | 1 | Result of a command (`COMMAND_OK`, `COMMAND_TIMEOUT`, etc.). |
| `0x12` | `REQUESTED_MSG_SEQ` | 2 | Packet sequence number being acknowledged or referenced. |
| `0x13` | `NEXT_HOP_ID` | 8 | Selected next neighbor in a mesh route. |
| `0x14` | `GATEWAY_ID` | 8 | Gateway root expected to receive reports and send gateway ACKs. |
| `0x15` | `SURVEY_ID` | 4 | Groups all reachability and pair measurements for one survey. |
| `0x16` | `PEER_ID_LIST` | variable (8 × count) | Reachable anchor IDs. |
| `0x17` | `REACHABILITY_ENTRY` | variable | One reachable peer plus signal/quality metadata. |
| `0x18` | `RANGE_FLAGS` | 1 or 2 | Marks diagnostic, survey, click-related, or retry ranging. |
| `0x19` | `LED_PATTERN_ID` | 1 | Gateway-selected LED pattern for setup/testing. |
| `0x1A` | `DURATION_MS` | 4 | Duration for LED patterns, scan windows, or survey slots. |
| `0x1B` | `RETRY_COUNT` | 1 or 2 | Retries attempted or allowed. |
| `0x1C` | `FW_VERSION` | variable | Firmware version string. |
| `0x1D` | `UPTIME_MS` | 4 | Device uptime in milliseconds. |
| `0x1E` | `REASON` | 1 or 2 | Short reason code for route changes, survey aborts, and command denials. |
| `0x1F` | `INITIATOR_ID` | 8 | Anchor or clicker that starts a UWB ranging exchange. |
| `0x20` | `RESPONDER_ID` | 8 | Anchor expected to respond in UWB ranging. |
| `0x21` | `RANGE_STATUS` | 1 | UWB result status (`RANGE_OK`, `RANGE_RX_TIMEOUT`, etc.). |
| `0x22` | `ROUTE_EPOCH` | 4 | Gateway-selected route generation. Newer epochs invalidate older candidates. |
| `0x23` | `HOP_COUNT` | 1 | Route-discovery hop distance. |
| `0x24` | `UWB_RSL_DBM` | 1 (i8) | UWB received signal level in dBm. `0` = unavailable. Read once per aggregated anchor report, not per sample. |
| `0x25` | `DISTANCE_SAMPLES_MM` | variable (4 × count) | Packed `i32` little-endian millimeter distances for aggregated click ranges. |
| `0x26` | `UWB_CIR_SAMPLE` | 6 | One raw DWM3000 Ipatov accumulator complex sample from the integer first-path index. Bytes are the device accumulator order: 24-bit real part followed by 24-bit imaginary part. Included only on the first packet of an aggregated or fragmented click range report. |

## Retired Legacy Discovery IDs

Message IDs `0x01` and `0x02` belonged to a historical discovery advertising design. They are reserved so older logs remain identifiable, but the native firmware core no longer builds codecs or tests for these payloads. Operational click discovery uses the UWB wake/discovery frames below.

## UWB Wake, Discovery, And Schedule Frames

Every UWB control frame begins with marker `0xCA`, version `0x01`, and a type byte, and ends with CRC-16/CCITT-FALSE over the frame bytes before the CRC. Multi-byte fields are little-endian.

| Frame | Length | Required identity/freshness fields | Purpose |
| --- | ---: | --- | --- |
| `WAKE_CLAIM` | 49 B | `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `priority_id`, `nonce`, wake/ranging channels, flags | Repeated long-preamble wake claim. Anchors create or update an ownership epoch only after this CRC-valid frame decodes and passes local network, channel, flag, freshness, and arbitration checks. |
| `DISCOVER` | 32 B | `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `nonce`, `discovery_slot_count`, flags | Sent by the selected clicker after the wake train. Anchors reply only if it matches their epoch. |
| `DISCOVERY_REPLY` | 44 B | `network_id`, `anchor_id`, `selected_clicker_id`, `click_event_id`, `attempt_index`, `nonce`, slot, status, quality, flags | Presence-only static-slot reply. It is not a range measurement. |
| `RANGE_SCHEDULE` | 40-120 B | `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `nonce`, ranging channel, reply delay, poll timing, selected anchors | Authorizes selected anchors and serializes DS-TWR poll order. Samples are round-robin across anchors. |

`WAKE_CLAIM` timing fields are bounded protocol inputs, not arbitrary sleep commands. `wake_train_ends_in_ms` and `discovery_starts_in_ms` must be no more than 1000 ms, `discovery_starts_in_ms` must be greater than or equal to `wake_train_ends_in_ms`, and `claimed_duration_ms` must cover both while staying no more than 2000 ms. The current clicker uses a 430 ms wake train. Each repeated claim sets the wake-train and discovery offsets to the remaining wake-train time from that frame and advertises `claimed_duration_ms` as that remaining wake time plus discovery, schedule reception, and the full scheduled range span for up to eight anchors with two samples each.

Wake-claim freshness is evaluated before competing-clicker priority arbitration. If a claim matches the active `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `priority_id`, `nonce`, and mode flags, it refreshes the same ownership epoch. If the same clicker/event/nonce/mode repeats the same `attempt_index` with a different `priority_id`, the claim is malformed and cannot refresh the epoch. If the same clicker/event/nonce/mode uses a higher `attempt_index`, it replaces the older ownership epoch; a lower `attempt_index` is stale. If `network_id`, `clicker_id`, and `click_event_id` match the active epoch but `nonce` or mode flags differ, the claim is malformed and does not enter priority arbitration. Claims for different clicker events then arbitrate by lower `priority_id`, lower `clicker_id`, lower `click_event_id`, and lower `attempt_index`.

Anchors reject schedules that do not match the active epoch, wrong network/channel/nonce, any reply delay other than the fixed 900 us firmware value, any poll spacing below 50 ms, duplicate selected anchors, missing selected samples, or per-anchor sequence ranges that would wrap past `255`.

## UWB Ranging Frames

UWB DS-TWR frames use a 41-byte identity-bound header before the radio FCS:

| Field | Size | Purpose |
| --- | ---: | --- |
| Marker | 8 bits | Rejects non-IMEC UWB frames. |
| Version | 8 bits | UWB frame format version. |
| Type | 8 bits | Poll, response, final, or report. |
| Sequence | 8 bits | Identifies one DS-TWR attempt inside the click event. `0` is invalid. |
| Network ID | 32 bits | Rejects frames from another deployment. |
| Session ID | 32 bits | Click or diagnostic event sequence. |
| Session nonce | 64 bits | Freshness token from the selected wake epoch. |
| Initiator short address | 16 bits | Compact UWB address derived from the initiator ID. |
| Responder short address | 16 bits | Compact UWB address derived from the selected anchor ID. |
| Flags | 8 bits | Normal click versus diagnostic. |
| Initiator ID | 64 bits | Full clicker or anchor ID that started this exchange. |
| Responder ID | 64 bits | Full selected anchor ID expected to respond. |

Frame sizes before the radio FCS: poll 41 B, response 49 B, final 53 B, report 49 B. Full IDs, short addresses, `network_id`, event/session ID, nonce, nonzero sequence, and flags must all match the selected clicker/event/anchor identity. For the DS-TWR timing rationale, equal reply-delay validation, and timing rejection policy, see [[UWB+BLE Architecture 0.5.27]].

Each POLL must use the selected anchor's full responder ID and matching short address from the schedule. Broadcast POLL is not valid for normal clicks, diagnostics, or survey ranging.

## Mesh Protocol

The mesh is reactive. Nodes discover a path when a real packet needs one, then send shared packets inside UWB mesh frames while the route stays fresh. For the full route state, cost formula, retry behavior, and click-priority details, see [[UWB+BLE Architecture 0.5.27]].

### Mesh Packet Shapes

| Packet | Header flags | Main payload TLVs | Purpose |
| --- | --- | --- | --- |
| Gateway-bound click report | `GATEWAY_ACK_REQUIRED`; `src_id`=anchor; `dst_id`=gateway; `ttl=4` | `CLICKER_ID`, `ANCHOR_ID`, `EVENT_SEQ`, `DISTANCE_MM`, `SAMPLE_COUNT`, `DISTANCE_SAMPLES_MM`, `QUALITY`, `RANGE_STATUS`, `UWB_RSL_DBM`, `UWB_CIR_SAMPLE` | Carries measured data toward the gateway. Click reports set `COUNT_AS_CLICK` and clear `DIAGNOSTIC`. |
| Gateway-bound self-test report | `GATEWAY_ACK_REQUIRED`; `src_id`=clicker; `dst_id`=gateway; `ttl=4` | `CLICKER_ID`, `EVENT_SEQ`, `ERROR_CODE`, `BATTERY_MV` | Carries the local clicker self-test result toward the gateway. Self-test reports set `DIAGNOSTIC` and clear `COUNT_AS_CLICK`. |
| Gateway ACK | `GATEWAY_ACK`; `src_id`=gateway; `dst_id`=original source; `ttl=4` | `REQUESTED_MSG_SEQ` | Confirms the gateway received the original packet. Routed back like a normal packet inside UWB mesh frames. |
| Gateway command | none; `src_id`=gateway; `dst_id`=target anchor; `ttl=4` | `COMMAND_ID` plus command-specific TLVs | Extensible command to one anchor. Completion is the matching `COMMAND_RESULT`. |
| Command result | `GATEWAY_ACK_REQUIRED`; `src_id`=target; `dst_id`=gateway; `ttl=4` | `COMMAND_ID`, `COMMAND_STATUS`, optional `ERROR_CODE` or result TLVs | Returns command outcome to the gateway. |
| Route request | `src_id`=requester; `dst_id`=broadcast; `ttl=4` | `INITIATOR_ID`, `RESPONDER_ID`, `ROUTE_EPOCH`, `HOP_COUNT`, `QUALITY` | UWB mesh discovery request. |
| Route reply | `src_id`=target; `dst_id`=requester; `ttl=4` | `INITIATOR_ID`, `RESPONDER_ID`, `ROUTE_EPOCH`, `HOP_COUNT`, `QUALITY` | UWB mesh discovery reply along the reverse path. |

### Forwarding Rules

1. **Validate** magic, version, payload length, and CRC. Invalid packets are dropped.
2. **Detect duplicates** by `(msg_type, src_id, dst_id, session_id, seq)`. A local duplicate is not delivered twice; the gateway may re-emit a gateway ACK. A directed duplicate that still needs forwarding is re-forwarded only when the relay is not already tracking a gateway-bound transmission. Duplicate cache entries expire after 60 s.
3. **Busy relay**: if a new packet would require forwarding or an immediate local response while the node already has a tracked gateway-bound transmission in flight, drop it and do not cache the duplicate.
4. **Local delivery**: if `dst_id` is this node, handle locally. Gateways emit `GATEWAY_ACK` for gateway-bound packets that requested it. Anchors receiving a command emit `COMMAND_RESULT`.
5. **TTL zero**: if `dst_id` is not local and `ttl` is zero, drop and record a route failure.
6. **Route selection**: expire stale candidates older than 30 s, then select the next hop. Routes use `effective_cost = hop_count * 100 + (100 - quality)`. Ties are broken by higher quality, fewer hops, newer observation time, then lower next-hop ID. See [[UWB+BLE Architecture 0.5.27]] for the full cost derivation and quality mapping.
7. **Forward** the same packet with `ttl - 1`. Relays never rewrite `src_id`, `dst_id`, `session_id`, `seq`, or payload.
8. **Gateway ACK**: for gateway-bound packets with `GATEWAY_ACK_REQUIRED`, keep the original sender's packet pending until the gateway ACK arrives or 2 s expires. Missing gateway ACKs retry the selected route up to three failures before rediscovery.
9. **Command serialization**: the gateway tracks one outstanding command at a time. A second command is rejected with `COMMAND_BUSY`. A matching `COMMAND_RESULT` clears the wait; no result within 12 s emits `COMMAND_TIMEOUT` over USB.
10. **Click priority**: an accepted local `WAKE_CLAIM` preempts active mesh forwarding and clears pending mesh RX work. CRC-valid claims that fail network, channel, flag, freshness, or arbitration checks are ignored for preemption. Already-built local click reports are requeued for later delivery. See [[UWB+BLE Architecture 0.5.27]] for the full click-priority mechanism.

### Route Formation

1. A sender with data and no usable route sends `ROUTE_REQ` inside UWB mesh frames during its route-listen opportunity.
2. Every receiver stores a reverse breadcrumb to the requester through the previous hop.
3. If the receiver is not the target, it rebroadcasts with `HOP_COUNT + 1` and reduced path quality (weakest-link propagation).
4. The target sends `ROUTE_REPLY` back through the reverse path.
5. Every receiver of the reply stores a route to the target through the previous hop.
6. The requester sends the data packet inside a UWB mesh frame to the selected next hop. All subsequent data, gateway ACKs, and command results for that route also travel inside UWB mesh frames.

```mermaid
sequenceDiagram
    participant A as Anchor A
    participant B as Anchor B
    participant G as Gateway G

    A->>B: UWB_MESH ROUTE_REQ target=Gateway, hop_count=0
    B->>G: UWB_MESH ROUTE_REQ target=Gateway, hop_count=1
    G-->>B: UWB_MESH ROUTE_REPLY target=Gateway, hop_count=0
    B-->>A: UWB_MESH ROUTE_REPLY target=Gateway, hop_count=1
    A->>B: UWB_MESH CLICK_REPORT
    B->>G: UWB_MESH CLICK_REPORT
    G-->>B: UWB_MESH GATEWAY_ACK
    B-->>A: UWB_MESH GATEWAY_ACK
```

### Downlink Directory

The gateway keeps a flat `target_id → next_hop_id` directory, not a full topology map. If no entry exists for a target, the gateway sends a `ROUTE_REQ` inside a UWB mesh frame and keeps the USB command pending. The gateway serializes commands: only one outstanding command at a time.

For `CMD_GET_STATUS`, the response includes `DEVICE_ROLE`, `UPTIME_MS`, `STATUS_BITS`, `GATEWAY_ID`, and either route fields (`NEXT_HOP_ID`, `ROUTE_EPOCH`, `HOP_COUNT`, `QUALITY`, `RETRY_COUNT`) or `REASON=7` (`NOT_FOUND`) when no upstream route is selected. `MSG_ANCHOR_HEARTBEAT` uses the same role, uptime, `STATUS_BITS`, and route telemetry shape.

Anchor UWB status bit layout:

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `UWB_SCAN_ACTIVE` | Low-duty wake scanning has run at least once since boot. |
| 1 | `UWB_WAKE_DECODE_FAILURE` | SFD timeout, frame timeout, CRC failure, or false-wake cooldown has occurred. |
| 2 | `UWB_CLAIM_COLLISION` | A competing wake claim collided or lost arbitration. |
| 3 | `UWB_DS_TWR_FAILURE` | A scheduled DS-TWR exchange failed. |
| 4 | `UWB_TIMING_REJECTION` | A DS-TWR exchange was rejected for timing or schedule identity. |
| 5 | `UWB_MESH_RX` | At least one UWB mesh frame has been received. |

### Report Fragmentation

An unfragmented aggregated click report can carry the configured single-packet sample limit inside one UWB mesh frame when diagnostic TLVs are present. If the anchor measured more samples, it sends multiple `CLICK_REPORT` packets for the same `(anchor_id, clicker_id, event_seq)`. The first packet carries one rounded-average `DISTANCE_MM`, one `QUALITY`, one `RANGE_STATUS`, one total `SAMPLE_COUNT`, the `UWB_RSL_DBM` TLV, the `UWB_CIR_SAMPLE` TLV, and the first chunk of `DISTANCE_SAMPLES_MM`. Subsequent packets carry the same aggregate fields, a `SAMPLE_INDEX` pointing to the first sample in that chunk, and the next sample chunk. `UWB_RSL_DBM` and `UWB_CIR_SAMPLE` appear only once per aggregate because DWM3000 RX diagnostics are sampled at most once per anchor UWB window.

### Duplicate Handling

If a relay receives a directed-unicast retry whose identity is already in its 60 s duplicate cache:
- **Idle with a route**: re-forward the packet. The duplicate cache prevents double delivery.
- **Busy or no route**: drop the packet. The sender retries later if its end-to-end confirmation times out.

Local or broadcast duplicates are not delivered twice.

## Self-Test and Diagnostic Flags

Self-test traffic sets `DIAGNOSTIC` and clears `COUNT_AS_CLICK`. Normal click traffic sets `COUNT_AS_CLICK` and clears `DIAGNOSTIC`. The firmware rejects packets where both flags are set. This prevents self-test dud events from appearing as real clicks in the server event stream. For the full self-test sequence, LED patterns, and click failure codes, see [[UWB+BLE Architecture 0.5.27]].

## Anchor Self-Distance Survey

The gateway starts survey setup by asking anchors for reachability. Anchors report which other anchors they can reach, producing a graph of possible measurement pairs. The gateway then schedules individual anchor pairs and requests exactly `n` measurements for each pair.

Pair measurements report `SURVEY_ID`, `INITIATOR_ID`, `RESPONDER_ID`, `SAMPLE_INDEX`, `SAMPLE_COUNT`, `DISTANCE_MM`, `QUALITY`, and `RANGE_STATUS`. The firmware only measures and reports; solving anchor positions from this distance network is off-site software.

Each survey DS-TWR sample uses `FLAG_DIAGNOSTIC`, the survey ID as the DS-TWR session ID, and a session nonce derived from `(survey_id, initiator_id, responder_id, sample_index)`. This keeps the full DS-TWR identity unique across the survey even though the compact 8-bit DS-TWR sequence value wraps in surveys longer than 255 samples.

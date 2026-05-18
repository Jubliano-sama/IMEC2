#internship #imec #protocol #documentation #UWB #BLE

# UWB+BLE Protocols and Strategies

Version: 0.2.42

Previous version: [[UWB+BLE Protocols and Strategies 0.2.41]]

This document defines the v1 wire protocol: binary packet formats, message types, TLVs, and forwarding rules. System architecture, timing budgets, power estimates, and state machine flows are in [[UWB+BLE Architecture 0.5.45]]. Runtime behavior and decision flows are in [[Firmware State Machines 0.1.37]].

## Changelog

### 2026-05-18 - 0.2.42

- Switched the documented 1024-symbol UWB PHY to the bundled Qorvo preset shape: PAC8 with a 1025-symbol SFD timeout.
- Updated references to architecture 0.5.45 and firmware state machines 0.1.37.

### 2026-05-18 - 0.2.41

- Documented the common UWB PHY used by wake, discovery, ranging, route contact, and channel-9 mesh payload. Version 0.2.42 applies the Qorvo preset PAC and SFD timeout.
- Added the lower DWM3000 first-path threshold (`IP_CONFIG_LO.IP_NTM=12`) to the protocol-level radio assumptions.
- Updated references to architecture 0.5.44 and firmware state machines 0.1.36.

### 2026-05-18 - 0.2.40

- Added `UWB_RANGE_RELEASE` (`0x0D`) so clickers can release anchors that replied when a normal click has fewer than four eligible discovery replies.
- Clarified that normal-click burst ranging requires at least four discovery replies, while schedules can still include up to six anchors.
- Updated route forwarding rules so route age alone does not invalidate a route; routes are invalidated by replacement, explicit clear, or delivery failure.
- Updated references to architecture 0.5.43 and firmware state machines 0.1.35.

### 2026-05-18 - 0.2.39

- Add the implemented channel-9 mesh event control message IDs and timing TLVs.
- Align range schedule validation with the shared 200 ms channel-5 burst, six-anchor maximum, 7 ms exchange stride, diagnostics-required flag, and no-STS mode.
- Add the compact clicker diagnostic UWB frame and diagnostic/report instrumentation TLVs.
- Clarify that mesh payloads use channel 9 only after channel-5 contact and negotiated event timing.

Older changes are in [[UWB+BLE Protocols and Strategies 0.2.41]].

## Abbreviations

| Abbreviation | Meaning |
| --- | --- |
| ACK | Acknowledgement. Confirms gateway delivery. |
| BLE | Bluetooth Low Energy. Used only as a clicker-to-clicker courtesy side channel during UWB politeness; operational wake, discovery, ranging, and mesh transport remain UWB-owned in v1 firmware. |
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
| `0x0D` | `UWB_RANGE_RELEASE` | Clicker → Replied anchors (release after insufficient normal-click discovery replies) |
| `0x10` | `UWB_POLL` | Clicker → Anchor (UWB) |
| `0x11` | `UWB_RESP` | Anchor → Clicker (UWB) |
| `0x12` | `UWB_FINAL` | Clicker → Anchor (UWB) |
| `0x13` | `UWB_REPORT` | Anchor → Clicker (UWB) |
| `0x14` | `UWB_CLICKER_DIAG` | Clicker → Anchor (compact post-FINAL diagnostics) |
| `0x20` | `CLICK_REPORT` | Anchor → Gateway (mesh) |
| `0x21` | `SELF_TEST_REPORT` | Clicker → Gateway (mesh) |
| `0x22` | `ANCHOR_HEARTBEAT` | Anchor → Gateway (mesh) |
| `0x30` | `MESH_DATA` | Any → Any (inside UWB mesh frame) |
| `0x31` | Reserved | Do not emit in v1 firmware |
| `0x32` | `GATEWAY_ACK` | Gateway → Sender (end-to-end ACK inside UWB mesh frame) |
| `0x33-0x34` | Reserved | Legacy `ROUTE_ADV`/`ROUTE_STATUS`; do not emit in v1 firmware |
| `0x35` | `ROUTE_REQ` | Any → Broadcast (inside UWB mesh frame) |
| `0x36` | `ROUTE_REPLY` | Target → Requester (inside UWB mesh frame) |
| `0x37` | `MESH_EVENT_PROPOSE` | Neighbor → Neighbor (propose channel-9 event timing after channel-5 contact) |
| `0x38` | `MESH_EVENT_ACCEPT` | Neighbor → Neighbor (accept or clip channel-9 event timing) |
| `0x39` | `MESH_EVENT_UPDATE` | Neighbor → Neighbor (refresh channel-9 timing) |
| `0x3A` | `MESH_EVENT_END` | Neighbor → Neighbor (clear channel-9 timing) |
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

For UWB wake, discovery, range release, range schedule, and DS-TWR frames, the flags byte must be exactly `DIAGNOSTIC` or exactly `COUNT_AS_CLICK`; no other packet flags are valid in these UWB session frames. The selected mode is part of the anchor ownership epoch. It must match through `DISCOVER`, then either `RANGE_RELEASE` or `RANGE_SCHEDULE`; if the attempt is scheduled, it must keep matching through `POLL`, `RESP`, `FINAL`, and `UWB_REPORT`.

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
| `0x000B` | `CMD_SYNC_TIME` | Gateway-initiated time sync. Broadcast by the gateway with 64-bit `TIMESTAMP_MS`; anchors apply it without command results. |
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
| 5 | `RANGE_STS_QUALITY_FAIL` | Reserved for STS-enabled future modes. No-STS ranging must not emit this status. |
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
| `0x07` | `TIMESTAMP_MS` | 8 | Gateway-synchronized millisecond timestamp for range reports, survey samples, status, heartbeats, and time-sync commands. If unsynced, anchors use local uptime and mark `TIME_SYNC_AGE_MS=UINT32_MAX`. |
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
| `0x1D` | `UPTIME_MS` | 4 | Compact device uptime in milliseconds for telemetry; local scheduled UWB deadlines use non-wrapping uptime internally. |
| `0x1E` | `REASON` | 1 or 2 | Short reason code for route changes, survey aborts, and command denials. |
| `0x1F` | `INITIATOR_ID` | 8 | Anchor or clicker that starts a UWB ranging exchange. |
| `0x20` | `RESPONDER_ID` | 8 | Anchor expected to respond in UWB ranging. |
| `0x21` | `RANGE_STATUS` | 1 | UWB result status (`RANGE_OK`, `RANGE_RX_TIMEOUT`, etc.). |
| `0x22` | `ROUTE_EPOCH` | 4 | Gateway-selected route generation. Newer epochs invalidate older candidates. |
| `0x23` | `HOP_COUNT` | 1 | Route-discovery hop distance. |
| `0x24` | `UWB_RSL_DBM` | 1 (i8) | UWB received signal level in dBm. `0` = unavailable. Read once per aggregated anchor report, not per sample. |
| `0x25` | `DISTANCE_SAMPLES_MM` | variable (4 × count) | Packed `i32` little-endian millimeter distances for aggregated click ranges. |
| `0x26` | `UWB_CIR_SAMPLE` | 6 | One raw DWM3000 Ipatov accumulator complex sample from the integer first-path index. Bytes are the device accumulator order: 24-bit real part followed by 24-bit imaginary part. Included only on the first packet of an aggregated or fragmented click range report. |
| `0x27` | `TIME_SYNC_AGE_MS` | 4 | Age of the last accepted gateway time sync at `TIMESTAMP_MS`. `UINT32_MAX` means the sender has not yet synced to the gateway. |
| `0x28` | `RANGE_ROUND_INDICES` | variable (1 × count) | Per-distance-sample round-robin index transmitted by the clicker in the accepted DS-TWR range header. Values match `DISTANCE_SAMPLES_MM` order. |
| `0x29` | `SEQUENCE_START_TIMESTAMPS_MS` | variable (8 × count) | Per-distance-sample gateway-synchronized millisecond start timestamp for the anchor's own observed ranging sequence. Values match `DISTANCE_SAMPLES_MM` order. |
| `0x2A` | `MESH_CHANNEL` | 1 | Mesh payload channel for a negotiated event. Current channel-9 events encode `9`. |
| `0x2B` | `MESH_EVENT_INTERVAL_MS` | 4 | Interval between negotiated channel-9 events. |
| `0x2C` | `MESH_EVENT_WINDOW_MS` | 2 | Receive/transmit window length for the negotiated channel-9 event. |
| `0x2D` | `MESH_NEXT_EVENT_TIME_MS` | 4 | Local/relative next channel-9 event start time carried in event-control messages. |
| `0x2E` | `MESH_EVENT_COUNTER` | 4 | Monotonic event counter for supervision and stale-event detection. |
| `0x2F` | `MESH_EVENT_GUARD_MS` | 2 | Guard time reserved for retune, PLL readiness, RX setup, and software jitter. |
| `0x30` | `MESH_CLOCK_SKEW_PPM` | 2 | Drift estimate or guard bound for the negotiated peer timing. |
| `0x31` | `MESH_MAX_MISSED_EVENTS` | 1 | Number of missed channel-9 events allowed before timing fallback. |
| `0x32` | `MESH_SUPERVISION_TIMEOUT_MS` | 4 | Maximum age of channel-9 timing before channel-5 refresh is required. |
| `0x33` | `DIAG_STATUS_FLAGS` | 4 | Diagnostic presence, truncation, drop, and unavailable flags. |
| `0x34` | `BURST_ID` | 4 | Identifies one shared ranging burst for reports and diagnostics. |
| `0x35` | `EXCHANGE_STRIDE_US` | 4 | Configured minimum stride between scheduled DS-TWR exchanges. |
| `0x36` | `BURST_DURATION_MS` | 2 | Shared responder burst duration. Current normal-click default is 200 ms. |
| `0x37` | `CLICK_LATENCY_MS` | 4 | Time from accepted click start to reportable ranging result. |
| `0x38` | `UWB_AWAKE_TIME_US` | 4 | Per-click UWB active time, excluding retained sleep. |
| `0x39` | `DIAG_BYTES_CAPTURED` | 2 | Diagnostic bytes captured locally before truncation. |
| `0x3A` | `DIAG_BYTES_TRANSMITTED` | 2 | Diagnostic bytes transmitted in report fragments. |
| `0x3B` | `DIAG_BYTES_TRUNCATED` | 2 | Diagnostic bytes omitted due to payload limits. |
| `0x3C` | `DIAG_FRAMES_DROPPED` | 2 | Clicker or anchor diagnostic frames unavailable or dropped. |
| `0x3D` | `REPORT_FRAGMENT_COUNT` | 1 | Number of report fragments for the aggregate. |
| `0x3E` | `CHANNEL9_REPORT_LATENCY_MS` | 4 | Report latency through negotiated channel-9 mesh. |
| `0x3F` | `GATEWAY_ACK_LATENCY_MS` | 4 | Time from report send to end-to-end gateway ACK. |
| `0x40` | `CLICKER_DIAG_BYTES` | variable | Compact clicker-side diagnostic bytes captured after `FINAL`. |
| `0x41` | `ANCHOR_DIAG_BYTES` | variable | Anchor-side diagnostic bytes captured after valid `FINAL`. |
| `0x42` | `PHY_CONFIG_ID` | 1 | Encodes channel/data-rate/preamble/PAC/no-STS PHY configuration. |
| `0x43` | `MESH_CHANNEL_SWITCHES` | 4 | Count of channel switches attempted by the mesh scheduler. |
| `0x44` | `MESH_PLL_READY_FAILURES` | 4 | Count of mesh retune/PLL readiness failures. |
| `0x45` | `MESH_LATE_CHANNEL5_RETURNS` | 4 | Count of late returns from channel 9 to required channel-5 work. |
| `0x46` | `MESH_DEFERRALS` | 4 | Count of channel-9 work deferred for higher-priority work. |
| `0x47` | `MESH_CH9_EVENT_MISSES` | 4 | Count of missed channel-9 events. |
| `0x48` | `MESH_CHANNEL5_PREEMPTIONS` | 4 | Count of channel-5 click or scan preemptions over channel-9 mesh. |
| `0x49` | `MESH_CH9_REPORT_LATENCY_MS` | 4 | Recent or aggregate channel-9 report latency exported in heartbeat/status. |

## Retired Legacy Discovery IDs

Message IDs `0x01` and `0x02` belonged to a historical discovery advertising design. They are reserved so older logs remain identifiable, but the native firmware core no longer builds codecs or tests for these payloads. Operational click discovery uses the UWB wake/discovery frames below.

## BLE Courtesy Advertisement

This section defines the courtesy payload. The system reason for courtesy BLE and the timing tradeoff are in [[UWB+BLE Architecture 0.5.45]].

The advertisement is legacy non-connectable manufacturer data on primary advertising channel 37 only. Channel 38 and 39 are disabled on transmit, and the passive scan is also limited to channel 37. If the controller cannot enforce that scan channel, the firmware disables BLE courtesy for that attempt and falls back to UWB-only randomized contention.

The clicker uses the fastest legal BLE 5.x non-connectable interval for the nRF52833 target: 20.0 to 20.625 ms plus the controller advertising delay. Passive scanning uses a 25 ms interval and a 20 ms window. The BLE scan and advertisement stop before the DWM3000 wake-claim train begins.

Manufacturer data payload:

| Offset | Length | Field | Notes |
| ---: | ---: | --- | --- |
| 0 | 2 | Company ID | Little-endian `0xffff` prototype company ID. |
| 2 | 1 | Marker/version | `0xc1`; rejects unrelated manufacturer payloads. |
| 3 | 4 | `network_id` | Little-endian project network ID. |
| 7 | 8 | `clicker_id` | Full clicker identity for stable tie-breaks. |
| 15 | 4 | `click_event_id` | Normal click event sequence. |
| 19 | 1 | `attempt_index` | Nonzero UWB wake attempt number. |
| 20 | 8 | `priority_id` | Attempt-local randomized priority ID. |

The serialized manufacturer data length is 28 bytes. Including the BLE AD length and type bytes, the single advertising data structure is 30 bytes and fits inside the 31-byte legacy advertising payload limit without needing scan responses or extended advertising.

## UWB Wake, Discovery, And Schedule Frames

All operational UWB modes use the same base PHY unless a frame explicitly says otherwise: 850 kbps, 1024-symbol preamble, PAC8, STS disabled, SFD timeout 1025, and DWM3000 `IP_CONFIG_LO.IP_NTM=12` for the lower recommended first-path threshold. Channel 5 carries wake, discovery, route contact, route timing refresh, and ranging. Channel 9 carries negotiated mesh payload events after channel-5 contact exists.

Every UWB control frame begins with marker `0xCA`, version `0x01`, and a type byte, and ends with CRC-16/CCITT-FALSE over the frame bytes before the CRC. Multi-byte fields are little-endian.

| Frame | Length | Required identity/freshness fields | Purpose |
| --- | ---: | --- | --- |
| `WAKE_CLAIM` | 49 B | `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `priority_id`, `nonce`, wake/ranging channels, flags | Repeated long-preamble wake claim. Anchors create or update an ownership epoch only after this CRC-valid frame decodes and passes local network, channel, flag, freshness, and arbitration checks. |
| `DISCOVER` | 32 B | `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `nonce`, `discovery_slot_count`, flags | Sent by the selected clicker after the wake train. Anchors reply only if it matches their epoch. |
| `DISCOVERY_REPLY` | 44 B | `network_id`, `anchor_id`, `selected_clicker_id`, `click_event_id`, `attempt_index`, `nonce`, slot, status, quality, flags | Presence-only static-slot reply. It is not a range measurement. |
| `RANGE_RELEASE` | 34 B | `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `nonce`, `discovered_anchor_count`, `min_anchor_count`, reason, flags | Releases anchors that replied when a normal click cannot range because fewer than four eligible anchors replied. |
| `RANGE_SCHEDULE` | 48-108 B | `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `nonce`, ranging channel, no-STS mode, diagnostics-required flag, burst window, exchange stride, maximum exchanges, minimum successful anchors, selected anchors | Authorizes selected anchors and serializes DS-TWR poll order. Samples are round-robin across anchors inside one shared burst. |

`WAKE_CLAIM` timing fields are bounded protocol inputs, not arbitrary sleep commands. `wake_train_ends_in_ms` and `discovery_starts_in_ms` must be no more than 1000 ms, `discovery_starts_in_ms` must be greater than or equal to `wake_train_ends_in_ms`, and `claimed_duration_ms` must cover both while staying no more than 2000 ms.

Wake-claim freshness is evaluated before competing-clicker priority arbitration. If a claim matches the active `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `priority_id`, `nonce`, and mode flags, it refreshes the same ownership epoch. If the same clicker/event/nonce/mode repeats the same `attempt_index` with a different `priority_id`, the claim is malformed and cannot refresh the epoch. If the same clicker/event/nonce/mode uses a higher `attempt_index`, it replaces the older ownership epoch; a lower `attempt_index` is stale. If `network_id`, `clicker_id`, and `click_event_id` match the active epoch but `nonce` or mode flags differ, the claim is malformed and does not enter priority arbitration. Claims for different clicker events then arbitrate by higher `attempt_index`, lower `priority_id`, lower `clicker_id`, and lower `click_event_id`.

Anchors accept `RANGE_RELEASE` only while waiting after a discovery reply for the matching selected epoch. The release must match network, clicker, click event, attempt, nonce, and flags. The only current reason is `INSUFFICIENT_ANCHORS=1`. `discovered_anchor_count` must be at least one and less than `min_anchor_count`. On acceptance, the anchor clears that click epoch and resumes low-duty scan.

Anchors reject schedules that do not match the active epoch, wrong network/channel/nonce, any ranging channel other than 5, any reply delay other than the fixed 900 us firmware value, STS enabled, diagnostics not required, a burst window below 200 ms, an exchange stride below 7000 us, more than 6 selected anchors, duplicate selected anchors, missing selected samples, or per-anchor sequence ranges that would wrap past `255`. Normal-click schedules also require at least 4 eligible anchors; diagnostic/self-test schedules may use fewer anchors. The legacy poll-spacing field remains bounded for compatibility, but the active burst schedule is governed by `burst_window_ms` and `exchange_stride_us`.

Anchor discovery replies use deterministic slots. Production provisioning must assign stable full device IDs and anchor slot values so scheduled discovery and ranging windows remain predictable without adding dynamic slot negotiation overhead.

## UWB Ranging Frames

UWB DS-TWR frames use a 42-byte identity-bound header before the radio FCS:

| Field | Size | Purpose |
| --- | ---: | --- |
| Marker | 8 bits | Rejects non-IMEC UWB frames. |
| Version | 8 bits | UWB frame format version. |
| Type | 8 bits | Poll, response, final, or report. |
| Sequence | 8 bits | Identifies one DS-TWR attempt inside the click event. `0` is invalid. |
| Round index | 8 bits | Clicker-transmitted round-robin round for this attempt. `0` is the first pass through the selected anchors. |
| Network ID | 32 bits | Rejects frames from another deployment. |
| Session ID | 32 bits | Click or diagnostic event sequence. |
| Session nonce | 64 bits | Freshness token from the selected wake epoch. |
| Initiator short address | 16 bits | Compact UWB address derived from the initiator ID. |
| Responder short address | 16 bits | Compact UWB address derived from the selected anchor ID. |
| Flags | 8 bits | Normal click versus diagnostic. |
| Initiator ID | 64 bits | Full clicker or anchor ID that started this exchange. |
| Responder ID | 64 bits | Full selected anchor ID expected to respond. |

Frame sizes before the radio FCS: poll 42 B, response 50 B, final 54 B, report 50 B. Full IDs, short addresses, `network_id`, event/session ID, nonce, nonzero sequence, clicker-transmitted round index, and flags must all match the selected clicker/event/anchor identity. For the DS-TWR timing rationale, equal reply-delay validation, and timing rejection policy, see [[UWB+BLE Architecture 0.5.45]].

Each POLL must use the selected anchor's full responder ID and matching short address from the schedule. Broadcast POLL is not valid for normal clicks, diagnostics, or survey ranging.

`UWB_CLICKER_DIAG` is a compact post-`FINAL` diagnostic frame from clicker to anchor. It uses the same identity header values as the completed exchange and adds diagnostic status flags plus up to 48 bytes of compact clicker-side diagnostic data. Missing or corrupt clicker diagnostics do not invalidate the completed range, but the anchor must record that state in report diagnostic TLVs.

## Mesh Protocol

The mesh is reactive. Nodes discover a path when a real packet needs one, then send shared packets inside UWB mesh frames while the route remains valid. Route age alone does not prove a path stale; replacement, explicit clear, or delivery failure does. For the full route state, cost formula, retry behavior, and click-priority details, see [[UWB+BLE Architecture 0.5.45]].

### Mesh Packet Shapes

| Packet | Header flags | Main payload TLVs | Purpose |
| --- | --- | --- | --- |
| Gateway-bound click report | `GATEWAY_ACK_REQUIRED`; `src_id`=anchor; `dst_id`=gateway; `ttl=4` | `CLICKER_ID`, `ANCHOR_ID`, `EVENT_SEQ`, `TIMESTAMP_MS`, `TIME_SYNC_AGE_MS`, `DISTANCE_MM`, `SAMPLE_COUNT`, `DISTANCE_SAMPLES_MM`, `RANGE_ROUND_INDICES`, `SEQUENCE_START_TIMESTAMPS_MS`, `QUALITY`, `RANGE_STATUS`, `UWB_RSL_DBM`, `UWB_CIR_SAMPLE` | Carries measured data toward the gateway. Click reports set `COUNT_AS_CLICK` and clear `DIAGNOSTIC`. |
| Gateway-bound self-test report | `GATEWAY_ACK_REQUIRED`; `src_id`=clicker; `dst_id`=gateway; `ttl=4` | `CLICKER_ID`, `EVENT_SEQ`, `ERROR_CODE`, `BATTERY_MV` | Carries the local clicker self-test result toward the gateway. Self-test reports set `DIAGNOSTIC` and clear `COUNT_AS_CLICK`. |
| Gateway ACK | `GATEWAY_ACK`; `src_id`=gateway; `dst_id`=original source; `ttl=4` | `REQUESTED_MSG_SEQ` | Confirms the gateway received the original packet. Routed back like a normal packet inside UWB mesh frames. |
| Gateway command | none; `src_id`=gateway; `dst_id`=target anchor; `ttl=4` | `COMMAND_ID` plus command-specific TLVs | Extensible command to one anchor. Completion is the matching `COMMAND_RESULT`. |
| Gateway time sync broadcast | none; `src_id`=gateway; `dst_id`=broadcast; `ttl=4` | `COMMAND_ID=CMD_SYNC_TIME`, `TIMESTAMP_MS` | Synchronizes anchors to gateway uptime without per-anchor command results. Relays forward this broadcast class. |
| Command result | `GATEWAY_ACK_REQUIRED`; `src_id`=target; `dst_id`=gateway; `ttl=4` | `COMMAND_ID`, `COMMAND_STATUS`, optional `ERROR_CODE` or result TLVs | Returns command outcome to the gateway. |
| Route request | `src_id`=requester; `dst_id`=broadcast; `ttl=4` | `INITIATOR_ID`, `RESPONDER_ID`, `ROUTE_EPOCH`, `HOP_COUNT`, `QUALITY` | UWB mesh discovery request. |
| Route reply | `src_id`=target; `dst_id`=requester; `ttl=4` | `INITIATOR_ID`, `RESPONDER_ID`, `ROUTE_EPOCH`, `HOP_COUNT`, `QUALITY` | UWB mesh discovery reply along the reverse path. |
| Mesh event propose/accept/update | directed unicast | `MESH_CHANNEL`, `MESH_EVENT_INTERVAL_MS`, `MESH_EVENT_WINDOW_MS`, `MESH_NEXT_EVENT_TIME_MS`, `MESH_EVENT_COUNTER`, `MESH_EVENT_GUARD_MS`, `MESH_CLOCK_SKEW_PPM`, `MESH_MAX_MISSED_EVENTS`, `MESH_SUPERVISION_TIMEOUT_MS` | Negotiates bounded channel-9 payload events after channel-5 contact exists. |
| Mesh event end | directed unicast | optional `REASON` | Clears channel-9 timing for the next hop. |

### Forwarding Rules

1. **Validate** magic, version, payload length, and CRC. Invalid packets are dropped.
2. **Detect duplicates** by `(msg_type, src_id, dst_id, session_id, seq)`. A local duplicate is not delivered twice; the gateway may re-emit a gateway ACK. A directed duplicate that still needs forwarding is re-forwarded only when the relay is not already tracking a gateway-bound transmission. Duplicate cache entries expire after 60 s.
3. **Busy relay**: if a new packet would require forwarding or an immediate local response while the node already has a tracked gateway-bound transmission in flight, drop it and do not cache the duplicate.
4. **Local delivery**: if `dst_id` is this node, handle locally. Gateways emit `GATEWAY_ACK` for gateway-bound packets that requested it. Anchors receiving a directed command emit `COMMAND_RESULT`. Broadcast `CMD_SYNC_TIME` is handled locally without command results.
5. **TTL zero**: if `dst_id` is not local and `ttl` is zero, drop and record a route failure.
6. **Route selection**: select among currently valid candidates. Do not expire a route by age alone. Routes use `effective_cost = hop_count * 100 + (100 - quality)`. Ties are broken by higher quality, fewer hops, newer observation time, then lower next-hop ID. See [[UWB+BLE Architecture 0.5.45]] for the full cost derivation and quality mapping.
7. **Forward** the same packet with `ttl - 1`. Relays never rewrite `src_id`, `dst_id`, `session_id`, `seq`, or payload. Broadcast forwarding is limited to survey reachability requests and `CMD_SYNC_TIME` broadcasts.
8. **Gateway ACK**: for gateway-bound packets with `GATEWAY_ACK_REQUIRED`, keep the original sender's packet pending until the gateway ACK arrives or 2 s expires. Missing gateway ACKs retry the selected route up to three failures before rediscovery.
9. **Command serialization**: the gateway tracks one outstanding command at a time. A second command is rejected with `COMMAND_BUSY`. A matching `COMMAND_RESULT` clears the wait; no result within 12 s emits `COMMAND_TIMEOUT` over USB.
10. **Channel selection**: route discovery, route refresh, unknown-contact recovery, and mesh event control start on channel 5. Payload data, gateway ACKs, command results, heartbeats, and reports use channel 9 only when the selected next hop has fresh accepted event timing.
11. **Click priority**: an accepted local `WAKE_CLAIM` preempts active mesh forwarding and clears pending mesh RX work. CRC-valid claims that fail network, channel, flag, freshness, or arbitration checks are ignored for preemption. Already-built local click reports are requeued for later delivery. See [[UWB+BLE Architecture 0.5.45]] for the full click-priority mechanism.

### Route Formation Rules

1. A sender with data and no usable route sends `ROUTE_REQ` inside UWB mesh frames during its channel-5 route-listen opportunity.
2. Every receiver stores a reverse breadcrumb to the requester through the previous hop.
3. If the receiver is not the target, it rebroadcasts with `HOP_COUNT + 1` and reduced path quality (weakest-link propagation).
4. The target sends `ROUTE_REPLY` back through the reverse path.
5. Every receiver of the reply stores a route to the target through the previous hop.
6. The requester proposes or refreshes bounded channel-9 event timing for the selected next hop.
7. After timing is accepted, the requester sends the data packet inside a UWB mesh frame during a negotiated channel-9 event. All subsequent data, gateway ACKs, and command results for that route prefer negotiated channel-9 events. If timing expires or supervision fails, the route itself can remain valid, but payload delivery returns to channel-5 contact refresh before using channel 9 again.

The operational route flow is shown in [[Firmware State Machines 0.1.37]].

### Downlink Directory

The gateway keeps a flat `target_id → next_hop_id` directory, not a full topology map. If no entry exists for a target, the gateway sends a `ROUTE_REQ` inside a UWB mesh frame and keeps the USB command pending. The gateway serializes commands: only one outstanding command at a time.

### Gateway Time Sync

The gateway initiates periodic time sync by broadcasting `MSG_COMMAND` with `COMMAND_ID=CMD_SYNC_TIME` and 64-bit `TIMESTAMP_MS`. Anchors accept both directed and broadcast sync commands, but broadcast sync does not return `COMMAND_RESULT`. Relays flood only this command class and survey reachability requests among broadcast traffic.

The sync interval calculation and the reason for gateway-initiated sync are in [[UWB+BLE Architecture 0.5.45]].

For `CMD_GET_STATUS`, the response includes `DEVICE_ROLE`, `UPTIME_MS`, `TIMESTAMP_MS`, `TIME_SYNC_AGE_MS`, `STATUS_BITS`, `GATEWAY_ID`, and either route fields (`NEXT_HOP_ID`, `ROUTE_EPOCH`, `HOP_COUNT`, `QUALITY`, `RETRY_COUNT`) or `REASON=7` (`NOT_FOUND`) when no upstream route is selected. `MSG_ANCHOR_HEARTBEAT` uses the same role, uptime, time-sync, `STATUS_BITS`, and route telemetry shape.

Anchor UWB status bit layout:

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `UWB_SCAN_ACTIVE` | Low-duty wake scanning has run at least once since boot. |
| 1 | `UWB_WAKE_DECODE_FAILURE` | SFD timeout, frame timeout, CRC failure, or false-wake cooldown has occurred. |
| 2 | `UWB_CLAIM_COLLISION` | A competing wake claim collided or lost arbitration. |
| 3 | `UWB_DS_TWR_FAILURE` | A scheduled DS-TWR exchange failed. |
| 4 | `UWB_TIMING_REJECTION` | A DS-TWR exchange was rejected for timing or schedule identity. |
| 5 | `UWB_MESH_RX` | At least one UWB mesh frame has been received. |
| 6 | `TIME_SYNCED` | The anchor has accepted at least one gateway time sync. |
| 7 | `TIME_SYNC_STALE` | The anchor has not synced yet or the last sync is older than the calculated drift-safe interval. |

### Report Fragmentation

An unfragmented aggregated click report can carry distance samples plus diagnostic TLVs inside one UWB mesh frame. If the anchor measured more samples or captured more diagnostics than fit, it sends multiple `CLICK_REPORT` packets for the same `(anchor_id, clicker_id, event_seq)`. Each packet's `TIMESTAMP_MS` is the gateway-synchronized start timestamp for the first distance sample in that chunk, and `SEQUENCE_START_TIMESTAMPS_MS` gives the direct per-sample start timestamps from that anchor. `RANGE_ROUND_INDICES` gives the clicker-transmitted round-robin round for each sample, so the gateway can group reports by `(clicker_id, event_seq, round_index)` and average the respective anchor start times for each round even when several round robins occur for one click. Anchors do not exchange start times with each other. The first packet carries one rounded-average `DISTANCE_MM`, one `QUALITY`, one `RANGE_STATUS`, one total `SAMPLE_COUNT`, burst and stride TLVs, diagnostic status/count TLVs, the PHY configuration ID, and the first chunks of distance, timing, clicker diagnostic, and anchor diagnostic bytes. Subsequent packets carry the same aggregate identity, a `SAMPLE_INDEX` pointing to the first sample in that chunk, and the next aligned sample or diagnostic chunks.

### Duplicate Handling

If a relay receives a directed-unicast retry whose identity is already in its 60 s duplicate cache:
- **Idle with a route**: re-forward the packet. The duplicate cache prevents double delivery.
- **Busy or no route**: drop the packet. The sender retries later if its end-to-end confirmation times out.

Local or broadcast duplicates are not delivered twice.

## Self-Test and Diagnostic Flags

Self-test traffic sets `DIAGNOSTIC` and clears `COUNT_AS_CLICK`. Normal click traffic sets `COUNT_AS_CLICK` and clears `DIAGNOSTIC`. The firmware rejects packets where both flags are set. This prevents self-test dud events from appearing as real clicks in the server event stream. For the full self-test sequence, LED patterns, and click failure codes, see [[UWB+BLE Architecture 0.5.45]].

## Anchor Self-Distance Survey

The gateway starts survey setup by asking anchors for reachability. Anchors report which other anchors they can reach, producing a graph of possible measurement pairs. The gateway then schedules individual anchor pairs and requests exactly `n` measurements for each pair.

Pair measurements report `SURVEY_ID`, `INITIATOR_ID`, `RESPONDER_ID`, `SAMPLE_INDEX`, `SAMPLE_COUNT`, `TIMESTAMP_MS`, `TIME_SYNC_AGE_MS`, `DISTANCE_MM`, `QUALITY`, and `RANGE_STATUS`. The timestamp marks when that individual DS-TWR ranging sequence started on the gateway timebase. The firmware only measures and reports; solving anchor positions from this distance network is off-site software.

Survey pair runs are long-running local work, not system command work. Anchors must continue to process mesh receive and commands while samples are being taken. `CMD_SURVEY_ABORT` must be accepted and must stop the active pair at the next sample boundary or bounded responder-listen check. The runtime handling is shown in [[Firmware State Machines 0.1.37]].

Each survey DS-TWR sample uses `FLAG_DIAGNOSTIC`, the survey ID as the DS-TWR session ID, and a session nonce derived from `(survey_id, initiator_id, responder_id, sample_index)`. This keeps the full DS-TWR identity unique across the survey even though the compact 8-bit DS-TWR sequence value wraps in surveys longer than 255 samples.

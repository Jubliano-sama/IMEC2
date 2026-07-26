#internship #imec #protocol #documentation #UWB #BLE

# UWB+BLE Protocols and Strategies

Version: 0.3.12.3

Previous version: [[UWB+BLE Protocols and Strategies 0.3.12.2]]

This document defines the v1 wire protocol: binary packet formats, message types, TLVs, and forwarding rules. System architecture, timing budgets, power estimates, and state machine flows are in [[UWB+BLE Architecture 0.6.6.1]]. Runtime mesh invariants are in [[Mesh Connected Routing Contract]].

## Changelog

### 2026-07-18 - 0.3.12.3

- Added `CMD_SURVEY_GO = 0x0105`. PREPARE and START carry a nonzero
  `SURVEY_ROUND_ID`; START arms the endpoint, while a matching broadcast GO
  carries the common future execute delay and requests no response.
- Added repeatable `OPERATION_POLICY = 0xAE` and
  `SURVEY_ROUND_ID = 0xAF`. Operation-policy values begin with
  `version, family, flags`: family 1 is the 11-byte assignment profile, family
  2 is the 19-byte discovery profile, and family 3 is the 5-byte pair profile.
- The assignment profile carries expected count, total budget, and equal
  randomized response spread. The discovery profile carries start delay, slot
  duration/count, one-to-four rounds, report grace, and total budget. The pair
  profile carries maximum reruns and parallel-lane cap.
- GUI commands repeat the same frozen complete profile in the separately
  correlated Here-I-Am preflight and target packet. Gateway route
  advertisements preserve that profile through relays; anchors use compiled
  defaults after reset until another accepted profile arrives.
- Assignment responses no longer use farthest-first priority. ACKed mappings
  form a useful successful subset when at least one table entry commits, while
  missing claims and ACKs remain explicit telemetry for optional qualification.
- Survey discovery is one continuous configurable randomized-round window.
  Every anchor listens before and after its one probe in each round; missed
  probes do not create a reserve horizon or invalidate peers already heard.
  One-way reachability is sufficient pair evidence.
- Pair rounds serialize shared endpoints, peer-neighborhoods, or reverse relay
  roots and may run up to 25 disjoint lanes after one common GO. When a peer
  report is saturated, a hop-depth difference of at least two is the
  conservative separation shortcut unless retained reverse routes prove a
  shared relay.
- The synchronized local responder window is 1,150 ms per sample, with a
  10 ms inter-sample gap and 50 ms gateway settle margin. PREPARE/START result
  delivery uses an independent 30-second direct bound plus 15 seconds per
  additional hop, with 90 seconds only for unknown depth.
- Routes remain usable across age and transient failures. Local anchor work is
  selected before transit, but accepted transit custody is never discarded.
  Upstream and gateway downlink paths invalidate only after four terminal
  failures, and fresh route evidence resets the counter.
- The current bounded-flood defaults are a 4,200 ms maximum randomized relay
  backoff, 600 ms slots, and two additional bursts. Older inherited values and
  the retired reserve-horizon/serial-survey text below are superseded by this
  changelog, [[Mesh Connected Routing Contract]], and
  [[Mesh Connected Routing Walkthrough]].

### 2026-07-12 - 0.3.12.2

- Required `SURVEY_DISCOVERY_START` to use the extended-PHR channel-5
  mesh-control PHY used by the anchor's control-follow-up receiver. A
  standard-wake-PHR survey-start frame is a PHY mismatch and is not decodable.
- Made the four survey-probe opportunities real attempts. A missed nominal
  transmit slot is retained for the same opportunity in a second bounded
  reserve horizon; it is not silently consumed. Every anchor processes the
  nominal and reserve horizons chronologically and listens through reserve
  slots used by deferred peers.
- Raised shared survey-slot validation to the complete worst-case radio
  envelope: the bounded 20 ms transmit timeout, 2 ms transition guard, and
  8 ms receive guard require at least a 30 ms slot. Timing remains relative and
  wrap-safe across the 32-bit uptime boundary.
- Required an anchor to retain one exact encoded discovery report until the
  local report queue accepts custody. Queue pressure and transient busy states
  use bounded retry after radio release; they do not silently discard the
  report or evict an older local-origin report.
- Added a survey-specific direct-gateway retry horizon for dense RF layouts.
  Twenty directly reachable anchors may contend for one continuously listening
  gateway without creating negotiated gateway connections: four real probes
  use anchor-, survey-, and attempt-diversified 2/4/8 second backoff windows,
  while a locally busy radio defers rather than consuming an attempt. Eight
  consecutive busy deferrals end with a distinct busy terminal and zero to
  four attempted transmissions; they are not reported as four attempted sends. Gateway
  collection now keeps the 5 second queue-admission allowance separate from a
  60.22 second conservative delivery tail covering probe policy, bounded
  scratch acquisition and radio transitions, gateway
  ACK retries, RX re-arm, and ACK turnaround.

### 2026-07-12 - 0.3.12.1

- Added optional one-byte `ATTEMPT_INDEX` and `DETECTION_SOURCE` TLVs to
  normal click reports. New mesh firmware emits source value 1 for a CRC-valid
  UWB wake claim; legacy packets omit both TLVs and remain valid.
- Kept an unprovisioned anchor eligible for local click ranging by using its
  deterministic hash slot until a valid gateway assignment is committed.
  Status and diagnostics continue to report `UNPROVISIONED`; the fallback is a
  bounded recovery path and does not replace gateway enumeration.

### 2026-07-10 - 0.3.12

- Removed the BLE debug-log characteristic and formatter backend. The gateway
  BLE service now carries only framed host commands, packet reports, and the
  read-only gateway identity; firmware diagnostics remain available over RTT.

### 2026-07-10 - 0.3.11

- Increased deterministic gateway survey planning from four to six peers per
  anchor. A 50-anchor deployment is therefore bounded to 150 unique pairs,
  compared with 1,225 pairs in the unrestricted complete graph.
- Expanded pair storage to the exact 150-pair ceiling and compacted the
  in-memory pair layout from 32 to 24 bytes so the larger topology uses less
  static gateway RAM than the previous 128-entry allocation.
- Corrected the documented extended channel-9 mesh capacity to the implemented
  1,021-byte frame limit, including a 958-byte shared-packet payload.

### 2026-07-10 - 0.3.10

- Made discovery-slot assignment a convergent, acknowledged transaction. Claim
  and table floods use bounded retries; anchor replies use assigned-slot and
  route-hop staggering with randomized exponential backoff; the gateway reports
  success only after every listed anchor acknowledges the exact table epoch.
- Defined persistent assignment behavior. Anchors keep a previously committed
  assignment while a new claim round is in progress, persist a validated table
  before acknowledging it, restore it after reboot, and stop replying only when
  an authoritative later table omits them.
- Bounded automatic anchor-pair survey planning to four peers per anchor, made
  pair selection deterministic, and required `CMD_SURVEY_START_PAIR` to match a
  prior `CMD_SURVEY_PREPARE_PAIR`. Omitted gateway sample count now defaults to
  the runtime queue capacity.
- Clarified that aborting a survey stops the gateway orchestrator as well as the
  addressed anchors, and that an already selected channel-9 ACK lane does not
  silently fall back to another neighbor or channel 5 after transmission fails.

### 2026-07-10 - 0.3.9

- Reserved the mandatory nine bytes of channel-9 batch metadata in extended
  normal-click CIR reports. The two raw-CIR chunks are now 881 and 271 bytes,
  so the first routed payload fits the 958-byte limit exactly.

### 2026-07-10 - 0.3.8

- Defined normal mesh-click partial-CIR delivery using the same 192-sample,
  1,152-byte window as the ML firmware, split into two independently routed
  extended diagnostic packets carrying 890 and 262 CIR bytes. Each packet may
  contain repeated byte-length CIR TLVs.
- Clarified that all gateway-originated commands, including anchor discovery,
  enter the priority channel-5 control path at the first safe radio boundary.
- Added `CMD_ASSIGN_DISCOVERY_SLOTS`: the gateway collects and verifies full
  anchor hashes, sorts by `(hash, anchor_id)`, assigns collision-free normal
  click reply slots, and floods the authoritative table. As clarified in
  0.3.12.1, an unassigned anchor uses a deterministic recovery slot so
  provisioning loss cannot suppress local click ownership.
- Extended `UWB_FINAL` by three bytes so the clicker can carry one signed
  response clock-offset sample plus a validity bit to the anchor. Click reports
  expose it separately from the anchor's own clock-offset sample.

### 2026-07-09 - 0.3.7

- Made click, channel-9 report, and gateway-ACK latency TLVs conditional on a
  real measurement; unknown latency is encoded by omitting the TLV.
- Removed duplicate click-report CIR encoding. The six-byte first-path sample
  is carried once as `UWB_CIR_SAMPLE`, while `ANCHOR_DIAG_BYTES` remains
  available for distinct anchor diagnostics.
- Reduced each bounded channel-5 flood burst to four opportunities and
  increased every flood-opportunity activity check to 20 ms. Successful
  repeats use 40 ms spacing; activity consumes an opportunity and applies
  randomized exponential backoff before the next remaining opportunity.
- Increased wake-train pre- and post-transmit activity probes to 20 ms.

### 2026-07-06 - 0.3.6

- Added explicit flood-control TLVs for gateway-originated channel-5 floods: random backoff maximum, random backoff slot size, retry count, and flood packet age.
- Clarified that gateway route advertisements and broadcast gateway commands keep the deterministic base relay delay, then add the advertised slotted random delay before the wake train and flood burst.
- Clarified that the flood packet age TLV is refreshed by each relay immediately before transmit, matching the shared packet header age.

### 2026-06-29 - 0.3.5

- Documented the ML clicker anchor-pair survey command `CMD_ML_START_ANCHOR_PAIR_SURVEY = 0x8002`: optional `DISCOVERY_SLOT_COUNT` only, request `SAMPLE_COUNT` ignored, and diagnostic `CLICK_REPORT` pair rows keyed by `INITIATOR_ID` and `RESPONDER_ID`.
- Added the UWB anchor-pair schedule/result frames to the protocol overview and clarified that partial pair rows remain valid when the final command result reports `COMMAND_TIMEOUT`.

### 2026-06-19 - 0.3.4

- Expanded `RANGE_SCHEDULE` to support up to eight selected anchors while keeping the standard UWB frame under 127 bytes.
- Changed `RANGE_SCHEDULE.max_exchanges` to a 16-bit field and removed serialized per-entry sample counts; selected-anchor entries now derive their sample count from the global `samples_per_anchor` field.
- Documented the ML collection limits of eight discovery/selected-anchor slots and up to 100 samples per selected anchor.

### 2026-06-18 - 0.3.3

- Replaced the gateway app-side USB serial host transport with a connected BLE GATT gateway service.
- Clarified that gateway host packets keep COBS stream framing over the BLE packet characteristic so full shared packets can span multiple ATT writes or notifications.
- Added the separate BLE debug-log notification stream used for gateway logs.

### 2026-06-18 - 0.3.2

- Mainlined the bring-up DS-TWR timing lesson as explicit provisional short-range and long-range reply-delay presets. The current long-range main firmware selects `UWB_RANGE_REPLY_DELAY_LONG_RANGE_UUS = 8000`; the lower `UWB_RANGE_REPLY_DELAY_SHORT_RANGE_UUS = 2750` candidate remains available for recalibration work.
- Documented that responder response and initiator final delayed transmissions are programmed relative to the last RX timestamp, matching the DWM3000 `DWT_START_TX_DLY_RS` path.

### 2026-06-17 - 0.3.1

- Updated the active UWB PHY contract to long-range mode: 850 kbps, 4096-symbol preamble, PAC32, 4073-symbol SFD timeout, no STS, and maximum configured DWM3000 TX power.
- Changed normal-click schedules to carry at most four anchors, reject burst windows below 400 ms, reject exchange strides below 30000 us, and require `UWB_RANGE_REPLY_DELAY_UUS = 8000`.
- Updated the `RANGE_SCHEDULE` frame size range and `BURST_DURATION_MS` default for the four-anchor, 400 ms schedule.

### 2026-06-15 - 0.3

Version 0.3 closes the 0.2.x protocol line after the firmware protocol grew from a compact packet reference into the full contract for UWB wake, discovery, ranging, mesh relay, gateway commands, and survey setup. The shared packet envelope now includes an always-present `MESSAGE_AGE_MS` field so in-flight relay delay travels with every packet instead of requiring gateway-synchronized anchor clocks. Mesh forwarding was tightened around route validity and timing freshness: route age alone no longer invalidates a path, channel-9 event timing is negotiated and supervised separately, and payload traffic runs only after channel-5 contact. Reliable delivery now has clearer mechanics, including hop-progress ACK telemetry, gateway ACK completion, randomized retransmission backoff, bounded rediscovery attempts, and the gateway-originated force-rediscovery command.

The click and ranging protocol also changed. Normal clicks now require three eligible discovery replies and three unique successful range results, matching the server-side solver requirement, while a schedule can still carry up to six anchors. `BURST_ID` is mandatory for grouping normal-click reports so retry bursts are never merged into the same logical result. Range-release behavior is explicit for one or two discovery replies, and zero replies do not create release traffic. The radio assumptions are now common across wake, discovery, route contact, ranging, and channel-9 mesh: 850 kbps, 1024-symbol preamble, PAC8, 1025-symbol SFD timeout, no STS, and the normalized DWM/DW3000 delayed-TX unit for `UWB_RANGE_REPLY_DELAY_UUS = 900`. Survey discovery was also converted from provisioned anchor slots to measured UWB discovery probes. Gateways send the slot count, defaulting to six, and anchors compute click and survey reply slots with `hash(anchor_id) % discovery_slot_count`, keeping setup deterministic without baking per-anchor slot values into the protocol.

### 2026-06-15 - 0.2.51

- Removed provisioned anchor slot values from the discovery protocol behavior.
- Defined discovery slot selection as `hash(anchor_id) % discovery_slot_count` for click discovery replies and survey discovery probes.
- Clarified that gateway survey discovery sends the slot count, defaulting to six slots unless the command includes `DISCOVERY_SLOT_COUNT`.

### 2026-06-15 - 0.2.50

- Added `MESH_HOP_ACK` progress telemetry so a gateway-bound sender can extend its gateway-ACK wait while downstream hops continue to move the packet.
- Added `CMD_FORCE_REDISCOVERY`, a gateway-originated command that lets an anchor report the command result, invalidate known mesh routes, and start bounded route rediscovery.
- Capped route rediscovery at five route-request attempts per target with exponential 250/500/1000/2000/4000 ms backoff plus jitter.
- Updated reliable-delivery rules so retransmit backoff is randomized and packet age includes ACK wait time plus retry backoff before retransmission.

### 2026-06-15 - 0.2.49

- Added the always-present shared packet `MESSAGE_AGE_MS` header field and updated mesh forwarding rules to age packets at each queue/relay step.
- Retired gateway time-sync command, sync-age TLV, and time-sync status bits; reports now carry local event uptime and rely on packet age for in-flight delay.
- Added survey discovery start/report mesh messages, the compact UWB survey discovery probe, deterministic discovery slots, and deterministic per-anchor mesh report slots.
- Updated broadcast forwarding so survey discovery start is the only floodable survey setup broadcast; discovery reports are gateway-bound and ACK-tracked.

### 2026-06-12 - 0.2.48

- Clarified that BLE courtesy scan and advertising can be active as host/controller roles while physical channel-37 RF remains single-event.
- Documented that local advertising TX is not passive scan RX time and must be modeled as receive blackout for courtesy interception estimates.

### 2026-05-29 - 0.2.47

- Changed the normal-click anchor threshold from four anchors to three because the server-side solver accepts three anchor distances.
- Clarified that normal-click range release applies to one or two discovery replies; zero replies send no release.
- Clarified that normal-click schedules may start from three discovery replies while still selecting up to six anchors.
- Changed normal-click burst acceptance to require three unique `RANGE_OK` anchors from the same click event and burst identity.
- Made `BURST_ID` mandatory for normal-click report grouping and documented that retry bursts must not be merged.
- Aligned arbitration wording with the firmware rule: freshness first, then higher attempt, lower priority, lower clicker, lower click event for different events.
- Normalized `UWB_RANGE_REPLY_DELAY_UUS = 900` wording as the DWM/DW3000 delayed-TX unit.

### 2026-05-18 - 0.2.46

- Added the BLE courtesy peer-finish wait byte used for higher-priority deferrals.

### 2026-05-18 - 0.2.45

- Clarified that channel-9 timing self-adjusts from received packets and that idle timing closes on supervision expiry.

### 2026-05-18 - 0.2.44

- Updated architecture and state-machine references after the full flowchart accuracy audit.

### 2026-05-18 - 0.2.43

- Updated architecture and state-machine references after the scheduled anchor ranging flowchart correction.

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

Older changes are in [[UWB+BLE Protocols and Strategies 0.2.45]].

## Abbreviations

| Abbreviation | Meaning |
| --- | --- |
| ACK | Acknowledgement. Confirms gateway delivery. |
| BLE | Bluetooth Low Energy. Used as a clicker-to-clicker courtesy side channel during UWB politeness and as the connected gateway-to-PC edge link. Operational wake, discovery, ranging, and mesh relay transport remain UWB-owned in v1 firmware. |
| COBS | Consistent Overhead Byte Stuffing. Frames binary shared packets over the connected gateway BLE packet stream. |
| CRC | Cyclic Redundancy Check. Detects corrupted packets. |
| DS-TWR | Double-Sided Two-Way Ranging. UWB distance measurement. |
| STS | Scrambled Timestamp Sequence. DWM3000 secure ranging feature. |
| TLV | Type-Length-Value. Extensible payload field format. |
| TTL | Time To Live. Hop limit that prevents infinite forwarding. |
| UWB | Ultra-Wideband. Used for distance measurements. |

## Packet Envelope

All mesh payloads and gateway host packets use the shared IMEC binary envelope. UWB mesh frames wrap one shared packet for over-the-air relay. Gateway host packets use the connected BLE packet characteristic. UWB control/ranging frames use separate compact layouts.

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
| 28 | 4 | Message age ms | Saturating age in milliseconds since the original source created this packet. |
| 32 | variable | Payload | TLV fields for the message type. |
| 32 + len | 2 | CRC16 | CRC-16/CCITT-FALSE over bytes 0 through 31 + len. Little-endian. |

Total header is 32 bytes. Maximum payload is 255 bytes. Maximum packet is 32 + 255 + 2 = 289 bytes.

The gateway BLE packet characteristic carries a byte stream of COBS-delimited shared packets. One COBS frame may span multiple ATT writes or notifications. UWB mesh transport wraps one shared packet in a `UWB_MESH` frame with `network_id`, previous-hop ID, next-hop ID, packet length, and CRC. The maximum extended UWB mesh frame before the radio FCS is 1,021 bytes: 25 bytes of UWB mesh header, one 994-byte extended shared packet (34-byte header, 958-byte payload, and 2-byte packet CRC), and a 2-byte mesh-frame CRC. The radio adds its 2-byte FCS to reach the 1,023-byte PHY limit.

`Message age ms` is initialized to zero when a source creates a new shared packet. Every queue, relay, retransmit, and forwarded send adds elapsed local wall time before serialization, saturating at `UINT32_MAX`. The field is not part of duplicate identity and does not replace `src_id`, `dst_id`, `session_id`, or `seq`; it lets receivers compensate for mesh delay without requiring gateway-synchronized anchor clocks.

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
| `0x15` | `UWB_SURVEY_DISCOVERY_PROBE` | Anchor → Nearby anchors (survey discovery slot probe) |
| `0x20` | `CLICK_REPORT` | Anchor → Gateway (mesh) |
| `0x21` | `SELF_TEST_REPORT` | Clicker → Gateway (mesh) |
| `0x22` | `ANCHOR_HEARTBEAT` | Anchor → Gateway (mesh) |
| `0x30` | `MESH_DATA` | Any → Any (inside UWB mesh frame) |
| `0x31` | `MESH_HOP_ACK` | Downstream relay → Original sender (progress ACK inside UWB mesh frame) |
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
| `0x54` | `SURVEY_DISCOVERY_START` | Gateway → Anchors (broadcast mesh) |
| `0x55` | `SURVEY_DISCOVERY_REPORT` | Anchor → Gateway (mesh) |
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
| `0x000B` | Reserved | Retired gateway time-sync command; do not emit in v1 firmware. |
| `0x000C` | `CMD_FORCE_REDISCOVERY` | Target anchor reports the command result, invalidates known mesh routes, and starts bounded rediscovery toward the gateway. |
| `0x0100` | `CMD_SURVEY_REACHABILITY` | Start anchor reachability survey. |
| `0x0101` | `CMD_SURVEY_PREPARE_PAIR` | Prepare an anchor pair for UWB ranging. |
| `0x0102` | `CMD_SURVEY_START_PAIR` | Start UWB ranging for a prepared pair. |
| `0x0103` | `CMD_SURVEY_ABORT` | Abort the current survey. |
| `0x0104` | `CMD_ASSIGN_DISCOVERY_SLOTS` | Ask reachable anchors for verified full hashes, assign their normal-click reply order, flood the resulting table, and wait for table acknowledgements. The host addresses this command to the gateway itself. |
| `0x8000-0xFFFF` | Vendor-specific range | Project-local command IDs. |
| `0x8000` | `CMD_ML_START_COLLECTION` | Start ML clicker full diagnostic collection. |
| `0x8001` | `CMD_ML_START_FAST_RANGING` | Start ML clicker range-only collection. |
| `0x8002` | `CMD_ML_START_ANCHOR_PAIR_SURVEY` | Start ML clicker anchor-pair survey collection. |
| `0x8003` | `CMD_ML_START_LIVE_TRACKING` | Start ML clicker repeated range-only tracking. |
| `0x8004` | `CMD_ML_LIVE_TRACKING_HEARTBEAT` | Keep an active ML clicker live-tracking command alive. |
| `0x8005` | `CMD_ML_STOP_LIVE_TRACKING` | Stop an active ML clicker live-tracking command. |

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
| `0x07` | `TIMESTAMP_MS` | 8 | Sender-local millisecond event timestamp for range reports, survey samples, status, and heartbeats. Mesh delay is represented by the shared packet `Message age ms` header field. |
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
| `0x27` | Reserved | Retired gateway time-sync age TLV; do not emit in v1 firmware. |
| `0x28` | `RANGE_ROUND_INDICES` | variable (1 × count) | Per-distance-sample round-robin index transmitted by the clicker in the accepted DS-TWR range header. Values match `DISTANCE_SAMPLES_MM` order. |
| `0x29` | `SEQUENCE_START_TIMESTAMPS_MS` | variable (8 × count) | Per-distance-sample sender-local millisecond start timestamp for the anchor's own observed ranging sequence. Values match `DISTANCE_SAMPLES_MM` order. |
| `0x2A` | `MESH_CHANNEL` | 1 | Mesh payload channel for a negotiated event. Current channel-9 events encode `9`. |
| `0x2B` | `MESH_EVENT_INTERVAL_MS` | 4 | Interval between negotiated channel-9 events. |
| `0x2C` | `MESH_EVENT_WINDOW_MS` | 2 | Receive/transmit window length for the negotiated channel-9 event. |
| `0x2D` | `MESH_NEXT_EVENT_TIME_MS` | 4 | Relative delay to the next channel-9 event when serialized; the receiver converts it into local uptime. |
| `0x2E` | `MESH_EVENT_COUNTER` | 4 | Monotonic event counter for supervision and stale-event detection. |
| `0x2F` | `MESH_EVENT_GUARD_MS` | 2 | Guard time reserved for retune, PLL readiness, RX setup, and software jitter. |
| `0x30` | `MESH_CLOCK_SKEW_PPM` | 2 | Drift estimate or guard bound for the negotiated peer timing. |
| `0x31` | `MESH_MAX_MISSED_EVENTS` | 1 | Missed channel-9 event accounting limit for diagnostics and stale-event detection. |
| `0x32` | `MESH_SUPERVISION_TIMEOUT_MS` | 4 | Maximum age of channel-9 timing before the timing entry closes and channel-5 refresh is required. |
| `0x33` | `DIAG_STATUS_FLAGS` | 4 | Diagnostic presence, truncation, drop, and unavailable flags. |
| `0x34` | `BURST_ID` | 4 | Identifies one shared ranging burst for reports and diagnostics. Mandatory for normal-click report grouping. |
| `0x35` | `EXCHANGE_STRIDE_US` | 4 | Configured minimum stride between scheduled DS-TWR exchanges. |
| `0x36` | `BURST_DURATION_MS` | 2 | Shared responder burst duration. Current normal-click default is 400 ms. |
| `0x37` | `CLICK_LATENCY_MS` | 4 | Time from accepted click start to reportable ranging result. Included only when measured. |
| `0x38` | `UWB_AWAKE_TIME_US` | 4 | Per-click UWB active time, excluding retained sleep. |
| `0x39` | `DIAG_BYTES_CAPTURED` | 2 | Diagnostic bytes captured locally before truncation. |
| `0x3A` | `DIAG_BYTES_TRANSMITTED` | 2 | Diagnostic bytes transmitted in report fragments. |
| `0x3B` | `DIAG_BYTES_TRUNCATED` | 2 | Diagnostic bytes omitted due to payload limits. |
| `0x3C` | `DIAG_FRAMES_DROPPED` | 2 | Clicker or anchor diagnostic frames unavailable or dropped. |
| `0x3D` | `REPORT_FRAGMENT_COUNT` | 1 | Number of report fragments for the aggregate. |
| `0x3E` | `CHANNEL9_REPORT_LATENCY_MS` | 4 | Report latency through negotiated channel-9 mesh. Included only when measured. |
| `0x3F` | `GATEWAY_ACK_LATENCY_MS` | 4 | Time from report send to end-to-end gateway ACK. Included only when measured; it cannot describe the ACK for the packet carrying it. |
| `0x40` | `CLICKER_DIAG_BYTES` | variable | Compact clicker-side diagnostic bytes captured after `FINAL`. |
| `0x41` | `ANCHOR_DIAG_BYTES` | variable | Distinct anchor-side diagnostic bytes captured after valid `FINAL`; it must not duplicate `UWB_CIR_SAMPLE`. |
| `0x42` | `PHY_CONFIG_ID` | 1 | Encodes channel/data-rate/preamble/PAC/no-STS PHY configuration. |
| `0x43` | `MESH_CHANNEL_SWITCHES` | 4 | Count of channel switches attempted by the mesh scheduler. |
| `0x44` | `MESH_PLL_READY_FAILURES` | 4 | Count of mesh retune/PLL readiness failures. |
| `0x45` | `MESH_LATE_CHANNEL5_RETURNS` | 4 | Count of late returns from channel 9 to required channel-5 work. |
| `0x46` | `MESH_DEFERRALS` | 4 | Count of channel-9 work deferred for higher-priority work. |
| `0x47` | `MESH_CH9_EVENT_MISSES` | 4 | Count of missed channel-9 events. |
| `0x48` | `MESH_CHANNEL5_PREEMPTIONS` | 4 | Count of channel-5 click or scan preemptions over channel-9 mesh. |
| `0x49` | `MESH_CH9_REPORT_LATENCY_MS` | 4 | Recent or aggregate channel-9 report latency exported in heartbeat/status. |
| `0x4A` | `DISCOVERY_START_DELAY_MS` | 4 | Delay from creation of a survey discovery start packet to the first survey discovery UWB slot. Receivers subtract packet age. |
| `0x4B` | `DISCOVERY_SLOT_MS` | 4 | Duration of each survey discovery UWB slot. |
| `0x4C` | `DISCOVERY_SLOT_COUNT` | 1 | Number of reply slots. Normal clicks use the fixed 50-slot assigned space; survey and ML discovery use their mode-specific slot count. |
| `0x4F` | `UWB_CIR_FULL_CHUNK` | variable | Raw six-byte accumulator data from a bounded CIR window. A normal mesh CIR packet concatenates repeated values in wire order; each value is at most 255 bytes. ML host reports retain their 128-byte chunking. |
| `0x50` | `UWB_CIR_BYTE_OFFSET` | 2 | Byte offset of this CIR chunk within the captured partial window. |
| `0x51` | `UWB_CIR_TOTAL_BYTES` | 2 | Expected byte length of the complete partial CIR window. |
| `0x52` | `UWB_CIR_FIRST_PATH_INDEX` | 2 | DWM3000 first-path accumulator index associated with the capture. |
| `0x55` | `DIAG_FRAGMENT_INDEX` | 2 | Zero-based fragment index within this diagnostic stream. |
| `0x56` | `DIAG_FRAGMENT_COUNT` | 2 | Total number of fragments in this diagnostic stream. |
| `0x58` | `UWB_CIR_START_INDEX` | 2 | Accumulator index represented by the first six-byte sample at byte offset zero. |
| `0x9E` | `FLOOD_RANDOM_BACKOFF_MAX_MS` | 4 | Maximum random delay a relay may add before forwarding a gateway-originated flood. Default is 2500 ms. |
| `0x9F` | `FLOOD_RANDOM_BACKOFF_SLOT_MS` | 2 | Slot size for the random flood delay. Relays choose an integer slot that does not exceed the maximum. Default is 600 ms. |
| `0xA0` | `FLOOD_RETRY_COUNT` | 1 | Number of additional full flood retransmission bursts requested by the gateway. Default is 0. |
| `0xA1` | `FLOOD_PACKET_AGE_MS` | 4 | Flood-local age mirror. Relays refresh it immediately before transmit to match the shared packet header age. |
| `0xA4` | `DISCOVERY_ASSIGNMENT_PHASE` | 1 | `1` requests anchor hash claims; `2` carries the authoritative assignment table; `3` acknowledges that exact table epoch after durable installation. |
| `0xA5` | `DISCOVERY_ASSIGNMENT_EPOCH` | 4 | Correlates claims and the resulting assignment table. |
| `0xA6` | `DISCOVERY_ASSIGNMENT_HASH` | 8 | Full deterministic hash claimed by an anchor and verified by the gateway against the packet source ID. |
| `0xA7` | `DISCOVERY_ASSIGNMENT_TABLE` | variable, repeated | Packed 17-byte entries: `anchor_id:u64`, `hash:u64`, `slot:u8`. Entries are ordered by `(hash, anchor_id)` and may span repeated TLVs. |
| `0xA8` | `CLICKER_CLOCK_OFFSET_RAW` | 2 (i16) | Clicker-side DWM3000 clock-offset sample captured while receiving `UWB_RESP`; present only when the clicker marked it valid in `UWB_FINAL`. |

## Retired Legacy Discovery IDs

Message IDs `0x01` and `0x02` belonged to a historical discovery advertising design. They are reserved so older logs remain identifiable, but the native firmware core no longer builds codecs or tests for these payloads. Operational click discovery uses the UWB wake/discovery frames below.

## BLE Courtesy Advertisement

This section defines the courtesy payload. The system reason for courtesy BLE and the timing tradeoff are in [[UWB+BLE Architecture 0.6.3]].

The advertisement is legacy non-connectable manufacturer data on primary advertising channel 37 only. Channel 38 and 39 are disabled on transmit, and the passive scan is also limited to channel 37. If the controller cannot enforce that scan channel, the firmware disables BLE courtesy for that attempt and relies on UWB politeness plus retry-side contention.

The clicker uses the fastest legal BLE 5.x non-connectable interval for the nRF52833 target: 20.0 to 20.625 ms plus the controller advertising delay. Passive scanning uses a 25 ms interval and a 20 ms window. The BLE scan and advertisement stop before the DWM3000 wake-claim train begins.

The firmware starts passive scanning before advertising. Both BLE roles can be active at the host/controller level, but the physical channel-37 radio is single-event: local advertising TX is not simultaneous scan RX. Simulations of peer detection must therefore treat each local advertising event as scan-RX blackout unless calibrated controller traces prove a different schedule.

Manufacturer data payload:

| Offset | Length | Field | Notes |
| ---: | ---: | --- | --- |
| 0 | 2 | Company ID | Little-endian `0xffff` prototype company ID. |
| 2 | 1 | Marker/version | `0xc2`; rejects unrelated manufacturer payloads. |
| 3 | 4 | `network_id` | Little-endian project network ID. |
| 7 | 8 | `clicker_id` | Full clicker identity for stable tie-breaks. |
| 15 | 4 | `click_event_id` | Normal click event sequence. |
| 19 | 1 | `attempt_index` | Nonzero UWB wake attempt number. |
| 20 | 8 | `priority_id` | Attempt-local randomized priority ID. |
| 28 | 1 | `defer_duration_units` | Higher-priority peer wait, in 10 ms units, rounded up and saturated. |

The serialized manufacturer data length is 29 bytes. Including the BLE AD length and type bytes, the single advertising data structure is 31 bytes and fits exactly inside the legacy advertising payload limit without needing scan responses or extended advertising.

## UWB Wake, Discovery, And Schedule Frames

All operational UWB modes use the same base PHY unless a frame explicitly says otherwise: 850 kbps, 4096-symbol preamble, PAC32, STS disabled, SFD timeout 4073, maximum configured DWM3000 TX power, and DWM3000 `IP_CONFIG_LO.IP_NTM=12` for the lower recommended first-path threshold. Channel 5 carries wake, discovery, route contact, route timing refresh, and ranging. Channel 9 carries negotiated mesh payload events after channel-5 contact exists.

Every UWB control frame begins with marker `0xCA`, version `0x01`, and a type byte, and ends with CRC-16/CCITT-FALSE over the frame bytes before the CRC. Multi-byte fields are little-endian.

| Frame | Length | Required identity/freshness fields | Purpose |
| --- | ---: | --- | --- |
| `WAKE_CLAIM` | 49 B | `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `priority_id`, `nonce`, wake/ranging channels, flags | Repeated long-preamble wake claim. Anchors create or update an ownership epoch only after this CRC-valid frame decodes and passes local network, channel, flag, freshness, and arbitration checks. |
| `DISCOVER` | 32 B | `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `nonce`, `discovery_slot_count`, flags | Sent by the selected clicker after the wake train. Anchors reply only if it matches their epoch. |
| `DISCOVERY_REPLY` | 44 B | `network_id`, `anchor_id`, `selected_clicker_id`, `click_event_id`, `attempt_index`, `nonce`, slot, status, quality, flags | Presence-only assigned-slot reply for normal mesh clicks. It is not a range measurement. |
| `SURVEY_DISCOVERY_PROBE` | 24 B | `network_id`, `survey_id`, `anchor_id`, anchor slot, slot count, flags | Anchor presence probe sent in that anchor's hash-derived survey discovery slot. It is not a range measurement. |
| `RANGE_RELEASE` | 34 B | `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `nonce`, `discovered_anchor_count`, `min_anchor_count`, reason, flags | Releases anchors that replied when a normal click cannot range because fewer than three eligible anchors replied. |
| `RANGE_SCHEDULE` | 58-121 B | `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `nonce`, ranging channel, no-STS mode, diagnostics-required flag, burst window, exchange stride, maximum exchanges, minimum successful anchors, selected anchors | Authorizes selected anchors and serializes DS-TWR poll order. Samples are round-robin across anchors inside one shared burst. |
| `ANCHOR_PAIR_SCHEDULE` | 60-114 B | `network_id`, `clicker_id`, `survey_id`, `attempt_index`, `nonce`, selected anchors, pair timing, reply delay, flags | ML clicker schedule that asks discovered anchors to measure every unique anchor pair once. |
| `ANCHOR_PAIR_RESULT` | 56 B | `network_id`, `clicker_id`, `survey_id`, `nonce`, initiator, responder, pair index/count, sequence, range status, quality, distance, RSL, flags | Anchor-to-clicker result for one scheduled anchor-pair DS-TWR exchange. |

`RANGE_SCHEDULE` has a 47-byte fixed portion, then one 9-byte entry per selected anchor, then a 2-byte CRC. `max_exchanges` is serialized as little-endian `u16`. Each selected-anchor entry carries `anchor_id:u64` and `seq_base:u8`; it does not carry a separate sample count. Receivers derive each entry's sample count from the global `samples_per_anchor` field. This keeps the maximum eight-anchor schedule at 121 bytes, below the 127-byte standard UWB frame limit.

`ANCHOR_PAIR_SCHEDULE` has a 40-byte fixed portion, then one 9-byte entry per selected anchor, then a 2-byte CRC. Each selected-anchor entry carries `anchor_id:u64` and that anchor's start-delay byte in 5 ms units. The ML clicker uses the schedule only for diagnostic anchor-pair survey collection: it selects 2-8 discovered anchors, schedules each unique pair once, and expects one `ANCHOR_PAIR_RESULT` frame per pair.

`WAKE_CLAIM` timing fields are bounded protocol inputs, not arbitrary sleep commands. `wake_train_ends_in_ms` and `discovery_starts_in_ms` must be no more than 1000 ms, `discovery_starts_in_ms` must be greater than or equal to `wake_train_ends_in_ms`, and `claimed_duration_ms` must cover both while staying no more than 2000 ms.

Wake-claim freshness is evaluated before competing-clicker priority arbitration. If a claim matches the active `network_id`, `clicker_id`, `click_event_id`, `attempt_index`, `priority_id`, `nonce`, and mode flags, it refreshes the same ownership epoch. If the same clicker/event/nonce/mode repeats the same `attempt_index` with a different `priority_id`, the claim is malformed and cannot refresh the epoch. If the same clicker/event/nonce/mode uses a higher `attempt_index`, it replaces the older ownership epoch; a lower `attempt_index` is stale. If `network_id`, `clicker_id`, and `click_event_id` match the active epoch but `nonce` or mode flags differ, the claim is malformed and does not enter priority arbitration. Claims for different clicker events then arbitrate by higher `attempt_index`, lower `priority_id`, lower `clicker_id`, and lower `click_event_id`.

Anchors accept `RANGE_RELEASE` only while waiting after a discovery reply for the matching selected epoch. The release must match network, clicker, click event, attempt, nonce, and flags. The only current reason is `INSUFFICIENT_ANCHORS=1`. `discovered_anchor_count` must be at least one and less than `min_anchor_count`; for normal clicks, `min_anchor_count` is `UWB_NORMAL_CLICK_MIN_ANCHORS = 3`, so release covers one or two eligible replies. Zero replies send no release because no anchor epoch needs clearing. On acceptance, the anchor clears that click epoch and resumes low-duty scan.

Anchors reject schedules that do not match the active epoch, wrong network/channel/nonce, any ranging channel other than 5, any reply delay other than the compiled `UWB_RANGE_REPLY_DELAY_UUS` in the DWM/DW3000 delayed-TX unit, STS enabled, diagnostics not required, a burst window below 400 ms, an exchange stride below 30000 us, more than 8 selected anchors, duplicate selected anchors, missing selected samples, or per-anchor sequence ranges that would wrap past `255`. The current long-range main firmware selects `UWB_RANGE_REPLY_DELAY_LONG_RANGE_UUS = 8000`; `UWB_RANGE_REPLY_DELAY_SHORT_RANGE_UUS = 2750` is a provisional lower-delay candidate. Both values still need recalibration against the final firmware timing path. Normal-click schedules also require at least three selected anchors; the current firmware encodes `minimum_successful_anchors` as `UWB_NORMAL_CLICK_MIN_ANCHORS = 3`, and validators reject values below three or above `selected_anchor_count`. Diagnostic/self-test schedules may use fewer anchors. The legacy poll-spacing field remains bounded for compatibility, but the active burst schedule is governed by `burst_window_ms` and `exchange_stride_us`.

Normal mesh-click discovery uses gateway-assigned slots. A host sends `CMD_ASSIGN_DISCOVERY_SLOTS` to the gateway. The gateway runs at least two bounded claim rounds and extends convergence when a previously unseen valid claim arrives. Each receiving anchor keeps its previously committed normal-click assignment while returning its full deterministic hash through the normal reliable mesh path. Claim and acknowledgement response delays combine a dense response slot with route hop count, scheduling farther anchors earlier within each slot, and randomized exponential retry backoff. The gateway rejects hashes that do not match the result packet's source ID, de-duplicates anchor IDs, sorts valid claims by `(hash, anchor_id)`, assigns dense slots starting at zero, and gives each generated table a unique command sequence within the assignment epoch.

The authoritative table declares the fixed 50-slot normal-click reply space separately from the number of listed anchors. An anchor validates the complete ordered table, finds its own ID, and persists `(gateway_id, anchor_id, epoch, slot, slot_count)` before sending phase `3` for that exact table sequence. A persistence failure is not acknowledged. An anchor omitted from the authoritative table clears its persisted assignment; an anchor that receives a table before its claim was collected sends another claim instead of inventing a hash slot. The gateway retries the table with bounded randomized exponential backoff and reports host success only when every listed anchor has acknowledged it. A committed assignment is restored after reboot. There is no hash-slot fallback.

Survey discovery and ML clicker discovery remain hash-derived and independent of the normal-click assignment table. Those modes use `hash(anchor_id) % slot_count`, with the configured or command-supplied slot count as their reply space. ML collection currently supports `1..8` slots for normal and fast-ranging collection and `2..8` for anchor-pair survey collection. Gateway survey discovery defaults to six slots and may override the count with `DISCOVERY_SLOT_COUNT` up to 50. Stable full device IDs remain required; per-anchor normal-click slot flash parameters are not part of the runtime contract.

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

Frame sizes before the radio FCS: poll 42 B, response 50 B, final 57 B, report 50 B. The final adds one diagnostic-validity byte and one signed 16-bit clicker clock-offset sample. Only bit 0 is defined; when it is clear the sample must be zero. Full IDs, short addresses, `network_id`, event/session ID, nonce, nonzero sequence, clicker-transmitted round index, and flags must all match the selected clicker/event/anchor identity. The responder response and initiator final are both delayed from the last RX timestamp, not from a host-side absolute timestamp; this keeps the selected fixed reply delay symmetric and avoids making firmware wake/parse latency part of the timestamp math. For the DS-TWR timing rationale, equal reply-delay validation, and timing rejection policy, see [[UWB+BLE Architecture 0.6.3]].

Each POLL must use the selected anchor's full responder ID and matching short address from the schedule. Broadcast POLL is not valid for normal clicks, diagnostics, or survey ranging.

`UWB_CLICKER_DIAG` remains an optional post-`FINAL` diagnostic frame for ML or high-debug collection. Normal mesh ranging does not require it for the clock-offset sample because that compact value travels in `UWB_FINAL`. Missing or corrupt optional clicker diagnostics do not invalidate the completed range.

## Mesh Protocol

The mesh is reactive. Nodes discover a path when a real packet needs one, then send shared packets inside UWB mesh frames while the route remains valid. Route age alone does not prove a path stale; replacement, explicit clear, or delivery failure does. For the full route state, cost formula, retry behavior, and click-priority details, see [[UWB+BLE Architecture 0.6.3]].

### Mesh Packet Shapes

| Packet | Header flags | Main payload TLVs | Purpose |
| --- | --- | --- | --- |
| Gateway-bound click report | `GATEWAY_ACK_REQUIRED`; `src_id`=anchor; `dst_id`=gateway; `ttl=4` | `CLICKER_ID`, `ANCHOR_ID`, `EVENT_SEQ`, `BURST_ID`, `TIMESTAMP_MS`, `DISTANCE_MM`, `SAMPLE_COUNT`, `DISTANCE_SAMPLES_MM`, `RANGE_ROUND_INDICES`, `SEQUENCE_START_TIMESTAMPS_MS`, `QUALITY`, `RANGE_STATUS`, `UWB_RSL_DBM`, `UWB_CIR_SAMPLE` | Carries measured data toward the gateway. Normal click reports set `COUNT_AS_CLICK`, clear `DIAGNOSTIC`, and include burst identity for grouping. Packet age records time spent queued or relayed after report creation. |
| Gateway-bound self-test report | `GATEWAY_ACK_REQUIRED`; `src_id`=clicker; `dst_id`=gateway; `ttl=4` | `CLICKER_ID`, `EVENT_SEQ`, `ERROR_CODE`, `BATTERY_MV` | Carries the local clicker self-test result toward the gateway. Self-test reports set `DIAGNOSTIC` and clear `COUNT_AS_CLICK`. |
| Gateway ACK | `GATEWAY_ACK`; `src_id`=gateway; `dst_id`=original source; `ttl=4` | `REQUESTED_MSG_SEQ` | Confirms the gateway received the original packet. Routed back like a normal packet inside UWB mesh frames. |
| Mesh hop ACK | none; `src_id`=downstream relay; `dst_id`=original source; `ttl=4` | `REQUESTED_MSG_SEQ` | Confirms hop progress for an ACK-required packet after a relay forwards it. It extends the sender's gateway-ACK wait but does not prove gateway delivery. |
| Gateway command | none; `src_id`=gateway; `dst_id`=target anchor; `ttl=4` | `COMMAND_ID` plus command-specific TLVs | Extensible command to one anchor. Completion is the matching `COMMAND_RESULT`. |
| Command result | `GATEWAY_ACK_REQUIRED`; `src_id`=target; `dst_id`=gateway; `ttl=4` | `COMMAND_ID`, `COMMAND_STATUS`, optional `ERROR_CODE` or result TLVs | Returns command outcome to the gateway. |
| Gateway route advertisement | none; `src_id`=gateway; `dst_id`=broadcast; `ttl=8` | `GATEWAY_ID`, `GATEWAY_EPOCH`, `GATEWAY_ROUTE_SEQ`, `HOP_COUNT`, `PATH_QUALITY_MIN`, `ACCUMULATED_COST`, `RELAY_CAPACITY_STATE`, `CAPACITY_VALIDITY_INTERVAL_MS`, `FLOOD_PROFILE_VERSION`, `FLOOD_EPOCH_ID`, `SLOT_SEED`, flood-control TLVs | Gateway "Here I Am" route seed. Anchors may install it as an upstream route and forward it as a controlled channel-5 flood. |
| Survey discovery start | none; `src_id`=gateway; `dst_id`=broadcast; `ttl=4` | `SURVEY_ID`, `DISCOVERY_START_DELAY_MS`, `DISCOVERY_SLOT_MS`, `DISCOVERY_SLOT_COUNT` | Flooded setup packet for the UWB survey discovery epoch. Receivers subtract packet age from the start delay so anchors start at roughly the same slot despite relay delay. |
| Survey discovery report | `GATEWAY_ACK_REQUIRED`; `src_id`=anchor; `dst_id`=gateway; `ttl=4` | `SURVEY_ID`, `ANCHOR_ID`, zero or more `REACHABILITY_ENTRY` values | Reports UWB-measured survey discovery reachability after that anchor's hash-derived mesh report slot opens. |
| Route request | `src_id`=requester; `dst_id`=broadcast; `ttl=4` | `INITIATOR_ID`, `RESPONDER_ID`, `ROUTE_EPOCH`, `HOP_COUNT`, `QUALITY` | UWB mesh discovery request. |
| Route reply | `src_id`=target; `dst_id`=requester; `ttl=4` | `INITIATOR_ID`, `RESPONDER_ID`, `ROUTE_EPOCH`, `HOP_COUNT`, `QUALITY` | UWB mesh discovery reply along the reverse path. |
| Mesh event propose/accept/update | directed unicast | `MESH_CHANNEL`, `MESH_EVENT_INTERVAL_MS`, `MESH_EVENT_WINDOW_MS`, `MESH_NEXT_EVENT_TIME_MS`, `MESH_EVENT_COUNTER`, `MESH_EVENT_GUARD_MS`, `MESH_CLOCK_SKEW_PPM`, `MESH_MAX_MISSED_EVENTS`, `MESH_SUPERVISION_TIMEOUT_MS` | Negotiates bounded channel-9 payload events after channel-5 contact exists. |
| Mesh event end | directed unicast | optional `REASON` | Clears channel-9 timing for the next hop. |

### Forwarding Rules

1. **Validate** magic, version, payload length, and CRC. Invalid packets are dropped.
2. **Detect duplicates** by `(msg_type, src_id, dst_id, session_id, seq)`. A local duplicate is not delivered twice; the gateway may re-emit a gateway ACK. A directed duplicate that still needs forwarding is re-forwarded only when the relay is not already tracking a gateway-bound transmission. Duplicate cache entries expire after 60 s.
3. **Busy relay**: if a new packet would require forwarding or an immediate local response while the node already has a tracked gateway-bound transmission in flight, drop it and do not cache the duplicate.
4. **Local delivery**: if `dst_id` is this node, handle locally. Gateways emit `GATEWAY_ACK` for gateway-bound packets that requested it. Anchors receiving a directed command emit `COMMAND_RESULT`. Broadcast survey discovery start packets are handled locally without command results.
5. **TTL zero**: if `dst_id` is not local and `ttl` is zero, drop and record a route failure.
6. **Route selection**: select among currently valid candidates. Do not expire a route by age alone. Routes use `effective_cost = hop_count * 100 + (100 - quality)`. Ties are broken by higher quality, fewer hops, newer observation time, then lower next-hop ID. See [[UWB+BLE Architecture 0.6.3]] for the full cost derivation and quality mapping.
7. **Forward** the same packet with `ttl - 1`. Relays never rewrite `src_id`, `dst_id`, `session_id`, `seq`, or payload. Before forwarding or retransmitting, they add local elapsed time to `Message age ms`, saturating at `UINT32_MAX`. Broadcast forwarding is limited to survey discovery start packets. After a relay successfully forwards an ACK-required unicast, it sends `MESH_HOP_ACK` back toward the original source.
8. **Gateway ACK**: for gateway-bound packets with `GATEWAY_ACK_REQUIRED`, keep the original sender's packet pending until the gateway ACK arrives. The base gateway-ACK timeout is 2 s, but each matching `MESH_HOP_ACK` resets that 2 s window because the packet is still progressing downstream. Missing gateway ACKs mark a route failure, wait a randomized 100/250/500 ms retry backoff, then retransmit if a current or alternate candidate remains. Packet age is refreshed at timeout and again before retransmit so the retried packet includes both the ACK wait and backoff delay.
9. **Command serialization**: the gateway BLE ingress queue holds two commands and preserves arrival order. The head retries transient radio, route, and queue-admission errors up to eight times with capped randomized exponential backoff; later commands cannot overtake it. End-to-end result tracking remains single-command: a matching `COMMAND_RESULT` clears the wait, while no result within 12 s emits `COMMAND_TIMEOUT` over the connected BLE gateway packet stream. A full ingress queue or exhausted retry budget produces an explicit failure instead of silent replacement.
10. **Channel selection**: route discovery, route refresh, unknown-contact recovery, and mesh event control start on channel 5. Payload data, gateway ACKs, command results, heartbeats, and reports use channel 9 only when the selected next hop has fresh accepted event timing. Once a gateway or hop ACK has selected a negotiated channel-9 lane, a transmission failure is returned to the ACK owner with the original packet still retained; that attempt does not silently switch to another channel-9 neighbor or channel 5.
11. **Click priority**: an accepted local `WAKE_CLAIM` preempts active mesh forwarding and clears pending mesh RX work. CRC-valid claims that fail network, channel, flag, freshness, or arbitration checks are ignored for preemption. Already-built local click reports are requeued for later delivery. See [[UWB+BLE Architecture 0.6.3]] for the full click-priority mechanism.

### Gateway-Originated Flood Controls

Gateway route advertisements and broadcast gateway commands carry an explicit flood-control contract. The gateway sets `FLOOD_RANDOM_BACKOFF_MAX_MS`, `FLOOD_RANDOM_BACKOFF_SLOT_MS`, `FLOOD_RETRY_COUNT`, and `FLOOD_PACKET_AGE_MS`; if a legacy command flood omits them, firmware adds the default `2500 ms` maximum random backoff, `600 ms` random slots, retry count `0`, and age `0`.

A relay that forwards one of these floods keeps the existing deterministic base delay from `SLOT_SEED`, hop count, and local ID. It then adds one slotted random delay from `0..FLOOD_RANDOM_BACKOFF_MAX_MS` before starting the channel-5 wake train; only slot multiples less than or equal to the maximum are valid. The wake train and bounded flood burst therefore move together; a relay must not send the wake train immediately and wait only for the final flood frames. One burst has four transmission opportunities. Each opportunity first checks channel 5 for 20 ms. Successful repeats use 40 ms spacing; detected activity consumes the current opportunity and schedules the next remaining opportunity with randomized exponential backoff. If `FLOOD_RETRY_COUNT` is nonzero, the relay sends that many additional full wake-train-plus-burst retransmissions after the first burst, separated by the normal post-root guard.

`FLOOD_PACKET_AGE_MS` is not a replacement for the shared packet header age. It is a flood-local mirror carried in the TLV payload for future receivers that inspect only the payload. Relays refresh it immediately before transmit so it matches the packet header age after queue and relay delay.

### Route Formation Rules

1. A sender with data and no usable route sends `ROUTE_REQ` inside UWB mesh frames during its channel-5 route-listen opportunity.
2. Every receiver stores a reverse breadcrumb to the requester through the previous hop.
3. If the receiver is not the target, it rebroadcasts with `HOP_COUNT + 1` and reduced path quality (weakest-link propagation).
4. The target sends `ROUTE_REPLY` back through the reverse path.
5. Every receiver of the reply stores a route to the target through the previous hop.
6. The requester proposes or refreshes bounded channel-9 event timing for the selected next hop.
7. After timing is accepted, the requester sends the data packet inside a UWB mesh frame during a negotiated channel-9 event. Each receiver adjusts its local next-event timing from the observed channel-9 packet arrival, so normal drift can heal without channel 5. Missed windows keep advancing on channel 9 until supervision expires; then the timing entry closes, the route can remain valid, and payload delivery refreshes channel-5 contact before using channel 9 again.

Route rediscovery is bounded per target: at most five `ROUTE_REQ` attempts are emitted without a successful route-ready event. Attempts use exponential 250/500/1000/2000/4000 ms backoff plus jitter. A route reply, successful gateway ACK, or command-installed route resets the budget. A previously invalidated candidate can become selected again if rediscovery advertises it with the current route epoch and it wins the normal route-cost comparison.

The operational route flow is shown in [[Mesh Connected Routing Walkthrough]].

### Downlink Directory

The gateway keeps a flat `target_id → next_hop_id` directory, not a full topology map. If no entry exists for a target, the gateway sends a `ROUTE_REQ` inside a UWB mesh frame and keeps the BLE host command pending. The gateway serializes commands: only one outstanding command at a time. `CMD_FORCE_REDISCOVERY` lets the gateway ask a target anchor to invalidate its known routes after attempting the command result, then begin bounded rediscovery toward the gateway.

### Packet Age And Local Time

There is no gateway time-sync broadcast in the current protocol. `CMD_SYNC_TIME`, `TIME_SYNC_AGE_MS`, `TIME_SYNCED`, and `TIME_SYNC_STALE` are retired. Anchors report local uptime/event timestamps, and every mesh packet carries `Message age ms` so receivers can reason about how long ago the sender created the packet.

For `CMD_GET_STATUS`, the response includes `DEVICE_ROLE`, `UPTIME_MS`, `TIMESTAMP_MS`, `STATUS_BITS`, `GATEWAY_ID`, and either route fields (`NEXT_HOP_ID`, `ROUTE_EPOCH`, `HOP_COUNT`, `QUALITY`, `RETRY_COUNT`) or `REASON=7` (`NOT_FOUND`) when no upstream route is selected. `MSG_ANCHOR_HEARTBEAT` uses the same role, uptime, `STATUS_BITS`, and route telemetry shape.

Anchor UWB status bit layout:

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `UWB_SCAN_ACTIVE` | Low-duty wake scanning has run at least once since boot. |
| 1 | `UWB_WAKE_DECODE_FAILURE` | SFD timeout, frame timeout, CRC failure, or false-wake cooldown has occurred. |
| 2 | `UWB_CLAIM_COLLISION` | A competing wake claim collided or lost arbitration. |
| 3 | `UWB_DS_TWR_FAILURE` | A scheduled DS-TWR exchange failed. |
| 4 | `UWB_TIMING_REJECTION` | A DS-TWR exchange was rejected for timing or schedule identity. |
| 5 | `UWB_MESH_RX` | At least one UWB mesh frame has been received. |
| 6 | Reserved | Retired time-sync status bit. |
| 7 | Reserved | Retired time-sync stale status bit. |

### Report Fragmentation

An unfragmented aggregated click report can carry distance samples plus diagnostic TLVs inside one UWB mesh frame. If the anchor measured more samples than fit, it sends multiple `CLICK_REPORT` packets for the same `(anchor_id, clicker_id, event_seq, burst_id)`. Each packet's `TIMESTAMP_MS` is the sender-local start timestamp for the first distance sample in that chunk, `SEQUENCE_START_TIMESTAMPS_MS` gives the direct per-sample local start timestamps from that anchor, and the packet header `Message age ms` gives the elapsed time since that packet was created. `RANGE_ROUND_INDICES` gives the clicker-transmitted round-robin round for each sample, so the gateway can group reports by `(clicker_id, event_seq, burst_id, round_index)` and average the respective anchor start times for each round even when several round robins occur for one click. Anchors do not exchange start times with each other. The first normal-click packet carries one rounded-average `DISTANCE_MM`, one `QUALITY`, one `RANGE_STATUS`, one total `SAMPLE_COUNT`, `BURST_ID`, burst and stride TLVs, diagnostic status/count TLVs, the PHY configuration ID, the canonical six-byte `UWB_CIR_SAMPLE`, clicker diagnostics, and the first aligned distance/timing chunk. New firmware also includes one-byte `ATTEMPT_INDEX` and `DETECTION_SOURCE`; source 1 means the anchor accepted a CRC-valid UWB wake claim for that attempt. Both fields are additive and may be absent in legacy records. The same six CIR bytes are not repeated as `ANCHOR_DIAG_BYTES`. Subsequent distance packets carry the same aggregate identity, a `SAMPLE_INDEX` pointing to the first sample in that chunk, and the next aligned distance, round-index, and timestamp samples; they omit first-packet diagnostics, RSL, and CIR.

For a normal mesh click, the anchor also captures the same bounded partial CIR used by the ML firmware: 192 accumulator samples centered around the detected first path, with 64 samples before it where the accumulator boundary permits. Each sample is six raw bytes, so a complete window is 1,152 bytes. The anchor emits two extended `CLICK_REPORT` diagnostic packets carrying 881 and 271 raw CIR bytes. The first report payload reserves the mandatory nine bytes needed for `MESH_CH9_BATCH_ID` and `MESH_CH9_BATCH_FLAGS`, making the routed payload exactly 958 bytes. Every packet repeats `CLICKER_ID`, `ANCHOR_ID`, `EVENT_SEQ`, and `TIMESTAMP_MS`, and carries `DIAG_FRAGMENT_INDEX`, `DIAG_FRAGMENT_COUNT`, `UWB_CIR_BYTE_OFFSET`, `UWB_CIR_TOTAL_BYTES`, `UWB_CIR_FIRST_PATH_INDEX`, and `UWB_CIR_START_INDEX`. Because a TLV value length is one byte, one packet may contain several consecutive `UWB_CIR_FULL_CHUNK` values; the host concatenates those values in wire order before applying the packet byte offset. Reassembly is valid only when identity and metadata agree, offsets are in bounds, chunks do not conflict, and the two packets cover the declared total length. Missing or invalid packets leave the CIR explicitly incomplete; they do not invalidate the core click result. The anchor streams the two packets into its normal local-origin report path without requiring both to occupy the report queue at once.

Server event handling may finalize a normal click from at least three valid anchor distances only when those distances share the same clicker, click event, and burst identity; it must not combine partial retry bursts. CIR completion is diagnostic and is not a prerequisite for click finalization.

### Duplicate Handling

If a relay receives a directed-unicast retry whose identity is already in its 60 s duplicate cache:
- **Idle with a route**: re-forward the packet. The duplicate cache prevents double delivery.
- **Busy or no route**: drop the packet. The sender retries later if its end-to-end confirmation times out.

Local or broadcast duplicates are not delivered twice.

## Self-Test and Diagnostic Flags

Self-test traffic sets `DIAGNOSTIC` and clears `COUNT_AS_CLICK`. Normal click traffic sets `COUNT_AS_CLICK` and clears `DIAGNOSTIC`. The firmware rejects packets where both flags are set. This prevents self-test dud events from appearing as real clicks in the server event stream. For the full self-test sequence, LED patterns, and click failure codes, see [[UWB+BLE Architecture 0.6.3]].

## Anchor Self-Distance Survey

The ML clicker firmware exposes a host-driven anchor-pair survey command, `CMD_ML_START_ANCHOR_PAIR_SURVEY = 0x8002`, for collecting pair distances through the clicker BLE transport. Its `MSG_COMMAND` payload must include the common `COMMAND_ID` TLV. It may include one `DISCOVERY_SLOT_COUNT` TLV with a one-byte value from `2..8`; if omitted, the clicker uses the compiled ML discovery-slot default. Request `SAMPLE_COUNT` is ignored for this command. The clicker performs UWB discovery, sends one `ANCHOR_PAIR_SCHEDULE` covering every unique selected-anchor pair, receives `ANCHOR_PAIR_RESULT` frames, and forwards each received pair result to the host as `MSG_CLICK_REPORT | DIAGNOSTIC`.

Each ML anchor-pair survey row is identified by `INITIATOR_ID` and `RESPONDER_ID`, not by a dedicated survey-result message. The report payload carries `SURVEY_ID`, `EVENT_SEQ`, `TIMESTAMP_MS`, `INITIATOR_ID`, `RESPONDER_ID`, `SAMPLE_INDEX`, `SAMPLE_COUNT`, `DISTANCE_MM`, `QUALITY`, `RANGE_STATUS`, and `UWB_RSL_DBM`. `SAMPLE_INDEX` is the zero-based pair index and `SAMPLE_COUNT` is the scheduled pair count for this command. The final `MSG_COMMAND_RESULT` may report `COMMAND_TIMEOUT` when not every scheduled pair produced a result before the receive deadline; any diagnostic click-report rows already emitted for that command remain valid input to the off-device geometry solver.

The gateway starts survey setup with `SURVEY_DISCOVERY_START`, a broadcast mesh packet carrying `SURVEY_ID`, `DISCOVERY_START_DELAY_MS`, `DISCOVERY_SLOT_MS`, and `DISCOVERY_SLOT_COUNT`. This packet uses the extended-PHR channel-5 mesh-control PHY, matching the control-follow-up receiver opened by the gateway-command wake train. The gateway uses six slots by default unless the host command includes `DISCOVERY_SLOT_COUNT`. Each anchor subtracts the packet header age from the start delay and joins the matching hash-derived slot when it receives the packet late but before the epoch has expired.

During the discovery epoch, every anchor gets four real `UWB_SURVEY_DISCOVERY_PROBE` transmit attempts. Each attempt has a deterministic collision-diversified slot. If an anchor cannot start a complete transmission inside its nominal slot, that same attempt moves to its deterministic slot in a second bounded reserve horizon; the blocked nominal slot does not consume the attempt, and the anchor never transmits late outside either slot. All anchors process later nominal windows before the reserve horizon and listen through reserve slots, so one blocked early attempt does not hide later peer probes. The probe uses the standard-wake channel-5 PHY: 850 kbps, a 4096-symbol preamble, PAC32, standard PHR, and no STS. A slot must be at least 30 ms so the 20 ms transmit timeout, 2 ms transition guard, and 8 ms receive guard fit even on the failure path. The operational default remains six 40 ms slots.

After both discovery horizons, each anchor reports the peer probes it heard as `SURVEY_DISCOVERY_REPORT`. Reports are not flooded at once: each anchor waits for `discovery_duration + anchor_slot * survey_report_mesh_slot_ms` before requesting gateway-bound queue custody. The anchor retains one exact encoded report while the local queue is full or temporarily busy and retries admission after radio release with bounded backoff through the collection allowance. It clears that pending state only after queue custody or an explicit terminal deadline/error. The current report mesh slot is longer than the gateway ACK timeout, so a typical anchor report has time to be sent, ACKed, or move into route recovery before the next anchor starts. The gateway builds the reachability graph from these measured reports and then deterministically schedules at most six peers per anchor, preferring mutual visibility, link quality, received level, and stable ID order. This caps a 50-anchor deployment at 150 pairs instead of the 1,225-pair complete graph.

Direct RF reachability does not allocate a negotiated channel-9 connection at the gateway; the gateway remains a continuous channel-9 receiver. When many direct anchors become report-ready together, survey route probes get up to four actual transmissions with 2, 4, and 8 second widening retry bases plus full-width anchor-, survey-, and attempt-specific mixing. External RNG correlation therefore cannot make every anchor repeat the same delay, including anchor IDs whose upper and lower 32-bit halves have the same XOR. A locally busy radio may defer without consuming a transmission. Eight consecutive busy deferrals produce a distinct busy terminal, so that path does not falsely claim four sends occurred. The gateway collection deadline includes the separately calculated 60.22 second conservative delivery tail after the 5 second local queue-admission allowance. That tail covers the bounded probe policy, scratch acquisition and radio-transition allowances, the complete production gateway-ACK retry budget, RX re-arm, and ACK guard; it does not reinterpret queue admission as gateway delivery. The retry-policy model proves collision and deadline behavior only. The unscheduled direct-gateway simulator must separately prove report queueing, payload transmission, gateway batch ACK, retry, and final custody.

Pair measurements report `SURVEY_ID`, `INITIATOR_ID`, `RESPONDER_ID`, `SAMPLE_INDEX`, `SAMPLE_COUNT`, `TIMESTAMP_MS`, `DISTANCE_MM`, `QUALITY`, and `RANGE_STATUS`. The timestamp marks when that individual DS-TWR ranging sequence started on the sender's local uptime. The firmware only measures and reports; solving anchor positions from this distance network is off-site software.

Survey pair runs are long-running local work, not system command work. Anchors must continue to process mesh receive and commands while samples are being taken. `CMD_SURVEY_START_PAIR` is accepted only when all pair fields match a prior `CMD_SURVEY_PREPARE_PAIR`. If a gateway reachability request omits `SAMPLE_COUNT`, the gateway uses the current bounded runtime queue capacity. `CMD_SURVEY_ABORT` must be accepted, must stop the gateway's local pair orchestrator, and must stop the active anchor pair at the next sample boundary or bounded responder-listen check. The runtime ownership and delivery flow is shown in [[Mesh Connected Routing Walkthrough]] and constrained by [[Mesh Connected Routing Contract]].

Each survey DS-TWR sample uses `FLAG_DIAGNOSTIC`, the survey ID as the DS-TWR session ID, and a session nonce derived from `(survey_id, initiator_id, responder_id, sample_index)`. This keeps the full DS-TWR identity unique across the survey even though the compact 8-bit DS-TWR sequence value wraps in surveys longer than 255 samples.

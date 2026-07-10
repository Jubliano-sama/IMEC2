# Gateway BLE Streaming

This document describes the gateway-side selective BLE stream implemented in
`firmware/app/src/app_gateway_ble_stream.c`.

## Scope

The stream is a host/server visibility path only. It is not part of UWB mesh
delivery correctness. Queue drops or BLE notification failures do not change
gateway ACK/EACK, custody, route, click service, command/result, channel-5, or
channel-9 state.

The existing host packet encoder remains available for direct gateway-originated
host packets. Locally delivered mesh packets at the gateway now use the
selective stream queue instead of emitting every mesh/control packet.

## Allowlist

`gateway_ble_should_stream_packet(type, flags, class)` allows only
server-relevant packet classes:

- Click: `MSG_CLICK_REPORT` and packets with `FLAG_COUNT_AS_CLICK`.
- Command/result: `MSG_COMMAND_RESULT`, `MSG_RESULT_BUNDLE`.
- Survey result visibility: `MSG_SURVEY_REACH_REPORT`,
  `MSG_SURVEY_PAIR_RESULT`, `MSG_SURVEY_DISCOVERY_REPORT`.
- Diagnostics: `MSG_UWB_CLICKER_DIAG`, `MSG_UWB_ANCHOR_DIAG`,
  `MSG_UWB_ANCHOR_DIAG_FRAGMENT`, `MSG_SELF_TEST_REPORT`, and diagnostic-flagged
  packets.
- Status: `MSG_ANCHOR_HEARTBEAT`.

Mesh control packets such as route requests, gateway ACKs, EACKs, route adverts,
event negotiation, hop ACKs, and busy responses are not streamed by default.

## Queue Policy

The stream uses fixed descriptor slots plus a compact record pool in RAM:

- Queue depth: `GATEWAY_BLE_STREAM_QUEUE_DEPTH` records.
- Maximum record size: `GATEWAY_BLE_STREAM_RECORD_MAX_LEN` bytes, sized for the
  40-byte stream header plus the full 958-byte extended shared-packet payload.
- Payload bytes per record: `GATEWAY_BLE_STREAM_PAYLOAD_MAX_LEN`.
- Record-pool size: `GATEWAY_BLE_STREAM_RECORD_POOL_BYTES`. A build-time guard
  requires enough space for the core click record plus its 890-byte and
  262-byte CIR records at the same time.
- RAM budget guard: `GATEWAY_BLE_STREAM_RAM_BUDGET_BYTES`.

There is no dynamic allocation. Enqueue is non-blocking and records are drained
from Zephyr workqueue context when BLE packet notifications are ready.

Priority order under pressure:

1. Click records.
2. Command/result and survey records.
3. Diagnostic records.
4. Status records.

A higher-priority incoming record may evict one lower-priority queued record.
If no lower-priority record is available, the incoming record is dropped and the
appropriate diagnostic counter is updated. Every protocol-valid click,
result, and survey payload fits without truncation. Inputs larger than the
shared-packet payload limit are rejected; oversize diagnostic and status records
are truncated to the record payload budget and marked with the truncated flag.
Normal mesh partial-CIR data arrives as two independently valid extended
click-report packets. A packet may contain repeated byte-length CIR TLVs. The
gateway forwards each packet intact; host software concatenates repeated CIR
values in wire order, then validates and reassembles the declared offsets
rather than relying on BLE-record truncation.

## Record Format

All multi-byte fields are little-endian.

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 2 | Magic, `GATEWAY_BLE_STREAM_MAGIC` |
| 2 | 1 | Version, `GATEWAY_BLE_STREAM_VERSION` |
| 3 | 1 | Header length, `GATEWAY_BLE_STREAM_RECORD_HEADER_LEN` |
| 4 | 1 | Record type, `GATEWAY_BLE_STREAM_RECORD_PACKET` |
| 5 | 1 | Stream class |
| 6 | 1 | Priority |
| 7 | 1 | Record flags, bit 0 means payload truncated |
| 8 | 1 | Protocol `msg_type` |
| 9 | 1 | Protocol packet flags |
| 10 | 2 | Protocol sequence |
| 12 | 4 | Protocol session ID |
| 16 | 8 | Source node ID |
| 24 | 8 | Destination node ID |
| 32 | 4 | Packet age at gateway enqueue, ms |
| 36 | 2 | Included payload length |
| 38 | 2 | CRC-16/CCITT-FALSE over included payload bytes |
| 40 | N | Payload bytes or compact/truncated payload bytes |

The payload length field lets the host reassemble records even when BLE
notifications split a record into smaller ATT chunks.

## Diagnostics

`gateway_ble_stream_get_status()` exposes:

- enqueue attempts,
- packets sent,
- bytes sent,
- drops because the queue was full,
- drops because an item was too large,
- drops because BLE was not ready and no queue space was available,
- lower-priority queued records dropped for priority,
- max queue depth observed,
- oldest queued age,
- last dropped packet type,
- last drop reason.

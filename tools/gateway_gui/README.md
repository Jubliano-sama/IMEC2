# IMEC2 Gateway BLE Console

Isolated desktop test GUI for the connected IMEC gateway BLE edge. It scans,
connects, reads the gateway identity, subscribes to the binary packet and
debug-log characteristics, sends the three proven gateway workflows, and inspects live packets without substituting
synthetic results when BLE or protocol operations fail.

## Setup

From the repository root, use the existing Python environment:

```sh
UV_CACHE_DIR=$PWD/.uv-cache uv pip install --python .venv/bin/python -r tools/gateway_gui/requirements.txt
```

Tkinter is supplied by the system Python, not PyPI. On Ubuntu/Pop!_OS, install
`python3-tk` if `import tkinter` fails. BLE access requires BlueZ, a powered
adapter, and permission for the desktop user to use the system Bluetooth stack.

## Run

```sh
.venv/bin/python -m tools.gateway_gui
```

1. Scan and select an IMEC device. The normal names are `IMEC Gateway` and,
   for the current production-successor preset, `IMEC Mesh Test Gateway`.
2. Connect. The GUI verifies the service, reads the explicit gateway identity,
   and subscribes to packet and log notifications before reporting a connected
   state.
3. Send `Anchor Survey Discovery`, `Here I Am`, or `Assign discovery slots`,
   then inspect the received `COMMAND_RESULT`, reports, and gateway log. A
   completed BLE write is shown as transport completion only, not command
   success.

All command controls remain disabled until the read-only identity characteristic
returns the connected gateway firmware `DEVICE_ID`. The GUI clears that identity
on disconnect and rejects contradictions from gateway-local packets; it never
derives `DEVICE_ID` from the BLE address.

## Test

```sh
.venv/bin/python -m unittest discover -s tools/gateway_gui/tests -v
.venv/bin/python -m compileall -q tools/gateway_gui
.venv/bin/python -m mypy --explicit-package-bases tools/gateway_gui
```

The tests cover shared-envelope CRC and COBS framing, current gateway stream
records split across ATT notifications, legacy COBS notifications, TLV parsing,
unknown/repeated TLV retention, click sample alignment, CIR decoding, and exact
command construction. They also cover out-of-order CIR fragment assembly,
missing fragments, overlaps, gaps, bounds, metadata mismatches, signed component
decoding, and magnitude math.

## BLE And Protocol Assumptions

The UUIDs are copied from `firmware/app/src/app_gateway_ble.c`:

| Purpose | UUID |
| --- | --- |
| Service | `494d4543-0001-4757-8000-000000000001` |
| Packet notify | `494d4543-0001-4757-8000-000000000002` |
| Packet write | `494d4543-0001-4757-8000-000000000003` |
| Debug-log notify | `494d4543-0001-4757-8000-000000000004` |
| Gateway identity read | `494d4543-0001-4757-8000-000000000005` |

Host commands are shared IMEC packets with CRC-16/CCITT-FALSE, COBS encoding,
and a trailing zero delimiter. The GUI chunks a complete frame into ordered
write-without-response ATT writes because firmware reassembles the byte stream.

Current gateway notifications use the v1 `GW` stream record from
`app_gateway_ble_stream.c`: a 40-byte record header followed by TLV payload.
The GUI also accepts the older/documented COBS notification stream. A stream
record does not carry the original shared-packet TTL, complete envelope, or
packet CRC. Its age field is gateway queue age. The raw record and TLV payload
are always shown; original shared-packet bytes are shown only when the transport
actually supplied them.

Only packet classes selected by gateway firmware are notified. The gateway
stream queue can drop lower-priority status or diagnostic records under
pressure, and non-mesh gateway builds may pause BLE activity around UWB work.
This GUI is therefore a host-delivery view, not a complete RF trace.

## Supported Commands

- **Anchor Survey Discovery** sends a gateway-local `MSG_COMMAND` with
  `CMD_SURVEY_REACHABILITY = 0x0100` to the identity read from GATT. Its payload
  contains required `SURVEY_ID` and `DURATION_MS`, plus `SAMPLE_COUNT` and
  `DISCOVERY_SLOT_COUNT`. Gateway firmware converts it to
  `SURVEY_DISCOVERY_START`, gathers reachability, and may continue into pair
  ranging. The current gateway-role build resolves its pair-sample runtime cap
  to `1`, so the GUI defaults to one sample; larger values can be rejected with
  `COMMAND_DENIED`.
- **Here I Am** sends local `CMD_FORCE_REDISCOVERY = 0x000c` to the gateway's
  own `DEVICE_ID`. The current special case returns `COMMAND_OK` and schedules
  the priority `MSG_GATEWAY_ROUTE_ADV` route-refresh flood.
- **Assign discovery slots** sends local `CMD_ASSIGN_DISCOVERY_SLOTS = 0x0104`
  to the gateway's own `DEVICE_ID` with no host-supplied assignment TLVs. The
  gateway collects anchor claims and floods the resulting table. A successful
  `COMMAND_RESULT` reports the assigned-anchor count in `REASON`.

There is no arbitrary command composer. Although the envelope is extensible,
firmware applies command-specific destinations, scopes, TLV validation, and
single-command tracking. Sending arbitrary IDs/TLVs from a generic form would
claim a safety contract that the host protocol does not provide.

## Click And CIR Inspection

For each click report the GUI shows the envelope fields, every TLV (including
unknown and repeated TLVs), aligned distance/round/timestamp arrays, aggregate
range fields, optional latency and diagnostic counters, raw diagnostic blocks,
payload bytes, and raw transport bytes.

Discovery-assignment diagnostics decode `A4` phase, `A5` epoch, `A6` hash, and
every repeated `A7` table value. Each table entry is exactly 17 bytes containing
an eight-byte little-endian anchor ID, eight-byte little-endian hash, and
one-byte slot. Click reports show the signed `A8` clicker clock offset separately
from the existing signed `0x4D` anchor clock offset.

`UWB_CIR_SAMPLE` is exactly one six-byte DW3000 accumulator sample: three bytes
for the real component and three for the imaginary component. The GUI shows the
raw bytes, signed 24-bit components, and magnitude. One complex point is not a
CIR waveform, so the GUI deliberately does not draw a trace from it.

Normal mesh click CIR diagnostics arrive as `MSG_CLICK_REPORT` packets carrying
repeated `UWB_CIR_FULL_CHUNK` values. The transport uses two extended packets:
the first carries 881 CIR bytes and the second carries the remaining 271 bytes.
The first routed payload also carries the mandatory 9 encoded bytes for the
mesh channel-9 batch ID and flags, keeping it at the 958-byte maximum. Each
individual chunk TLV remains limited to 255 bytes. The GUI concatenates repeated
chunk values in packet wire order, groups packets by clicker ID, anchor ID, and
event sequence, then validates fragment metadata, byte bounds, ordering,
overlaps, duplicate indices, gaps, and exact coverage. It never fills missing
bytes. Gateway stream records accept the 40-byte stream header plus the 958-byte
extended-packet payload maximum.

A complete 1,152-byte window is decoded as 192 samples in device order:
little-endian signed 24-bit real followed by little-endian signed 24-bit
imaginary. The CIR inspector plots `hypot(real, imaginary)` by absolute
accumulator index, marks the declared start and first-path indices, and retains
every six-byte sample in the table. Incomplete and malformed streams show their
exact state and errors without synthesizing a waveform.

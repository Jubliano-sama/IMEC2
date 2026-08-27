# IMEC2 Gateway BLE Console

Isolated desktop test GUI for the connected IMEC gateway BLE edge. It scans,
connects, reads the gateway identity, subscribes to binary packet reports,
sends the three proven gateway workflows, and inspects live packets without
substituting synthetic results when BLE or protocol operations fail.

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
   and subscribes to packet notifications before reporting a connected state.
3. Send `Here I Am` or `Assign discovery slots`,
   then inspect the received `COMMAND_RESULT`, reports, and activity log. A
   completed BLE write is shown as transport completion only, not command
   success.
4. Use `Run Survey` for the complete current workflow. The GUI runs a fresh
   RAM-only enumeration, binds its exact slot map to the new survey generation,
   submits the mutual-pair plan, shows every command and pair transition live,
   and solves the usable distances without blocking the Tk event loop.

For every ordinary command, the GUI freezes the command and its runtime policy,
sends a separately correlated Here-I-Am, waits for its typed successful
terminal, and only then sends the frozen target. Failure, timeout, or disconnect
drops the unsent target. Manual Here-I-Am and immediate recovery/liveness
commands are exempt so preflight cannot recurse or delay recovery.

All command controls remain disabled until the read-only identity characteristic
returns the connected gateway firmware `DEVICE_ID`. The GUI clears that identity
on disconnect and rejects contradictions from gateway-local packets; it never
derives `DEVICE_ID` from the BLE address.

## Test

```sh
.venv/bin/python -m unittest discover -s tools/gateway_gui/tests -v
.venv/bin/python -m compileall -q tools/gateway_gui
.venv/bin/python -m mypy --explicit-package-bases --exclude 'tools/gateway_gui/tests/' tools/gateway_gui
```

The tests cover shared-envelope CRC and COBS framing, current gateway stream
records split across ATT notifications, legacy COBS notifications, TLV parsing,
unknown/repeated TLV retention, click sample alignment, CIR decoding, and exact
command construction. They also cover out-of-order CIR fragment assembly,
missing fragments, overlaps, gaps, bounds, metadata mismatches, signed component
decoding, and magnitude math. Survey tests cover exact command-result
correlation, early reliable-event buffering, stale-generation rejection,
accepted-plan pair binding, immutable range results, and geometry readiness.

## BLE And Protocol Assumptions

The UUIDs are copied from `firmware/app/src/app_gateway_ble.c`:

| Purpose | UUID |
| --- | --- |
| Service | `494d4543-0001-4757-8000-000000000001` |
| Packet notify | `494d4543-0001-4757-8000-000000000002` |
| Packet write | `494d4543-0001-4757-8000-000000000003` |
| Gateway identity read | `494d4543-0001-4757-8000-000000000005` |

The optional strict live transport check is useful for qualification after
flashing a gateway. It is separate from normal GUI operation and fails unless
the identity read and packet notification subscription work:

```sh
.venv/bin/python -m tools.gateway_gui.ble_smoke [BLE_ADDRESS]
```

One strict hardware survey can be rerun through the production GUI itself. The
command exits zero only after accepted START and PLAN results, exactly three
anchors and pairs, five usable samples per pair, no partial reason, and the
receipt-backed GUI terminal. `script` preserves the terminal proof while still
giving Tk and the BLE client a TTY:

```sh
mkdir -p logs/manual-gateway-gui-survey
script -q -e -c ".venv/bin/python -m tools.gateway_gui.survey_hil E0:85:31:10:C4:17" logs/manual-gateway-gui-survey/run.typescript
```

The click HIL runner likewise owns the BLE receipt consumer before injecting
the RTT click. Its checked-in probe map is the four-probe bench described by
the script's `--help`; select `direct` for DDD and `forced` for a topology with
forced-hop anchors:

```sh
.venv/bin/python firmware/scripts/test_clicker_hil.py --config direct --output-name manual-ddd
```

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

Gateway host records can replay after gateway retries or resets because source
custody is not released at BLE notification completion. That completion moves
the stream item to `WAIT_GUI_RECEIPT`; the GUI first stores and deduplicates the
exact record in process RAM, then sends an exact identity-plus-record-digest
receipt. Only the gateway's accepted `HOST_ITEM_ACCEPTED` receipt permits a
mesh ACK and later stream-item retirement. A disconnect before the receipt
rewinds the item for resend, while a reset leaves source custody upstream.
The deduplicator is bounded and scoped to the firmware `DEVICE_ID` read from
GATT, so different gateways cannot share entries. Its history survives a
disconnect/reconnect to the same gateway while this process remains alive; an
exact replay stays one visible/CIR-merged record, while a same-identity
mutation is shown as a conflict. LRU eviction or a GUI-process restart removes
old history, so a later replay may be shown again; there is no durable host
acknowledgement or NVS-backed journal.

## Supported Commands

- **Here I Am** sends local `CMD_FORCE_REDISCOVERY = 0x000c` to the gateway's
  own `DEVICE_ID`. Its correlated successful terminal follows completion of the
  gateway's bounded `MSG_GATEWAY_ROUTE_ADV` flood custody; it does not claim
  per-anchor reception. It also carries the current complete runtime policy.
- **Assign discovery slots** sends local `CMD_ASSIGN_DISCOVERY_SLOTS = 0x0104`
  to the gateway's own `DEVICE_ID`. Its optional **Expected anchors** value is
  encoded as `TLV_EXPECTED_NODE_COUNT`; when the roster is known, this lets a
  delivered CLAIM flood advance as soon as every expected unique claim arrives.
  Leave it blank when the roster is unknown so the gateway waits the complete
  conservative multi-hop horizon. The GUI also exposes the total assignment
  budget and equal randomized response spread. The gateway floods the resulting
  table and commits the ACKed subset; a nonempty useful subset may finish with
  `COMMAND_OK`, while terminal counters preserve missing claims or ACKs for
  optional strict qualification. The assigned-anchor count is returned in
  `REASON`.
- **Run Survey** chains the current `CMD_SURVEY_START = 0x0105` and
  `CMD_SURVEY_PLAN = 0x0106` controls behind a fresh RAM-only enumeration. Each
  control owns one exact host session/sequence until its reliable result or
  timeout. The gateway internally retries retryable `COMMAND_BUSY` pressure;
  the GUI does not mistake that backoff for a finished survey. An explicit
  `CMD_SURVEY_CANCEL = 0x0107` remains available after the generation is known.

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

## Geometry And Mesh Diagnostics

`Survey & Geometry` is connected to the current generation-bound survey event
stream. Enumeration supplies the exact discovery-slot-to-anchor mapping; the
accepted gateway plan supplies stable pair indices; only immutable results with
at least three successful samples become distance constraints. Pending and
insufficient pairs remain visible, but they never become invented coordinates.
The solver runs on one background worker and stale completions are discarded by
GUI-run serial plus geometry revision, so packet and command progress stays live.

The default solver is the exact `visibility_branching_tuned` profile adapted
from the user-owned AnchorGeometrySolver commit
`01c3edb470bcd868403e04a6cded754360decdf0`. This is a RAM-only relative 2D
diagnostic, because the survey exports pair distances without anchor heights or
workplace-frame registration. It does not claim the unresolved production 3D
self-setup result.

`Click Location` groups ranges by protocol session, event sequence, and clicker
ID and solves against the current geometry generation. Duplicate, stale,
invalid, unknown-anchor, and collinear inputs remain unsolved. The wake monitor
uses the firmware-derived 1000 ms collision window. Current click reports do
not export anchor detection attempt, so they remain `unknown` until a structured
field exists rather than being silently treated as normal.

`Mesh Commands` decodes typed `MSG_GATEWAY_COMMAND_EVENT` schema-1 records into
a correlated timeline and topology view. A complete terminal enumeration can
be explicitly accepted as the baseline under
`~/.config/imec2-gateway-gui/anchor-baseline.json`; incomplete or lossy runs
remain unknown and never update the baseline automatically.

The survey tab complements that generic timeline with the complete live chain:
route refresh, enumeration, neighbor collection, plan acceptance, pair ranging,
and geometry solve. Each row updates from retained protocol evidence rather
than elapsed-time guesses.

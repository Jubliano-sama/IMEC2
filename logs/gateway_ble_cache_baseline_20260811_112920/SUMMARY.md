# Pre-upgrade same-identity BLE baseline

Capture started at `2026-08-11T11:29:20+02:00`. No firmware was flashed, no
BlueZ device or cache entry was removed, Bluetooth was not restarted, and no
RTT client was attached.

## Read-only BlueZ commands

```sh
bluetoothctl show
bluetoothctl devices
bluetoothctl info EF:BD:42:B8:83:0C
busctl tree org.bluez
busctl introspect org.bluez /org/bluez/hci0/dev_EF_BD_42_B8_83_0C/service0010
busctl introspect org.bluez /org/bluez/hci0/dev_EF_BD_42_B8_83_0C/service0010/char0011
busctl introspect org.bluez /org/bluez/hci0/dev_EF_BD_42_B8_83_0C/service0010/char0014
busctl introspect org.bluez /org/bluez/hci0/dev_EF_BD_42_B8_83_0C/service0010/char0016
```

Before the live transaction, BlueZ retained the unpaired, unbonded, and
disconnected random-address device `EF:BD:42:B8:83:0C`, named
`IMEC Mesh Test Gateway`. Its cached layout was:

- IMEC service `494d4543-0001-4757-8000-000000000001`, handle `0x0010`.
- packet notify `...0002`, handle `0x0011`, CCC descriptor `0x0013`.
- packet write `...0003`, handle `0x0014`, properties
  `write-without-response, write`.
- identity read `...0005`, handle `0x0016`, cached value
  `66 66 77 77 88 88 99 99`, which decodes to
  `0x9999888877776666`.

The post-run BlueZ capture shows the same address and exact object paths and
handles still cached, `Connected: no`, and `Notifying: false` after the clean
disconnect.

## Normal GUI Here-I-Am transaction

The full executable command and the complete no-source-edit runtime trace are
embedded in `normal_gui_here_i_am.typescript`. It instantiated the normal
`GatewayGui`, selected the existing cached address, invoked the GUI's existing
Connect and Here-I-Am actions, and only wrapped the existing Bleak read,
subscribe, and write calls to print their arguments and successful returns.

Live evidence:

- The identity read returned `66 66 77 77 88 88 99 99`, exactly
  `0x9999888877776666`.
- Live GATT discovery returned the same service and characteristic UUIDs and
  handles as the retained BlueZ cache.
- `start_notify(...0002)` returned successfully, proving CCC subscription.
- The 81-byte Here-I-Am frame was sent to `...0003` in five successful ATT
  chunks of `20 + 20 + 20 + 20 + 1` bytes; every call used
  `response=False`, so this was write-without-response.
- Three `GATEWAY_COMMAND_EVENT` stream-v1 notifications were decoded from the
  live gateway, proving the notification path.

The full protocol baseline did not pass. The immediate self-addressed
`COMMAND_RESULT` notification was rejected by the current GUI as
`gateway local command result envelope is invalid`, so no exact host receipt
was sent. The transaction was disconnected at the 65-second bound.

The failure is source/image skew rather than a BlueZ caching failure. The
current dirty source adds `FLAG_GATEWAY_ACK_REQUIRED` to local gateway command
results and makes the GUI require it; the older flashed image emits a successful
local result with flags zero. Consequently, this capture establishes the
identity/discovery/CCC/write-without-response/notification half of step 1, but
it does not establish the host-receipt/full-protocol half. A caching-enabled
image aligned with the current receipt semantics is required before claiming a
complete pre-upgrade baseline.

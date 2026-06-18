# DW3xxx API Audit 2026-06-18

This audit compares the firmware DWM3000 call surface against the 2025 Qorvo
DW3xxx API Guide, with emphasis on sleep/wake restore and DS-TWR timing.

## Applied Changes

- Replaced the firmware wake path's direct use of deprecated
  `dwt_restoreconfig()` with the split restore sequence documented for current
  development:
  `dwt_restore_common()` followed by `dwt_restore_txrx(DWT_RESTORE_TXRX_MODE)`.
- Added a compatibility implementation of the split restore API to the checked
  DWM3000 SDK submodule, because the bundled driver only exposed the old
  zero-argument `dwt_restoreconfig()` API.
- Kept `dwt_restoreconfig()` as a deprecated compatibility wrapper in the
  submodule so older examples still build.
- Changed DS-TWR delayed response/final transmissions from absolute delayed TX
  (`DWT_START_TX_DELAYED`) to RX-timestamp-relative delayed TX
  (`DWT_START_TX_DLY_RS`). This matches the operation we need: transmit a fixed
  turnaround after the last received frame.

## Sleep/Wake Result

The verified anchor Stage 1 RTT capture after the split restore change showed:

- `sleep_wake_count=38`
- `sleep_wake_avg_us=7109`
- `sleep_wake_fail=0`

The previous full PHY reapply path was about `8142 us`, so the split restore
path saved about `1033 us` per wake in that capture while still catching wake
claims over repeated sleep/wake cycles.

The current driver intentionally uses DWM3000 DEEPSLEEP for low-duty anchor
scans:

- `DWM3000_SLEEP_WAKE_FLAGS` omits `DWT_SLEEP`; the local API comments define
  that as DEEPSLEEP.
- `dwt_entersleep(DWT_DW_IDLE_RC)` still wakes through the IDLE_RC-ready wait
  path before the split restore sequence runs.

## DS-TWR Timing Audit

Already good:

- Poll and response/final transmit paths already use `DWT_RESPONSE_EXPECTED`,
  so RX turn-on after TX is handled by the IC instead of by host-side polling.
- `dwt_setrxaftertxdelay()` is configured before `dwt_starttx()` in the
  response-expected paths.
- Frame wait timeout and preamble timeout are set before RX enable.

Changed:

- The responder response and initiator final now use `DWT_START_TX_DLY_RS`,
  with the delay encoded as a DX_TIME offset from the last RX timestamp.
  The frame timestamp fields are derived from the same programmed offset, so
  protocol validation and range math remain tied to the actual scheduled TX.
- The firmware now names separate provisional short-range and long-range reply
  delay presets. The long-range main PHY currently selects the conservative
  long-range value; both presets still need recalibration with final logging,
  SPI timing, and stage-one traffic enabled.

Deferred candidates:

- `DWT_START_RX_DLY_RS` / delayed RX could reduce receive-on time for tightly
  scheduled windows, but the current wake/discovery/listen paths intentionally
  need open receive windows. Apply only to a path with known incoming timing.
- `dwt_setsniffmode()` can lower preamble-hunt current, but it reduces RX
  sensitivity and persists until disabled or reset. It should be tested as a
  separate low-power scan experiment, not silently enabled in the bring-up path.
- `dwt_readclockoffset()` or `dwt_readcarrierintegrator()` could add useful
  diagnostics for clock drift. DS-TWR does not need this as an immediate fix,
  but logging it during range tests may help explain marginal turnaround or
  long-range behavior.

## No Change Needed

- `dwt_spicswakeup()` is not used because the board has a dedicated WAKEUP pin.
  The port currently drives WAKEUP high for 500 us and then waits for IDLE_RC.
- `dwt_rxenable(DWT_START_RX_IMMEDIATE)` remains correct for open anchor scans,
  discovery listens, and responder poll windows.
- `dwt_setpreambledetecttimeout(0)` remains deliberate for continuous long
  preamble windows where a too-short preamble timeout can abort a frame already
  in progress.

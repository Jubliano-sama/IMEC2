# DW3000 configuration loss after charger use

The reported anchor was captured without resetting it. All four flash images
were re-read against the previously qualified hashes; all matched. Anchor A
(`c1c2306a5138ab2d`, probe `E46070D247233537`) continued its idle scans but failed
to decode the same activation that anchors B and C received. The first real GUI
survey found only B and C and failed the strict three-anchor assertion.

The initial RAM snapshots held identical intended wake configurations on all
three anchors. Direct SPI register reads with the nRF briefly halted showed:

| Register | Deaf A | Healthy B |
|---|---:|---:|
| DEV_ID | `deca0302` | `deca0302` |
| CHAN_CTRL | `0000094e` | `0000094c` |
| DTUNE0 | `0041101c` | `1001101d` |
| TX_FCTRL | `00001c0c` | `0000380c` |

A second A snapshot after further sleep/wake cycles reproduced those fields.
A used SFD type 3 and timeout 65 while its software expected type 2 and timeout
4097. The chip identity was valid, so the old wake path reused the bad PHY.
The register-reading script did not write DW registers or reset either chip;
it borrowed idle SPIM3 while the CPU was halted, then restored its DMA RAM,
configuration and events. Halting extends that scan, so these snapshots prove
configuration rather than normal timing. Raw whole-RAM snapshots are retained
locally and are not published.

Setting only the idle driver's `radio_configured=false` and
`radio_state_unknown=true` caused its existing DW-only reset/configuration path.
The nRF, assignment and protocol RAM were preserved. The same live cohort then
passed a three-anchor survey (all three pairs usable) and all three battery
reads (`gui-dw-recovery.log`). No firmware was flashed before this causal test.

The fix validates stable PHY fields after fresh configuration and after each
retained wake, before restoration. A mismatch invalidates the cached radio
state and takes the bounded full DW recovery path. Dynamic packet lengths,
TX offsets, ranging bits, RX timeouts and status are excluded. Normal wake
adds four register reads; scan/wake timing and continuous acquisition remain
unchanged. This is not a measured-current claim.

The native fixture compiles the unchanged production validation/wake/recovery
functions against independently controlled physical registers. It covers
same/cross-PHY requests, individual mismatches, successful retention, mutable
TX fields, all four read-error positions and failed reset. Removing the
comparison makes the regression fail. Both mandatory labels pass: 178 mesh
integration tests and 170 hardware-model tests. Anchor, gateway and clicker
builds pass; RAM use is 124928, 125824 and 97872 bytes respectively.

All three anchors and the gateway were flashed with normal sector erase at
4 MHz and read back byte-for-byte. Probe roles remain three mesh anchors and
one mesh gateway, as listed in `fixed-firmware-readback.json`.

The electrical event causing configuration loss was not measured. A debugger
reset alone is not evidence of a physical charger transition.

## Additional live failure and repair

The first run with retention verification passed three-anchor survey and
battery reads, but the fifth repeated blink timed out on C before fault
injection. This run is preserved as `gui-pre-promotion.log` and
`pre-promotion/`. C was in an ordinary followup listener for A's routing
traffic. Its standard-PHR probe decoded gateway wake copies but did not return
control candidates in that listener mode. It closed at host monotonic
225017.328 and rescheduled after 380 ms; the gateway wake ended at 225017.680
and command payload transmitted at 225017.763. This was separate from retained
PHY loss; no `DBG_DWM_PHY_LOST` occurred.

The ordinary listener now recognizes valid control wakes and adopts the
existing depth-derived control receive window only after replay/network
validation succeeds. Its original hard deadline and pending route identity
remain intact. A focused regression covers promotion near route-window expiry,
foreign/replayed wakes, and the unchanged hard cap. Survey ownership still
prevents unrelated receive work.

## Final live qualification

`gui-qualified.log` passed in 178.61 seconds: 27 identification commands,
nine battery reads and two three-anchor surveys with five successful samples
for each of three pairs (30/30). After the first survey and nine blinks,
`dw_fault.py` independently wrote the exact observed bad CHAN_CTRL, DTUNE0
and TX_FCTRL values into every anchor's awake DW3000. Each write was read back.
The injector did not change nRF RAM or request a reset.

All three emitted `DBG_DWM_PHY_LOST` on their next wake, recovered, then passed
battery reads and nine further blinks before any re-enumeration. The final
survey, battery batch and nine blinks also passed. Battery replies retained
boot counters A=90, B=249 and C=16 throughout all three phases. The final LED
completion markers reported 10000 ms for each anchor. No natural retention
mismatches appeared outside the three injected faults in this run.

The final source again passed 178 mesh-integration and 170 hardware-model
tests, including the listener-promotion regression. All production roles
built. The final anchor image was flashed onto A/B/C and all four boards were
read back against their final HEX images. Gateway byte content was unchanged
by the anchor-only listener promotion.

The user was asked to repeat the physical charger transition after this
qualification; its outcome is separate from the injected-fault pass above.

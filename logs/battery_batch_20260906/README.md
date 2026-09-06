# Whole-roster batteries and map actions — 2026-09-06

GUI-only follow-up to firmware merge eb7bc3e25. No firmware was changed or flashed for this work.

## Reproduce

Launch from this checkout with `.venv/bin/python -m tools.gateway_gui`. Connect and enumerate (Run Survey also establishes enumeration), then press **Read all batteries**. The separate voltage window lists each current anchor and request outcome. **Show voltages** reopens it without RF work. Right-click an anchor in Click Location or Survey & Geometry and select **Blink RGB (10 s)**; hover reads cached voltage and its age.

The default window is 1240x660 to fit a 1280x720 desktop with decorations. Editors and survey tables open separately; both fullscreen maps share the same editors. Advanced survey/network commands are under **More network actions…**. The packet inspector is hidden on map/command tabs and restored on Packets/Activity.

## Live evidence

The four-probe RTT logs in each capture folder are committed as lossless `.log.gz` archives, preserving their original bytes and line endings.

`capture/gui.log` records one real BLE survey with three solved anchors, two complete battery batches (six fresh conversions total), cached tooltip text, and three map-menu identify actions. The window was set to 1280x720. First and second batch source timestamps advance on all three targets; the latest voltages were 2.942 V, 3.734 V, and 2.934 V. Voltage calibration was not checked with a meter.

`capture/gpio-checks.json` and `capture/gpio-observations.json` record nonhalting GPIO/RAM observations. Each selected anchor showed exactly one indication start, all three RGB channel states, no other concurrently indicating anchor, and an active interval of 9.924–9.965 seconds at the observation cadence. DW3000 sleep was observed in 239–243 of 246–250 active-indication samples. This is state evidence, not measured current or optical verification. `bench-v2.log` ends in `ANCHOR_ACTIONS_HIL_OK`.

Final roles, unchanged by this test:

| Probe | Role | Stable anchor ID |
| --- | --- | --- |
| E46070D247233537 | mesh_anchor A | c1c2306a5138ab2d |
| E46070D247394D36 | mesh_anchor B | 0f6d3a3bdac0f858 |
| E4645C15CB365D30 | mesh_anchor C | 9699122bd60a64e3 |
| E4645C15CB0F3B37 | mesh_gateway | gateway 9999888877776666 |

The unsuccessful first UI run is retained under `capture-map-clipped/`: both battery batches succeeded, but the inline voltage table squeezed out the click canvas. This caused the separate-window layout repair. Synthetic pointer testing uses actual pointer movement (`warp=True`) so a real Leave event cannot invalidate its hover input.

## Software gates

- 328 GUI tests pass, including 50-anchor serialized batches, saved-depth deadlines, individual failure/timeout continuation, late-response rejection, cache preservation, enumeration/disconnect/survey guards, map context gestures, and existing geometry/solver/lock regressions.
- Real Tk layout tests check both 1280x720 and 1240x660, every editor tab, scroll access to the 50th battery row, packet-inspector restoration, and both fullscreen editors.
- `mypy --explicit-package-bases --exclude 'tools/gateway_gui/tests/' tools/gateway_gui`: no issues in 40 source files.
- `compileall` and `git diff --check`: pass.
- A final focused rerun passed 20 battery, action, and compact-layout tests after type annotations and the existing survey gateway-ID type narrowing were corrected.

The final layout cleanup shares editors between main/fullscreen, places windows relative to their owner, and keeps voltage tooltips within screen edges. These presentation changes were checked with Tk tests after the live capture. Firmware/native gates were not rerun for this GUI-only change; prior firmware qualification remains recorded in Documentation/Reviews/scan-click-actions-survey-2026-09-06.md.

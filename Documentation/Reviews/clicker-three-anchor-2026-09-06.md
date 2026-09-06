> Historical review/evidence: findings, timings, probe mappings and test totals below belong to the recorded checkout and experiment. Revalidate against the current implementation; use the [documentation index](../README.md) for current contracts. Recommendations here are not user-approved requirements.

# Clicker qualification with three anchors — 2026-09-06

This follows the recovery implementation pass. The user replaced the gateway's debug target with a separate physical clicker. Fresh FICR reads identified that clicker as `0xa378b0f6495e09ac`, node `0xea35f5b508104aae`, on probe `E46070D247394D36`. The original gateway remained powered and reachable over BLE at `E4:16:B7:C7:E6:95`, so delivery can be checked with five powered boards and four debug probes.

The clicker's existing 24 KiB NVS partition was double-read and backed up before sector-preserving flashing; no storage initialization or role migration was required. Backup SHA-256: `52be9e642d9f47fe8ddc4ba16c001c8159d1ea21b1c424fa56391d490bb5d6a4`.

## Test configuration and evidence

All participants use the production three-anchor quorum. Temporary `CONFIG_IMEC_CLICKER_RTT_CONTROL=y` provides automated gestures; the two-anchor bench option is disabled. Native pre-flash gates passed: 170 mesh-integration and 162 hardware-model tests. Logs are in `logs/clicker_three_anchor_20260906/`.

The initial DDD case passed five clicks and fifteen exact host-receipted reports. Every report contained four successful samples, matching the current three-anchor ranging budget. F2F1D reachability then qualified the actual expected depths without retry: A (`0xc1c2306a5138ab2d`) at depth 3 through B (`0x9699122bd60a64e3`) at depth 2, then C (`0x0f6d3a3bdac0f858`) at depth 1 and the gateway. Each hop is visible in the captured C5 bank TX peer and ACK records.

Ten F2F1D clicks passed with thirty complete, receipted reports. A clicker-only reboot passed three more clicks and nine complete reports, with event IDs advancing from the previous block to 17301505–17301507. A rapid three-click burst passed another nine complete, receipted reports.

F2F1D self-test (`LONG,CLICK`) delivered one successful self-test result with a host receipt. Letting the self-test arm expire before clicking produced a normal three-anchor click with three complete reports. Halting all three anchors produced one bounded failed click (`ret=-116`) and no report. Resuming the anchors allowed the next three clicks to complete with nine exact host-receipted reports. These cases preceded the battery-policy flash.

After flashing the battery-policy update, three normal F2F1D clicks and a rapid three-click burst passed with eighteen complete, host-receipted reports, four successful samples per report and six successful click completions. Evidence is under `battery-policy/`; event IDs are 17367041–17367046.

Final board state: the physical clicker runs `mesh_clicker` with RTT command control disabled; all three anchors run the normal `mesh_anchor` image. Sector-preserving flashing retained durable configuration. Final durable enumeration passed with the three expected node IDs at depth 1, no multihop nodes and no retries (`final-ddd-provision.log`). The separately powered gateway was not reflashed during this clicker task.

Forced-hop isolation proves the configured decode-layer chain, not a physically hidden RF topology. RTT gestures exercise the button state machine and click protocol; they do not mechanically press the switch.

## Exact battery measurement sequence

The following describes the implemented battery-policy update.

1. The battery worker services the clicker's watchdog on each indicator wake. It reads the charger's active-low power-good pin P0.15 with a temporary pull-up, then disconnects the pin. It samples battery voltage on the first pulse after entering idle, hourly on battery, every 30 seconds with valid USB power, and after detecting a USB transition. When sampling is due, it drives **P0.07 low**, enabling the P-channel MOSFET-controlled battery divider.
2. It sleeps for **6 ms** for settling, then configures and reads SAADC channel 5 on **AIN5/P0.29**: 12-bit resolution, internal 600 mV reference, gain 1/6, default 10 µs acquisition, one conversion, no oversampling and no requested offset calibration.
3. On conversion END, the local Zephyr SAADC driver issues STOP and disables SAADC before completing the synchronous read. Application cleanup drives **P0.07 high** to disable the divider. Thus the ADC and divider are already off during the following LED pulse.
4. The sample is converted to millivolts, clamped at zero if negative, then doubled to undo the 1:2 divider. These are voltage bands rather than a measured state-of-charge estimate: LED0 is red below 3.4 V, blue from 3.4 through 3.8 V inclusive, and green above 3.8 V.
5. On battery, the cached voltage drives a pulse every **5 seconds**, lasting **25 ms** for green/blue or **15 ms** for red. With valid USB power, the pulse lasts **100 ms every second**: red below 3.4 V, blue through 4.0 V inclusive, one green above 4.0 V, and both green above 4.15 V. These last two thresholds are the existing linear voltage scale's >80% and >95%, not a fuel-gauge measurement. Both LEDs are switched off and disconnected after the pulse; the divider is not enabled for cached pulses. LED0 uses P0.20 red, P0.14 green and P0.17 blue.
6. Cleanup is attempted even after partial divider-enable or ADC failure. Divider disable gets up to three attempts with 100 µs between attempts; the periodic worker proves off again after an error. If off cannot be established, it stops watchdog feeds and cold-restarts after one second rather than leaving the divider energized indefinitely.

Battery indication is suspended during click/self-test actions and restarted on return to idle. Resuming marks the cached sample for refresh on the next pulse, so an action burst coalesces into one post-action reading. The self-test report still carries `battery_mv = 0`. Anchor sampling remains approximately every five seconds with a 50 ms pulse and different voltage bands. No sample or charging state is written to NVS.

The BQ24090 CHG output can remain high impedance during refresh charging or with a missing battery; it is not used as an independent full-charge indication. See the [TI datasheet](https://www.ti.com/lit/ds/symlink/bq24090.pdf).

## Low-power boundaries

The battery-policy change builds at 430,456 bytes flash and 97,744 bytes RAM for the production clicker, versus 429,904 and 97,680 immediately before this policy update: +552 bytes flash and +64 bytes RAM. The temporary RTT-control test build is 432,996 bytes flash and 97,808 bytes RAM. The production anchor builds at 455,648 bytes flash and 124,800 bytes RAM; its behavior and RAM use are unchanged. The updated pre-flash gates pass 170 mesh-integration and 162 hardware-model tests.

The production clicker uses retained System ON with tickless scheduling and the 32 kHz crystal. Idle stops BLE, parks the DWM3000 in retained sleep, disables the divider, floats DWM SPI/CS/WAKE/RESET pins, disconnects LEDs and arms the P0.26 button edge interrupt. A held button waits for its release interrupt rather than continuously polling. Watchdog supervision shares the battery wake. The logging worker blocks when no messages are pending.

The final production flash has RTT command control disabled and the three-anchor quorum enabled. Full HEX readback matched 430,439 programmed bytes; HEX SHA-256 is `b00cfa9240e397357ab91197fa162093907de822dc3e6d0e2ffc9daef3a97b70`. Five live snapshots showed a sleeping CPU, SAADC and SPIM3 disabled, divider off, `radio_awake=0`, DWM pins disconnected, and PG disconnected without a pull-up. The cached battery reading was 3,774 mV. The attached debug supply did not assert charger power-good, so physical USB charging transitions and voltage thresholds were not exercised on this bench; those branches passed the extracted production-code tests.

A forty-second SWD observation of the final image measured a median battery pulse period of 5.0007 seconds and width of 26.5 ms, consistent with the requested 25 ms timer plus scheduling/debug-sampling resolution. The cached sample timestamp did not change across these pulses. This proves the implemented cache is used on hardware, not a current-consumption value. Evidence is `final-battery-policy-observation.json`.

The battery-only green/blue duty cycle remains 0.5%; red falls from 0.5% to 0.3%. Click/self-test feedback still lasts two seconds. No battery-current measurement or battery-life claim is made from debugger-attached functional tests.

With no clicks and no USB, hourly sampling reduces reads from 8,640 to 24 per day (99.72%). The 6 ms divider-settling contribution falls from 51.84 seconds to 0.144 seconds per day. For two documented 680 kΩ divider resistors at 3.7 V, the resistor-only average saving is about 0.0016 µA; the ADC and CPU work also falls, but has not been measured. The documented red LED estimate drops from 0.12 to 0.072 mAh/day, a saving of 0.048 mAh/day. Green/blue LED energy is unchanged, and timer wakes double, so sample-count reduction is not a total-idle-power reduction estimate.

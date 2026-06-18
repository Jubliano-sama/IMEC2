# IMEC Power Profile Test Firmware

This app is for external battery-emulator measurements. It disables USB,
console, logging, inherited nRF52833 DK LEDs/buttons/peripherals, the battery
ADC divider, and all status LEDs by default. It uses the real DWM3000 port and
driver for UWB current profiles.

Build examples:

```sh
.venv/bin/west build --no-sysbuild -p always -s firmware/power_profile_test -b nrf52833dk/nrf52833 --build-dir build/power-clicker-sleep -- -DPOWER_PROFILE_MODE=clicker_sleep
.venv/bin/west build --no-sysbuild -p always -s firmware/power_profile_test -b nrf52833dk/nrf52833 --build-dir build/power-clicker-systemoff -- -DPOWER_PROFILE_MODE=clicker_systemoff
.venv/bin/west build --no-sysbuild -p always -s firmware/power_profile_test -b nrf52833dk/nrf52833 --build-dir build/power-clicker-systemoff-nowake -- -DPOWER_PROFILE_MODE=clicker_systemoff_nowake
.venv/bin/west build --no-sysbuild -p always -s firmware/power_profile_test -b nrf52833dk/nrf52833 --build-dir build/power-anchor-sleep -- -DPOWER_PROFILE_MODE=anchor_sleep
.venv/bin/west build --no-sysbuild -p always -s firmware/power_profile_test -b nrf52833dk/nrf52833 --build-dir build/power-anchor-low-duty -- -DPOWER_PROFILE_MODE=anchor_low_duty
.venv/bin/west build --no-sysbuild -p always -s firmware/power_profile_test -b nrf52833dk/nrf52833 --build-dir build/power-clicker-click -- -DPOWER_PROFILE_MODE=clicker_click
.venv/bin/west build --no-sysbuild -p always -s firmware/power_profile_test -b nrf52833dk/nrf52833 --build-dir build/power-responder -- -DPOWER_PROFILE_MODE=responder
```

Modes:

- `clicker_sleep`: initializes DWM3000, puts it in retained sleep, then idles forever with RAM/kernel retained.
- `clicker_systemoff`: initializes DWM3000, puts it in retained sleep, configures the click button as wake source, then enters nRF SYSTEMOFF.
- `clicker_systemoff_nowake`: same DWM3000 parking path, but enters nRF SYSTEMOFF without a GPIO wake source. Use this to isolate true board/DWM leakage from button-wake issues; recover with SWD reset or power cycle.
- `anchor_sleep`: same retained-sleep baseline using the anchor/responder ID.
- `anchor_low_duty`: repeats a wake-mode UWB RX window, then returns DWM3000 to retained sleep.
- `clicker_click`: repeats one DS-TWR click burst every fixed period, then returns DWM3000 to retained sleep.
- `responder`: continuous DS-TWR responder listen slices for pairing with `clicker_click`.

Useful options:

- `POWER_PROFILE_BOOT_SETTLE_MS=5000u`: quiet boot delay before measurement.
- `POWER_PROFILE_ANCHOR_SCAN_INTERVAL_MS=1000u`: anchor low-duty period.
- `POWER_PROFILE_ANCHOR_RX_WINDOW_MS=40u`: anchor low-duty RX window.
- `POWER_PROFILE_CLICK_PERIOD_MS=10000u`: repeated click-burst period.
- `POWER_PROFILE_TARGET_COUNT=1u`: responders ranged per click burst, up to 4.
- `POWER_PROFILE_LED_MARKERS=0`: set to `1` only if LED marker current is acceptable.

Flash example:

```sh
.venv/bin/pyocd flash -t nrf52833 build/power-clicker-sleep/zephyr/zephyr.hex
.venv/bin/pyocd reset -t nrf52833
```

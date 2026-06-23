# Refactor Minimized

This file is the active remaining task list derived from `Refactor.md`.
Remove a task from this file only after its implementation and validation rules
below are satisfied. Use `Refactor.md` as the unedited reference when this file
has no remaining tasks.

## Preservation Rules

- Keep behavior-preserving refactor scope. Do not redesign protocol behavior.
- Preserve all build presets, queue depths, timing, sleep/wake decisions, Zephyr callback registration, BLE behavior, mesh/report behavior, ML behavior, survey behavior, and high-debug/staged behavior.
- Do not delete or simplify specialized paths just because they look narrow: ML clicker, ML anchor, deterministic ML anchor slots, high-debug/staged presets, `tag_stage1_wake_spam`, gateway BLE connectivity test, clicker system-off idle, retained system-on idle, BLE courtesy scan, mesh channel-9 event-control, survey discovery/pair survey, and DWM3000 sleep/wake/SPI/polling behavior.
- Keep app-private headers under `firmware/app/src/`; do not move app-private APIs into `firmware/include/`.
- Keep `LOG_MODULE_REGISTER` in `.c` files only.
- Keep Zephyr registration macros and callback implementations in the same translation unit as their registrations.
- Keep `BUILD_ASSERT` blocks in a translation unit that sees all timing macros.
- Keep module-private helpers and globals `static` unless another module must call them.

## Validation Rules

Run the task-specific commands before removing a task. For final cleanup, run
the full matrix:

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker -- -DFIRMWARE_ROLE=clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor -- -DFIRMWARE_ROLE=anchor
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway -- -DFIRMWARE_ROLE=gateway
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-clicker -- -DIMEC_BUILD_PRESET=ml_clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-1 -- -DIMEC_BUILD_PRESET=ml_anchor_1
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/tag-stage1-wake-spam -- -DIMEC_BUILD_PRESET=tag_stage1_wake_spam
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/gateway-ble-connectivity-test -- -DIMEC_BUILD_PRESET=gateway_ble_connectivity_test

git diff --check
rg "dwm3000_driver_initialise|dwm3000_driver_listen_activity|dwm3000_driver_stats_reset" firmware Documentation
```

Expected final `rg` result for the removed DWM3000 APIs: no declarations,
definitions, documentation references, or call sites.

## Remaining Tasks

## Review Checklist

- No DWM3000 timing, sleep/wake, SPI speed transition, retained-config, or SYS_STATUS polling behavior changed.
- No protocol packet structs changed.
- No TLV layouts changed.
- No report payload formats changed.
- No queue depths changed.
- No mesh routing behavior changed.
- No one-frame-per-window RX behavior changed.
- No power mode decision changed.
- No LED meaning changed.
- No ML deterministic slot behavior changed.
- No stage/high-debug behavior removed.
- Gateway BLE connectivity-test preset still builds.
- BLE GATT callbacks remain referenced through Zephyr registration macros.
- GPIO ISR callback remains registered correctly.
- Work handlers remain visible to their `k_work_init*` registration.
- `LOG_MODULE_REGISTER` appears only in `.c` files.
- `BUILD_ASSERT` blocks remain in a translation unit that sees all timing macros.
- `*_UNUSED` attribution macros remain available to all config-gated modules.
- Shared globals are defined once.
- Module-private globals remain static.
- No new headers are added to `firmware/include` for app-private APIs.
- `main.c` contains orchestration only after final cleanup.
- Every moved function appears in exactly one new owner module.
- Role-specific code is not accidentally compiled into unrelated roles unless preserved as existing guarded stub behavior.
- `git diff --check` passes.
- Native tests and all required Zephyr role/preset builds pass.

## Out Of Scope

- Moving protocol-independent helpers from `firmware/app/src` into `firmware/src`.
- Adding native tests for newly moved app helpers.
- Replacing extern shared state with accessors.
- Renaming public protocol enums/macros.
- Changing DWM3000 driver behavior beyond the three dead public API removals.
- Reworking mesh/report payload semantics.
- Optimizing power behavior.
- Splitting `app_anchor.c` further unless it becomes unreviewably large.

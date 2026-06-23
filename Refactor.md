1. Refactor intent

This is a behavior-preserving refactor of the Zephyr/west firmware app. The app currently builds clicker, anchor, gateway, ML, high-debug, staged, wake-spam, and gateway BLE connectivity-test variants from a very large firmware/app/src/main.c, while shared protocol/state code lives under firmware/src and firmware/include. The uploaded split plans agree that main.c is roughly 13.5k lines and currently mixes Zephyr entry/init, role orchestration, BLE, GPIO, UWB, mesh, reporting, ML, survey, power, and high-debug paths.

The goal is not to redesign protocol behavior. The goal is to make the app maintainable by:

Removing only confirmed-dead public DWM3000 APIs.
Keeping main.c as the composition root.
Moving cohesive app-local subsystems into firmware/app/src/*.c with narrow private headers.
Preserving all build presets, queue depths, timing, sleep/wake decisions, Zephyr callback registration, BLE behavior, mesh/report behavior, ML behavior, and high-debug/staged behavior.
2. Non-negotiable preservation rules

Do not delete or simplify these paths just because they look specialized:

CONFIG_IMEC_ML_CLICKER
CONFIG_IMEC_ML_ANCHOR
IMEC_ML_ANCHOR_SLOT
CONFIG_IMEC_HIGH_DEBUG
Stage 0/1/2/3 high-debug presets
tag_stage1_wake_spam
Gateway BLE connectivity-test preset
Clicker system-off idle
Clicker retained system-on idle
BLE courtesy scan behavior
Mesh channel-9 RX/event-control behavior
Survey discovery and pair-survey behavior
DWM3000 sleep/wake, standby, SPI speed transition, retained-config, and polling behavior

Every extraction should be a move-only or near move-only change. Any semantic change should be deferred unless required to compile after the split.

3. Phase 0: Baseline and guardrails

Before editing, capture a clean baseline:

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

The uploaded split plan also notes that role builds are the primary app-level correctness check, with native tests still useful for catching shared include/build drift.

4. Phase 1: Remove only confirmed-dead DWM3000 public APIs

Remove these public APIs from firmware/app/src/dwm3000_driver.c and firmware/app/src/dwm3000_driver.h:

dwm3000_driver_initialise(bool idle_after_init)
dwm3000_driver_listen_activity(uint32_t timeout_ms, bool *activity_detected)
dwm3000_driver_stats_reset(void)

Rationale:

dwm3000_driver_initialise(bool idle_after_init) is superseded by the real initialization path through dwm3000_driver_configure_default(), which calls private initialise_radio(false).
dwm3000_driver_listen_activity(...) has no firmware or documentation caller; receive paths use frame receive, continuous receive, responder polling, or range initiator paths.
dwm3000_driver_stats_reset(void) has no caller; diagnostics read stats through dwm3000_driver_stats_get.

Do not remove:

dwm3000_driver_receive_frame_detailed

It remains live through dwm3000_driver_receive_frame and firmware/uwb_smoke_test/src/main.c.

Do not remove private helpers such as:

initialise_radio
wake_configured_radio
restore_txrx_after_sleep
status polling helpers

These helpers are part of current DWM3000 sleep/wake and IRQ-free polling behavior.

Validation after this phase:

cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure

rg "dwm3000_driver_initialise|dwm3000_driver_listen_activity|dwm3000_driver_stats_reset" firmware Documentation
git diff --check

Expected rg result: no declarations, definitions, or call sites for the three removed names.

5. Canonical app-local module layout

All new files live under:

firmware/app/src/

Headers are app-private and stay beside the .c files. Do not move these headers into firmware/include, because that directory is for protocol-level public headers.

Use this fused layout:

File	Header	Responsibility
main.c	—	Composition root only: main(void), boot banner ordering, build/role selection, shared hardware init ordering, workqueue start/init, role dispatch, gateway BLE connectivity-test alternate path, BUILD_ASSERT blocks, and LOG_MODULE_REGISTER(uwb_app, ...).
app_config.h	—	Header-only role/build constants: ROLE_CLICKER, ROLE_ANCHOR, ROLE_GATEWAY, DEVICE_ROLE, DEVICE_ID, GATEWAY_ID, NETWORK_ID, build metadata defaults, timing macros, devicetree alias macros, stage/preset macros, and *_UNUSED attribution macros.
app_state.c	app_state.h	Shared cross-module state and cross-cutting utilities. Use this only for state that is genuinely touched by multiple modules. Module-private state stays static in the owning module.
app_board.c	app_board.h	Board-local GPIO/ADC/serial helpers: status LEDs, LED disconnect/park helpers, battery ADC divider/sample helpers, debug serial init/readiness helpers.
app_gateway_ble.c	app_gateway_ble.h	Gateway BLE GATT service, packet RX/TX transport, BLE log backend, BLE connection callbacks, host frame encode/decode, gateway command ingress/result forwarding, gateway BLE connectivity-test helpers.
app_clicker.c	app_clicker.h	Clicker button GPIO, ISR/work handlers, button FSM glue, clicker action worker, normal/diagnostic click paths, clicker UWB discovery/ranging, clicker idle entry, system-off wake capture, retained system-on idle, clicker BLE courtesy scan, wake-spam path.
app_ml.c	app_ml.h	ML clicker collection runtime, ML clicker BLE frame handling, anchor cache, ML post-burst diagnostics, buffered host packets, ML anchor battery LED sampling, ML anchor BLE debug-log startup, deterministic ML discovery slot handling.
app_mesh_report.c	app_mesh_report.h	Mesh RX/TX work handlers, report TX queueing, mesh UWB RX scheduling, channel-9 event timing/control, route-discovery follow-up, report builders, gateway local delivery of mesh reports.
app_anchor.c	app_anchor.h	Anchor UWB scan work, wake-claim handling, scheduled responder orchestration, anchor heartbeat/reboot work, command dispatch, survey discovery, pair survey, survey abort/state helpers, anchor DWM3000 standby/idle choices.
app_high_debug.c	app_high_debug.h	High-debug log formatting, counter dumping, boot banner helpers, serial command polling, bootloader request handling, staged LED/radio/BLE tests, stage1 click/anchor instrumentation.

This layout intentionally fuses the more granular uploaded split proposal with the narrower module set in your prompt: app_board and app_state/app_config are retained as support modules, while mesh/report and anchor ops/UWB are grouped into cohesive first-pass modules. The uploaded plans identify the same key need: keep public interfaces private to firmware/app/src, use app_state.h for first-pass shared globals, and leave subsystem internals static wherever possible.

Optional follow-up after the first refactor builds cleanly: if app_anchor.c or app_mesh_report.c remains too large, split them further into app_anchor_uwb.c, app_anchor_ops.c, app_mesh_glue.c, and app_report.c. Do not do that in the first pass unless size or build coupling forces it.

6. main.c final shape

main.c remains as the application composition root. It should contain:

LOG_MODULE_REGISTER(uwb_app, ...)
BUILD_ASSERT blocks that depend on timing macros
role/preset constants initially, or includes of app_config.h
gateway_ble_connectivity_test_run() if that preset needs an alternate top-level path
main(void)
shared hardware/init ordering
workqueue start/init calls
role dispatch into app modules
no BLE GATT internals
no mesh RX/TX internals
no clicker button FSM internals
no ML collection internals
no high-debug command parser internals
no anchor survey/ranging internals

A target shape:

int main(void)
{
    enum button_action boot_action = BUTTON_ACTION_NONE;
    int ret;

    app_board_pre_init();
    app_board_init();

    app_high_debug_boot_banner();

    app_gateway_ble_init_if_enabled();
    app_mesh_report_init();

    if (DEVICE_ROLE == ROLE_CLICKER) {
        app_clicker_init(&boot_action);
    } else if (DEVICE_ROLE == ROLE_ANCHOR) {
        app_anchor_init();
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        app_anchor_init();
        app_gateway_ble_start_if_enabled();
    }

    app_ml_init_if_enabled();

    if (DEVICE_ROLE == ROLE_CLICKER) {
        if (boot_action != BUTTON_ACTION_NONE) {
            app_clicker_handle_button_action(boot_action);
        } else {
            app_clicker_enter_idle();
        }
    }

    return 0;
}

Do not force this exact call sequence if the current main.c ordering differs. Preserve current ordering exactly, then clean naming afterward.

7. Shared state and header policy

First-pass shared state strategy:

Define genuinely cross-module objects once in app_state.c.
Declare them as extern in app_state.h.
Keep module-private state static in the owning .c.
Do not introduce getters/setters during the mechanical split unless needed to avoid circular dependencies.

Likely shared state candidates:

uwb_rf_active
uwb_rf_lock
anchor_uwb_lock
anchor_uwb_busy
mesh_uwb_rx_active
anchor_heartbeat_enabled
mesh_runtime
mesh_event_stats
anchor_uwb_session
anchor_uwb_scan_interval_ms
next_event_seq
mesh_event_control_seq

Likely cross-cutting utility candidates for app_state.c / app_state.h:

role_name
command_status_name
claim_decision_name
range_status_name
range_status_valid
mesh_id_is_unicast
uptime_deadline_reached
uptime_ms_until_deadline
packet_age_add
packet_age_add_elapsed
mesh_outbound_refresh_age
mesh_outbound_ready_for_tx
mesh_rx_pending_refresh_age
next_click_event_seq
nonzero_uptime_session_id
mesh_next_event_control_seq
average_i32_nearest
u32_saturating_add
ceil_us_to_ms
delay_ms_to_u16
uwb_schedule_burst_id
scheduled_range_sample_target_us
scheduled_post_burst_diag_target_us
scheduled_post_burst_diag_seq
mix64
clicker_priority_id
clicker_nonce
survey_sample_seq
local_uwb_short_addr
discovery_window_ms_for_slots
local_anchor_discovery_slot
local_survey_discovery_slot
sleep_until_ms
sleep_with_uwb_standby_until_ms
sleep_with_uwb_idle_until_ms
sleep_precise_us
sleep_until_us
radio_guard_uwb_start
radio_guard_uwb_stop

Header rules:

app_config.h may be included everywhere.
app_state.h may be included by modules that need shared state or shared utilities.
Module headers expose only functions needed by another module.
Keep helper functions static unless another module must call them.
Delete the old giant forward-declaration block once prototypes live in module headers.
Never place LOG_MODULE_REGISTER in a header.
Give each new .c its own LOG_MODULE_REGISTER(<module>, LOG_LEVEL_DBG) where useful.

The uploaded plan specifically calls out that Zephyr macro bodies and callback registrations should remain file-local, and that LOG_MODULE_REGISTER should remain one-per-translation-unit rather than in headers.

8. Kernel object ownership

Move kernel objects to the module that owns their lifecycle and handlers:

Object family	Owner
mesh_rx_msgq, mesh_rx_work, mesh_uwb_rx_work, mesh_tx_timeout_work, route-waiting TX state	app_mesh_report.c
report_tx_msgq, report_tx_work	app_mesh_report.c
gateway_ble_rx_msgq, BLE RX work, gateway command result timeout work	app_gateway_ble.c
clicker action workqueue/stack, clicker pending action state, BLE courtesy state	app_clicker.c
anchor scan work, anchor heartbeat work, reboot work, survey workqueue/stack	app_anchor.c
ML clicker collect work, ML anchor battery LED work	app_ml.c

Exception: if a kernel object is genuinely touched by several modules and moving it creates brittle coupling, keep it in app_state.c for the first pass.

9. Zephyr callback and macro constraints

Keep these in the same translation unit as their registration macros or initialization:

BT_GATT_SERVICE_DEFINE(...) → app_gateway_ble.c
BT_CONN_CB_DEFINE(...) → app_gateway_ble.c
BLE log backend callbacks → app_gateway_ble.c
GATT write/CCC callbacks → app_gateway_ble.c
gpio_init_callback target callback and button ISR → app_clicker.c
Work handlers may remain static in their owner module, with k_work_init* performed by the same module or called through a narrow module init function

Do not move macro callback implementations into headers.

10. CMake strategy

Update firmware/app/CMakeLists.txt for both source modes:

Normal/preset builds.
Reduced gateway_ble_connectivity_test source set.

The fused source list should include at minimum:

target_sources(app PRIVATE
    src/main.c
    src/app_state.c
    src/app_board.c
    src/app_gateway_ble.c
    src/app_clicker.c
    src/app_ml.c
    src/app_mesh_report.c
    src/app_anchor.c
    src/app_high_debug.c

    src/dwm3000_driver.c
    src/dwm3000_port.c
    src/dwm3000_sdk_port.c

    "${DWM3000_SDK_DIR}/decadriver/deca_device.c"

    ../src/debug_log.c
    ../src/protocol.c
    ../src/gateway_command.c
    ../src/uwb.c
    ../src/uwb_ble_courtesy.c
    ../src/uwb_session.c
    ../src/mesh.c
    ../src/mesh_relay.c
    ../src/report.c
    ../src/serial_frame.c
    ../src/status.c
    ../src/route.c
    ../src/survey.c
)

For the gateway_ble_connectivity_test branch, include:

src/main.c
src/app_config.h   # header only, not in target_sources unless desired by IDE convention
src/app_state.c
src/app_board.c
src/app_gateway_ble.c
src/app_high_debug.c

Also include any minimal dependency modules needed by compile-time references. Safer first-pass option: list all new app-local modules in the reduced branch too, with code protected by the same existing #if defined(...) guards and no-op stubs. This avoids hidden link failures from Zephyr macro references or helper calls. The uploaded plan explicitly flags the reduced connectivity-test branch as a CMake risk that should be handled deliberately.

11. Implementation order
Step 1 — Driver cleanup

Remove only:

dwm3000_driver_initialise
dwm3000_driver_listen_activity
dwm3000_driver_stats_reset

Then run:

cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
rg "dwm3000_driver_initialise|dwm3000_driver_listen_activity|dwm3000_driver_stats_reset" firmware Documentation
git diff --check
Step 2 — Add empty module shells and CMake wiring

Create files and headers:

firmware/app/src/app_config.h
firmware/app/src/app_state.c
firmware/app/src/app_state.h
firmware/app/src/app_board.c
firmware/app/src/app_board.h
firmware/app/src/app_gateway_ble.c
firmware/app/src/app_gateway_ble.h
firmware/app/src/app_clicker.c
firmware/app/src/app_clicker.h
firmware/app/src/app_ml.c
firmware/app/src/app_ml.h
firmware/app/src/app_mesh_report.c
firmware/app/src/app_mesh_report.h
firmware/app/src/app_anchor.c
firmware/app/src/app_anchor.h
firmware/app/src/app_high_debug.c
firmware/app/src/app_high_debug.h

Initial files may contain only includes, no-op init functions, and guarded stubs. Update CMake for normal and connectivity-test source sets. Build all standard roles before moving logic.

Step 3 — Extract app_config.h and app_state.[ch]

Move:

role constants
device/network IDs
build metadata defaults
timing macros
devicetree alias macros
attribution macros such as BLE_CONNECTIVITY_TEST_UNUSED, ML_ANCHOR_ONLY_UNUSED, ML_CLICKER_BUTTON_UNUSED, GATEWAY_BLE_HOST_COMMAND_UNUSED
cross-module state definitions
cross-cutting utility functions
radio guard helpers
sleep timing helpers

Keep BUILD_ASSERT blocks in main.c, but make them include app_config.h.

Build:

cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker -- -DFIRMWARE_ROLE=clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor -- -DFIRMWARE_ROLE=anchor
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway -- -DFIRMWARE_ROLE=gateway
Step 4 — Extract app_board.[ch]

Move:

status LED GPIO specs
configure_output
set_output
status_leds_set
status_led0_set
status_leds_init
status_leds_disconnect
disconnect_gpio
status_apply
battery ADC specs
battery divider enable/disable
battery_sample_lithium_mv
debug serial init/readiness helpers

Keep board behavior identical. Do not change LED meaning.

Build standard roles.

Step 5 — Extract app_gateway_ble.[ch]

Move:

BLE GATT service
BLE connection callbacks
CCC/write callbacks
packet RX/TX framing
BLE notify helpers
gateway BLE RX queue/work
BLE log backend
UWB quiet enter/exit for BLE
gateway command ingress
gateway command result forwarding
gateway command timeout tracking
gateway BLE connectivity-test helpers
TLV parse helpers used by gateway command flow

Preserve:

CONFIG_IMEC_GATEWAY_BLE
CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST
all real/stub #if/#else walls
registration macro locality

Build:

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway -- -DFIRMWARE_ROLE=gateway
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-clicker -- -DIMEC_BUILD_PRESET=ml_clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/gateway-ble-connectivity-test -- -DIMEC_BUILD_PRESET=gateway_ble_connectivity_test
Step 6 — Extract app_clicker.[ch]

Move:

click button GPIO setup
button ISR
debounce/release work
button FSM glue
boot button latch/capture
clicker action worker
handle_button_action
normal click path
diagnostic click path
wake-claim train
anchor discovery
range schedule/release
scheduled ranging burst
politeness/attempt gate
retry/contention delay
clicker idle entry
system-off idle
retained system-on idle
RAM retention helpers
clicker BLE courtesy scan
self-test and wake-spam clicker paths

Preserve exactly:

CONFIG_IMEC_CLICKER_SYSTEMOFF_IDLE
CONFIG_IMEC_CLICKER_SYSTEMON_RETAINED_IDLE
CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS
USB/connectivity-test BLE courtesy variants
current low-power decisions
DWM3000 standby/idle decisions before system-off

Build:

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker -- -DFIRMWARE_ROLE=clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/tag-stage1-wake-spam -- -DIMEC_BUILD_PRESET=tag_stage1_wake_spam
Step 7 — Extract app_high_debug.[ch]

Move everything under CONFIG_IMEC_HIGH_DEBUG, including:

high-debug log event bus
counter struct/macros
counter dump work
boot banner helpers
bootloader request handling
CDC/serial command polling
serial command parser
DWM3000 probe helpers
stage0 LED/radio/BLE/self-test helpers
stage1 LED phase/result helpers
stage1 click trace helpers
stage1 anchor focused RX instrumentation

Expose from app_high_debug.h only what other modules need:

high_debug_log_event
HIGH_DEBUG_COUNTER_INC
high_debug_dump_counters
high_debug_boot_banner
high_debug_request_bootloader
stage1_led_phase
stage1_led_result
stage1_click_diag
stage1_anchor_focused_note_*

Build at least one high-debug clicker/tag preset and one high-debug anchor preset if available.

Step 8 — Extract app_ml.[ch]

Move ML-only paths without treating them as dead code:

For CONFIG_IMEC_ML_CLICKER:

ML clicker collect work
BLE host frame handling
deterministic discovery slot override
anchor cache
pair schedule build/send
pair survey result buffering
range sample record emission
post-burst diagnostic emission
CIR chunk emission
host packet buffering/flushing
ML command result emission
range-start failure continuation behavior

For CONFIG_IMEC_ML_ANCHOR:

ML anchor battery LED sampling
cached battery voltage
ML anchor BLE debug-log startup
full CIR buffer ownership
deterministic anchor ID/slot behavior

Preserve:

CONFIG_IMEC_ML_CLICKER
CONFIG_IMEC_ML_ANCHOR
IMEC_ML_ANCHOR_SLOT
ml_clicker
ml_anchor_1..8

Build:

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-clicker -- -DIMEC_BUILD_PRESET=ml_clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-1 -- -DIMEC_BUILD_PRESET=ml_anchor_1

Optionally build ml_anchor_2 as an extra deterministic-slot check.

Step 9 — Extract app_mesh_report.[ch]

Move:

mesh send outbound
tracked TX
route request/follow-up
route waiting TX state
channel-9 RX scheduling
event control send/handle
mesh RX work handler
mesh TX timeout handler
frame-to-message queueing
mesh UWB RX work
report TX scheduling
report TX work handler
anchor report queueing
range report sample builders
sequence timestamp TLVs
raw timestamp TLVs
anchor status TLVs
gateway local delivery of mesh reports

Preserve:

mesh_rx_msgq
report_tx_msgq
one-frame-per-window RX behavior
timeout scheduling
queue depths
mesh route semantics
report payload format
TLV layout

Build all three standard roles.

Step 10 — Extract app_anchor.[ch]

Move:

anchor UWB scan work
wake-claim handling
responder polling orchestration
scheduled UWB ranging responder flow
post-burst diagnostics trigger points
UWB range-window records/finalization
anchor heartbeat work
anchor reboot work
local/gateway command dispatch
LED pattern command handling
route command handling
scan duty command handling
survey discovery
survey pair prepare
survey pair initiator/responder
survey abort helpers
gateway-side survey orchestration if currently tightly coupled to anchor survey state

Preserve:

current scan-window timing macros
DWM3000 standby/idle choices
anchor_uwb_busy semantics
anchor scan start/stop behavior
survey queueing
survey abort behavior
gateway survey command routing

Build:

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor -- -DFIRMWARE_ROLE=anchor
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway -- -DFIRMWARE_ROLE=gateway
Step 11 — Final main.c cleanup

After all extractions:

remove stale forward declarations
remove dead local helpers that moved
keep only orchestration
confirm every former main.c function lives in exactly one file
confirm all module headers expose only cross-module functions
confirm module-private state is static
confirm app_state.h contains only truly shared externs
run formatting/diff checks

Final main.c target size: roughly a few hundred lines, not another subsystem file.

12. Full verification matrix

Run after driver cleanup, after each extraction batch, and after final cleanup:

cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure

Standard role builds:

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker -- -DFIRMWARE_ROLE=clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor -- -DFIRMWARE_ROLE=anchor
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway -- -DFIRMWARE_ROLE=gateway

Preset builds:

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-clicker -- -DIMEC_BUILD_PRESET=ml_clicker

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-1 -- -DIMEC_BUILD_PRESET=ml_anchor_1

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/tag-stage1-wake-spam -- -DIMEC_BUILD_PRESET=tag_stage1_wake_spam

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/gateway-ble-connectivity-test -- -DIMEC_BUILD_PRESET=gateway_ble_connectivity_test

Optional broader preset sweep:

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-2 -- -DIMEC_BUILD_PRESET=ml_anchor_2

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/tag-stage0-highdebug -- -DIMEC_BUILD_PRESET=tag_stage0_highdebug

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/anchor-stage1-highdebug -- -DIMEC_BUILD_PRESET=anchor_stage1_highdebug

Final checks:

git diff --check

rg "dwm3000_driver_initialise|dwm3000_driver_listen_activity|dwm3000_driver_stats_reset" firmware Documentation

Expected final rg result: no declarations, definitions, or call sites for the removed APIs.

13. Review checklist

Use this checklist for each patch:

No DWM3000 timing, sleep/wake, SPI speed transition, retained-config, or SYS_STATUS polling behavior changed.
No protocol packet structs changed.
No TLV layouts changed.
No report payload formats changed.
No queue depths changed.
No mesh routing behavior changed.
No one-frame-per-window RX behavior changed.
No power mode decision changed.
No LED meaning changed.
No ML deterministic slot behavior changed.
No stage/high-debug behavior removed.
Gateway BLE connectivity-test preset still builds.
BLE GATT callbacks remain referenced through Zephyr registration macros.
GPIO ISR callback remains registered correctly.
Work handlers remain visible to their k_work_init* registration.
LOG_MODULE_REGISTER appears only in .c files.
BUILD_ASSERT blocks remain in a TU that sees all timing macros.
*_UNUSED attribution macros remain available to all config-gated modules.
Shared globals are defined once.
Module-private globals remain static.
No new headers are added to firmware/include for app-private APIs.
main.c contains orchestration only.
Every moved function appears in exactly one new owner module.
Role-specific code is not accidentally compiled into unrelated roles unless preserved as existing guarded stub behavior.
git diff --check passes.
Native tests and all required Zephyr role/preset builds pass.
14. Out of scope for this refactor

Do not include these in the first-pass refactor:

Moving protocol-independent helpers from firmware/app/src into firmware/src.
Adding native tests for newly moved app helpers.
Replacing extern shared state with accessors.
Renaming public protocol enums/macros.
Changing DWM3000 driver behavior beyond the three dead public API removals.
Reworking mesh/report payload semantics.
Optimizing power behavior.
Splitting app_anchor.c further unless it becomes unreviewably large.

Those are good follow-ups after the mechanical split is stable.

Thought for 6m 2s
IMEC2 Firmware Refactor Ultra Plan
Dead DWM3000 APIs + Behavior-Preserving main.c Decomposition

This fused plan merges the three developer plans into one implementation-grade roadmap. The guiding rule is: delete only proven-dead driver APIs; otherwise preserve firmware behavior exactly while splitting firmware/app/src/main.c into cohesive app-local modules.

The repository context is consistent across the supplied plans: this is a Zephyr/west firmware app for an nRF52833 + DWM3000 UWB/BLE system with clicker, anchor, and gateway roles selected at build time. The current main.c is around 13.5k lines and mixes role runtime, Zephyr glue, BLE, GPIO, mesh, reporting, survey, ML, high-debug, and boot/init logic. The uploaded plans also agree that new app-only headers should live under firmware/app/src/, not firmware/include/, and that shared state should initially be exposed through a single app_state.h/app_state.c pair for mechanical safety.

1. Non-Negotiable Refactor Principles
1.1 Behavior preservation comes before architecture cleanliness

This is not a rewrite. The first pass must be mostly cut-and-paste extraction with minimal renames. Preserve:

DWM3000 timing, sleep/wake, SPI speed transitions, retained configuration, standby/idle choices, and SYS_STATUS polling.
UWB scan windows, DS-TWR scheduling, discovery-slot timing, wake-claim behavior, wake-spam behavior, and post-burst diagnostics.
Protocol packet structs, TLV layouts, report payloads, sequence IDs, queue depths, workqueue timing, mesh route behavior, and gateway command semantics.
LED meanings, battery sampling behavior, clicker idle policy, system-off wake capture, retained system-on idle, and BLE courtesy behavior.
ML presets, high-debug staged presets, Stage 0–3 paths, gateway BLE connectivity-test paths, and any “unused-looking” Zephyr callback registered through macros.
1.2 Keep the app/protocol boundary clean

All new split files go under:

firmware/app/src/

Use matching private app headers in the same directory:

firmware/app/src/app_*.h

Do not move these headers into firmware/include/. That directory remains for public, portable protocol/state APIs. One uploaded plan explicitly calls out the existing convention: firmware/include/*.h for public protocol headers, firmware/src/*.c for hardware-independent modules with native tests, and firmware/app/src/ for Zephyr/HW glue.

1.3 Keep main.c as the composition root first

Some plans propose renaming residual main.c to app_main.c; the safer fused decision is:

Phase 1: keep firmware/app/src/main.c as the composition root to minimize CMake/preset churn.
Optional Phase 2: after all builds pass, rename residual main.c to app_main.c only if the team wants the name consistency.

Final residual main.c should contain only:

main(void)
boot banner ordering
role/preset selection
shared hardware init ordering
workqueue start/init glue
gateway_ble_connectivity_test_run if that preset still needs a direct alternate path
BUILD_ASSERT blocks that depend on timing macros
LOG_MODULE_REGISTER(uwb_app, ...)

Target size: 300–700 lines, not 13.5k.

2. Driver Cleanup Plan: Remove Only Confirmed-Dead Public APIs
2.1 Delete these APIs

Remove exactly these public APIs from both dwm3000_driver.c and dwm3000_driver.h:

int dwm3000_driver_initialise(bool idle_after_init);
int dwm3000_driver_listen_activity(uint32_t timeout_ms, bool *activity_detected);
void dwm3000_driver_stats_reset(void);

Rationale:

API	Why it can go
dwm3000_driver_initialise(bool idle_after_init)	Only appears in driver source/header. Real initialization path is dwm3000_driver_configure_default(), which calls the private initialise_radio(false).
dwm3000_driver_listen_activity(uint32_t timeout_ms, bool *activity_detected)	No firmware or documentation caller. Current RX paths use receive-frame, continuous receive, responder polling, or range initiator paths.
dwm3000_driver_stats_reset(void)	Diagnostics read stats through dwm3000_driver_stats_get; no path resets stats.
2.2 Do not delete these

Do not delete:

dwm3000_driver_receive_frame_detailed

It is live because it is used by dwm3000_driver_receive_frame and by firmware/uwb_smoke_test/src/main.c.

Do not delete private helpers such as:

initialise_radio
wake_configured_radio
restore_txrx_after_sleep
status polling helpers

These are part of the DWM3000 sleep/wake and IRQ-free polling behavior.

2.3 Driver cleanup verification

Run immediately after the deletion:

rg "dwm3000_driver_initialise|dwm3000_driver_listen_activity|dwm3000_driver_stats_reset" firmware Documentation

Expected result: no source/header declarations or call sites for the removed APIs.

Then build/test:

cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
3. Final Target Module Layout

This plan uses a moderate split: fewer files than the most granular proposal, but enough separation to make ownership clear. It borrows the uploaded plan’s shared-state strategy and module-ownership rules, while aligning with the pasted plan’s gateway/clicker/ML/mesh/anchor/high-debug grouping. The uploaded plans specifically recommend app_state.c/.h, app-local headers, module-private static helpers, and source ownership for Zephyr kernel objects.

3.1 Core configuration and shared state
File	Header	Ownership
app_config.h	header only	Role/build constants: ROLE_CLICKER, ROLE_ANCHOR, ROLE_GATEWAY, DEVICE_ROLE, DEVICE_ID, GATEWAY_ID, NETWORK_ID, build metadata defaults, timing-budget macros, devicetree alias helpers, *_UNUSED attribution macros.
app_state.c	app_state.h	Only truly cross-module state and cross-cutting utilities. Defines shared locks, flags, event counters, mesh runtime, session handles, and timing helpers that cannot yet be owned by one subsystem.

app_state.h is the one controlled escape hatch for globals. It may declare externs for items such as:

extern bool uwb_rf_active;
extern struct k_mutex uwb_rf_lock;
extern struct k_mutex anchor_uwb_lock;
extern bool anchor_uwb_busy;
extern bool mesh_uwb_rx_active;
extern bool anchor_heartbeat_enabled;
extern struct mesh_relay mesh_runtime;
extern struct mesh_event_stats mesh_event_stats;
extern struct uwb_session anchor_uwb_session;
extern uint32_t anchor_uwb_scan_interval_ms;
extern uint32_t next_event_seq;
extern uint32_t mesh_event_control_seq;

Also keep cross-cutting helpers here initially:

role_name
command_status_name
claim_decision_name
range_status_name
range_status_valid
mesh_id_is_unicast
uptime_deadline_reached
uptime_ms_until_deadline
packet_age_add
packet_age_add_elapsed
mesh_outbound_refresh_age
mesh_outbound_ready_for_tx
mesh_rx_pending_refresh_age
next_click_event_seq
nonzero_uptime_session_id
mesh_next_event_control_seq
average_i32_nearest
u32_saturating_add
ceil_us_to_ms
delay_ms_to_u16
uwb_schedule_burst_id
scheduled_range_sample_target_us
scheduled_post_burst_diag_target_us
scheduled_post_burst_diag_seq
mix64
clicker_priority_id
clicker_nonce
survey_sample_seq
local_uwb_short_addr
discovery_window_ms_for_slots
local_anchor_discovery_slot
local_survey_discovery_slot
sleep_until_ms
sleep_with_uwb_standby_until_ms
sleep_with_uwb_idle_until_ms
sleep_precise_us
sleep_until_us

My addition: add a top-of-file comment to app_state.h saying:

/*
 * Temporary mechanical-refactor shared state.
 * New code should not add globals here unless two or more modules truly require ownership.
 * Prefer moving state into the owning module after the first behavior-preserving split.
 */

That prevents app_state.h from becoming the new dumping ground.

3.2 Board and hardware glue
File	Header	Ownership
app_board.c	app_board.h	Status LEDs, LED disconnect, GPIO output helpers, battery ADC/divider, debug serial init/DTR checks.
app_radio_guard.c	app_radio_guard.h	radio_guard_uwb_start, radio_guard_uwb_stop, BLE quiet coordination, UWB RF active transitions.

The most granular uploaded plan splits LEDs, battery, radio guard, and power into separate files; the first uploaded plan groups LEDs/battery/debug serial under app_board.c. The fused plan uses app_board.c to avoid over-fragmentation, while keeping radio guard separate because it coordinates BLE quiet and UWB RF ownership.

3.3 Gateway BLE and host command path
File	Header	Ownership
app_gateway_ble.c	app_gateway_ble.h	BLE GATT service, advertising, notify path, RX frame queue, BLE log backend, connection callbacks, CCC callback, packet RX write callback, quiet-while-UWB behavior, gateway BLE connectivity-test helpers.
app_gateway_cmd.c	app_gateway_cmd.h	Host command framing/routing, command sequence allocation, pending command result wait, command result timeout, gateway local command delivery, survey-command routing hooks, command TLV parse helpers.

Why split gateway into two files instead of one giant app_gateway.c:

Zephyr BLE macro callbacks are special and should be visibly local in app_gateway_ble.c.
Command routing and survey command logic are protocol/workflow-heavy and easier to review separately.
Gateway BLE connectivity-test reduced builds can include app_gateway_ble.c without pulling in all command/survey orchestration unless needed.

Must stay in app_gateway_ble.c:

BT_GATT_SERVICE_DEFINE(...)
BT_CONN_CB_DEFINE(...)
BLE log backend callback structs/macros
GATT write callbacks
CCC callbacks
connection callbacks

The uploaded plan explicitly warns that Zephyr macro bodies such as BT_GATT_SERVICE_DEFINE, BT_CONN_CB_DEFINE, and GPIO callback registration must stay file-scope in their owning translation units, not headers.

3.4 Clicker button, power, and click runtime
File	Header	Ownership
app_clicker_button.c	app_clicker_button.h	Button GPIO setup, ISR, debounce/release work, button FSM glue, click action submission, system-off wake capture, idle interrupt arming.
app_clicker_power.c	app_clicker_power.h	CONFIG_IMEC_CLICKER_SYSTEMOFF_IDLE, retained system-on idle, RAM retention, DWM3000 pin parking before system-off, clicker_enter_idle, clicker_systemon_retained_idle_enabled.
app_clicker.c	app_clicker.h	Normal click path, UWB diagnostic click, continuous wake claims, self-test, wake-claim train, anchor discovery, range scheduling/release, scheduled ranging, politeness/contention/retry gates, BLE courtesy scan hooks.

Why not put all clicker behavior in one file? The pasted plan grouped click button, power, action worker, and idle together, but that module could become too large. The fused plan keeps button + wake capture, power/idle policy, and click runtime separate because those are different risk profiles:

Button ISR/debounce bugs are easy to isolate.
Power/idle changes are high risk and should review independently.
UWB click/ranging logic is timing-sensitive and should not be mixed with GPIO plumbing.

Preserve exactly:

CONFIG_IMEC_CLICKER_SYSTEMOFF_IDLE
CONFIG_IMEC_CLICKER_SYSTEMON_RETAINED_IDLE
CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS

Do not collapse #if/#else stubs. The uploaded plan warns that config-guarded duplicate definitions and stubs must keep the same guard walls during the split.

3.5 Mesh and reporting
File	Header	Ownership
app_mesh_report.c	app_mesh_report.h	Mesh RX/TX work handlers, route waiting TX, channel-9 event control, UWB RX scheduling, local gateway delivery of mesh reports, report TX queueing, range report builders, anchor status TLVs.

The uploaded plans split mesh and range reporting into app_mesh.c and app_range_report.c / app_report.c; the pasted plan suggests app_mesh_report.[ch] because these paths share queues, work items, and callbacks. The fused decision is:

Use one file initially, app_mesh_report.c, to avoid hidden queue ownership changes.
Add clear internal sections:
/* Mesh TX/RX */
/* Route waiting TX */
/* Channel-9 event control */
/* UWB RX window scheduling */
/* Report TX queue */
/* Range/report TLV builders */
If the file exceeds ~2.5k lines after extraction, split later into:
app_mesh.c
app_report.c

Must preserve:

mesh_rx_msgq
report_tx_msgq
one-frame-per-window RX behavior
mesh UWB RX timeout scheduling
route-discovery follow-up behavior
gateway local delivery semantics

The uploaded detailed plan assigns mesh_rx_msgq, mesh work items, report queue, and report work ownership to the mesh/report modules, with only truly shared kernel objects falling back to app_state.c.

3.6 Anchor runtime, survey, and UWB scan
File	Header	Ownership
app_anchor_uwb.c	app_anchor_uwb.h	Anchor UWB scan work, wake-claim handling, responder polling orchestration, scheduled UWB ranges, post-burst diagnostics, range-window accounting, DWM3000 standby/idle choices.
app_anchor_ops.c	app_anchor_ops.h	Heartbeat, reboot work, local command handling, LED/route/scan-duty commands, survey discovery, survey pair prepare/run/abort, gateway-side survey orchestration if tightly coupled.

Why split anchor this way:

Anchor UWB scan/ranging is DWM3000 timing-sensitive.
Heartbeat, reboot, command handling, and survey are workflow-heavy but less directly tied to radio wake/standby sequencing.
Survey may still become large; if app_anchor_ops.c exceeds ~2k–2.5k lines, split a later app_survey.c.

Must preserve:

DWM3000 standby/idle choices
anchor scan-window timing macros
anchor survey abort/state helpers
responder polling order
discovery-slot scheduling
heartbeat enable/disable behavior
gateway survey orchestration

The detailed uploaded plan specifically separates app_anchor_ops.c for heartbeat/reboot/commands/survey and app_anchor_uwb.c for scan, claim handling, scheduled ranges, diagnostics, and UWB range-window bookkeeping.

3.7 ML presets
File	Header	Ownership
app_ml.c	app_ml.h	ML clicker collection runtime, ML anchor battery LED sampling, ML anchor BLE debug-log startup, deterministic discovery slot behavior used by ML presets, post-burst diagnostic emission/buffering.

Preserve exactly:

CONFIG_IMEC_ML_CLICKER
CONFIG_IMEC_ML_ANCHOR
IMEC_ML_ANCHOR_SLOT
ml_clicker preset behavior
ml_anchor_1..8 deterministic anchor IDs/slots

The fused plan uses one app_ml.c initially because ML clicker and ML anchor paths are specialized and preset-gated. If it becomes awkward, split later into:

app_ml_clicker.c
app_ml_anchor.c

The uploaded plan lists ML clicker and ML anchor as possible separate modules, while the pasted plan prefers app_ml.[ch]; keeping one file initially reduces CMake complexity while preserving reachability.

3.8 High-debug and staged presets
File	Header	Ownership
app_high_debug.c	app_high_debug.h	High-debug log formatting, counters, boot banner helpers, serial command polling, bootloader request handling, staged LED tests, DWM3000 probes, simulated click, Stage 0/1 instrumentation.

Expose from app_high_debug.h only what other modules actually call:

high_debug_log_event
HIGH_DEBUG_COUNTER_INC
high_debug_dump_counters
high_debug_boot_banner
high_debug_request_bootloader
stage1_led_phase
stage1_led_result
stage1_click_diag
stage1_anchor_focused_note_*

Everything else stays static.

Important: high-debug is not dead code. It is reachable through staged presets and must be compiled in the relevant builds.

The detailed uploaded plan calls out high-debug as a large logging/counter/stage helper subsystem and notes that high_debug_log_event is called from many sites, so its header must be included by modules that log or count.

4. Final File List

Initial target:

firmware/app/src/
  main.c                         # residual composition root

  app_config.h

  app_state.c
  app_state.h

  app_board.c
  app_board.h

  app_radio_guard.c
  app_radio_guard.h

  app_gateway_ble.c
  app_gateway_ble.h

  app_gateway_cmd.c
  app_gateway_cmd.h

  app_clicker_button.c
  app_clicker_button.h

  app_clicker_power.c
  app_clicker_power.h

  app_clicker.c
  app_clicker.h

  app_mesh_report.c
  app_mesh_report.h

  app_anchor_uwb.c
  app_anchor_uwb.h

  app_anchor_ops.c
  app_anchor_ops.h

  app_ml.c
  app_ml.h

  app_high_debug.c
  app_high_debug.h

Optional later splits if files exceed reviewable size:

app_mesh_report.c  -> app_mesh.c + app_report.c
app_anchor_ops.c   -> app_anchor_ops.c + app_survey.c
app_ml.c           -> app_ml_clicker.c + app_ml_anchor.c
main.c             -> app_main.c
5. Public Interface Shape
5.1 Composition-root API

main.c should call only coarse init/start functions:

int app_board_init(void);
int app_radio_guard_init(void);

int app_gateway_ble_init(void);
int app_gateway_cmd_init(void);

int app_mesh_report_init(void);

int app_clicker_button_init(enum button_action *boot_action);
int app_clicker_power_pre_init(void);
void app_clicker_enter_idle(void);
void app_clicker_handle_button(enum button_action action);
int app_clicker_init(void);

int app_anchor_uwb_init(void);
int app_anchor_ops_init(void);

int app_ml_init(void);

int app_high_debug_init(void);
void app_high_debug_boot_banner(void);

A representative final main.c should look like this, not necessarily byte-for-byte:

int main(void)
{
    enum button_action boot_action = BUTTON_ACTION_NONE;
    int ret;

    app_clicker_power_pre_init();

    ret = app_board_init();
    if (ret) {
        return ret;
    }

    app_high_debug_boot_banner();

    ret = app_radio_guard_init();
    if (ret) {
        return ret;
    }

    ret = app_mesh_report_init();
    if (ret) {
        return ret;
    }

#if defined(CONFIG_IMEC_GATEWAY_BLE)
    ret = app_gateway_ble_init();
    if (ret) {
        return ret;
    }
#endif

    if (DEVICE_ROLE == ROLE_CLICKER) {
        app_clicker_button_init(&boot_action);
        app_clicker_init();
        app_ml_init();

        if (boot_action != BUTTON_ACTION_NONE) {
            app_clicker_handle_button(boot_action);
        } else {
            app_clicker_enter_idle();
        }
    } else if (DEVICE_ROLE == ROLE_ANCHOR) {
        app_anchor_uwb_init();
        app_anchor_ops_init();
        app_ml_init();
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        app_anchor_uwb_init();
        app_anchor_ops_init();
        app_gateway_cmd_init();
    }

    return 0;
}

The uploaded plan’s target for app_main.c is similar: only init sequence, role branch, button boot action dispatch, and idle entry should remain; no subsystem internals should remain in the composition root.

5.2 Header rules

Use these rules aggressively:

Headers expose only functions needed by other modules.
Do not expose helpers just because they were non-static during the move.
Keep module-private helpers static.
Do not define objects in headers.
Do not put LOG_MODULE_REGISTER in headers.
Do not put Zephyr callback macro bodies in headers.
Use app_config.h for shared compile-time constants and *_UNUSED attributes.
Use app_state.h only for temporary cross-module state and cross-cutting helpers.
6. CMake Strategy
6.1 Normal/preset builds

Update firmware/app/CMakeLists.txt so normal builds include:

target_sources(app PRIVATE
    src/main.c
    src/app_state.c
    src/app_board.c
    src/app_radio_guard.c
    src/app_gateway_ble.c
    src/app_gateway_cmd.c
    src/app_clicker_button.c
    src/app_clicker_power.c
    src/app_clicker.c
    src/app_mesh_report.c
    src/app_anchor_uwb.c
    src/app_anchor_ops.c
    src/app_ml.c
    src/app_high_debug.c

    src/dwm3000_driver.c
    src/dwm3000_port.c
    src/dwm3000_sdk_port.c
    "${DWM3000_SDK_DIR}/decadriver/deca_device.c"

    ../src/debug_log.c
    ../src/protocol.c
    ../src/gateway_command.c
    ../src/uwb.c
    ../src/uwb_ble_courtesy.c
    ../src/uwb_session.c
    ../src/mesh.c
    ../src/mesh_relay.c
    ../src/report.c
    ../src/serial_frame.c
    ../src/status.c
    ../src/route.c
    ../src/survey.c
)

A more conditional source list is possible, but the safer first pass is to include the app modules broadly and let their existing #if defined(CONFIG_IMEC_*) guards compile stubs or empty sections. One uploaded plan notes that some files can be empty-stubbed or omitted by role, while also warning that app_ble_gateway.c is needed by gateway and ML variants.

6.2 Gateway BLE connectivity-test build

The reduced gateway_ble_connectivity_test source set is a known trap. The fused decision:

Include main.c, app_config.h, app_state.c, app_board.c, app_radio_guard.c, app_gateway_ble.c, and any direct dependency needed by BLE logging/framing.
Prefer including all new app .c files if they compile cleanly under guards.
Do not leave this branch with only src/main.c after gateway BLE extraction; otherwise the test preset will fail when BLE callbacks live in app_gateway_ble.c.

The uploaded detailed plan specifically flags the connectivity-test CMake branch and suggests that listing all new files may be safer because many are config-gated.

7. Extraction Order

Use small, buildable commits. Each step should compile before moving to the next.

Step 0 — Baseline

Before changes:

cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure

Then run role/preset builds listed in Section 8.

Capture:

git status
git diff --check
rg "dwm3000_driver_initialise|dwm3000_driver_listen_activity|dwm3000_driver_stats_reset" firmware Documentation
Step 1 — Remove dead DWM3000 APIs

Delete only:

dwm3000_driver_initialise
dwm3000_driver_listen_activity
dwm3000_driver_stats_reset

Run the reference scan and standard builds.

Step 2 — Add skeletal module framework

Add empty/minimal files and headers:

app_config.h
app_state.c/h
app_board.c/h
app_radio_guard.c/h
app_gateway_ble.c/h
app_gateway_cmd.c/h
app_clicker_button.c/h
app_clicker_power.c/h
app_clicker.c/h
app_mesh_report.c/h
app_anchor_uwb.c/h
app_anchor_ops.c/h
app_ml.c/h
app_high_debug.c/h

Update CMake for normal and gateway BLE connectivity-test source sets.

At this point, most functions still live in main.c.

Step 3 — Extract app_config and app_state

Move shared constants, role/build macros, *_UNUSED attributes, and the truly cross-module state.

Keep BUILD_ASSERT blocks in main.c for now, but make sure they include app_config.h.

Run all three role builds.

Step 4 — Extract board and radio guard

Move:

LEDs
battery ADC/divider
debug serial
radio guard
BLE quiet coordination hooks

Build all roles.

Step 5 — Extract gateway BLE first

Move:

BLE GATT service
advertising
notifications
RX/TX frame transport
BLE log backend
connection callbacks
CCC and GATT write callbacks
gateway BLE connectivity-test helpers

Reason: these functions are easy to misclassify as unused because Zephyr macros reference them indirectly. Building after this step proves the macro registration survived.

Build:

.venv/bin/west build ... -DFIRMWARE_ROLE=gateway
.venv/bin/west build ... -DIMEC_BUILD_PRESET=gateway_ble_connectivity_test
.venv/bin/west build ... -DIMEC_BUILD_PRESET=ml_clicker
Step 6 — Extract clicker button and power

Move button and low-power code before moving the big click runtime:

Button GPIO setup
ISR/work handlers
button FSM glue
system-off wake capture
retained system-on idle
clicker idle entry
DWM3000 pin parking before system-off

Build clicker and wake-spam/stage presets.

Step 7 — Extract high-debug

Move:

high-debug log bus
counters
boot banner helpers
CDC command parser
bootloader request
staged LED tests
DWM3000 probes
simulated click
Stage 0/1 instrumentation

Build at least:

-DIMEC_BUILD_PRESET=tag_stage0_highdebug
-DIMEC_BUILD_PRESET=anchor_stage1_highdebug
-DIMEC_BUILD_PRESET=tag_stage1_wake_spam
Step 8 — Extract mesh/report

Move:

mesh RX/TX handlers
report TX queueing
route waiting TX
channel-9 event timing
UWB RX scheduling
gateway local report delivery
range/report TLV builders

Build all three roles.

Step 9 — Extract gateway command path

Move:

host packet framing
command routing
pending command result tracking
timeout work
survey command routing hooks
command TLV parse helpers

Build gateway, anchor, and ML clicker.

Step 10 — Extract anchor UWB

Move:

anchor scan work
wake-claim handling
responder polling orchestration
scheduled ranges
post-burst diagnostics
UWB window accounting
DWM3000 standby/idle transitions

Build anchor and gateway.

Step 11 — Extract anchor ops/survey

Move:

heartbeat
reboot
local command handling
LED/route/scan-duty commands
survey discovery
survey pair prepare/run/abort
gateway survey orchestration

Build anchor and gateway.

Step 12 — Extract clicker runtime

Move:

normal click
diagnostic click
continuous wake claims
self-test
anchor discovery
range schedule/release
scheduled ranging
politeness/contention/retry gates
BLE courtesy scan hooks

Build clicker, ML clicker, and high-debug clicker preset.

Step 13 — Extract ML

Move:

ML clicker collection
ML BLE host frame handling
anchor cache
deterministic ML discovery slots
ML anchor battery LED sampling
ML anchor BLE debug-log startup
post-burst diagnostic buffering/emission

Build:

-DIMEC_BUILD_PRESET=ml_clicker
-DIMEC_BUILD_PRESET=ml_anchor_1
-DIMEC_BUILD_PRESET=ml_anchor_2
-DIMEC_BUILD_PRESET=ml_anchor_8
Step 14 — Final main.c cleanup

Delete the old forward-declaration block once each module header exposes the needed prototypes. The uploaded plan explicitly calls out deleting the current forward-declaration block after module headers take over.

Then verify main.c contains no subsystem internals.

8. Test Matrix

Run before changes, after driver cleanup, and after each extraction batch.

Native/library build
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
Standard Zephyr roles
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker -- -DFIRMWARE_ROLE=clicker

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor -- -DFIRMWARE_ROLE=anchor

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway -- -DFIRMWARE_ROLE=gateway
ML presets
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-clicker -- -DIMEC_BUILD_PRESET=ml_clicker

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-1 -- -DIMEC_BUILD_PRESET=ml_anchor_1

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-2 -- -DIMEC_BUILD_PRESET=ml_anchor_2
Stage/high-debug/wake-spam presets
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/tag-stage1-wake-spam -- -DIMEC_BUILD_PRESET=tag_stage1_wake_spam

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/tag-stage0-highdebug -- -DIMEC_BUILD_PRESET=tag_stage0_highdebug

.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/anchor-stage1-highdebug -- -DIMEC_BUILD_PRESET=anchor_stage1_highdebug
Gateway BLE connectivity test
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/gateway-ble-connectivity-test -- -DIMEC_BUILD_PRESET=gateway_ble_connectivity_test
Hygiene
git diff --check

rg "dwm3000_driver_initialise|dwm3000_driver_listen_activity|dwm3000_driver_stats_reset" firmware Documentation

The uploaded plans agree that role builds are the primary correctness check for app-code refactors, with native tests still useful to catch include drift.

9. Review Checklist

A reviewer should verify:

main.c is only composition/init/dispatch.
Every moved function from original main.c appears in exactly one new file.
No DWM3000 timing, standby, idle, wake, SPI, retained-config, or polling behavior changed.
No protocol structs, TLV layouts, packet formats, queue depths, sequence allocation, or mesh route behavior changed.
Zephyr macro callbacks remain file-scope in their owning .c files.
LOG_MODULE_REGISTER appears only in .c files, never headers.
BUILD_ASSERT blocks remain in a translation unit that includes all required timing macros.
*_UNUSED attribution macros are visible in config-gated modules.
Shared globals are defined once.
Module-private state stays static.
K_MSGQ_DEFINE, K_THREAD_STACK_DEFINE, and work objects live in the owning module unless truly shared.
Gateway BLE connectivity-test still builds despite the reduced CMake source set.
ML presets still build and preserve deterministic anchor IDs/slots.
High-debug staged presets still build and preserve serial command/counter behavior.
git diff --check is clean.
Removed DWM3000 public API names are gone.

The uploaded review checklists emphasize exactly these points: no logic left in app_main.c/residual main.c, Zephyr macro bodies remaining file-local, shared globals centralized, correct CMake role conditionals, and no accidental role leakage.

10. Risk Register and Mitigations
Risk	Mitigation
Hidden Zephyr macro references look unused	Extract gateway BLE and button ISR modules early; build immediately.
Config-gated real/stub pairs accidentally collapsed	Preserve #if/#else walls exactly in first pass. No “cleanup” while moving.
Shared state explodes	app_state.h is temporary and restricted to truly cross-module state. Add comment discouraging new globals.
Role leakage	Build clicker/anchor/gateway after each step. Keep role-specific code behind existing guards.
Gateway BLE connectivity-test breaks	Include required split files in that reduced source set from the start.
ML/high-debug paths get dropped	Treat ML and high-debug as live presets; include them in the test matrix.
Linker/function ordering changes obscure behavior review	Expect symbol-address changes; compare key function disassembly if needed rather than requiring byte-identical firmware.
Anchor survey grows too large	Split app_anchor_ops.c into app_survey.c only after first behavior-preserving extraction passes.
Hardware-independent helpers remain in app layer	Defer moving TLV/timing/math helpers into firmware/src/ with native tests until a follow-up.
11. Out of Scope for This Refactor

Do not do these in the first pass:

Do not redesign protocol packets or TLVs.
Do not rename public enums/macros in firmware/include/.
Do not convert all globals to accessors yet.
Do not move hardware-independent helpers into firmware/src/ yet.
Do not alter power policy, scan duty defaults, timing budgets, BLE courtesy policy, or DWM3000 sleep/wake behavior.
Do not delete ML, high-debug, Stage 0–3, wake-spam, or connectivity-test paths.

Follow-up candidates after the split is stable:

1. Move TLV parse helpers into firmware/src/ with native tests.
2. Move timing-budget calculators into firmware/src/ with native tests.
3. Replace app_state externs with owner APIs.
4. Split oversized modules after measuring actual extracted line counts.
5. Optionally rename residual main.c to app_main.c.
12. The “Ultra” Implementation Contract

A patch series following this plan should be structured like this:

01-dwm3000-remove-dead-public-apis
02-add-app-module-skeletons-and-cmake
03-extract-app-config-and-state
04-extract-board-and-radio-guard
05-extract-gateway-ble
06-extract-clicker-button-and-power
07-extract-high-debug
08-extract-mesh-report
09-extract-gateway-command
10-extract-anchor-uwb
11-extract-anchor-ops-survey
12-extract-clicker-runtime
13-extract-ml
14-shrink-main-and-delete-forward-decls
15-final-build-matrix-and-rg-cleanup

Each commit should be reviewable on its own. The rule for every extraction commit is:

Move code. Do not improve code. Do not rename unless visibility requires it. Do not change guards. Do not change behavior.

That gives you the safest path from “13.5k-line monolith” to an app with clear ownership, without losing the specialized but live presets that make this firmware tricky.
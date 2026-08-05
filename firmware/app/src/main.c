#include "app_anchor.h"
#include "app_board.h"
#include "app_click_event_sequence.h"
#include "app_clicker.h"
#include "app_config.h"
#include "app_device_identity.h"
#include "app_gateway_ble.h"
#include "app_high_debug.h"
#include "app_ml.h"
#include "app_mesh_report.h"
#include "app_mesh_direct_probe_diag.h"
#include "app_mesh_test.h"
#include "app_node_comm.h"
#include "app_state.h"
#include "app_stack_diag.h"
#include "app_wake_train_politeness.h"
#include "app_watchdog.h"
#include "dwm3000_driver.h"
#include "dwm3000_port.h"
#include "gateway_command.h"
#include "route.h"
#include "serial_frame.h"
#include "survey.h"
#include "survey_gateway_transaction.h"
#include "uwb_ble_courtesy.h"

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(uwb_app, LOG_LEVEL_DBG);

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST) || \
    defined(CONFIG_IMEC_STACK_STRESS_DIAGNOSTICS)
#define IMEC_RETAIN_FATAL_BREADCRUMB 1
#else
#define IMEC_RETAIN_FATAL_BREADCRUMB 0
#endif

#if IMEC_RETAIN_FATAL_BREADCRUMB
#define MESH_FATAL_BREADCRUMB_MAGIC UINT32_C(0x4641544c)
volatile uint32_t mesh_route_test_fatal_magic __attribute__((section(".noinit")));
volatile uint32_t mesh_route_test_fatal_count __attribute__((section(".noinit")));
volatile uint32_t mesh_route_test_fatal_reason __attribute__((section(".noinit")));
volatile uint32_t mesh_route_test_fatal_pc __attribute__((section(".noinit")));
volatile uint32_t mesh_route_test_fatal_lr __attribute__((section(".noinit")));
volatile uint32_t mesh_route_test_fatal_thread __attribute__((section(".noinit")));
volatile uint32_t mesh_route_test_fatal_stack_start __attribute__((section(".noinit")));
volatile uint32_t mesh_route_test_fatal_stack_size __attribute__((section(".noinit")));

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    k_tid_t thread = k_current_get();

    if (mesh_route_test_fatal_magic != MESH_FATAL_BREADCRUMB_MAGIC) {
        mesh_route_test_fatal_count = 0u;
    }
    mesh_route_test_fatal_magic = MESH_FATAL_BREADCRUMB_MAGIC;
    if (mesh_route_test_fatal_count < UINT32_MAX) {
        mesh_route_test_fatal_count++;
    }
    mesh_route_test_fatal_reason = reason;
    mesh_route_test_fatal_thread = (uint32_t)(uintptr_t)thread;
    if (thread != NULL) {
        mesh_route_test_fatal_stack_start =
            (uint32_t)(uintptr_t)thread->stack_info.start;
        mesh_route_test_fatal_stack_size = (uint32_t)thread->stack_info.size;
    } else {
        mesh_route_test_fatal_stack_start = 0u;
        mesh_route_test_fatal_stack_size = 0u;
    }
    if (esf != NULL) {
        mesh_route_test_fatal_pc = esf->basic.pc;
        mesh_route_test_fatal_lr = esf->basic.lr;
    } else {
        mesh_route_test_fatal_pc = 0u;
        mesh_route_test_fatal_lr = 0u;
    }

    sys_reboot(SYS_REBOOT_COLD);
    for (;;) {
        k_cpu_idle();
    }
}
#endif

#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS)
static const struct app_clicker_wake_train_config clicker_wake_train_config = {
    .wake_adv_ms = WAKE_ADV_MS,
    .post_wake_claimed_duration_ms = UWB_POST_WAKE_CLAIMED_DURATION_MS,
    .control_tx_timeout_ms = UWB_CONTROL_TX_TIMEOUT_MS,
};
#endif

#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
#define STAGE1_CLICK_SPAM_SUCCESS_DELAY_MS 1000u
#define STAGE1_CLICK_SPAM_FAILURE_DELAY_MS 250u
static const struct app_clicker_continuous_click_sessions_config clicker_session_spam_config = {
    .success_delay_ms = STAGE1_CLICK_SPAM_SUCCESS_DELAY_MS,
    .failure_delay_ms = STAGE1_CLICK_SPAM_FAILURE_DELAY_MS,
};
BUILD_ASSERT(STAGE1_CLICK_SPAM_SUCCESS_DELAY_MS > 0u,
             "Stage 1 click-session spam must leave a bounded delay after success");
BUILD_ASSERT(STAGE1_CLICK_SPAM_FAILURE_DELAY_MS > 0u,
             "Stage 1 click-session spam must leave a bounded delay after failure");

static void stage1_click_spam_boot_marker(const char *phase)
{
    high_debug_log_event("STAGE1_CLICK_SPAM_BOOT", "phase=%s", phase);
}
#endif

BUILD_ASSERT(ANCHOR_UWB_SCAN_RX_MS * 1000u >= ANCHOR_UWB_SCAN_RX_US,
             "anchor scan millisecond timeout must cover configured RX microseconds");
BUILD_ASSERT(ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS >=
             ANCHOR_UWB_SCAN_ACTIVITY_MIN_COMPLETION_MS,
             "anchor scan activity extension must cover a clipped wake packet");
#if DEVICE_ROLE == ROLE_ANCHOR && \
    !IS_ENABLED(CONFIG_IMEC_ML_ANCHOR) && \
    !IS_ENABLED(CONFIG_IMEC_STAGE1_ANCHOR_CONTINUOUS_SCAN) && \
    !IS_ENABLED(CONFIG_IMEC_STAGE1_ANCHOR_SCAN_ALLOW_OVER_RX_BUDGET)
BUILD_ASSERT(ANCHOR_UWB_SCAN_INTERVAL_MS >= ANCHOR_UWB_SCAN_MIN_INTERVAL_MS,
             "anchor wake scan interval must keep channel-5 idle scan inside the calibrated RX budget");
BUILD_ASSERT(ANCHOR_UWB_SCAN_RX_US_PER_S <= ANCHOR_UWB_IDLE_RX_BUDGET_US_PER_S,
             "anchor periodic channel-5 UWB scan must stay inside the calibrated RX budget");
BUILD_ASSERT(ANCHOR_UWB_SCAN_MIN_INTERVAL_MS <= ANCHOR_UWB_SCAN_MAX_INTERVAL_MS,
             "anchor scan duty command range must satisfy duty and wake overlap limits");
BUILD_ASSERT(((uint64_t)ANCHOR_UWB_SCAN_MAX_INTERVAL_MS * 1000ull) +
             ANCHOR_UWB_STARTUP_US + ANCHOR_UWB_PLL_US <
             ((uint64_t)WAKE_ADV_MS * 1000ull),
             "maximum anchor scan duty command interval must preserve wake overlap");
#endif
#if !IS_ENABLED(CONFIG_IMEC_STAGE1_ANCHOR_CONTINUOUS_SCAN)
BUILD_ASSERT(WAKE_ADV_MS * 1000u > ANCHOR_UWB_IDLE_SCAN_RX_OFF_GAP_US,
             "clicker wake train must cover the anchor RX-off gap");
BUILD_ASSERT(WAKE_ADV_MS * 1000u > ANCHOR_UWB_IDLE_SCAN_AWAKE_US,
             "clicker wake train must exceed one anchor scan awake window");
#endif
BUILD_ASSERT(WAKE_ADV_MS <= UWB_WAKE_CLAIM_MAX_WAKE_TRAIN_MS,
             "wake claim timing bounds must cover the configured clicker wake train");
BUILD_ASSERT(MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS >=
             UWB_WAKE_CLAIM_MAX_WAKE_TRAIN_MS +
             MESH_ROUTE_TEST_REPLY_WINDOW_GUARD_MS,
             "control follow-up RX must cover wake train and TX transition");
BUILD_ASSERT(UWB_CLICKER_CLAIMED_DURATION_MS <=
             UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS,
             "wake claim timing bounds must cover the configured advertised click epoch");
BUILD_ASSERT(UWB_POLITE_RELEVANT_FRAME_WAIT_MS <=
             UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS,
             "decoded UWB politeness wait fallback must stay bounded");
BUILD_ASSERT(APP_WAKE_TRAIN_POLITE_SNIFF_MS == 20u,
             "wake trains must sniff channel 5 for 20 ms before and after TX");
BUILD_ASSERT(APP_WAKE_TRAIN_POLITE_BACKOFF_MIN_MS == 200u &&
             APP_WAKE_TRAIN_POLITE_BACKOFF_MAX_MS == 2000u,
             "wake train C5 activity backoff must stay within 200-2000 ms");
BUILD_ASSERT(APP_WAKE_TRAIN_POLITE_MAX_RETRIES > 0u,
             "wake train C5 activity guard must have at least one retry");
BUILD_ASSERT(UWB_POLITE_REQUIRED_QUIET_SAMPLES == 2u,
             "clicker politeness requires two consecutive quiet UWB samples");
BUILD_ASSERT(UWB_POLITE_REQUIRED_QUIET_SAMPLES * UWB_POLITE_SAMPLE_RX_MS >= 100u,
             "clicker politeness must require at least 100 ms of quiet channel 5");
BUILD_ASSERT((UWB_POLITE_REQUIRED_QUIET_SAMPLES * UWB_POLITE_SAMPLE_RX_MS) <=
             BLE_COURTESY_MIN_WINDOW_MS,
             "BLE courtesy must remain active through the required UWB quiet window");
BUILD_ASSERT(FLOOD_RELAY_REPEAT_COUNT == 4u,
             "channel-5 floods must use exactly four transmission opportunities");
BUILD_ASSERT(C5_POLITE_SNIFF_MS == 20u,
             "each channel-5 flood opportunity must use a 20 ms quiet check");
BUILD_ASSERT(UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN + 2u <= 31u,
             "BLE courtesy must fit in one legacy advertising data structure");
BUILD_ASSERT(BLE_COURTESY_PEER_FINISH_MS <= UWB_BLE_COURTESY_MAX_DURATION_MS,
             "BLE courtesy peer finish duration must fit in the advertised field");
BUILD_ASSERT(BLE_COURTESY_SCAN_WINDOW_UNITS <= BLE_COURTESY_SCAN_INTERVAL_UNITS,
             "BLE courtesy scan window must fit inside scan interval");
BUILD_ASSERT(CLICK_UWB_TIMEOUT_MS + UWB_SCHEDULE_GUARD_MS <= UWB_ANCHOR_RANGE_WINDOW_MS,
             "failed clicker DS-TWR response waits must fit inside one scheduled anchor slot");
BUILD_ASSERT(GATEWAY_COMMAND_RESULT_TIMEOUT_MS >=
             (UWB_MESH_ANCHOR_RX_INTERVAL_MS + ROUTE_GATEWAY_ACK_TIMEOUT_MS +
              GATEWAY_COMMAND_MESH_TIMEOUT_MARGIN_MS),
             "gateway command timeout must cover anchor UWB mesh RX cadence and ACK");
BUILD_ASSERT(UWB_RANGE_FIRST_POLL_DELAY_MS > UWB_SCHEDULE_GUARD_MS,
             "first scheduled poll must leave room for the anchor schedule guard");
BUILD_ASSERT(SERIAL_FRAME_MAX_LEN <= UINT16_MAX,
             "gateway BLE frame queue length field must hold a full COBS frame");
BUILD_ASSERT(GATEWAY_BLE_RX_FRAME_QUEUE_DEPTH >= 2u,
             "gateway BLE RX queue must absorb at least one pending and one arriving frame");
BUILD_ASSERT(GATEWAY_BLE_TX_RETRY_MAX_MS >= GATEWAY_BLE_TX_RETRY_MS,
             "gateway BLE notification retry cap must cover the base delay");
BUILD_ASSERT(SURVEY_DISCOVERY_SLOT_MS >= SURVEY_DISCOVERY_MIN_SLOT_MS,
             "survey discovery slots must fit the physical probe envelope");
BUILD_ASSERT(UWB_DISCOVERY_SLOT_COUNT <= SURVEY_DISCOVERY_MAX_SLOT_COUNT,
             "survey discovery slot helper must cover the UWB slot count");
BUILD_ASSERT(MAX_SCHEDULED_ANCHORS > 0u &&
             MAX_SCHEDULED_ANCHORS <= UWB_DISCOVERY_SLOT_COUNT,
             "scheduled anchor count must fit UWB discovery slot field");
BUILD_ASSERT(UWB_RANGE_SCHEDULE_MAX_LEN <= 127u,
             "range schedule must fit one standard UWB frame");
BUILD_ASSERT((UWB_RANGE_SCHEDULE_MAX_ANCHORS +
              UWB_RANGING_REQUESTS_MAX_PER_ANCHOR - 1u) <= UINT8_MAX,
             "scheduled DS-TWR sequence numbers must fit one byte");
BUILD_ASSERT(UWB_ML_MAX_SCHEDULED_EXCHANGES <= UINT16_MAX,
             "ML scheduled sample count must fit BLE sample-count TLV");
BUILD_ASSERT(SURVEY_DISCOVERY_DEFAULT_SLOT_COUNT > 0u &&
             SURVEY_DISCOVERY_DEFAULT_SLOT_COUNT <= SURVEY_DISCOVERY_MAX_SLOT_COUNT,
             "default survey discovery slots must fit survey TLV limits");
BUILD_ASSERT(SURVEY_RESULT_MESH_SLOT_MS > ROUTE_GATEWAY_ACK_TIMEOUT_MS,
             "survey result mesh slots must leave room for one tracked TX ACK wait");
BUILD_ASSERT(SURVEY_GATEWAY_RESPONSE_ACK_SETTLE_MS >=
             (NODE_COMM_PROTOCOL_RESPONSE_RETRY_BACKOFF_MAX_MS +
              APP_MESH_DIRECT_GATEWAY_ACK_RX_MS +
              APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS +
              APP_MESH_DIRECT_GATEWAY_SURVEY_SERVICE_GUARD_MS),
             "survey response settle must cover the maximum response retry and ACK window");
#if IMEC_HIGH_DEBUG_ANCHOR_SLOT_ENABLED
BUILD_ASSERT(IMEC_HIGH_DEBUG_ANCHOR_SLOT < UWB_DISCOVERY_SLOT_COUNT,
             "flashed high-debug anchor slot must fit the UWB discovery slot field");
#endif
BUILD_ASSERT(UWB_ANCHOR_PAIR_SCHEDULE_MAX_LEN <= UWB_RANGE_SCHEDULE_MAX_LEN,
             "anchor-pair schedule must fit the anchor schedule RX buffer");
#if defined(CONFIG_IMEC_ML_CLICKER)
BUILD_ASSERT(CONFIG_IMEC_ML_DEFAULT_SAMPLES_PER_ANCHOR <=
             UWB_RANGING_REQUESTS_MAX_PER_ANCHOR,
             "ML default samples per anchor must fit the UWB schedule field");
BUILD_ASSERT(CONFIG_IMEC_ML_MAX_ANCHORS <= UWB_RANGE_SCHEDULE_MAX_ANCHORS,
             "ML selected anchors must fit the production range schedule frame");
BUILD_ASSERT(CONFIG_IMEC_ML_DISCOVERY_SLOT_COUNT <= UWB_RANGE_SCHEDULE_MAX_ANCHORS,
             "ML discovery slot count must fit the selected-anchor schedule");
BUILD_ASSERT(CONFIG_IMEC_ML_DISCOVERY_SLOT_COUNT >=
                 SURVEY_ML_ANCHOR_PAIR_MIN_DISCOVERY_SLOT_COUNT &&
             CONFIG_IMEC_ML_DISCOVERY_SLOT_COUNT <=
                 SURVEY_ML_ANCHOR_PAIR_MAX_DISCOVERY_SLOT_COUNT,
             "ML anchor-pair survey default discovery slots must fit the 0x8002 contract");
BUILD_ASSERT(UWB_ML_EXCHANGE_STRIDE_US >= UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US,
             "ML exchange stride must satisfy range schedule validation");
BUILD_ASSERT(ML_CLICKER_COLLECTION_DEADLINE_MS > UWB_ML_MAX_BURST_MS,
             "ML collection deadline must cover the largest scheduled burst");
BUILD_ASSERT(ML_CLICKER_COLLECTION_DEADLINE_MS + 2500u <
                 APP_WATCHDOG_PROGRESS_LEASE_MS,
             "ML click action must finish before its watchdog lease expires");
BUILD_ASSERT(UWB_ANCHOR_PAIR_SURVEY_MAX_PAIRS >=
             ((UWB_ANCHOR_PAIR_SCHEDULE_MAX_ANCHORS *
               (UWB_ANCHOR_PAIR_SCHEDULE_MAX_ANCHORS - 1u)) / 2u),
             "ML anchor-pair result storage must cover every scheduled pair");
#endif
BUILD_ASSERT(CLICK_REPORT_DEADLINE_MS + 2500u <
                 APP_WATCHDOG_PROGRESS_LEASE_MS,
             "normal click action must finish before its watchdog lease expires");
#if defined(CONFIG_IMEC_HIGH_DEBUG)
BUILD_ASSERT(!(IS_ENABLED(CONFIG_IMEC_ROLE_TAG) || IS_ENABLED(CONFIG_IMEC_ROLE_CLICKER)) ||
             DEVICE_ROLE == ROLE_CLICKER,
             "tag/clicker high-debug role config must match DEVICE_ROLE");
BUILD_ASSERT(!IS_ENABLED(CONFIG_IMEC_ROLE_ANCHOR) || DEVICE_ROLE == ROLE_ANCHOR,
             "anchor high-debug role config must match DEVICE_ROLE");
BUILD_ASSERT(!IS_ENABLED(CONFIG_IMEC_ROLE_GATEWAY) || DEVICE_ROLE == ROLE_GATEWAY,
             "gateway high-debug role config must match DEVICE_ROLE");
#endif
#if defined(CONFIG_IMEC_HIGH_DEBUG)
static bool local_command_poll_enabled(void)
{
    return false;
}

#endif

static void watchdog_init_fail_closed(int error)
{
    printk("fatal: hardware watchdog initialization failed: %d; rebooting in %u ms\n",
           error,
           APP_WATCHDOG_INIT_RETRY_DELAY_MS);
    app_watchdog_stop_feeding();
    k_msleep(APP_WATCHDOG_INIT_RETRY_DELAY_MS);
    sys_reboot(SYS_REBOOT_COLD);
    for (;;) {
        k_cpu_idle();
    }
}

static void runtime_start_fail_closed(const char *phase, int error)
{
    printk("fatal: %s failed: %d; rebooting in %u ms\n",
           phase == NULL ? "runtime startup" : phase,
           error,
           APP_WATCHDOG_INIT_RETRY_DELAY_MS);
    app_watchdog_stop_feeding();
    k_msleep(APP_WATCHDOG_INIT_RETRY_DELAY_MS);
    sys_reboot(SYS_REBOOT_COLD);
    for (;;) {
        k_cpu_idle();
    }
}

int main(void)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    const struct app_high_debug_callbacks high_debug_callbacks = {
        .command_poll_enabled = local_command_poll_enabled,
        .handle_command = high_debug_handle_command,
    };
#endif
#if !defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS) || \
    defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    const struct app_clicker_callbacks clicker_callbacks = {
        .early_led = high_debug_clicker_early_led,
#if defined(CONFIG_IMEC_HIGH_DEBUG)
        .run_stage0_simulated_click = high_debug_stage0_simulated_click,
#endif
#if defined(CONFIG_IMEC_HIGH_DEBUG)
        .run_stage0_hardware_self_test = high_debug_stage0_hardware_self_test,
#endif
#if !defined(CONFIG_IMEC_ML_CLICKER) && \
    !defined(CONFIG_IMEC_ML_ANCHOR)
        .send_mesh_outbound = app_node_comm_send,
#endif
#if defined(CONFIG_IMEC_ML_CLICKER)
        .ml_discovery_slot_count_override = ml_clicker_discovery_slot_count_override,
        .ml_cache_note_discovery_reply = ml_clicker_cache_note_discovery_reply,
        .ml_seed_cached_anchors = ml_clicker_seed_cached_anchors,
        .ml_note_cached_discovery_used = ml_clicker_note_cached_discovery_used,
        .ml_relax_range_schedule = ml_clicker_relax_range_schedule,
        .ml_runtime_active = ml_clicker_runtime_active,
        .ml_emit_range_sample_if_active = ml_clicker_emit_range_sample_if_active,
        .ml_continue_after_range_start_failure = ml_clicker_continue_after_range_start_failure,
        .ml_should_continue_ranging = ml_clicker_should_continue_ranging,
        .ml_enter_range_quiet = ml_clicker_enter_range_quiet,
        .ml_exit_range_quiet = ml_clicker_exit_range_quiet,
        .ml_run_post_burst_diagnostics = ml_clicker_run_post_burst_diagnostics,
#endif
    };
#endif
#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    enum button_action boot_button_action __attribute__((unused)) = BUTTON_ACTION_NONE;
#else
    enum button_action boot_button_action STAGE1_WAKE_SPAM_UNUSED = BUTTON_ACTION_NONE;
#endif
    int ret;
    int battery_adc_ret;
#if IMEC_RETAIN_FATAL_BREADCRUMB
    bool fatal_recovery_boot = false;
#endif

#if IMEC_USE_HARDWARE_ANCHOR_ID
    ret = app_device_identity_init();
    if (ret < 0) {
        printk("fatal: invalid nRF FICR device identity: %d\n", ret);
        k_panic();
    }
    printk("mesh anchor identity: ficr=0x%016llx node=0x%016llx preset=%s\n",
           (unsigned long long)app_device_hardware_id(),
           (unsigned long long)DEVICE_ID,
           IMEC_BUILD_PRESET_NAME);
#endif

#if IMEC_RETAIN_FATAL_BREADCRUMB
    if (mesh_route_test_fatal_magic == MESH_FATAL_BREADCRUMB_MAGIC) {
        uint32_t recovery_delay_ms = mesh_route_test_fatal_count >= 6u ?
                                     30000u :
                                     mesh_route_test_fatal_count * 5000u;

        fatal_recovery_boot = true;
        printk("retained fatal: count=%u reason=%u pc=0x%08x lr=0x%08x thread=0x%08x stack_start=0x%08x stack_size=%u recovery_delay_ms=%u\n",
               mesh_route_test_fatal_count,
               mesh_route_test_fatal_reason,
               mesh_route_test_fatal_pc,
               mesh_route_test_fatal_lr,
               mesh_route_test_fatal_thread,
               mesh_route_test_fatal_stack_start,
               mesh_route_test_fatal_stack_size,
               recovery_delay_ms);
        k_msleep(recovery_delay_ms);
    }
#endif
    ret = app_watchdog_init();
    if (ret < 0) {
        watchdog_init_fail_closed(ret);
    }

#if defined(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
    gateway_ble_connectivity_test_run();
    return 0;
#endif

    battery_adc_ret = battery_adc_divider_disable();
    if (DEVICE_ROLE == ROLE_CLICKER) {
        ret = app_click_event_sequence_init();
        if (ret < 0) {
            printk("fatal: click event identity reservation unavailable: %d\n",
                   ret);
            k_panic();
            return ret;
        }
    }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    ret = status_leds_init();
    if (ret < 0) {
        LOG_WRN("status LED setup incomplete: %d", ret);
    }
    status_debug_tx_boot_test();
    status_debug_gateway_boot_test();
    status_debug_anchor_boot_test();
#endif
#if !defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS) || \
    defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    ret = app_clicker_init(&clicker_callbacks);
    if (ret < 0) {
        LOG_WRN("clicker runtime init failed: %d", ret);
    }
#if !defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    app_clicker_prepare_startup_idle(&boot_button_action);
#endif
#endif

    if (clicker_systemon_retained_idle_enabled()) {
        ret = 0;
        printk("local debug input skipped for retained clicker idle\n");
    } else {
        (void)debug_serial_init();
    }

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    app_mesh_direct_probe_breadcrumb_boot_diagnostics();
#endif
#if IMEC_RETAIN_FATAL_BREADCRUMB
    if (fatal_recovery_boot) {
        status_debug_printf("DBG_MESH_FATAL_BOOT count=%u reason=%u pc=0x%08x lr=0x%08x thread=0x%08x stack_start=0x%08x stack_size=%u\n",
                            mesh_route_test_fatal_count,
                            mesh_route_test_fatal_reason,
                            mesh_route_test_fatal_pc,
                            mesh_route_test_fatal_lr,
                            mesh_route_test_fatal_thread,
                            mesh_route_test_fatal_stack_start,
                            mesh_route_test_fatal_stack_size);
    }
#endif

#if defined(CONFIG_IMEC_HIGH_DEBUG)
    app_high_debug_set_callbacks(&high_debug_callbacks);
    (void)app_high_debug_init();
    HIGH_DEBUG_COUNTER_INC(boot_count);
    high_debug_boot_banner();
    high_debug_log_event("DEBUG_TRANSPORT_READY",
                         "rtt_logs=%u gateway_ble=%u command_parser=%u",
                         IS_ENABLED(CONFIG_IMEC_RTT_LOGS) ? 1u : 0u,
                         gateway_ble_transport_enabled() ? 1u : 0u,
                         app_high_debug_command_poll_enabled() ? 1u : 0u);
    high_debug_log_event("BOOTLOADER_READY",
                         "configured=0 entry_command=0 recovery=disabled");
    app_high_debug_start(!clicker_systemon_retained_idle_enabled());
#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    stage1_click_spam_boot_marker("after_high_debug_start");
#endif
#endif

#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    stage1_click_spam_boot_marker("before_node_comm_init");
#endif
#if !defined(CONFIG_IMEC_ML_CLICKER) && \
    !defined(CONFIG_IMEC_ML_ANCHOR)
    ret = app_node_comm_init(app_anchor_mesh_report_callbacks());
#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    stage1_click_spam_boot_marker("after_node_comm_init");
#endif
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    status_debug_printf("DBG_NODE_COMM_BOOT stage=init ret=%d running=%u uptime=%u\n",
                        ret,
                        app_node_comm_policy_running() ? 1u : 0u,
                        k_uptime_get_32());
#endif
    if (ret < 0) {
        LOG_ERR("node communication initialization failed: %d", ret);
        runtime_start_fail_closed("node communication initialization", ret);
    }
#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    stage1_click_spam_boot_marker("before_mesh_test_init");
#endif
    (void)app_mesh_test_init();
#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    stage1_click_spam_boot_marker("after_mesh_test_init");
#endif
    gateway_command_result_tracking_init();
#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    stage1_click_spam_boot_marker("after_gateway_tracking_init");
#endif
#endif
#if !defined(CONFIG_IMEC_ML_CLICKER)
    ret = app_anchor_init();
    if (ret < 0) {
        LOG_ERR("anchor/gateway runtime initialization failed closed: %d",
                ret);
        k_panic();
        return ret;
    }
#endif
#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    stage1_click_spam_boot_marker("after_anchor_init");
#endif
    LOG_INF("UWB firmware starting as %s", role_name());
    if (battery_adc_ret < 0) {
        LOG_WRN("battery ADC divider disable failed: %d", battery_adc_ret);
    }
    LOG_INF("runtime config: device_id=0x%016llx gateway_id=0x%016llx max_scheduled=%u wake_ms=%u max_attempts=%u min_unique_anchors=%u anchor_scan_interval_ms=%u anchor_scan_window_ms=%u anchor_mesh_rx_interval_ms=%u anchor_idle_uwb_rx_us_per_s=%u anchor_idle_uwb_awake_us_per_s=%u anchor_uwb_wait_ms=%u anchor_slot_source=%s highdebug_anchor_slot=%u",
            (unsigned long long)DEVICE_ID,
            (unsigned long long)GATEWAY_ID,
            MAX_SCHEDULED_ANCHORS,
            WAKE_ADV_MS,
            MAX_WAKE_ATTEMPTS,
            UWB_NORMAL_CLICK_MIN_ANCHORS,
            anchor_uwb_scan_interval_ms,
            ANCHOR_UWB_SCAN_RX_MS,
            UWB_MESH_ANCHOR_RX_INTERVAL_MS,
            (unsigned int)ANCHOR_UWB_SCAN_RX_US_PER_S,
            (unsigned int)ANCHOR_UWB_PERIODIC_IDLE_US_PER_S,
            ANCHOR_UWB_WAIT_MS,
            ANCHOR_DISCOVERY_SLOT_SOURCE,
            (unsigned int)IMEC_HIGH_DEBUG_ANCHOR_SLOT);

#if !defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    ret = status_leds_init();
    if (ret < 0) {
        LOG_WRN("status LED setup incomplete: %d", ret);
    }
#endif
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    stage1_led_phase(STAGE1_LED_PHASE_IDLE);
    stage1_led_result(STAGE1_LED_RESULT_OFF);
    high_debug_stage0_rainbow_led_test();
#endif

    ret = dwm3000_port_init();
#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    stage1_click_spam_boot_marker("after_dwm_port_init");
#endif
    if (ret < 0) {
        LOG_ERR("DWM3000 reset/wake setup failed closed: %d", ret);
        runtime_start_fail_closed("DWM3000 port initialization", ret);
    } else {
        LOG_INF("DWM3000 wake pin parked inactive; SYS_STATUS polling ready; radio init waits for UWB wake windows");
    }
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (ret == 0) {
#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
        stage1_click_spam_boot_marker("before_dwm_probe");
#endif
        ret = high_debug_probe_dwm3000();
#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
        stage1_click_spam_boot_marker("after_dwm_probe");
#endif
        if (ret < 0) {
            LOG_WRN("high-debug DWM3000 boot probe failed: %d", ret);
        }
    }
#endif

#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS)
    if (DEVICE_ROLE == ROLE_CLICKER) {
        const struct app_clicker_continuous_wake_claims_config wake_spam_config = {
            .wake_adv_ms = WAKE_ADV_MS,
            .min_anchor_count = app_clicker_debug_min_anchor_count(),
            .max_anchor_count = app_clicker_debug_max_anchor_count(),
            .max_attempts = MAX_WAKE_ATTEMPTS,
            .samples_per_anchor = app_clicker_debug_samples_per_anchor(),
            .wake_channel = UWB_WAKE_CHANNEL,
            .ranging_channel = UWB_RANGING_CHANNEL,
            .flags = app_clicker_debug_session_flags(),
            .wake_train = clicker_wake_train_config,
        };

        LOG_INF("continuous Stage 1 WAKE_CLAIM transmitter enabled; button/system-off path bypassed");
        app_clicker_run_continuous_wake_claims(&wake_spam_config);
    }
#endif

#if defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
    if (DEVICE_ROLE == ROLE_CLICKER) {
        stage1_click_spam_boot_marker("before_runner");
        high_debug_log_event("STAGE1_CLICK_SPAM",
                             "phase=enabled preset=%s physical_button=disabled path=normal_click",
                             IMEC_BUILD_PRESET_NAME);
        LOG_INF("Stage 1 click-session spam enabled; physical button path bypassed");
        ret = app_clicker_start_continuous_click_sessions(&clicker_session_spam_config);
        if (ret < 0) {
            high_debug_log_event("STAGE1_CLICK_SPAM",
                                 "phase=worker_submit_failed ret=%d",
                                 ret);
            LOG_ERR("Stage 1 click-session spam worker submit failed: %d", ret);
            runtime_start_fail_closed(
                "Stage 1 click-session worker submission", ret);
        }
    }
#endif

    if (DEVICE_ROLE == ROLE_CLICKER) {
#if defined(CONFIG_IMEC_ML_CLICKER)
        ret = app_ml_init();
        if (ret < 0) {
            LOG_ERR("ML clicker runtime initialization failed closed: %d",
                    ret);
            k_panic();
            return ret;
        }
#else
#if !defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS) && \
    !defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_CLICK_SESSIONS)
        (void)app_clicker_start_work_queue();
        ret = app_clicker_button_init();
        if (ret < 0) {
            LOG_WRN("click button unavailable: %d", ret);
        }
        if (boot_button_action != BUTTON_ACTION_NONE) {
            app_clicker_submit_button_action(boot_button_action);
        } else if (clicker_systemon_retained_idle_enabled() ||
                   IS_ENABLED(CONFIG_IMEC_CLICKER_SYSTEMOFF_IDLE)) {
            app_clicker_enter_idle();
        }
#endif
#endif
    }

    if (DEVICE_ROLE == ROLE_ANCHOR) {
        status_debug_note("DBG_BOOT_ANCHOR_ROLE_BEGIN\n");
        high_debug_log_event("MESH_BOOT_STAGE",
                             "stage=anchor_role_start_begin role=%s",
                             role_name());
        ret = app_anchor_start_anchor_role();
        status_debug_note("DBG_BOOT_ANCHOR_ROLE_DONE\n");
        high_debug_log_event("MESH_BOOT_STAGE",
                             "stage=anchor_role_start_done role=%s ret=%d",
                             role_name(),
                             ret);
        if (ret < 0) {
            runtime_start_fail_closed("anchor role startup", ret);
        }
#if !defined(CONFIG_IMEC_ML_ANCHOR)
        if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
            high_debug_log_event("MESH_BOOT_STAGE",
                                 "stage=mesh_rx_anchor_idle_owned_by_low_duty_scan role=%s",
                                 role_name());
            status_debug_note("DBG_BOOT_MESH_RX_ANCHOR_LOW_DUTY_OWNER\n");
#else
            high_debug_log_event("MESH_BOOT_STAGE",
                                 "stage=mesh_rx_start_begin role=%s",
                                 role_name());
            status_debug_note("DBG_BOOT_MESH_RX_BEGIN\n");
            ret = mesh_start_uwb_rx("anchor startup");
            status_debug_note("DBG_BOOT_MESH_RX_DONE\n");
            high_debug_log_event("MESH_BOOT_STAGE",
                                 "stage=mesh_rx_start_done role=%s ret=%d",
                                 role_name(),
                                 ret);
            if (ret < 0) {
                LOG_ERR("anchor UWB mesh RX unavailable: %d", ret);
                runtime_start_fail_closed("anchor UWB mesh RX startup", ret);
            }
#endif
        } else {
            status_debug_note("DBG_BOOT_MESH_RX_SKIPPED_TX\n");
        }
        high_debug_log_event("MESH_BOOT_STAGE",
                             "stage=mesh_test_start_begin role=%s",
                             role_name());
        status_debug_note("DBG_BOOT_MESH_TEST_BEGIN\n");
        ret = app_mesh_test_start();
        status_debug_note("DBG_BOOT_MESH_TEST_DONE\n");
        high_debug_log_event("MESH_BOOT_STAGE",
                             "stage=mesh_test_start_done role=%s ret=%d",
                             role_name(),
                             ret);
        if (ret < 0) {
            LOG_ERR("mesh-test runtime unavailable: %d", ret);
        }
        app_stack_diag_start();
#else
        LOG_INF("ML anchor full-duty UWB scan active; connected mesh runtime disabled");
#endif
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        ret = app_anchor_start_gateway_role();
        if (ret < 0) {
            runtime_start_fail_closed("gateway role startup", ret);
        }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
        status_debug_printf("DBG_GATEWAY_BOOT stage=ble_begin uptime=%u\n",
                            k_uptime_get_32());
#endif
        ret = gateway_ble_init();
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
        status_debug_printf("DBG_GATEWAY_BOOT stage=ble_done ret=%d uptime=%u\n",
                            ret,
                            k_uptime_get_32());
#endif
        if (ret < 0) {
            LOG_ERR("gateway BLE PC link unavailable: %d", ret);
        }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
        status_debug_printf("DBG_GATEWAY_BOOT stage=ch9_begin uptime=%u\n",
                            k_uptime_get_32());
#endif
        ret = mesh_start_uwb_rx("gateway startup");
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
        status_debug_printf("DBG_GATEWAY_BOOT stage=ch9_done ret=%d uptime=%u\n",
                            ret,
                            k_uptime_get_32());
#endif
        if (ret < 0) {
            LOG_ERR("gateway UWB mesh RX unavailable: %d", ret);
            runtime_start_fail_closed("gateway UWB mesh RX startup", ret);
        }
        app_stack_diag_start();
        LOG_INF("gateway reactive mesh root active; BLE packet/log link %s",
                gateway_ble_transport_enabled() ? "advertising" : "disabled");
    }

#if IMEC_RETAIN_FATAL_BREADCRUMB
    if (fatal_recovery_boot) {
        LOG_INF("fatal recovery boot reached normal role startup: thread=0x%08x stack_start=0x%08x stack_size=%u",
                mesh_route_test_fatal_thread,
                mesh_route_test_fatal_stack_start,
                mesh_route_test_fatal_stack_size);
        mesh_route_test_fatal_magic = 0u;
        mesh_route_test_fatal_count = 0u;
    }
#endif

    return 0;
}

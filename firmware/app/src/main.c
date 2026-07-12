#include "app_anchor.h"
#include "app_board.h"
#include "app_clicker.h"
#include "app_config.h"
#include "app_device_identity.h"
#include "app_gateway_ble.h"
#include "app_high_debug.h"
#include "app_ml.h"
#include "app_mesh_report.h"
#include "app_mesh_test.h"
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
#include "uwb_ble_courtesy.h"

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(uwb_app, LOG_LEVEL_DBG);

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
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

    app_watchdog_stop_feeding();
    if (mesh_route_test_fatal_magic != MESH_FATAL_BREADCRUMB_MAGIC) {
        mesh_route_test_fatal_count = 0u;
    }
    mesh_route_test_fatal_magic = MESH_FATAL_BREADCRUMB_MAGIC;
    if (mesh_route_test_fatal_count < UINT32_MAX) {
        mesh_route_test_fatal_count++;
    }
    mesh_route_test_fatal_reason = reason;
    mesh_route_test_fatal_thread = (uint32_t)thread;
    mesh_route_test_fatal_stack_start = (uint32_t)thread->stack_info.start;
    mesh_route_test_fatal_stack_size = (uint32_t)thread->stack_info.size;
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
BUILD_ASSERT(SURVEY_DISCOVERY_SLOT_MS >=
             (SURVEY_DISCOVERY_RX_GUARD_MS + SURVEY_DISCOVERY_TX_TIMEOUT_MS + 2u),
             "survey discovery slots must fit guard time and one probe transmission");
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
BUILD_ASSERT(UWB_ANCHOR_PAIR_SURVEY_MAX_PAIRS >=
             ((UWB_ANCHOR_PAIR_SCHEDULE_MAX_ANCHORS *
               (UWB_ANCHOR_PAIR_SCHEDULE_MAX_ANCHORS - 1u)) / 2u),
             "ML anchor-pair result storage must cover every scheduled pair");
#endif
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

int main(void)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    const struct app_high_debug_callbacks high_debug_callbacks = {
        .command_poll_enabled = local_command_poll_enabled,
        .handle_command = high_debug_handle_command,
    };
#endif
#if !defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS)
    const struct app_clicker_callbacks clicker_callbacks = {
        .early_led = high_debug_clicker_early_led,
#if defined(CONFIG_IMEC_HIGH_DEBUG)
        .run_stage0_simulated_click = high_debug_stage0_simulated_click,
#endif
#if defined(CONFIG_IMEC_HIGH_DEBUG)
        .run_stage0_hardware_self_test = high_debug_stage0_hardware_self_test,
#endif
        .send_mesh_outbound = mesh_send_outbound,
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
    enum button_action boot_button_action STAGE1_WAKE_SPAM_UNUSED = BUTTON_ACTION_NONE;
    int ret;
    int battery_adc_ret;
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
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

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (mesh_route_test_fatal_magic == MESH_FATAL_BREADCRUMB_MAGIC) {
        uint32_t recovery_delay_ms = mesh_route_test_fatal_count >= 6u ?
                                     30000u :
                                     mesh_route_test_fatal_count * 5000u;

        fatal_recovery_boot = true;
        printk("retained fatal: count=%u reason=%u pc=0x%08x lr=0x%08x recovery_delay_ms=%u\n",
               mesh_route_test_fatal_count,
               mesh_route_test_fatal_reason,
               mesh_route_test_fatal_pc,
               mesh_route_test_fatal_lr,
               recovery_delay_ms);
        k_msleep(recovery_delay_ms);
    }
#endif
    ret = app_watchdog_init();
    if (ret < 0) {
        printk("hardware watchdog unavailable: %d\n", ret);
    }

#if defined(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
    gateway_ble_connectivity_test_run();
    return 0;
#endif

    battery_adc_ret = battery_adc_divider_disable();
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    ret = status_leds_init();
    if (ret < 0) {
        LOG_WRN("status LED setup incomplete: %d", ret);
    }
    status_debug_tx_boot_test();
    status_debug_gateway_boot_test();
    status_debug_anchor_boot_test();
#endif
#if !defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS)
    ret = app_clicker_init(&clicker_callbacks);
    if (ret < 0) {
        LOG_WRN("clicker runtime init failed: %d", ret);
    }
    app_clicker_prepare_startup_idle(&boot_button_action);
#endif

    if (clicker_systemon_retained_idle_enabled()) {
        ret = 0;
        printk("local debug input skipped for retained clicker idle\n");
    } else {
        (void)debug_serial_init();
    }

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
#endif

    (void)app_mesh_report_init(app_anchor_mesh_report_callbacks());
    (void)app_mesh_test_init();
    gateway_command_result_tracking_init();
    (void)app_anchor_init();
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
    if (ret < 0) {
        LOG_WRN("DWM3000 reset/wake setup failed: %d", ret);
    } else {
        LOG_INF("DWM3000 wake pin parked inactive; SYS_STATUS polling ready; radio init waits for UWB wake windows");
    }
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (ret == 0) {
        ret = high_debug_probe_dwm3000();
        if (ret < 0) {
            LOG_WRN("high-debug DWM3000 boot probe failed: %d", ret);
        }
        (void)dwm3000_driver_standby();
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

    if (DEVICE_ROLE == ROLE_CLICKER) {
#if defined(CONFIG_IMEC_ML_CLICKER)
        (void)app_ml_init();
#else
#if !defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS)
        (void)app_clicker_start_work_queue();
        ret = app_clicker_button_init();
        if (ret < 0) {
            LOG_WRN("click button unavailable: %d", ret);
        }
        if (boot_button_action != BUTTON_ACTION_NONE) {
            app_clicker_handle_button_action(boot_button_action);
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
            return 0;
        }
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
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        ret = app_anchor_start_gateway_role();
        if (ret < 0) {
            return 0;
        }
        ret = gateway_ble_init();
        if (ret < 0) {
            LOG_ERR("gateway BLE PC link unavailable: %d", ret);
        }
        mesh_gateway_route_adv_start();
        ret = mesh_start_uwb_rx("gateway startup");
        if (ret < 0) {
            LOG_ERR("gateway UWB mesh RX unavailable: %d", ret);
        }
        app_stack_diag_start();
        LOG_INF("gateway reactive mesh root active; BLE packet/log link %s",
                gateway_ble_transport_enabled() ? "advertising" : "disabled");
    }

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (fatal_recovery_boot) {
        mesh_route_test_fatal_magic = 0u;
        mesh_route_test_fatal_count = 0u;
        LOG_INF("fatal recovery boot reached normal role startup");
    }
#endif

    return 0;
}

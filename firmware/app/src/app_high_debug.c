#include "app_high_debug.h"

#include "app_board.h"
#include "app_click_event_sequence.h"
#include "app_clicker.h"
#include "app_config.h"
#include "app_radio_recovery.h"
#include "app_state.h"
#include "debug_log.h"
#include "dwm3000_driver.h"
#include "dwm3000_port.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#endif
#if defined(CONFIG_RETENTION_BOOT_MODE)
#include <zephyr/retention/bootmode.h>
#endif
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>

LOG_MODULE_REGISTER(app_high_debug, LOG_LEVEL_DBG);

#if defined(CONFIG_IMEC_HIGH_DEBUG)
static void high_debug_counter_work_handler(struct k_work *work);
static void high_debug_command_work_handler(struct k_work *work);

static struct k_work_delayable high_debug_counter_work;
static struct k_work_delayable high_debug_command_work;
static struct app_high_debug_callbacks high_debug_callbacks;
static bool high_debug_work_initialized;
static char high_debug_command_buf[80];
static size_t high_debug_command_len;
static bool high_debug_manual_uwb_awake;

int app_high_debug_init(void)
{
    if (!high_debug_work_initialized) {
        k_work_init_delayable(&high_debug_counter_work,
                              high_debug_counter_work_handler);
        k_work_init_delayable(&high_debug_command_work,
                              high_debug_command_work_handler);
        high_debug_work_initialized = true;
    }
    return 0;
}

void app_high_debug_set_callbacks(const struct app_high_debug_callbacks *callbacks)
{
    if (callbacks == NULL) {
        high_debug_callbacks.command_poll_enabled = NULL;
        high_debug_callbacks.handle_command = NULL;
        return;
    }
    high_debug_callbacks = *callbacks;
}

bool app_high_debug_command_poll_enabled(void)
{
    if (high_debug_callbacks.command_poll_enabled == NULL) {
        return false;
    }
    return high_debug_callbacks.command_poll_enabled();
}

void app_high_debug_start(bool schedule_counter_work)
{
    (void)app_high_debug_init();
    if (schedule_counter_work) {
        (void)k_work_schedule(&high_debug_counter_work,
                              K_MSEC(CONFIG_IMEC_HIGH_DEBUG_COUNTER_PERIOD_MS));
    }
    if (app_high_debug_command_poll_enabled()) {
        (void)k_work_schedule(&high_debug_command_work,
                              K_MSEC(CONFIG_IMEC_HIGH_DEBUG_COMMAND_POLL_MS));
    }
}

struct high_debug_counters {
    uint32_t boot_count;
    uint32_t dwm_dev_id_successes;
    uint32_t dwm_dev_id_failures;
    uint32_t wake_claim_tx;
    uint32_t wake_claim_rx;
    uint32_t wake_claim_accepted;
    uint32_t wake_claim_rejected;
    uint32_t discovery_tx;
    uint32_t discovery_rx;
    uint32_t discovery_reply_tx;
    uint32_t discovery_reply_rx;
    uint32_t schedules_tx;
    uint32_t schedules_rx;
    uint32_t schedules_accepted;
    uint32_t schedules_rejected;
    uint32_t ds_twr_attempts;
    uint32_t ds_twr_successes;
    uint32_t ds_twr_failures;
    uint32_t ds_twr_timing_rejects;
    uint32_t mesh_tx;
    uint32_t mesh_rx;
    uint32_t mesh_ack;
    uint32_t mesh_retry;
    uint32_t mesh_drop;
    uint32_t gateway_packets_emitted;
    uint32_t gateway_ble_connects;
    uint32_t gateway_ble_disconnects;
    uint32_t gateway_ble_notify_failures;
    uint32_t gateway_ble_rx_drops;
    uint32_t bootloader_entry_requests;
    uint32_t command_rx;
    uint32_t command_result_tx;
};

static struct high_debug_counters high_debug_counters;

struct stage1_anchor_focused_rx_trace {
    uint32_t frame_count;
    int64_t last_frame_ms;
    size_t last_frame_len;
    uint8_t last_frame_quality;
    int last_decode_ret;
    uint32_t claim_count;
    int64_t last_claim_ms;
    uint64_t last_claim_clicker_id;
    uint32_t last_claim_event_seq;
    uint32_t last_claim_attempt;
    uint64_t last_claim_nonce;
};

static struct stage1_anchor_focused_rx_trace stage1_anchor_focused_rx_trace = {
    .last_frame_ms = -1,
    .last_claim_ms = -1,
};

static const char *debug_role_name(void)
{
#if defined(CONFIG_IMEC_ROLE_TAG)
    if (IS_ENABLED(CONFIG_IMEC_ROLE_TAG)) {
        return "tag";
    }
#endif
    return role_name();
}

bool stage1_anchor_focused_rx_logs_enabled(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR &&
           IS_ENABLED(CONFIG_IMEC_STAGE1_ANCHOR_FOCUSED_RX_LOGS);
}

static uint32_t stage1_anchor_focused_age_ms(int64_t now_ms, int64_t then_ms)
{
    if (then_ms < 0 || now_ms < then_ms) {
        return UINT32_MAX;
    }
    if ((now_ms - then_ms) > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)(now_ms - then_ms);
}

void stage1_anchor_focused_note_rx_frame(size_t frame_len,
                                         uint8_t quality,
                                         int decode_ret)
{
    if (!stage1_anchor_focused_rx_logs_enabled()) {
        return;
    }
    stage1_anchor_focused_rx_trace.frame_count++;
    stage1_anchor_focused_rx_trace.last_frame_ms = k_uptime_get();
    stage1_anchor_focused_rx_trace.last_frame_len = frame_len;
    stage1_anchor_focused_rx_trace.last_frame_quality = quality;
    stage1_anchor_focused_rx_trace.last_decode_ret = decode_ret;
}

void stage1_anchor_focused_note_wake_claim(
    const struct uwb_wake_claim_frame *claim)
{
    if (!stage1_anchor_focused_rx_logs_enabled() || claim == NULL) {
        return;
    }
    stage1_anchor_focused_rx_trace.claim_count++;
    stage1_anchor_focused_rx_trace.last_claim_ms = k_uptime_get();
    stage1_anchor_focused_rx_trace.last_claim_clicker_id = claim->clicker_id;
    stage1_anchor_focused_rx_trace.last_claim_event_seq = claim->click_event_id;
    stage1_anchor_focused_rx_trace.last_claim_attempt = claim->attempt_index;
    stage1_anchor_focused_rx_trace.last_claim_nonce = claim->nonce;
}

void stage1_anchor_focused_log_diagnostics(
    int ret,
    const char *rx_failure,
    bool preamble_detected,
    const struct uwb_anchor_session *session)
{
    static int64_t next_focused_diag_ms;
    static uint32_t last_focused_claims;
    static uint32_t last_focused_replies;
    static uint32_t last_focused_schedules;
    struct dwm3000_driver_stats radio_stats = {0};
    int64_t now_ms;
    uint32_t last_frame_age_ms;
    uint32_t last_claim_age_ms;
    uint32_t sleep_wake_avg_us;
    bool counters_changed;
    bool diagnostics_due;

    if (!stage1_anchor_focused_rx_logs_enabled() || session == NULL) {
        return;
    }

    now_ms = k_uptime_get();
    last_frame_age_ms =
        stage1_anchor_focused_age_ms(
            now_ms,
            stage1_anchor_focused_rx_trace.last_frame_ms);
    last_claim_age_ms =
        stage1_anchor_focused_age_ms(
            now_ms,
            stage1_anchor_focused_rx_trace.last_claim_ms);
    counters_changed =
        session->diagnostics.claims != last_focused_claims ||
        session->diagnostics.discovery_replies != last_focused_replies ||
        session->diagnostics.schedules != last_focused_schedules;
    diagnostics_due = ret == 0 || counters_changed || now_ms >= next_focused_diag_ms;

    dwm3000_driver_stats_get(&radio_stats);
    sleep_wake_avg_us = radio_stats.sleep_wake_count == 0u ? 0u :
        radio_stats.sleep_wake_total_us / radio_stats.sleep_wake_count;

    if (!diagnostics_due) {
        return;
    }

    LOG_INF("anchor focused UWB diagnostics: last_ret=%d last_rx_failure=%s last_preamble=%u scans=%u preambles=%u sfd_timeouts=%u frame_timeouts=%u crc_failures=%u claims=%u collisions=%u wins=%u losses=%u replies=%u schedules=%u ds_ok=%u ds_fail=%u timing_rejections=%u mesh_packets=%u sample_order=%u awake_us=%u sleep_wake_count=%u sleep_wake_avg_us=%u sleep_wake_max_us=%u sleep_wake_fail=%u valid_frames=%u last_frame_age_ms=%u last_len=%u last_q=%u last_decode_ret=%d valid_claims=%u last_claim_age_ms=%u last_clicker=0x%016llx last_event=%u last_attempt=%u last_nonce=0x%016llx",
            ret,
            rx_failure == NULL ? "unknown" : rx_failure,
            preamble_detected ? 1u : 0u,
            session->diagnostics.scans,
            session->diagnostics.preambles,
            session->diagnostics.sfd_timeouts,
            session->diagnostics.frame_timeouts,
            session->diagnostics.crc_failures,
            session->diagnostics.claims,
            session->diagnostics.collisions,
            session->diagnostics.arbitration_wins,
            session->diagnostics.arbitration_losses,
            session->diagnostics.discovery_replies,
            session->diagnostics.schedules,
            session->diagnostics.ds_twr_successes,
            session->diagnostics.ds_twr_failures,
            session->diagnostics.timing_rejections,
            session->diagnostics.uwb_mesh_packets,
            session->diagnostics.sample_order_count,
            session->diagnostics.awake_time_us,
            radio_stats.sleep_wake_count,
            sleep_wake_avg_us,
            radio_stats.sleep_wake_max_us,
            radio_stats.sleep_wake_failures,
            stage1_anchor_focused_rx_trace.frame_count,
            last_frame_age_ms,
            (unsigned int)stage1_anchor_focused_rx_trace.last_frame_len,
            stage1_anchor_focused_rx_trace.last_frame_quality,
            stage1_anchor_focused_rx_trace.last_decode_ret,
            stage1_anchor_focused_rx_trace.claim_count,
            last_claim_age_ms,
            (unsigned long long)stage1_anchor_focused_rx_trace.last_claim_clicker_id,
            stage1_anchor_focused_rx_trace.last_claim_event_seq,
            stage1_anchor_focused_rx_trace.last_claim_attempt,
            (unsigned long long)stage1_anchor_focused_rx_trace.last_claim_nonce);
    next_focused_diag_ms = now_ms + 5000;
    last_focused_claims = session->diagnostics.claims;
    last_focused_replies = session->diagnostics.discovery_replies;
    last_focused_schedules = session->diagnostics.schedules;
}

void high_debug_counter_inc(enum high_debug_counter_field field)
{
    switch (field) {
#define CASE_COUNTER(name) \
    case HIGH_DEBUG_COUNTER_##name: \
        high_debug_counters.name++; \
        break
    CASE_COUNTER(boot_count);
    CASE_COUNTER(dwm_dev_id_successes);
    CASE_COUNTER(dwm_dev_id_failures);
    CASE_COUNTER(wake_claim_tx);
    CASE_COUNTER(wake_claim_rx);
    CASE_COUNTER(wake_claim_accepted);
    CASE_COUNTER(wake_claim_rejected);
    CASE_COUNTER(discovery_tx);
    CASE_COUNTER(discovery_rx);
    CASE_COUNTER(discovery_reply_tx);
    CASE_COUNTER(discovery_reply_rx);
    CASE_COUNTER(schedules_tx);
    CASE_COUNTER(schedules_rx);
    CASE_COUNTER(schedules_accepted);
    CASE_COUNTER(schedules_rejected);
    CASE_COUNTER(ds_twr_attempts);
    CASE_COUNTER(ds_twr_successes);
    CASE_COUNTER(ds_twr_failures);
    CASE_COUNTER(ds_twr_timing_rejects);
    CASE_COUNTER(mesh_tx);
    CASE_COUNTER(mesh_rx);
    CASE_COUNTER(mesh_ack);
    CASE_COUNTER(mesh_retry);
    CASE_COUNTER(mesh_drop);
    CASE_COUNTER(gateway_packets_emitted);
    CASE_COUNTER(gateway_ble_connects);
    CASE_COUNTER(gateway_ble_disconnects);
    CASE_COUNTER(gateway_ble_notify_failures);
    CASE_COUNTER(gateway_ble_rx_drops);
    CASE_COUNTER(bootloader_entry_requests);
    CASE_COUNTER(command_rx);
    CASE_COUNTER(command_result_tx);
#undef CASE_COUNTER
    default:
        break;
    }
}

void high_debug_log_event(const char *event, const char *fmt, ...)
{
    char prefix[96];
    char message[192];
    va_list args;
    int ret;

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    if (event == NULL) {
        event = "UNKNOWN";
    }
    ret = debug_log_format_prefix(prefix,
                                  sizeof(prefix),
                                  k_uptime_get_32(),
                                  debug_role_name(),
                                  DEVICE_ID,
                                  CONFIG_IMEC_BENCH_STAGE,
                                  event);
    if (ret < 0) {
        LOG_WRN("high-debug prefix format failed for event=%s", event);
        return;
    }

    if (fmt == NULL || fmt[0] == '\0') {
        LOG_INF("%s", prefix);
        return;
    }

    va_start(args, fmt);
    ret = vsnprintk(message, sizeof(message), fmt, args);
    va_end(args);
    if (ret < 0) {
        LOG_WRN("%s message_format_failed", prefix);
        return;
    }
    LOG_INF("%s %s", prefix, message);
}

static enum stage1_led_phase stage1_led_last_phase = STAGE1_LED_PHASE_IDLE;
static enum stage1_led_result stage1_led_last_result = STAGE1_LED_RESULT_OFF;

#define STAGE1_CLICK_TRACE_DEPTH 32u
#define STAGE1_CLICK_TRACE_MSG_LEN 128u

struct stage1_click_trace_entry {
    uint32_t uptime_ms;
    char message[STAGE1_CLICK_TRACE_MSG_LEN];
};

static struct stage1_click_trace_entry stage1_click_trace[STAGE1_CLICK_TRACE_DEPTH];
static uint8_t stage1_click_trace_count;
static bool stage1_click_trace_overflow;

static bool stage1_leds_enabled(void)
{
    return CONFIG_IMEC_BENCH_STAGE == 1;
}

static const char *stage1_led_phase_name(enum stage1_led_phase phase)
{
    switch (phase) {
    case STAGE1_LED_PHASE_WAKE:
        return "wake";
    case STAGE1_LED_PHASE_DISCOVERY:
        return "discovery";
    case STAGE1_LED_PHASE_SCHEDULE:
        return "schedule";
    case STAGE1_LED_PHASE_RANGE:
        return "range";
    case STAGE1_LED_PHASE_SCAN:
        return "scan";
    case STAGE1_LED_PHASE_IDLE:
    default:
        return "idle";
    }
}

static const char *stage1_led_result_name(enum stage1_led_result result)
{
    switch (result) {
    case STAGE1_LED_RESULT_ACTIVE:
        return "active";
    case STAGE1_LED_RESULT_OK:
        return "ok";
    case STAGE1_LED_RESULT_TIMEOUT:
        return "timeout";
    case STAGE1_LED_RESULT_ERROR:
        return "error";
    case STAGE1_LED_RESULT_OFF:
    default:
        return "off";
    }
}

static enum stage1_led_result stage1_led_result_for_ret(int ret)
{
    if (ret == 0) {
        return STAGE1_LED_RESULT_OK;
    }
    return ret == -ETIMEDOUT ? STAGE1_LED_RESULT_TIMEOUT : STAGE1_LED_RESULT_ERROR;
}

void stage1_click_diag(const char *fmt, ...)
{
    va_list args;
    uint8_t index;

    if (!stage1_leds_enabled() || DEVICE_ROLE != ROLE_CLICKER) {
        return;
    }

    if (stage1_click_trace_count < STAGE1_CLICK_TRACE_DEPTH) {
        index = stage1_click_trace_count++;
    } else {
        index = STAGE1_CLICK_TRACE_DEPTH - 1u;
        stage1_click_trace_overflow = true;
    }

    stage1_click_trace[index].uptime_ms = k_uptime_get_32();
    va_start(args, fmt);
    (void)vsnprintk(stage1_click_trace[index].message,
                    sizeof(stage1_click_trace[index].message),
                    fmt,
                    args);
    va_end(args);
    printk("CLICK_DIAG %s\n", stage1_click_trace[index].message);
}

void stage1_click_trace_reset(void)
{
    if (!stage1_leds_enabled() || DEVICE_ROLE != ROLE_CLICKER) {
        return;
    }

    stage1_click_trace_count = 0u;
    stage1_click_trace_overflow = false;
}

void stage1_click_trace_dump(const char *reason)
{
    if (!stage1_leds_enabled() || DEVICE_ROLE != ROLE_CLICKER) {
        return;
    }

    printk("CLICK_DIAG_DUMP reason=%s count=%u overflow=%u\n",
           reason == NULL ? "unknown" : reason,
           stage1_click_trace_count,
           stage1_click_trace_overflow ? 1u : 0u);
    for (uint8_t i = 0u; i < stage1_click_trace_count; i++) {
        printk("CLICK_DIAG_DUMP[%u] t=%u %s\n",
               i,
               stage1_click_trace[i].uptime_ms,
               stage1_click_trace[i].message);
    }
}

static void stage1_led0_set(bool red, bool green, bool blue)
{
    if (!stage1_leds_enabled()) {
        return;
    }
    status_led0_set(red, green, blue);
}

static void stage1_led1_set(bool red, bool green, bool blue)
{
    if (!stage1_leds_enabled()) {
        return;
    }
    status_led1_set(red, green, blue);
}

void stage1_led_phase(enum stage1_led_phase phase)
{
    stage1_led_last_phase = phase;
    if (DEVICE_ROLE == ROLE_CLICKER && stage1_leds_enabled()) {
        high_debug_log_event("STAGE1_LED_PHASE",
                             "phase=%s r=%u g=%u b=%u",
                             stage1_led_phase_name(phase),
                             phase == STAGE1_LED_PHASE_DISCOVERY ||
                             phase == STAGE1_LED_PHASE_SCHEDULE ||
                             phase == STAGE1_LED_PHASE_RANGE,
                             phase == STAGE1_LED_PHASE_WAKE ||
                             phase == STAGE1_LED_PHASE_DISCOVERY ||
                             phase == STAGE1_LED_PHASE_RANGE,
                             phase == STAGE1_LED_PHASE_IDLE ||
                             phase == STAGE1_LED_PHASE_SCAN ||
                             phase == STAGE1_LED_PHASE_WAKE ||
                             phase == STAGE1_LED_PHASE_SCHEDULE ||
                             phase == STAGE1_LED_PHASE_RANGE);
        stage1_click_diag("led_phase=%s",
                          stage1_led_phase_name(phase));
    }

    switch (phase) {
    case STAGE1_LED_PHASE_WAKE:
        stage1_led0_set(false, true, true);
        break;
    case STAGE1_LED_PHASE_DISCOVERY:
        stage1_led0_set(true, true, false);
        break;
    case STAGE1_LED_PHASE_SCHEDULE:
        stage1_led0_set(true, false, true);
        break;
    case STAGE1_LED_PHASE_RANGE:
        stage1_led0_set(true, true, true);
        break;
    case STAGE1_LED_PHASE_SCAN:
    case STAGE1_LED_PHASE_IDLE:
    default:
        stage1_led0_set(false, false, true);
        break;
    }
}

void stage1_led_result(enum stage1_led_result result)
{
    stage1_led_last_result = result;
    if (DEVICE_ROLE == ROLE_CLICKER && stage1_leds_enabled()) {
        high_debug_log_event("STAGE1_LED_RESULT",
                             "result=%s r=%u g=%u b=%u",
                             stage1_led_result_name(result),
                             result == STAGE1_LED_RESULT_TIMEOUT ||
                             result == STAGE1_LED_RESULT_ERROR,
                             result == STAGE1_LED_RESULT_OK ||
                             result == STAGE1_LED_RESULT_TIMEOUT,
                             result == STAGE1_LED_RESULT_ACTIVE);
        stage1_click_diag("led_result=%s",
                          stage1_led_result_name(result));
    }

    switch (result) {
    case STAGE1_LED_RESULT_ACTIVE:
        stage1_led1_set(false, false, true);
        break;
    case STAGE1_LED_RESULT_OK:
        stage1_led1_set(false, true, false);
        break;
    case STAGE1_LED_RESULT_TIMEOUT:
        stage1_led1_set(true, true, false);
        break;
    case STAGE1_LED_RESULT_ERROR:
        stage1_led1_set(true, false, false);
        break;
    case STAGE1_LED_RESULT_OFF:
    default:
        stage1_led1_set(false, false, false);
        break;
    }
}

bool stage1_led_hold_click_result(int ret, uint32_t hold_ms)
{
    enum stage1_led_result forced_result = stage1_led_result_for_ret(ret);

    if (!stage1_leds_enabled() || DEVICE_ROLE != ROLE_CLICKER) {
        return false;
    }

    high_debug_log_event("STAGE1_LED_HOLD",
                         "ret=%d forced_result=%s previous_phase=%s previous_result=%s hold_ms=%u",
                         ret,
                         stage1_led_result_name(forced_result),
                         stage1_led_phase_name(stage1_led_last_phase),
                         stage1_led_result_name(stage1_led_last_result),
                         hold_ms);
    stage1_click_diag("led_hold ret=%d forced=%s previous_phase=%s previous_result=%s hold_ms=%u",
                      ret,
                      stage1_led_result_name(forced_result),
                      stage1_led_phase_name(stage1_led_last_phase),
                      stage1_led_result_name(stage1_led_last_result),
                      hold_ms);
    stage1_led_phase(stage1_led_last_phase);
    stage1_led_result(forced_result);
    k_msleep(hold_ms);
    return true;
}

void stage1_clicker_early_led(const char *where,
                              enum stage1_led_phase phase,
                              enum stage1_led_result result,
                              uint32_t hold_ms)
{
    if (!stage1_leds_enabled() || DEVICE_ROLE != ROLE_CLICKER) {
        return;
    }

    (void)status_leds_connect();
    stage1_led_phase(phase);
    stage1_led_result(result);
    printk("CLICKER_LED_CODE where=%s phase=%s result=%s hold_ms=%u\n",
           where == NULL ? "unknown" : where,
           stage1_led_phase_name(phase),
           stage1_led_result_name(result),
           hold_ms);
    if (hold_ms > 0u) {
        k_msleep(hold_ms);
    }
}

void high_debug_clicker_early_led(enum app_clicker_early_led_event event)
{
    switch (event) {
    case APP_CLICKER_EARLY_LED_SYSTEMOFF_BUTTON_WAKE:
        stage1_clicker_early_led("systemoff_button_wake",
                                 STAGE1_LED_PHASE_WAKE,
                                 STAGE1_LED_RESULT_ACTIVE,
                                 0u);
        break;
    case APP_CLICKER_EARLY_LED_SYSTEMOFF_CAPTURE_FAILED:
        stage1_clicker_early_led("systemoff_button_capture_failed",
                                 STAGE1_LED_PHASE_WAKE,
                                 STAGE1_LED_RESULT_ERROR,
                                 750u);
        break;
    case APP_CLICKER_EARLY_LED_BUTTON_WAKE_INPUT_UNAVAILABLE:
        stage1_clicker_early_led("button_wake_input_unavailable",
                                 STAGE1_LED_PHASE_IDLE,
                                 STAGE1_LED_RESULT_ERROR,
                                 750u);
        break;
    case APP_CLICKER_EARLY_LED_BUTTON_STILL_HELD_NO_WAKE_ARM:
        stage1_clicker_early_led("button_still_held_no_wake_arm",
                                 STAGE1_LED_PHASE_IDLE,
                                 STAGE1_LED_RESULT_TIMEOUT,
                                 750u);
        break;
    case APP_CLICKER_EARLY_LED_BUTTON_WAKE_ARM_FAILED:
        stage1_clicker_early_led("button_wake_arm_failed",
                                 STAGE1_LED_PHASE_IDLE,
                                 STAGE1_LED_RESULT_ERROR,
                                 750u);
        break;
    case APP_CLICKER_EARLY_LED_SYSTEMON_BUTTON_PRESS:
        stage1_clicker_early_led("systemon_button_press",
                                 STAGE1_LED_PHASE_WAKE,
                                 STAGE1_LED_RESULT_ACTIVE,
                                 0u);
        break;
    default:
        break;
    }
}

void high_debug_dump_counters(const char *event)
{
    struct dwm3000_driver_stats radio_stats = {0};
    uint32_t sleep_wake_avg_us;

    dwm3000_driver_stats_get(&radio_stats);
    sleep_wake_avg_us = radio_stats.sleep_wake_count == 0u ? 0u :
        radio_stats.sleep_wake_total_us / radio_stats.sleep_wake_count;
    high_debug_log_event(event == NULL ? "COUNTERS" : event,
                         "boot=%u dev_id_ok=%u dev_id_fail=%u "
                         "sys_poll_loops=%u sys_poll_max_us=%u sys_poll_timeouts=%u "
                         "sleep_wake_count=%u sleep_wake_avg_us=%u sleep_wake_max_us=%u sleep_wake_fail=%u "
                         "rx_start=%u rx_done=%u rx_timeout=%u rx_crc=%u rx_fail=%u "
                         "tx_start=%u tx_done=%u tx_fail=%u "
                         "wake_tx=%u wake_rx=%u wake_accept=%u wake_reject=%u "
                         "discover_tx=%u discover_rx=%u discovery_reply_tx=%u discovery_reply_rx=%u "
                         "schedule_tx=%u schedule_rx=%u schedule_accept=%u schedule_reject=%u "
                         "ds_attempt=%u ds_ok=%u ds_fail=%u ds_timing_reject=%u "
                         "mesh_tx=%u mesh_rx=%u mesh_ack=%u mesh_retry=%u mesh_drop=%u "
                         "gateway_packets=%u ble_connect=%u ble_disconnect=%u ble_notify_fail=%u ble_rx_drop=%u "
                         "bootloader_req=%u command_rx=%u command_result_tx=%u",
                         high_debug_counters.boot_count,
                         high_debug_counters.dwm_dev_id_successes,
                         high_debug_counters.dwm_dev_id_failures,
                         radio_stats.sys_status_poll_loops,
                         radio_stats.sys_status_poll_max_duration_us,
                         radio_stats.sys_status_poll_timeouts,
                         radio_stats.sleep_wake_count,
                         sleep_wake_avg_us,
                         radio_stats.sleep_wake_max_us,
                         radio_stats.sleep_wake_failures,
                         radio_stats.rx_starts,
                         radio_stats.rx_dones,
                         radio_stats.rx_timeouts,
                         radio_stats.rx_crc_failures,
                         radio_stats.rx_failures,
                         radio_stats.tx_starts,
                         radio_stats.tx_dones,
                         radio_stats.tx_failures,
                         high_debug_counters.wake_claim_tx,
                         high_debug_counters.wake_claim_rx,
                         high_debug_counters.wake_claim_accepted,
                         high_debug_counters.wake_claim_rejected,
                         high_debug_counters.discovery_tx,
                         high_debug_counters.discovery_rx,
                         high_debug_counters.discovery_reply_tx,
                         high_debug_counters.discovery_reply_rx,
                         high_debug_counters.schedules_tx,
                         high_debug_counters.schedules_rx,
                         high_debug_counters.schedules_accepted,
                         high_debug_counters.schedules_rejected,
                         high_debug_counters.ds_twr_attempts,
                         high_debug_counters.ds_twr_successes,
                         high_debug_counters.ds_twr_failures,
                         high_debug_counters.ds_twr_timing_rejects,
                         high_debug_counters.mesh_tx,
                         high_debug_counters.mesh_rx,
                         high_debug_counters.mesh_ack,
                         high_debug_counters.mesh_retry,
                         high_debug_counters.mesh_drop,
                         high_debug_counters.gateway_packets_emitted,
                         high_debug_counters.gateway_ble_connects,
                         high_debug_counters.gateway_ble_disconnects,
                         high_debug_counters.gateway_ble_notify_failures,
                         high_debug_counters.gateway_ble_rx_drops,
                         high_debug_counters.bootloader_entry_requests,
                         high_debug_counters.command_rx,
                         high_debug_counters.command_result_tx);
}

void high_debug_boot_banner(void)
{
    high_debug_log_event("BOOT_START",
                         "preset=%s git=%s build_time=%s role=%s stage=%d board=%s",
                         IMEC_BUILD_PRESET_NAME[0] == '\0' ? "manual-highdebug" :
                         IMEC_BUILD_PRESET_NAME,
                         IMEC_GIT_VERSION,
                         IMEC_BUILD_TIMESTAMP,
                         debug_role_name(),
                         CONFIG_IMEC_BENCH_STAGE,
                         IMEC_BOARD_TARGET);
    high_debug_log_event("BOOT_CONFIG",
                         "device_id=0x%016llx gateway_id=0x%016llx network_id=0x%08x "
                         "uwb_channel=%u mesh_payload_channel=%u spi_hz=%u sys_status_polling=1 "
                         "rtt_logs=%u gateway_ble=%u anchor_slot_source=%s highdebug_anchor_slot=%u",
                         (unsigned long long)DEVICE_ID,
                         (unsigned long long)GATEWAY_ID,
                         NETWORK_ID,
                         UWB_CHANNEL_WAKE_CONTACT,
                         UWB_CHANNEL_MESH_PAYLOAD,
                         (unsigned int)dwm3000_port_current_spi_hz(),
                         IS_ENABLED(CONFIG_IMEC_RTT_LOGS) ? 1u : 0u,
                         gateway_ble_transport_enabled() ? 1u : 0u,
                         ANCHOR_DISCOVERY_SLOT_SOURCE,
                         (unsigned int)IMEC_HIGH_DEBUG_ANCHOR_SLOT);
}

int high_debug_probe_dwm3000(void)
{
    uint32_t dev_id = 0u;
    int cleanup_ret;
    int ret;

    ret = radio_guard_uwb_start("high-debug DWM3000 probe");
    if (ret < 0) {
        return ret;
    }
    high_debug_log_event("DWM_RESET_ASSERT", "action=probe_start");
    ret = dwm3000_port_init();
    if (ret < 0) {
        high_debug_log_event("DWM_DEV_ID_FAIL", "phase=port_init ret=%d", ret);
        HIGH_DEBUG_COUNTER_INC(dwm_dev_id_failures);
        goto cleanup;
    }

    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        high_debug_log_event("DWM_DEV_ID_FAIL", "phase=wakeup ret=%d", ret);
        HIGH_DEBUG_COUNTER_INC(dwm_dev_id_failures);
        goto cleanup;
    }
    high_debug_log_event("UWB_WAKE", "source=probe");

    ret = dwm3000_port_hw_reset();
    high_debug_log_event("DWM_RESET_RELEASE", "ret=%d", ret);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(dwm_dev_id_failures);
        goto cleanup;
    }

    high_debug_log_event("DWM_DEV_ID_READ", "spi_hz=%u",
                         (unsigned int)dwm3000_port_current_spi_hz());
    ret = dwm3000_driver_probe(&dev_id);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(dwm_dev_id_failures);
        high_debug_log_event("DWM_DEV_ID_FAIL", "ret=%d dev_id=0x%08x", ret, dev_id);
        goto cleanup;
    }

    HIGH_DEBUG_COUNTER_INC(dwm_dev_id_successes);
    high_debug_log_event("DWM_DEV_ID_OK", "dev_id=0x%08x", dev_id);
    ret = dwm3000_port_set_fast_spi();
    high_debug_log_event("DWM_SPI_SPEED_SET", "ret=%d spi_hz=%u",
                         ret,
                         (unsigned int)dwm3000_port_current_spi_hz());

cleanup:
    cleanup_ret = app_radio_standby_with_bounded_recovery(
        "high-debug DWM3000 probe");
    radio_guard_uwb_stop();
    if (cleanup_ret < 0) {
        high_debug_log_event("UWB_SLEEP",
                             "phase=probe_cleanup operation_ret=%d cleanup_ret=%d",
                             ret,
                             cleanup_ret);
        return cleanup_ret;
    }
    return ret;
}

void high_debug_stage0_rainbow_led_test(void)
{
    if (DEVICE_ROLE != ROLE_CLICKER || CONFIG_IMEC_BENCH_STAGE != 0) {
        return;
    }

    high_debug_log_event("LED_TEST", "pattern=all_blink cycles=20 dwell_ms=250");
    for (uint8_t i = 0u; i < 20u; i++) {
        status_leds_set(true, true, true);
        k_msleep(250);
        status_leds_set(false, false, false);
        k_msleep(250);
    }
    status_leds_set(false, false, false);
    high_debug_log_event("LED_TEST", "pattern=all_blink complete=1");
}

#if defined(CONFIG_BT) && DEVICE_ROLE == ROLE_CLICKER
int high_debug_stage0_ble_advertise_test(uint32_t event_seq,
                                         uint32_t duration_ms)
{
    static const char name[] = "IMEC Clicker";
    const struct bt_le_adv_param adv_param = {
        .id = BT_ID_DEFAULT,
        .sid = 0u,
        .secondary_max_skip = 0u,
        .options = BT_LE_ADV_OPT_USE_IDENTITY,
        .interval_min = BLE_COURTESY_ADV_INTERVAL_MIN_UNITS,
        .interval_max = BLE_COURTESY_ADV_INTERVAL_MAX_UNITS,
        .peer = NULL,
    };
    const struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA(BT_DATA_NAME_COMPLETE, name, sizeof(name) - 1u),
    };
    int ret;

    ret = bt_enable(NULL);
    high_debug_log_event("BLE_TEST", "phase=bt_enable ret=%d event_seq=%u", ret, event_seq);
    if (ret != 0 && ret != -EALREADY) {
        return ret;
    }

    (void)bt_le_adv_stop();
    ret = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0u);
    high_debug_log_event("BLE_TEST",
                         "phase=adv_start ret=%d event_seq=%u duration_ms=%u",
                         ret,
                         event_seq,
                         duration_ms);
    if (ret != 0) {
        return ret;
    }

    k_msleep(duration_ms);
    ret = bt_le_adv_stop();
    high_debug_log_event("BLE_TEST", "phase=adv_stop ret=%d event_seq=%u", ret, event_seq);
    return ret;
}
#else
int high_debug_stage0_ble_advertise_test(uint32_t event_seq,
                                         uint32_t duration_ms)
{
    ARG_UNUSED(event_seq);
    ARG_UNUSED(duration_ms);
    return -ENOTSUP;
}
#endif

int high_debug_stage0_hardware_self_test(void)
{
    int cleanup_ret;
    int ret;

    ret = high_debug_probe_dwm3000();
    if (ret < 0) {
        return ret;
    }

    k_msleep(10);
    ret = radio_guard_uwb_start("high-debug stage0 self-test wake");
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_configure_default();
    high_debug_log_event("UWB_WAKE", "phase=stage0_self_test ret=%d spi_hz=%u",
                         ret,
                         (unsigned int)dwm3000_port_current_spi_hz());
    cleanup_ret = app_radio_standby_with_bounded_recovery(
        "high-debug stage0 self-test wake");
    radio_guard_uwb_stop();
    high_debug_log_event("UWB_SLEEP",
                         "phase=stage0_self_test_complete operation_ret=%d cleanup_ret=%d",
                         ret,
                         cleanup_ret);
    if (cleanup_ret < 0) {
        return cleanup_ret;
    }
    return ret;
}

static int high_debug_send_wake_claim_once(void)
{
    struct uwb_clicker_session session;
    struct uwb_wake_claim_frame claim;
    uint32_t event_seq;
    struct uwb_clicker_config config;
    uint8_t frame[UWB_WAKE_CLAIM_LEN];
    size_t frame_len = 0u;
    int cleanup_ret;
    int ret;

    if (DEVICE_ROLE != ROLE_CLICKER) {
        return -EINVAL;
    }
    ret = app_click_event_sequence_next(&event_seq);
    if (ret < 0) {
        return ret;
    }
    config = (struct uwb_clicker_config) {
        .network_id = NETWORK_ID,
        .clicker_id = DEVICE_ID,
        .click_event_id = event_seq,
        .nonce = clicker_nonce(event_seq),
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .max_attempts = 1u,
        .samples_per_anchor = 1u,
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
        .flags = FLAG_DIAGNOSTIC,
    };

    ret = uwb_clicker_session_start(&session, &config);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = uwb_clicker_build_wake_claim(&session,
                                       clicker_priority_id(config.click_event_id, 1u),
                                       0u,
                                       0u,
                                       UWB_POST_WAKE_CLAIMED_DURATION_MS,
                                       &claim);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = radio_guard_uwb_start("high-debug WAKE_CLAIM once");
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        high_debug_log_event("WAKE_CLAIM_TX",
                             "mode=single event_seq=%u attempt=1 nonce=0x%016llx",
                             config.click_event_id,
                             (unsigned long long)config.nonce);
        ret = dwm3000_driver_send_frame(frame, frame_len, UWB_CONTROL_TX_TIMEOUT_MS);
    }
    cleanup_ret = app_radio_standby_with_bounded_recovery(
        "high-debug WAKE_CLAIM once");
    radio_guard_uwb_stop();
    if (cleanup_ret < 0) {
        high_debug_log_event("UWB_SLEEP",
                             "phase=wake_claim_once operation_ret=%d cleanup_ret=%d",
                             ret,
                             cleanup_ret);
        return cleanup_ret;
    }
    if (ret == 0) {
        HIGH_DEBUG_COUNTER_INC(wake_claim_tx);
    }
    return ret;
}

static int high_debug_send_wake_train_command(void)
{
    static const struct app_clicker_wake_train_config clicker_wake_train_config = {
        .wake_adv_ms = WAKE_ADV_MS,
        .post_wake_claimed_duration_ms = UWB_POST_WAKE_CLAIMED_DURATION_MS,
        .control_tx_timeout_ms = UWB_CONTROL_TX_TIMEOUT_MS,
    };
    struct uwb_clicker_session session;
    uint32_t event_seq;
    struct uwb_clicker_config config;
    int ret;

    if (DEVICE_ROLE != ROLE_CLICKER) {
        return -EINVAL;
    }
    ret = app_click_event_sequence_next(&event_seq);
    if (ret < 0) {
        return ret;
    }
    config = (struct uwb_clicker_config) {
        .network_id = NETWORK_ID,
        .clicker_id = DEVICE_ID,
        .click_event_id = event_seq,
        .nonce = clicker_nonce(event_seq),
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .max_attempts = 1u,
        .samples_per_anchor = 1u,
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
        .flags = FLAG_DIAGNOSTIC,
    };

    ret = uwb_clicker_session_start(&session, &config);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    high_debug_log_event("WAKE_CLAIM_TX",
                         "mode=train event_seq=%u attempt=1 nonce=0x%016llx",
                         config.click_event_id,
                         (unsigned long long)config.nonce);
    return app_clicker_send_wake_claim_train(
        &session,
        clicker_priority_id(config.click_event_id, 1u),
        &clicker_wake_train_config);
}

static int high_debug_manual_uwb_wake(void)
{
    int cleanup_ret;
    int ret;

    if (high_debug_manual_uwb_awake) {
        return -EALREADY;
    }
    ret = radio_guard_uwb_start("high-debug manual UWB wake");
    if (ret < 0) {
        return ret;
    }

    ret = dwm3000_driver_configure_default();
    high_debug_log_event("UWB_WAKE", "command=uwb_wake ret=%d", ret);
    if (ret < 0) {
        cleanup_ret = app_radio_standby_with_bounded_recovery(
            "high-debug manual UWB wake failure");
        radio_guard_uwb_stop();
        if (cleanup_ret < 0) {
            high_debug_log_event(
                "UWB_SLEEP",
                "phase=manual_wake_failure operation_ret=%d cleanup_ret=%d",
                ret,
                cleanup_ret);
            return cleanup_ret;
        }
        return ret;
    }

    high_debug_manual_uwb_awake = true;
    return 0;
}

static int high_debug_manual_uwb_sleep(void)
{
    int ret;

    if (!high_debug_manual_uwb_awake) {
        ret = radio_guard_uwb_start("high-debug manual UWB sleep");
        if (ret < 0) {
            return ret;
        }
    }

    ret = app_radio_standby_with_bounded_recovery(
        "high-debug manual UWB sleep");
    high_debug_manual_uwb_awake = false;
    radio_guard_uwb_stop();
    high_debug_log_event("UWB_SLEEP", "command=uwb_sleep ret=%d", ret);
    return ret;
}

int high_debug_handle_command(const char *command)
{
    int ret = -EINVAL;

    if (command == NULL || command[0] == '\0') {
        return 0;
    }

    HIGH_DEBUG_COUNTER_INC(command_rx);
    high_debug_log_event("COMMAND_RX", "command=%s", command);

    if (strcmp(command, "status") == 0) {
        high_debug_boot_banner();
        high_debug_dump_counters("COUNTERS");
        ret = 0;
    } else if (strcmp(command, "dump_counters") == 0) {
        high_debug_dump_counters("COUNTERS");
        ret = 0;
    } else if (strcmp(command, "uwb_probe") == 0) {
        ret = high_debug_probe_dwm3000();
    } else if (strcmp(command, "uwb_sleep") == 0) {
        ret = high_debug_manual_uwb_sleep();
    } else if (strcmp(command, "uwb_wake") == 0) {
        ret = high_debug_manual_uwb_wake();
    } else if (strcmp(command, "send_wake_claim_once") == 0) {
        ret = high_debug_send_wake_claim_once();
    } else if (strcmp(command, "send_wake_train") == 0) {
        ret = high_debug_send_wake_train_command();
    } else if (strcmp(command, "reboot") == 0) {
        high_debug_log_event("COMMAND_RESULT_TX", "command=reboot status=ok reboot=now");
        k_msleep(50);
        sys_reboot(SYS_REBOOT_COLD);
        ret = 0;
    } else if (strcmp(command, "bootloader") == 0) {
        ret = high_debug_request_bootloader();
    } else {
        high_debug_log_event("COMMAND_RESULT_TX",
                             "command=%s status=unsupported reason=unknown_command",
                             command);
        HIGH_DEBUG_COUNTER_INC(command_result_tx);
        return -EINVAL;
    }

    high_debug_log_event("COMMAND_RESULT_TX",
                         "command=%s status=%s ret=%d",
                         command,
                         ret == 0 ? "ok" : "failed",
                         ret);
    HIGH_DEBUG_COUNTER_INC(command_result_tx);
    return ret;
}

int high_debug_stage0_simulated_click(void)
{
    const uint32_t ble_test_ms = 10000u;
    uint32_t event_seq;
    int ret = 0;
    int ble_ret;

    ret = app_click_event_sequence_next(&event_seq);
    if (ret < 0) {
        return ret;
    }
    high_debug_log_event("COMMAND_RX",
                         "source=button action=simulated_click event_seq=%u",
                         event_seq);
    status_leds_set(false, true, true);
    k_msleep(80);
    status_leds_set(false, false, false);
    ble_ret = high_debug_stage0_ble_advertise_test(event_seq, ble_test_ms);
    high_debug_log_event("BLE_TEST",
                         "action=advertise_test ret=%d duration_ms=%u",
                         ble_ret,
                         ble_test_ms);
    high_debug_log_event("RANGE_OK",
                         "BENCH_ONLY simulated=1 event_seq=%u anchor_required=0",
                         event_seq);
    if (IS_ENABLED(CONFIG_IMEC_STAGE0_SEND_WAKE_CLAIM_ON_CLICK)) {
        ret = high_debug_send_wake_train_command();
    }
    return ret;
}

int high_debug_request_bootloader(void)
{
    HIGH_DEBUG_COUNTER_INC(bootloader_entry_requests);
#if defined(CONFIG_RETENTION_BOOT_MODE)
    int ret = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);

    if (ret < 0) {
        high_debug_log_event("BOOTLOADER_READY",
                             "request=failed ret=%d method=retention-boot-mode",
                             ret);
        return ret;
    }
    high_debug_log_event("BOOTLOADER_READY",
                         "request=armed method=retention-boot-mode reboot=now");
    k_msleep(50);
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
#else
    high_debug_log_event("BOOTLOADER_READY",
                         "request=manual-only reason=retention_boot_mode_not_enabled");
    return -ENOTSUP;
#endif
}

static void high_debug_counter_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    high_debug_dump_counters("HEARTBEAT_TX");
    (void)k_work_reschedule(&high_debug_counter_work,
                             K_MSEC(CONFIG_IMEC_HIGH_DEBUG_COUNTER_PERIOD_MS));
}

static void high_debug_command_work_handler(struct k_work *work)
{
    unsigned char byte;

    ARG_UNUSED(work);

    if (!app_high_debug_command_poll_enabled()) {
        return;
    }

    while (debug_serial_poll_in(&byte) == 0) {
        if (byte == '\r' || byte == '\n') {
            if (high_debug_command_len > 0u) {
                high_debug_command_buf[high_debug_command_len] = '\0';
                if (high_debug_callbacks.handle_command != NULL) {
                    (void)high_debug_callbacks.handle_command(high_debug_command_buf);
                }
                high_debug_command_len = 0u;
            }
            continue;
        }
        if (byte == '\b' || byte == 0x7fu) {
            if (high_debug_command_len > 0u) {
                high_debug_command_len--;
            }
            continue;
        }
        if (high_debug_command_len + 1u < sizeof(high_debug_command_buf)) {
            high_debug_command_buf[high_debug_command_len++] = (char)byte;
        } else {
            high_debug_command_len = 0u;
            high_debug_log_event("COMMAND_RESULT_TX",
                                 "status=failed reason=line_too_long");
        }
    }
    (void)k_work_reschedule(&high_debug_command_work,
                            K_MSEC(CONFIG_IMEC_HIGH_DEBUG_COMMAND_POLL_MS));
}
#else
int app_high_debug_init(void)
{
    return 0;
}
#endif

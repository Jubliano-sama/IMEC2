#include "app_ml.h"

#include "app_board.h"
#include "app_clicker.h"
#include "app_config.h"
#include "app_gateway_ble.h"
#include "app_high_debug.h"
#include "app_state.h"
#include "dwm3000_driver.h"
#include "gateway_command.h"
#include "protocol.h"
#include "report.h"
#include "serial_frame.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_ml, LOG_LEVEL_DBG);

#if defined(CONFIG_IMEC_ML_CLICKER)
struct ml_clicker_request {
    struct proto_packet command;
    enum command_id command_id;
    uint64_t host_id;
    uint8_t samples_per_anchor;
    uint8_t max_anchor_count;
    uint8_t discovery_slot_count;
    bool range_only;
    bool allow_cached_discovery;
    bool anchor_pair_survey;
    bool live_tracking;
    uint32_t live_watchdog_ms;
};

struct ml_clicker_anchor_cache_entry {
    uint64_t anchor_id;
    int64_t last_found_ms;
    int64_t last_ranged_ms;
    uint8_t anchor_slot;
    uint8_t rx_quality;
};

struct ml_clicker_buffered_frame {
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    uint16_t len;
};

struct ml_clicker_buffered_sample {
    uint32_t timestamp_delta_ms;
    int32_t distance_mm;
    uint16_t sample_index;
    int8_t rsl_dbm;
    uint8_t quality;
    uint8_t range_status;
    uint8_t round_index;
    bool rsl_sampled;
};

struct ml_clicker_post_burst_diagnostic {
    struct dwm3000_range_result result;
    uint8_t anchor_full_cir[DWM3000_FULL_CIR_BYTES];
    uint8_t entry_index;
    bool valid;
};

struct ml_clicker_anchor_pair_result {
    struct uwb_anchor_pair_result_frame result;
    uint64_t timestamp_ms;
    bool valid;
};

struct ml_clicker_runtime {
    struct ml_clicker_request request;
    uint32_t event_seq;
    uint32_t burst_id;
    uint64_t timestamp_base_ms;
    uint16_t next_packet_seq;
    uint32_t emitted_samples;
    uint16_t notify_failures;
    uint16_t attempted_ranges;
    uint16_t selected_anchors;
    uint16_t buffered_frames;
    uint16_t buffered_samples;
    uint16_t flushed_frames;
    uint16_t post_burst_diagnostics;
    uint16_t anchor_pair_results;
    bool cached_discovery_used;
    struct ml_clicker_buffered_frame buffered_frames_storage[ML_CLICKER_BUFFERED_FRAME_MAX];
    struct ml_clicker_buffered_sample buffered_samples_storage[ML_CLICKER_BUFFERED_SAMPLE_MAX];
    struct ml_clicker_post_burst_diagnostic
        post_burst_diagnostics_storage[ML_CLICKER_POST_BURST_DIAG_MAX];
    struct ml_clicker_anchor_pair_result
        anchor_pair_results_storage[UWB_ANCHOR_PAIR_SURVEY_MAX_PAIRS];
    bool active;
};
#endif

#if defined(CONFIG_IMEC_ML_CLICKER)
static struct k_work ml_clicker_collect_work;
static struct k_work_delayable ml_clicker_rainbow_led_work;
static const struct app_clicker_attempt_gate_config clicker_attempt_gate_config = {
    .wake_adv_ms = WAKE_ADV_MS,
    .max_politeness_wait_ms = MAX_POLITENESS_WAIT_MS,
    .polite_sample_rx_ms = UWB_POLITE_SAMPLE_RX_MS,
    .polite_sample_period_ms = UWB_POLITE_SAMPLE_PERIOD_MS,
    .polite_required_quiet_samples = UWB_POLITE_REQUIRED_QUIET_SAMPLES,
    .polite_relevant_frame_wait_ms = UWB_POLITE_RELEVANT_FRAME_WAIT_MS,
    .ble_courtesy_min_window_ms = BLE_COURTESY_MIN_WINDOW_MS,
    .ble_courtesy_peer_finish_ms = BLE_COURTESY_PEER_FINISH_MS,
    .ble_courtesy_max_defers_per_attempt = BLE_COURTESY_MAX_DEFERS_PER_ATTEMPT,
    .ble_courtesy_poll_sleep_ms = BLE_COURTESY_POLL_SLEEP_MS,
};
static const struct app_clicker_wake_train_config clicker_wake_train_config = {
    .wake_adv_ms = WAKE_ADV_MS,
    .post_wake_claimed_duration_ms = UWB_POST_WAKE_CLAIMED_DURATION_MS,
    .control_tx_timeout_ms = UWB_CONTROL_TX_TIMEOUT_MS,
};
static atomic_t ml_clicker_busy;
static atomic_t ml_clicker_live_heartbeat_ms;
static atomic_t ml_clicker_live_stop_requested;
static struct ml_clicker_request ml_clicker_pending_request;
static struct ml_clicker_runtime ml_clicker_runtime;
static struct ml_clicker_anchor_cache_entry
    ml_clicker_anchor_cache[UWB_RANGE_SCHEDULE_MAX_ANCHORS];
static uint8_t ml_clicker_discovery_slot_override;
static uint8_t ml_clicker_rainbow_phase;
#endif
#if defined(CONFIG_IMEC_ML_ANCHOR)
static struct k_work_delayable ml_anchor_battery_led_work;
static uint8_t ml_anchor_full_cir_buffer[DWM3000_FULL_CIR_BYTES];
static uint16_t ml_anchor_battery_mv;
static bool ml_anchor_battery_led_on;
#endif

#if defined(CONFIG_IMEC_ML_CLICKER)
static void ml_clicker_collect_work_handler(struct k_work *work);
static void ml_clicker_rainbow_led_work_handler(struct k_work *work);
void ml_clicker_handle_ble_frame(const uint8_t *frame, size_t frame_len);
uint8_t ml_clicker_discovery_slot_count_override(void);
void ml_clicker_cache_note_discovery_reply(
    const struct uwb_discovery_reply_frame *reply);
static void ml_clicker_cache_note_range_result(uint64_t anchor_id,
                                               enum range_status status);
int ml_clicker_seed_cached_anchors(struct uwb_clicker_session *session,
                                          uint8_t max_anchor_count);
static uint16_t ml_clicker_next_packet_seq(void);
static int ml_clicker_run_anchor_pair_survey(struct uwb_clicker_session *session,
                                             uint64_t priority_id,
                                             int64_t click_deadline_ms);
int ml_clicker_emit_range_sample_if_active(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule,
    const struct uwb_range_step *step,
    const struct dwm3000_range_result *range_result);
void ml_clicker_run_post_burst_diagnostics(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule,
    int64_t schedule_tx_ms,
    int64_t click_deadline_ms);
static void ml_clicker_emit_stored_post_burst_diagnostics(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule);
static void ml_clicker_emit_stored_anchor_pair_results(void);
bool ml_clicker_continue_after_range_start_failure(void);
bool ml_clicker_should_continue_ranging(void);
void ml_clicker_enter_range_quiet(void);
void ml_clicker_exit_range_quiet(void);
static enum command_status ml_clicker_run_live_tracking(
    const struct ml_clicker_request *request);
#endif
#if defined(CONFIG_IMEC_ML_ANCHOR)
static void ml_anchor_battery_led_work_handler(struct k_work *work);
uint16_t ml_anchor_cached_battery_mv(void);
#endif
#if defined(CONFIG_IMEC_ML_CLICKER)
#define ML_CLICKER_RAINBOW_LED_PERIOD_MS 160u
#define ML_CLICKER_LIVE_DEFAULT_WATCHDOG_MS 3000u
#define ML_CLICKER_LIVE_MIN_WATCHDOG_MS 500u
#define ML_CLICKER_LIVE_MAX_WATCHDOG_MS 10000u

static void ml_clicker_rainbow_set_led(uint8_t phase, bool led1)
{
    bool red = false;
    bool green = false;
    bool blue = false;

    switch (phase % 6u) {
    case 0u:
        red = true;
        break;
    case 1u:
        red = true;
        green = true;
        break;
    case 2u:
        green = true;
        break;
    case 3u:
        green = true;
        blue = true;
        break;
    case 4u:
        blue = true;
        break;
    default:
        red = true;
        blue = true;
        break;
    }

    if (led1) {
        status_led1_set(red, green, blue);
    } else {
        status_led0_set(red, green, blue);
    }
}

static void ml_clicker_rainbow_led_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_CLICKER || !IS_ENABLED(CONFIG_IMEC_ML_CLICKER)) {
        return;
    }

    ml_clicker_rainbow_set_led(ml_clicker_rainbow_phase, false);
    ml_clicker_rainbow_set_led((uint8_t)(ml_clicker_rainbow_phase + 3u), true);
    ml_clicker_rainbow_phase++;
    (void)k_work_reschedule(&ml_clicker_rainbow_led_work,
                            K_MSEC(ML_CLICKER_RAINBOW_LED_PERIOD_MS));
}
#endif
#if defined(CONFIG_IMEC_ML_ANCHOR)
#define ML_ANCHOR_BATTERY_LED_ON_MS 100u
#define ML_ANCHOR_BATTERY_LED_PERIOD_MS 1000u
#define ML_ANCHOR_BATTERY_RED_MAX_MV 3450u
#define ML_ANCHOR_BATTERY_GREEN_MIN_MV 3850u

static void ml_anchor_battery_led_set_for_mv(uint16_t battery_mv)
{
    if (battery_mv == 0u || battery_mv <= ML_ANCHOR_BATTERY_RED_MAX_MV) {
        status_led0_set(true, false, false);
    } else if (battery_mv < ML_ANCHOR_BATTERY_GREEN_MIN_MV) {
        status_led0_set(false, false, true);
    } else {
        status_led0_set(false, true, false);
    }
}

static void ml_anchor_battery_led_work_handler(struct k_work *work)
{
    uint16_t sampled_mv = 0u;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !IS_ENABLED(CONFIG_IMEC_ML_ANCHOR)) {
        return;
    }

    if (!ml_anchor_battery_led_on) {
        ret = battery_sample_lithium_mv(&sampled_mv);
        if (ret == 0) {
            ml_anchor_battery_mv = sampled_mv;
        } else {
            LOG_WRN("ML anchor battery ADC sample failed: %d", ret);
        }
        ml_anchor_battery_led_set_for_mv(ml_anchor_battery_mv);
        ml_anchor_battery_led_on = true;
        (void)k_work_reschedule(&ml_anchor_battery_led_work,
                                K_MSEC(ML_ANCHOR_BATTERY_LED_ON_MS));
        return;
    }

    status_led0_set(false, false, false);
    ml_anchor_battery_led_on = false;
    (void)k_work_reschedule(
        &ml_anchor_battery_led_work,
        K_MSEC(ML_ANCHOR_BATTERY_LED_PERIOD_MS - ML_ANCHOR_BATTERY_LED_ON_MS));
}

uint16_t ml_anchor_cached_battery_mv(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR ? ml_anchor_battery_mv : 0u;
}
uint8_t ml_anchor_discovery_slot(uint8_t discovery_slot_count)
{
    if (discovery_slot_count == 0u) {
        return 0u;
    }
    return (uint8_t)(IMEC_ML_ANCHOR_SLOT % discovery_slot_count);
}

#endif

#if defined(CONFIG_IMEC_ML_ANCHOR)
uint32_t ml_anchor_run_post_burst_diagnostics(
    const struct uwb_range_schedule_frame *schedule,
    int64_t schedule_rx_ms,
    size_t total_samples)
{
    uint32_t retained_sleep_us = 0u;

    if (schedule == NULL) {
        return 0u;
    }

    for (uint8_t entry_index = 0u; entry_index < schedule->selected_count; entry_index++) {
        const struct uwb_range_schedule_entry *entry = &schedule->entries[entry_index];
        struct dwm3000_range_request expected;
        struct dwm3000_range_result range_result;
        int64_t target_us;
        int64_t listen_start_ms;
        int64_t listen_deadline_ms;
        int ret = -ETIMEDOUT;

        if (entry->anchor_id != DEVICE_ID) {
            continue;
        }

        target_us = scheduled_post_burst_diag_target_us(schedule_rx_ms,
                                                        schedule,
                                                        total_samples,
                                                        entry_index);
        listen_start_ms = (target_us / 1000) - UWB_POST_BURST_DIAG_RX_GUARD_MS;
        listen_deadline_ms = listen_start_ms + UWB_POST_BURST_DIAG_LISTEN_MS;
        retained_sleep_us = u32_saturating_add(
            retained_sleep_us,
            sleep_with_uwb_idle_until_ms(listen_start_ms));

        memset(&expected, 0, sizeof(expected));
        memset(&range_result, 0, sizeof(range_result));
        range_result.status = RANGE_RX_TIMEOUT;
        expected.initiator_id = schedule->clicker_id;
        expected.responder_id = DEVICE_ID;
        expected.network_id = schedule->network_id;
        expected.session_nonce = schedule->nonce;
        expected.responder_short_addr = local_uwb_short_addr();
        expected.session_id = schedule->click_event_id;
        expected.seq = scheduled_post_burst_diag_seq(entry);
        expected.round_index = UWB_POST_BURST_DIAG_ROUND_INDEX;
        expected.flags = schedule->flags;
        expected.timeout_ms = UWB_POST_BURST_DIAG_LISTEN_MS;
        expected.reply_delay_uus = UWB_POST_BURST_DIAG_REPLY_DELAY_UUS;
        expected.capture_rsl = true;
        expected.skip_responder_report = true;
        expected.expect_clicker_diag = false;
        expected.send_anchor_diag = true;
        expected.send_anchor_diag_fragments = true;
        expected.anchor_full_cir = ml_anchor_full_cir_buffer;
        expected.anchor_full_cir_cap = sizeof(ml_anchor_full_cir_buffer);

        LOG_INF("anchor post-burst diagnostic listen: clicker=0x%016llx event_seq=%u anchor_slot=%u seq=%u timeout_ms=%u",
                (unsigned long long)schedule->clicker_id,
                schedule->click_event_id,
                entry_index,
                expected.seq,
                expected.timeout_ms);
        high_debug_log_event("POST_BURST_DIAG_RX",
                             "listen=1 clicker=0x%016llx event_seq=%u attempt=%u anchor_slot=%u seq=%u target_us=%lld listen_start_ms=%lld timeout_ms=%u",
                             (unsigned long long)schedule->clicker_id,
                             schedule->click_event_id,
                             schedule->attempt_index,
                             entry_index,
                             expected.seq,
                             (long long)target_us,
                             (long long)listen_start_ms,
                             expected.timeout_ms);

        while (k_uptime_get() < listen_deadline_ms) {
            int64_t remaining_ms = listen_deadline_ms - k_uptime_get();

            memset(&range_result, 0, sizeof(range_result));
            range_result.status = RANGE_RX_TIMEOUT;
            expected.timeout_ms = (uint32_t)MAX(1, remaining_ms);
            ret = dwm3000_driver_responder_poll_expected(DEVICE_ID,
                                                         &expected,
                                                         expected.timeout_ms,
                                                         &range_result);
            if (ret == -EAGAIN) {
                LOG_DBG("anchor post-burst diagnostic ignored pre-POLL frame: clicker=0x%016llx event_seq=%u seq=%u status=%s(%u)",
                        (unsigned long long)schedule->clicker_id,
                        schedule->click_event_id,
                        expected.seq,
                        range_status_name(range_result.status),
                        range_result.status);
                high_debug_log_event("POST_BURST_DIAG_RX",
                                     "ignored=1 clicker=0x%016llx event_seq=%u seq=%u status=%u remaining_ms=%lld",
                                     (unsigned long long)schedule->clicker_id,
                                     schedule->click_event_id,
                                     expected.seq,
                                     range_result.status,
                                     (long long)remaining_ms);
                continue;
            }
            break;
        }
        if (ret == 0 && range_result.exchange_started) {
            LOG_INF("anchor post-burst diagnostic complete: clicker=0x%016llx event_seq=%u seq=%u status=%s(%u) rsl_present=%u cir_present=%u clicker_diag=%u",
                    (unsigned long long)range_result.initiator_id,
                    range_result.session_id,
                    range_result.seq,
                    range_status_name(range_result.status),
                    range_result.status,
                    range_result.rsl_sampled ? 1u : 0u,
                    range_result.cir_sampled ? 1u : 0u,
                    range_result.clicker_diag_received ? 1u : 0u);
            high_debug_log_event("POST_BURST_DIAG_RX",
                                 "result=ok clicker=0x%016llx event_seq=%u seq=%u status=%u rsl_present=%u cir_present=%u clicker_diag=%u",
                                 (unsigned long long)range_result.initiator_id,
                                 range_result.session_id,
                                 range_result.seq,
                                 range_result.status,
                                 range_result.rsl_sampled ? 1u : 0u,
                                 range_result.cir_sampled ? 1u : 0u,
                                 range_result.clicker_diag_received ? 1u : 0u);
        } else {
            LOG_WRN("anchor post-burst diagnostic failed: clicker=0x%016llx event_seq=%u seq=%u ret=%d status=%s(%u) exchange_started=%u",
                    (unsigned long long)schedule->clicker_id,
                    schedule->click_event_id,
                    expected.seq,
                    ret,
                    range_status_name(range_result.status),
                    range_result.status,
                    range_result.exchange_started ? 1u : 0u);
            high_debug_log_event("POST_BURST_DIAG_RX",
                                 "result=fail clicker=0x%016llx event_seq=%u seq=%u ret=%d status=%u exchange_started=%u",
                                 (unsigned long long)schedule->clicker_id,
                                 schedule->click_event_id,
                                 expected.seq,
                                 ret,
                                 range_result.status,
                                 range_result.exchange_started ? 1u : 0u);
        }
    }

    return retained_sleep_us;
}
#endif

#if defined(CONFIG_IMEC_ML_CLICKER)
int ml_clicker_relax_range_schedule(struct uwb_range_schedule_frame *schedule,
                                           bool post_burst_diagnostics)
{
    size_t total_samples;
    uint32_t burst_window_ms;

    if (schedule == NULL) {
        return -EINVAL;
    }

    total_samples = (size_t)schedule->selected_count * schedule->samples_per_anchor;
    if (total_samples == 0u || total_samples > UINT16_MAX) {
        return -EINVAL;
    }

    schedule->exchange_stride_us = UWB_ML_EXCHANGE_STRIDE_US;
    schedule->max_exchanges = (uint16_t)total_samples;
    schedule->diagnostics_required = post_burst_diagnostics ?
                                     UWB_RANGE_SCHEDULE_DIAGNOSTICS_REQUIRED :
                                     UWB_RANGE_SCHEDULE_DIAGNOSTICS_OMITTED;
    burst_window_ms = (uint32_t)ceil_us_to_ms((int64_t)total_samples *
                                             UWB_ML_EXCHANGE_STRIDE_US);
    if (burst_window_ms > UINT16_MAX) {
        return -EINVAL;
    }
    schedule->burst_window_ms = (uint16_t)MAX(burst_window_ms,
                                              UWB_RANGE_SCHEDULE_MIN_BURST_WINDOW_MS);
    if (uwb_validate_range_schedule(schedule) != PROTO_OK) {
        return -EINVAL;
    }

    LOG_INF("ML range schedule relaxed: selected=%u total_samples=%u stride_us=%u burst_ms=%u max_exchanges=%u diagnostics_required=%u",
            schedule->selected_count,
            (unsigned int)total_samples,
            schedule->exchange_stride_us,
            schedule->burst_window_ms,
            schedule->max_exchanges,
            schedule->diagnostics_required);
    return 0;
}
#endif


#if defined(CONFIG_IMEC_ML_CLICKER)
uint8_t ml_clicker_discovery_slot_count_override(void)
{
    return ml_clicker_discovery_slot_override;
}

bool ml_clicker_continue_after_range_start_failure(void)
{
    return ml_clicker_runtime.active && ml_clicker_should_continue_ranging();
}

void ml_clicker_note_cached_discovery_used(void)
{
    ml_clicker_runtime.cached_discovery_used = true;
}

bool ml_clicker_runtime_active(void)
{
    return ml_clicker_runtime.active;
}

static bool ml_clicker_live_tracking_active(void)
{
    return ml_clicker_runtime.active &&
           ml_clicker_runtime.request.live_tracking &&
           atomic_get(&ml_clicker_busy) != 0;
}

static void ml_clicker_live_tracking_touch(void)
{
    atomic_set(&ml_clicker_live_heartbeat_ms, (atomic_val_t)k_uptime_get_32());
}

static bool ml_clicker_live_tracking_heartbeat_ok(uint32_t now_ms,
                                                  uint32_t watchdog_ms)
{
    uint32_t last_ms = (uint32_t)atomic_get(&ml_clicker_live_heartbeat_ms);

    if (watchdog_ms == 0u) {
        return false;
    }
    return (uint32_t)(now_ms - last_ms) <= watchdog_ms;
}

bool ml_clicker_should_continue_ranging(void)
{
    if (!ml_clicker_live_tracking_active()) {
        return true;
    }
    if (atomic_get(&ml_clicker_live_stop_requested) != 0) {
        return false;
    }
    return ml_clicker_live_tracking_heartbeat_ok(
        k_uptime_get_32(),
        ml_clicker_runtime.request.live_watchdog_ms);
}

void ml_clicker_enter_range_quiet(void)
{
    if (ml_clicker_runtime.active) {
        gateway_ble_enter_uwb_quiet("ml-clicker-range-exchange");
    }
}

void ml_clicker_exit_range_quiet(void)
{
    if (gateway_ble_uwb_quiet_active()) {
        gateway_ble_exit_uwb_quiet("ml-clicker-range-exchange");
    }
}

static struct ml_clicker_anchor_cache_entry *ml_clicker_cache_entry_for(uint64_t anchor_id)
{
    struct ml_clicker_anchor_cache_entry *empty = NULL;

    if (anchor_id == 0u) {
        return NULL;
    }

    for (uint8_t i = 0u; i < ARRAY_SIZE(ml_clicker_anchor_cache); i++) {
        struct ml_clicker_anchor_cache_entry *entry = &ml_clicker_anchor_cache[i];

        if (entry->anchor_id == anchor_id) {
            return entry;
        }
        if (empty == NULL && entry->anchor_id == 0u) {
            empty = entry;
        }
    }

    return empty;
}

void ml_clicker_cache_note_discovery_reply(
    const struct uwb_discovery_reply_frame *reply)
{
    struct ml_clicker_anchor_cache_entry *entry;

    if (reply == NULL ||
        reply->status != UWB_DISCOVERY_REPLY_PRESENT ||
        reply->anchor_id == 0u ||
        reply->anchor_slot >= UWB_DISCOVERY_SLOT_COUNT ||
        reply->rx_quality > 100u) {
        return;
    }

    entry = ml_clicker_cache_entry_for(reply->anchor_id);
    if (entry == NULL) {
        return;
    }
    entry->anchor_id = reply->anchor_id;
    entry->last_found_ms = k_uptime_get();
    entry->anchor_slot = reply->anchor_slot;
    entry->rx_quality = reply->rx_quality;
}

static void ml_clicker_cache_note_range_result(uint64_t anchor_id,
                                               enum range_status status)
{
    struct ml_clicker_anchor_cache_entry *entry;

    if (status != RANGE_OK) {
        return;
    }
    entry = ml_clicker_cache_entry_for(anchor_id);
    if (entry == NULL || entry->anchor_id == 0u) {
        return;
    }
    entry->last_ranged_ms = k_uptime_get();
}

static bool ml_clicker_cache_entry_fresh(
    const struct ml_clicker_anchor_cache_entry *entry,
    int64_t now_ms)
{
    if (entry == NULL ||
        entry->anchor_id == 0u ||
        entry->last_ranged_ms <= 0) {
        return false;
    }
    return now_ms - entry->last_ranged_ms <= ML_CLICKER_FAST_CACHE_FRESH_MS;
}

int ml_clicker_seed_cached_anchors(struct uwb_clicker_session *session,
                                          uint8_t max_anchor_count)
{
    int64_t now_ms = k_uptime_get();
    uint8_t fresh_count = 0u;
    uint8_t seeded_count = 0u;

    if (session == NULL || max_anchor_count == 0u) {
        return -EINVAL;
    }

    for (uint8_t i = 0u; i < ARRAY_SIZE(ml_clicker_anchor_cache); i++) {
        if (ml_clicker_cache_entry_fresh(&ml_clicker_anchor_cache[i], now_ms)) {
            fresh_count++;
        }
    }
    if (fresh_count < ML_CLICKER_FAST_CACHE_MIN_ANCHORS) {
        return -ENOENT;
    }

    for (uint8_t i = 0u;
         i < ARRAY_SIZE(ml_clicker_anchor_cache) && seeded_count < max_anchor_count;
         i++) {
        const struct ml_clicker_anchor_cache_entry *entry = &ml_clicker_anchor_cache[i];
        int ret;

        if (!ml_clicker_cache_entry_fresh(entry, now_ms)) {
            continue;
        }
        ret = uwb_clicker_seed_discovered_anchor(session,
                                                 entry->anchor_id,
                                                 entry->anchor_slot,
                                                 entry->rx_quality);
        if (ret != PROTO_OK) {
            LOG_WRN("ML cached anchor seed failed: anchor=0x%016llx slot=%u quality=%u ret=%d",
                    (unsigned long long)entry->anchor_id,
                    entry->anchor_slot,
                    entry->rx_quality,
                    ret);
            continue;
        }
        seeded_count++;
    }

    return seeded_count >= ML_CLICKER_FAST_CACHE_MIN_ANCHORS ?
           (int)seeded_count : -ENOENT;
}

static int ml_clicker_build_anchor_pair_schedule(
    const struct uwb_clicker_session *session,
    struct uwb_anchor_pair_schedule_frame *schedule)
{
    bool used[UWB_SESSION_DISCOVERY_CAPACITY] = {0};
    uint8_t selected_count;

    if (session == NULL || schedule == NULL) {
        return -EINVAL;
    }
    if (session->candidate_count < UWB_ANCHOR_PAIR_SCHEDULE_MIN_ANCHORS) {
        return -ETIMEDOUT;
    }

    selected_count = MIN(session->candidate_count, session->config.max_anchor_count);
    if (selected_count < UWB_ANCHOR_PAIR_SCHEDULE_MIN_ANCHORS) {
        return -ETIMEDOUT;
    }

    memset(schedule, 0, sizeof(*schedule));
    schedule->network_id = session->config.network_id;
    schedule->clicker_id = session->config.clicker_id;
    schedule->survey_id = session->config.click_event_id;
    schedule->attempt_index = session->attempt_index;
    schedule->nonce = session->config.nonce;
    schedule->anchor_count = selected_count;
    schedule->pair_count = uwb_anchor_pair_count(selected_count);
    schedule->ranging_channel = session->config.ranging_channel;
    schedule->first_pair_delay_ms = UWB_ANCHOR_PAIR_SURVEY_DEFAULT_FIRST_DELAY_MS;
    schedule->pair_stride_ms = UWB_ANCHOR_PAIR_SURVEY_DEFAULT_STRIDE_MS;
    schedule->pair_window_ms = UWB_ANCHOR_PAIR_SURVEY_DEFAULT_WINDOW_MS;
    schedule->reply_delay_us = UWB_RANGE_REPLY_DELAY_UUS;
    schedule->flags = session->config.flags;

    for (uint8_t out_index = 0u; out_index < selected_count; out_index++) {
        int selected = -1;

        for (uint8_t i = 0u; i < session->candidate_count; i++) {
            const struct uwb_anchor_candidate *candidate = &session->candidates[i];

            if (used[i]) {
                continue;
            }
            if (selected < 0 ||
                candidate->anchor_slot < session->candidates[selected].anchor_slot ||
                (candidate->anchor_slot == session->candidates[selected].anchor_slot &&
                 candidate->anchor_id < session->candidates[selected].anchor_id)) {
                selected = (int)i;
            }
        }
        if (selected < 0) {
            return -EINVAL;
        }
        used[selected] = true;
        schedule->anchor_ids[out_index] = session->candidates[selected].anchor_id;
        schedule->anchor_start_delay_ms[out_index] =
            UWB_ANCHOR_PAIR_SURVEY_DEFAULT_FIRST_DELAY_MS;
    }

    return uwb_anchor_pair_schedule_encoded_len(schedule->anchor_count) == 0u ?
           -EINVAL : 0;
}

static int ml_clicker_send_anchor_pair_schedule(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    int64_t *schedule_tx_ms)
{
    uint8_t frame[UWB_ANCHOR_PAIR_SCHEDULE_MAX_LEN];
    size_t frame_len = 0u;
    int ret;

    if (schedule_tx_ms != NULL) {
        *schedule_tx_ms = k_uptime_get();
    }

    ret = uwb_encode_anchor_pair_schedule(schedule, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = radio_guard_uwb_start("clicker UWB ANCHOR_PAIR_SCHEDULE");
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        ret = dwm3000_driver_send_frame(frame, frame_len, UWB_CONTROL_TX_TIMEOUT_MS);
    }
    (void)dwm3000_driver_idle();
    radio_guard_uwb_stop();
    if (ret < 0) {
        LOG_WRN("clicker anchor-pair schedule TX failed: survey=%u ret=%d",
                schedule->survey_id,
                ret);
        return ret;
    }
    if (schedule_tx_ms != NULL) {
        *schedule_tx_ms = k_uptime_get();
    }
    LOG_INF("clicker anchor-pair schedule TX complete: survey=%u anchors=%u pairs=%u schedule_start_ms=%u stride_ms=%u window_ms=%u rx_guard_ms=%u",
            schedule->survey_id,
            schedule->anchor_count,
            schedule->pair_count,
            schedule->first_pair_delay_ms,
            schedule->pair_stride_ms,
            schedule->pair_window_ms,
            UWB_ANCHOR_PAIR_SURVEY_RX_EARLY_GUARD_MS);
    return 0;
}

static bool ml_clicker_anchor_pair_result_matches(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    const struct uwb_anchor_pair_result_frame *result)
{
    uint64_t initiator_id = 0u;
    uint64_t responder_id = 0u;

    if (schedule == NULL || result == NULL ||
        result->network_id != schedule->network_id ||
        result->clicker_id != schedule->clicker_id ||
        result->survey_id != schedule->survey_id ||
        result->nonce != schedule->nonce ||
        result->pair_count != schedule->pair_count ||
        result->pair_index >= schedule->pair_count) {
        return false;
    }
    if (uwb_anchor_pair_at(schedule,
                           result->pair_index,
                           &initiator_id,
                           &responder_id) != PROTO_OK) {
        return false;
    }
    return result->initiator_id == initiator_id &&
           result->responder_id == responder_id;
}

static uint16_t ml_clicker_anchor_pair_max_start_delay_ms(
    const struct uwb_anchor_pair_schedule_frame *schedule)
{
    uint16_t max_delay_ms = 0u;

    if (schedule == NULL) {
        return 0u;
    }
    for (uint8_t i = 0u; i < schedule->anchor_count; i++) {
        max_delay_ms = MAX(max_delay_ms, schedule->anchor_start_delay_ms[i]);
    }
    return max_delay_ms == 0u ? schedule->first_pair_delay_ms : max_delay_ms;
}

static int ml_clicker_receive_anchor_pair_results(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    int64_t schedule_tx_ms,
    int64_t click_deadline_ms)
{
    uint8_t frame[UWB_ANCHOR_PAIR_RESULT_LEN];
    size_t frame_len = 0u;
    uint16_t received_count = 0u;
    int64_t schedule_end_ms;
    int64_t rx_deadline_ms;
    int ret;

    if (schedule == NULL || schedule->pair_count > UWB_ANCHOR_PAIR_SURVEY_MAX_PAIRS) {
        return -EINVAL;
    }

    schedule_end_ms = schedule_tx_ms +
                      ml_clicker_anchor_pair_max_start_delay_ms(schedule) +
                      ((int64_t)schedule->pair_count * schedule->pair_stride_ms) +
                      schedule->pair_window_ms + 1000;
    rx_deadline_ms = MIN(schedule_end_ms,
                         click_deadline_ms - CLICK_REPORT_BUILD_GUARD_MS);
    if (rx_deadline_ms <= k_uptime_get()) {
        return -ETIMEDOUT;
    }

    ret = radio_guard_uwb_start("clicker anchor-pair result RX");
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_configure_range_mode();
    if (ret < 0) {
        radio_guard_uwb_stop();
        return ret;
    }

    while (received_count < schedule->pair_count &&
           k_uptime_get() < rx_deadline_ms) {
        struct uwb_anchor_pair_result_frame result = {0};
        int64_t remaining_ms = rx_deadline_ms - k_uptime_get();

        frame_len = 0u;
        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      frame,
                                                      sizeof(frame),
                                                      &frame_len,
                                                      NULL,
                                                      NULL,
                                                      NULL);
        if (ret == -ETIMEDOUT) {
            break;
        }
        if (ret < 0) {
            continue;
        }
        ret = uwb_decode_anchor_pair_result(frame, frame_len, &result);
        if (ret != PROTO_OK) {
            continue;
        }
        if (!ml_clicker_anchor_pair_result_matches(schedule, &result)) {
            LOG_DBG("ML anchor-pair result ignored: survey=%u pair=%u ret=identity",
                    result.survey_id,
                    result.pair_index);
            continue;
        }
        if (ml_clicker_runtime.anchor_pair_results_storage[result.pair_index].valid) {
            continue;
        }
        ml_clicker_runtime.anchor_pair_results_storage[result.pair_index].result = result;
        ml_clicker_runtime.anchor_pair_results_storage[result.pair_index].timestamp_ms =
            (uint64_t)k_uptime_get();
        ml_clicker_runtime.anchor_pair_results_storage[result.pair_index].valid = true;
        ml_clicker_runtime.anchor_pair_results++;
        received_count++;
        LOG_INF("ML anchor-pair result RX: survey=%u pair=%u/%u initiator=0x%016llx responder=0x%016llx status=%s(%u) distance_mm=%d",
                result.survey_id,
                (unsigned int)(result.pair_index + 1u),
                result.pair_count,
                (unsigned long long)result.initiator_id,
                (unsigned long long)result.responder_id,
                range_status_name(result.status),
                result.status,
                result.distance_mm);
    }

    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();
    return received_count == schedule->pair_count ? 0 : -ETIMEDOUT;
}

static int ml_clicker_run_anchor_pair_survey(struct uwb_clicker_session *session,
                                             uint64_t priority_id,
                                             int64_t click_deadline_ms)
{
    struct uwb_anchor_pair_schedule_frame pair_schedule;
    int64_t schedule_tx_ms = 0;
    int ret;

    if (session == NULL) {
        return -EINVAL;
    }

    ret = app_clicker_send_wake_claim_train(session,
                                            priority_id,
                                            &clicker_wake_train_config);
    if (ret < 0) {
        return ret;
    }

    ret = app_clicker_discover_uwb_anchors(session);
    if (ret < 0) {
        return ret;
    }
    if (session->candidate_count < UWB_ANCHOR_PAIR_SCHEDULE_MIN_ANCHORS) {
        LOG_WRN("ML anchor-pair survey needs at least %u anchors: discovered=%u",
                UWB_ANCHOR_PAIR_SCHEDULE_MIN_ANCHORS,
                session->candidate_count);
        return -ETIMEDOUT;
    }

    ret = ml_clicker_build_anchor_pair_schedule(session, &pair_schedule);
    if (ret < 0) {
        return ret;
    }
    ml_clicker_runtime.selected_anchors = pair_schedule.anchor_count;
    ml_clicker_runtime.attempted_ranges = pair_schedule.pair_count;

    ret = ml_clicker_send_anchor_pair_schedule(&pair_schedule, &schedule_tx_ms);
    if (ret < 0) {
        return ret;
    }

    ret = ml_clicker_receive_anchor_pair_results(&pair_schedule,
                                                 schedule_tx_ms,
                                                 click_deadline_ms);
    LOG_INF("ML anchor-pair survey receive complete: survey=%u anchors=%u pairs=%u received=%u ret=%d",
            pair_schedule.survey_id,
            pair_schedule.anchor_count,
            pair_schedule.pair_count,
            ml_clicker_runtime.anchor_pair_results,
            ret);
    return ret;
}

static int ml_clicker_emit_or_buffer_host_packet(const struct proto_packet *packet,
                                                 const uint8_t *payload,
                                                 size_t payload_len)
{
    struct proto_packet frame_packet;
    struct ml_clicker_buffered_frame *buffered;
    size_t frame_len = 0u;
    int ret;

    if (!gateway_ble_uwb_quiet_active()) {
        return gateway_emit_host_packet(packet, payload, payload_len);
    }

    if (ml_clicker_runtime.buffered_frames >= ML_CLICKER_BUFFERED_FRAME_MAX) {
        return -ENOSPC;
    }

    buffered = &ml_clicker_runtime.buffered_frames_storage[ml_clicker_runtime.buffered_frames];
    ret = gateway_encode_host_packet_frame(packet,
                                           payload,
                                           payload_len,
                                           buffered->frame,
                                           sizeof(buffered->frame),
                                           &frame_len,
                                           &frame_packet);
    if (ret < 0) {
        return ret;
    }
    if (frame_len > UINT16_MAX) {
        return -EMSGSIZE;
    }

    buffered->len = (uint16_t)frame_len;
    ml_clicker_runtime.buffered_frames++;
    return 0;
}

static int ml_clicker_emit_range_sample_record(
    const struct ml_clicker_buffered_sample *sample,
    const struct uwb_range_schedule_frame *schedule)
{
    struct proto_packet packet = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t distance_sample_bytes[sizeof(int32_t)];
    uint8_t timestamp_bytes[sizeof(uint64_t)];
    size_t payload_len = 0u;
    uint8_t round_index;
    uint16_t sample_count;
    uint64_t anchor_id = 0u;
    uint64_t timestamp_ms;
    uint8_t seq = 0u;
    enum range_status status;
    int ret;

    if (sample == NULL || schedule == NULL) {
        return -EINVAL;
    }
    ret = uwb_range_schedule_sample_at(schedule,
                                       sample->sample_index,
                                       &anchor_id,
                                       &seq);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    if (anchor_id == 0u || anchor_id == DEVICE_ID || sample->quality > 100u) {
        return -EINVAL;
    }

    sample_count = (uint16_t)MIN(uwb_range_schedule_total_samples(schedule),
                                 (size_t)UINT16_MAX);
    if (sample_count == 0u || sample->sample_index >= sample_count) {
        return -EINVAL;
    }
    status = range_status_valid((enum range_status)sample->range_status) ?
             (enum range_status)sample->range_status : RANGE_INTERNAL_ERROR;
    round_index = sample->round_index;
    timestamp_ms = ml_clicker_runtime.timestamp_base_ms + sample->timestamp_delta_ms;

    proto_put_u32_le(distance_sample_bytes, (uint32_t)sample->distance_mm);
    proto_put_u64_le(timestamp_bytes, timestamp_ms);

    ret = tlv_append_u64(payload, sizeof(payload), &payload_len,
                         TLV_CLICKER_ID, DEVICE_ID);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u64(payload, sizeof(payload), &payload_len,
                         TLV_ANCHOR_ID, anchor_id);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u32(payload, sizeof(payload), &payload_len,
                         TLV_EVENT_SEQ, ml_clicker_runtime.event_seq);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u64(payload, sizeof(payload), &payload_len,
                         TLV_TIMESTAMP_MS, timestamp_ms);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_i32(payload, sizeof(payload), &payload_len,
                         TLV_DISTANCE_MM, sample->distance_mm);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                         TLV_SAMPLE_COUNT, sample_count);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                         TLV_SAMPLE_INDEX, sample->sample_index);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_bytes(payload, sizeof(payload), &payload_len,
                           TLV_DISTANCE_SAMPLES_MM,
                           distance_sample_bytes,
                           sizeof(distance_sample_bytes));
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_bytes(payload, sizeof(payload), &payload_len,
                           TLV_RANGE_ROUND_INDICES,
                           &round_index,
                           sizeof(round_index));
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_bytes(payload, sizeof(payload), &payload_len,
                           TLV_SEQUENCE_START_TIMESTAMPS_MS,
                           timestamp_bytes,
                           sizeof(timestamp_bytes));
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u8(payload, sizeof(payload), &payload_len,
                        TLV_QUALITY, sample->quality);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    if (sample->rsl_sampled) {
        ret = tlv_append_i8(payload, sizeof(payload), &payload_len,
                            TLV_UWB_RSL_DBM, sample->rsl_dbm);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
    }
    ret = tlv_append_u8(payload, sizeof(payload), &payload_len,
                        TLV_RANGE_STATUS, (uint8_t)status);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    packet.msg_type = MSG_CLICK_REPORT;
    packet.flags = FLAG_DIAGNOSTIC;
    packet.src_id = DEVICE_ID;
    packet.dst_id = ml_clicker_runtime.request.host_id == 0u ?
                    GATEWAY_ID : ml_clicker_runtime.request.host_id;
    packet.session_id = ml_clicker_runtime.event_seq;
    packet.seq = ml_clicker_runtime.next_packet_seq++;
    if (packet.seq == 0u) {
        packet.seq = ml_clicker_runtime.next_packet_seq++;
    }
    packet.ttl = 1u;
    packet.payload_len = (uint8_t)payload_len;

    return gateway_emit_host_packet(&packet, payload, payload_len);
}

static void ml_clicker_flush_buffered_frames(
    const struct uwb_range_schedule_frame *schedule)
{
    for (uint16_t i = 0u; i < ml_clicker_runtime.buffered_samples; i++) {
        const struct ml_clicker_buffered_sample *sample =
            &ml_clicker_runtime.buffered_samples_storage[i];
        int ret;

        ret = ml_clicker_emit_range_sample_record(sample, schedule);
        if (ret < 0) {
            ml_clicker_runtime.notify_failures++;
            LOG_WRN("ML buffered range sample notify failed: index=%u/%u ret=%d",
                    (unsigned int)(i + 1u),
                    ml_clicker_runtime.buffered_samples,
                    ret);
            continue;
        }

        ml_clicker_runtime.flushed_frames++;
    }
    ml_clicker_runtime.buffered_samples = 0u;

    for (uint16_t i = 0u; i < ml_clicker_runtime.buffered_frames; i++) {
        const struct ml_clicker_buffered_frame *buffered =
            &ml_clicker_runtime.buffered_frames_storage[i];
        int ret;

        ret = gateway_ble_send_packet_frame(buffered->frame, buffered->len);
        if (ret < 0) {
            ml_clicker_runtime.notify_failures++;
            LOG_WRN("ML buffered range sample notify failed: index=%u/%u ret=%d",
                    (unsigned int)(i + 1u),
                    ml_clicker_runtime.buffered_frames,
                    ret);
            continue;
        }

        ml_clicker_runtime.flushed_frames++;
        HIGH_DEBUG_COUNTER_INC(gateway_packets_emitted);
    }
    ml_clicker_runtime.buffered_frames = 0u;
}

static int ml_clicker_read_u8_tlv(const uint8_t *payload,
                                  size_t payload_len,
                                  uint8_t type,
                                  uint8_t default_value,
                                  uint8_t min_value,
                                  uint8_t max_value,
                                  uint8_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    if (value == NULL || min_value > max_value) {
        return -EINVAL;
    }

    *value = default_value;
    ret = tlv_find(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return 0;
    }
    if (ret != PROTO_OK || tlv_value == NULL) {
        return -EINVAL;
    }
    if (tlv_len == 1u) {
        *value = tlv_value[0];
    } else if (tlv_len == 2u) {
        uint16_t raw = proto_get_u16_le(tlv_value);

        if (raw > UINT8_MAX) {
            return -ERANGE;
        }
        *value = (uint8_t)raw;
    } else {
        return -EINVAL;
    }

    return *value >= min_value && *value <= max_value ? 0 : -ERANGE;
}

static int ml_clicker_read_live_watchdog_ms(const uint8_t *payload,
                                            size_t payload_len,
                                            uint32_t *watchdog_ms)
{
    uint32_t value = ML_CLICKER_LIVE_DEFAULT_WATCHDOG_MS;
    int ret;

    if (watchdog_ms == NULL) {
        return -EINVAL;
    }

    ret = gateway_command_extract_duration_ms(payload,
                                              payload_len,
                                              ML_CLICKER_LIVE_DEFAULT_WATCHDOG_MS,
                                              &value);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    if (value < ML_CLICKER_LIVE_MIN_WATCHDOG_MS ||
        value > ML_CLICKER_LIVE_MAX_WATCHDOG_MS) {
        return -ERANGE;
    }

    *watchdog_ms = value;
    return 0;
}

static int ml_clicker_emit_anchor_pair_result_record(
    const struct ml_clicker_anchor_pair_result *stored)
{
    struct proto_packet packet = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    const struct uwb_anchor_pair_result_frame *result;
    int ret;

    if (stored == NULL || !stored->valid) {
        return -EINVAL;
    }
    result = &stored->result;

    ret = tlv_append_u64(payload, sizeof(payload), &payload_len,
                         TLV_CLICKER_ID, result->clicker_id);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u32(payload, sizeof(payload), &payload_len,
                         TLV_SURVEY_ID, result->survey_id);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u32(payload, sizeof(payload), &payload_len,
                         TLV_EVENT_SEQ, result->survey_id);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u64(payload, sizeof(payload), &payload_len,
                         TLV_TIMESTAMP_MS, stored->timestamp_ms);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u64(payload, sizeof(payload), &payload_len,
                         TLV_INITIATOR_ID, result->initiator_id);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u64(payload, sizeof(payload), &payload_len,
                         TLV_RESPONDER_ID, result->responder_id);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                         TLV_SAMPLE_INDEX, result->pair_index);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                         TLV_SAMPLE_COUNT, result->pair_count);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_i32(payload, sizeof(payload), &payload_len,
                         TLV_DISTANCE_MM, result->distance_mm);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u8(payload, sizeof(payload), &payload_len,
                        TLV_QUALITY, result->quality);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u8(payload, sizeof(payload), &payload_len,
                        TLV_RANGE_STATUS, (uint8_t)result->status);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_i8(payload, sizeof(payload), &payload_len,
                        TLV_UWB_RSL_DBM, result->rsl_dbm);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    packet.msg_type = MSG_CLICK_REPORT;
    packet.flags = FLAG_DIAGNOSTIC;
    packet.src_id = DEVICE_ID;
    packet.dst_id = ml_clicker_runtime.request.host_id == 0u ?
                    GATEWAY_ID : ml_clicker_runtime.request.host_id;
    packet.session_id = result->survey_id;
    packet.seq = ml_clicker_next_packet_seq();
    packet.ttl = 1u;
    packet.payload_len = (uint8_t)payload_len;
    return gateway_emit_host_packet(&packet, payload, payload_len);
}

static void ml_clicker_emit_stored_anchor_pair_results(void)
{
    for (uint8_t i = 0u; i < UWB_ANCHOR_PAIR_SURVEY_MAX_PAIRS; i++) {
        const struct ml_clicker_anchor_pair_result *stored =
            &ml_clicker_runtime.anchor_pair_results_storage[i];
        int ret;

        if (!stored->valid) {
            continue;
        }
        ret = ml_clicker_emit_anchor_pair_result_record(stored);
        if (ret < 0) {
            ml_clicker_runtime.notify_failures++;
            LOG_WRN("ML anchor-pair result notify failed: pair=%u ret=%d",
                    i,
                    ret);
            continue;
        }
        ml_clicker_runtime.flushed_frames++;
        ml_clicker_runtime.emitted_samples++;
    }
}

static int ml_clicker_send_command_result(const struct ml_clicker_request *request,
                                          enum command_status status,
                                          uint8_t reason)
{
    struct proto_packet result = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    bool collection_context;
    enum command_id result_command_id;
    int ret;

    if (request == NULL) {
        return -EINVAL;
    }

    result_command_id = request->command_id == 0u ?
                        CMD_ML_START_COLLECTION : request->command_id;
    ret = mesh_append_command_result(payload,
                                     sizeof(payload),
                                     &payload_len,
                                     result_command_id,
                                     status,
                                     reason);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    collection_context =
        ml_clicker_runtime.event_seq != 0u &&
        request->command.seq == ml_clicker_runtime.request.command.seq &&
        request->command.session_id == ml_clicker_runtime.request.command.session_id;
    if (collection_context) {
        ret = tlv_append_u32(payload,
                             sizeof(payload),
                             &payload_len,
                             TLV_EVENT_SEQ,
                             ml_clicker_runtime.event_seq);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        ret = tlv_append_u16(payload,
                             sizeof(payload),
                             &payload_len,
                             TLV_SAMPLE_COUNT,
                             (uint16_t)MIN(ml_clicker_runtime.emitted_samples,
                                           (uint32_t)UINT16_MAX));
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
    }

    result.msg_type = MSG_COMMAND_RESULT;
    result.flags = FLAG_DIAGNOSTIC;
    if (status != COMMAND_OK) {
        result.flags |= FLAG_ERROR;
    }
    result.src_id = DEVICE_ID;
    result.dst_id = request->host_id == 0u ? GATEWAY_ID : request->host_id;
    result.session_id = request->command.session_id == 0u ?
                        k_uptime_get_32() : request->command.session_id;
    if (result.session_id == 0u) {
        result.session_id = 1u;
    }
    result.seq = request->command.seq;
    if (result.seq == 0u) {
        result.seq = gateway_next_command_seq();
    }
    result.ttl = 1u;
    result.payload_len = (uint8_t)payload_len;
    return gateway_emit_host_packet(&result, payload, payload_len);
}

int ml_clicker_emit_range_sample_if_active(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule,
    const struct uwb_range_step *step,
    const struct dwm3000_range_result *range_result)
{
    struct ml_clicker_buffered_sample sample = {0};
    uint64_t timestamp_ms = 0u;
    int64_t sample_ms;
    uint64_t base_ms;
    int ret;

    if (!ml_clicker_runtime.active) {
        return 0;
    }
    if (session == NULL || schedule == NULL || step == NULL || range_result == NULL) {
        ml_clicker_runtime.notify_failures++;
        return -EINVAL;
    }

    sample_ms = range_result->exchange_started ?
                range_result->exchange_start_ms : k_uptime_get();
    anchor_sequence_timestamp_at(sample_ms, &timestamp_ms);
    base_ms = ml_clicker_runtime.timestamp_base_ms;

    sample.timestamp_delta_ms = timestamp_ms > base_ms ?
                                (uint32_t)MIN(timestamp_ms - base_ms,
                                              (uint64_t)UINT32_MAX) : 0u;
    sample.distance_mm = range_result->distance_mm;
    sample.rsl_dbm = range_result->rsl_dbm;
    sample.sample_index = (uint16_t)MIN(step->sample_index, (size_t)UINT16_MAX);
    sample.quality = range_result->quality;
    sample.range_status = range_status_valid(range_result->status) ?
                          (uint8_t)range_result->status : (uint8_t)RANGE_INTERNAL_ERROR;
    sample.round_index = step->round_index;
    sample.rsl_sampled = range_result->rsl_sampled;
    ml_clicker_cache_note_range_result(step->anchor_id,
                                       (enum range_status)sample.range_status);

    if (gateway_ble_uwb_quiet_active()) {
        if (ml_clicker_runtime.buffered_samples >= ML_CLICKER_BUFFERED_SAMPLE_MAX) {
            ret = -ENOSPC;
        } else {
            ml_clicker_runtime.buffered_samples_storage[ml_clicker_runtime.buffered_samples] =
                sample;
            ml_clicker_runtime.buffered_samples++;
            ret = 0;
        }
    } else {
        ret = ml_clicker_emit_range_sample_record(&sample, schedule);
    }
    if (ret < 0) {
        ml_clicker_runtime.notify_failures++;
        LOG_WRN("ML range sample notify failed: ret=%d anchor=0x%016llx sample=%u",
                ret,
                (unsigned long long)step->anchor_id,
                (unsigned int)step->sample_index);
        return ret;
    }

    ml_clicker_runtime.emitted_samples++;
    return 0;
}

static uint16_t ml_clicker_next_packet_seq(void)
{
    uint16_t seq = ml_clicker_runtime.next_packet_seq++;

    if (seq == 0u) {
        seq = ml_clicker_runtime.next_packet_seq++;
    }
    return seq;
}

static void ml_clicker_init_report_packet(struct proto_packet *packet,
                                          const struct uwb_clicker_session *session)
{
    if (packet == NULL || session == NULL) {
        return;
    }

    packet->msg_type = MSG_CLICK_REPORT;
    packet->flags = FLAG_DIAGNOSTIC;
    packet->src_id = DEVICE_ID;
    packet->dst_id = ml_clicker_runtime.request.host_id == 0u ?
                     GATEWAY_ID : ml_clicker_runtime.request.host_id;
    packet->session_id = session->config.click_event_id;
    packet->seq = ml_clicker_next_packet_seq();
    packet->ttl = 1u;
}

static int ml_clicker_append_post_burst_common_tlvs(
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule,
    const struct uwb_range_schedule_entry *entry,
    const struct dwm3000_range_result *range_result,
    uint64_t timestamp_ms,
    uint8_t source)
{
    int ret;

    ret = tlv_append_u64(payload, payload_cap, payload_len,
                         TLV_CLICKER_ID, session->config.clicker_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, payload_len,
                         TLV_ANCHOR_ID, entry->anchor_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, payload_len,
                         TLV_EVENT_SEQ, session->config.click_event_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, payload_len,
                         TLV_TIMESTAMP_MS, timestamp_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_i32(payload, payload_cap, payload_len,
                         TLV_DISTANCE_MM, range_result->distance_mm);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, payload_len,
                        TLV_QUALITY, range_result->quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, payload_len,
                        TLV_RANGE_STATUS,
                        range_status_valid(range_result->status) ?
                        (uint8_t)range_result->status : (uint8_t)RANGE_INTERNAL_ERROR);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, payload_len,
                         TLV_BURST_ID, ml_clicker_runtime.burst_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, payload_len,
                         TLV_EXCHANGE_STRIDE_US, schedule->exchange_stride_us);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, payload_len,
                         TLV_BURST_DURATION_MS, schedule->burst_window_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload, payload_cap, payload_len,
                         TLV_DIAG_SOURCE, source);
}

static int ml_clicker_append_raw_timestamps_tlv(
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    const struct dwm3000_range_result *range_result)
{
    uint8_t timestamps[6u * sizeof(uint32_t)];

    proto_put_u32_le(&timestamps[0], range_result->poll_tx_ts_32);
    proto_put_u32_le(&timestamps[4], range_result->poll_rx_ts_32);
    proto_put_u32_le(&timestamps[8], range_result->resp_tx_ts_32);
    proto_put_u32_le(&timestamps[12], range_result->resp_rx_ts_32);
    proto_put_u32_le(&timestamps[16], range_result->final_tx_ts_32);
    proto_put_u32_le(&timestamps[20], range_result->final_rx_ts_32);
    return tlv_append_bytes(payload, payload_cap, payload_len,
                            TLV_UWB_RAW_TIMESTAMPS,
                            timestamps,
                            sizeof(timestamps));
}

static int ml_clicker_emit_rx_diag_block_if_present(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule,
    const struct uwb_range_schedule_entry *entry,
    const struct dwm3000_range_result *range_result,
    uint64_t timestamp_ms,
    uint8_t source,
    const uint8_t *diag,
    uint8_t diag_len)
{
    struct proto_packet packet = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int ret;

    if (diag == NULL || diag_len == 0u) {
        return 0;
    }

    ret = ml_clicker_append_post_burst_common_tlvs(payload,
                                                   sizeof(payload),
                                                   &payload_len,
                                                   session,
                                                   schedule,
                                                   entry,
                                                   range_result,
                                                   timestamp_ms,
                                                   source);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                         TLV_DIAG_FRAGMENT_INDEX, 0u);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                         TLV_DIAG_FRAGMENT_COUNT, 1u);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = tlv_append_bytes(payload, sizeof(payload), &payload_len,
                           TLV_UWB_RX_DIAG_BYTES, diag, diag_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ml_clicker_init_report_packet(&packet, session);
    packet.payload_len = (uint8_t)payload_len;
    ret = ml_clicker_emit_or_buffer_host_packet(&packet, payload, payload_len);
    if (ret < 0) {
        ml_clicker_runtime.notify_failures++;
    }
    return ret;
}

static int ml_clicker_emit_full_cir_chunks_if_present(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule,
    const struct uwb_range_schedule_entry *entry,
    const struct dwm3000_range_result *range_result,
    const uint8_t *cir,
    uint64_t timestamp_ms)
{
    uint16_t total_len;
    uint16_t offset = 0u;
    uint16_t fragment_count;

    if (cir == NULL || !range_result->anchor_full_cir_sampled ||
        range_result->anchor_full_cir_len == 0u) {
        return 0;
    }

    total_len = range_result->anchor_full_cir_len;
    fragment_count = (uint16_t)((total_len + ML_FULL_CIR_BLE_CHUNK_BYTES - 1u) /
                                ML_FULL_CIR_BLE_CHUNK_BYTES);
    for (uint16_t fragment_index = 0u; fragment_index < fragment_count; fragment_index++) {
        struct proto_packet packet = {0};
        uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
        size_t payload_len = 0u;
        uint16_t chunk_len = MIN((uint16_t)ML_FULL_CIR_BLE_CHUNK_BYTES,
                                 (uint16_t)(total_len - offset));
        int ret;

        ret = ml_clicker_append_post_burst_common_tlvs(payload,
                                                       sizeof(payload),
                                                       &payload_len,
                                                       session,
                                                       schedule,
                                                       entry,
                                                       range_result,
                                                       timestamp_ms,
                                                       ML_DIAG_SOURCE_ANCHOR_CIR);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                             TLV_DIAG_FRAGMENT_INDEX, fragment_index);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                             TLV_DIAG_FRAGMENT_COUNT, fragment_count);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                             TLV_UWB_CIR_BYTE_OFFSET, offset);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                             TLV_UWB_CIR_TOTAL_BYTES,
                             range_result->anchor_full_cir_total_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                             TLV_UWB_CIR_FIRST_PATH_INDEX,
                             range_result->anchor_full_cir_first_path_index);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                             TLV_UWB_CIR_START_INDEX,
                             range_result->anchor_full_cir_start_index);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        ret = tlv_append_bytes(payload, sizeof(payload), &payload_len,
                               TLV_UWB_CIR_FULL_CHUNK,
                               &cir[offset],
                               (uint8_t)chunk_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        ml_clicker_init_report_packet(&packet, session);
        packet.payload_len = (uint8_t)payload_len;
        ret = ml_clicker_emit_or_buffer_host_packet(&packet, payload, payload_len);
        if (ret < 0) {
            ml_clicker_runtime.notify_failures++;
            return ret;
        }
        offset += chunk_len;
    }

    return 0;
}

static int ml_clicker_emit_post_burst_diagnostic_if_active(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule,
    const struct uwb_range_schedule_entry *entry,
    uint8_t entry_index,
    const struct dwm3000_range_result *range_result,
    const uint8_t *anchor_full_cir)
{
    struct range_report_diagnostics diagnostics = {0};
    struct range_report_fields fields = {0};
    struct proto_packet packet = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t anchor_cir[UWB_CIR_SAMPLE_LEN] = {0};
    size_t payload_len = 0u;
    uint64_t timestamp_ms = 0u;
    int64_t sample_ms;
    int ret;

    if (!ml_clicker_runtime.active) {
        return 0;
    }
    if (session == NULL || schedule == NULL || entry == NULL || range_result == NULL) {
        ml_clicker_runtime.notify_failures++;
        return -EINVAL;
    }

    sample_ms = range_result->exchange_started ?
                range_result->exchange_start_ms : k_uptime_get();
    anchor_sequence_timestamp_at(sample_ms, &timestamp_ms);
    if (range_result->cir_sampled) {
        memcpy(anchor_cir, range_result->cir_sample, sizeof(anchor_cir));
    }

    {
        uint32_t anchor_diag_len = range_result->cir_sampled ? UWB_CIR_SAMPLE_LEN : 0u;
        uint32_t clicker_diag_len = range_result->clicker_diag_received ?
                                    range_result->clicker_diag_len : 0u;
        uint32_t clicker_raw_diag_len = range_result->clicker_rx_diag_sampled ?
                                        range_result->clicker_rx_diag_raw_len : 0u;
        uint32_t anchor_raw_diag_len = range_result->anchor_rx_diag_sampled ?
                                       range_result->anchor_rx_diag_raw_len : 0u;
        uint32_t anchor_full_cir_len = range_result->anchor_full_cir_sampled ?
                                       range_result->anchor_full_cir_len : 0u;
        uint32_t anchor_full_cir_total_len = range_result->anchor_full_cir_total_len == 0u ?
                                             anchor_full_cir_len :
                                             range_result->anchor_full_cir_total_len;

        diagnostics.status_flags = range_result->clicker_diag_received ||
                                   range_result->clicker_rx_diag_sampled ?
                                   RANGE_DIAG_CLICKER_PRESENT :
                                   RANGE_DIAG_CLICKER_MISSING;
        diagnostics.status_flags |= range_result->cir_sampled ||
                                    range_result->anchor_rx_diag_sampled ||
                                    range_result->anchor_full_cir_sampled ||
                                    range_result->clock_offset_sampled ||
                                    range_result->carrier_integrator_sampled ?
                                    RANGE_DIAG_ANCHOR_PRESENT :
                                    RANGE_DIAG_ANCHOR_MISSING;
        if (range_result->clicker_diag_truncated ||
            range_result->anchor_full_cir_truncated) {
            diagnostics.status_flags |= RANGE_DIAG_TRUNCATED;
        }
        if (range_result->clicker_diag_dropped) {
            diagnostics.status_flags |= RANGE_DIAG_CAPTURE_FAILED;
        }
        diagnostics.diag_bytes_captured = anchor_diag_len + clicker_diag_len +
                                          clicker_raw_diag_len + anchor_raw_diag_len +
                                          anchor_full_cir_total_len;
        diagnostics.diag_bytes_transmitted = anchor_diag_len + clicker_diag_len +
                                             clicker_raw_diag_len + anchor_raw_diag_len +
                                             anchor_full_cir_len;
        diagnostics.diag_bytes_truncated =
            anchor_full_cir_total_len > anchor_full_cir_len ?
            anchor_full_cir_total_len - anchor_full_cir_len : 0u;
        diagnostics.diag_frames_dropped = range_result->clicker_diag_dropped ? 1u : 0u;
        diagnostics.clicker_diag = range_result->clicker_diag_received ?
                                   range_result->clicker_diag : NULL;
        diagnostics.clicker_diag_len = range_result->clicker_diag_received ?
                                       range_result->clicker_diag_len : 0u;
        diagnostics.anchor_diag = range_result->cir_sampled ? anchor_cir : NULL;
        diagnostics.anchor_diag_len = range_result->cir_sampled ?
                                      UWB_CIR_SAMPLE_LEN : 0u;
    }
    diagnostics.burst_id = ml_clicker_runtime.burst_id;
    diagnostics.exchange_stride_us = schedule->exchange_stride_us;
    diagnostics.burst_duration_ms = schedule->burst_window_ms;
    diagnostics.report_fragment_count = 1u;
    diagnostics.phy_config_id = schedule->ranging_channel;
    diagnostics.clock_offset_raw = range_result->clock_offset_raw;
    diagnostics.clock_offset_present = range_result->clock_offset_sampled;
    diagnostics.carrier_integrator = range_result->carrier_integrator;
    diagnostics.carrier_integrator_present =
        range_result->carrier_integrator_sampled;

    fields.clicker_id = session->config.clicker_id;
    fields.anchor_id = entry->anchor_id;
    fields.event_seq = session->config.click_event_id;
    fields.timestamp_ms = timestamp_ms;
    fields.distance_mm = range_result->distance_mm;
    fields.quality = range_result->quality;
    fields.rsl_dbm = range_result->rsl_dbm;
    fields.cir_sample = range_result->cir_sampled ? anchor_cir : NULL;
    fields.range_status = range_status_valid(range_result->status) ?
                          range_result->status : RANGE_INTERNAL_ERROR;
    fields.sample_count = 0u;
    fields.distance_sample_count = 0u;
    fields.omit_rsl = !range_result->rsl_sampled;
    fields.omit_cir = !range_result->cir_sampled;
    fields.diagnostics = &diagnostics;

    ret = report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields);
    if (ret != PROTO_OK) {
        ml_clicker_runtime.notify_failures++;
        LOG_WRN("ML post-burst diagnostic TLV build failed: ret=%d anchor=0x%016llx entry=%u",
                ret,
                (unsigned long long)entry->anchor_id,
                entry_index);
        return -EINVAL;
    }
    ret = tlv_append_u8(payload,
                        sizeof(payload),
                        &payload_len,
                        TLV_DIAG_SOURCE,
                        ML_DIAG_SOURCE_SUMMARY);
    if (ret != PROTO_OK) {
        ml_clicker_runtime.notify_failures++;
        LOG_WRN("ML post-burst source TLV build failed: ret=%d anchor=0x%016llx entry=%u",
                ret,
                (unsigned long long)entry->anchor_id,
                entry_index);
        return -EINVAL;
    }
    ret = ml_clicker_append_raw_timestamps_tlv(payload,
                                               sizeof(payload),
                                               &payload_len,
                                               range_result);
    if (ret != PROTO_OK) {
        ml_clicker_runtime.notify_failures++;
        LOG_WRN("ML post-burst timestamp TLV build failed: ret=%d anchor=0x%016llx entry=%u",
                ret,
                (unsigned long long)entry->anchor_id,
                entry_index);
        return -EINVAL;
    }

    ml_clicker_init_report_packet(&packet, session);
    packet.payload_len = (uint8_t)payload_len;

    ret = ml_clicker_emit_or_buffer_host_packet(&packet, payload, payload_len);
    if (ret < 0) {
        ml_clicker_runtime.notify_failures++;
        LOG_WRN("ML post-burst diagnostic notify failed: ret=%d anchor=0x%016llx entry=%u",
                ret,
                (unsigned long long)entry->anchor_id,
                entry_index);
        return ret;
    }

    ret = ml_clicker_emit_rx_diag_block_if_present(session,
                                                   schedule,
                                                   entry,
                                                   range_result,
                                                   timestamp_ms,
                                                   ML_DIAG_SOURCE_CLICKER,
                                                   range_result->clicker_rx_diag_raw,
                                                   range_result->clicker_rx_diag_sampled ?
                                                   range_result->clicker_rx_diag_raw_len : 0u);
    if (ret < 0) {
        return ret;
    }
    ret = ml_clicker_emit_rx_diag_block_if_present(session,
                                                   schedule,
                                                   entry,
                                                   range_result,
                                                   timestamp_ms,
                                                   ML_DIAG_SOURCE_ANCHOR,
                                                   range_result->anchor_rx_diag_raw,
                                                   range_result->anchor_rx_diag_sampled ?
                                                   range_result->anchor_rx_diag_raw_len : 0u);
    if (ret < 0) {
        return ret;
    }
    return ml_clicker_emit_full_cir_chunks_if_present(session,
                                                      schedule,
                                                      entry,
                                                      range_result,
                                                      anchor_full_cir,
                                                      timestamp_ms);
}

void ml_clicker_run_post_burst_diagnostics(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule,
    int64_t schedule_tx_ms,
    int64_t click_deadline_ms)
{
    size_t total_samples;
    bool quiet_owned = false;

    if (!ml_clicker_runtime.active || session == NULL || schedule == NULL) {
        return;
    }

    total_samples = uwb_range_schedule_total_samples(schedule);
    if (!gateway_ble_uwb_quiet_active()) {
        gateway_ble_enter_uwb_quiet("ml-clicker-post-burst-diagnostic-uwb");
        quiet_owned = true;
    }

    for (uint8_t entry_index = 0u; entry_index < schedule->selected_count; entry_index++) {
        const struct uwb_range_schedule_entry *entry = &schedule->entries[entry_index];
        struct ml_clicker_post_burst_diagnostic *stored = NULL;
        struct dwm3000_range_request range_request;
        struct dwm3000_range_result range_result;
        int64_t target_us;
        bool diag_complete = false;
        int ret = -ETIMEDOUT;

        target_us = scheduled_post_burst_diag_target_us(schedule_tx_ms,
                                                        schedule,
                                                        total_samples,
                                                        entry_index);

        if (ml_clicker_runtime.post_burst_diagnostics >= ML_CLICKER_POST_BURST_DIAG_MAX) {
            ml_clicker_runtime.notify_failures++;
            LOG_WRN("ML post-burst diagnostic store full: anchor=0x%016llx entry=%u stored=%u",
                    (unsigned long long)entry->anchor_id,
                    entry_index,
                    ml_clicker_runtime.post_burst_diagnostics);
            continue;
        }
        stored = &ml_clicker_runtime.post_burst_diagnostics_storage[
            ml_clicker_runtime.post_burst_diagnostics];
        memset(stored, 0, sizeof(*stored));
        stored->entry_index = entry_index;

        memset(&range_request, 0, sizeof(range_request));
        memset(&range_result, 0, sizeof(range_result));
        range_result.status = RANGE_RX_TIMEOUT;
        range_request.initiator_id = DEVICE_ID;
        range_request.responder_id = entry->anchor_id;
        range_request.network_id = session->config.network_id;
        range_request.session_nonce = session->config.nonce;
        range_request.responder_short_addr = uwb_session_short_addr_from_id(entry->anchor_id);
        range_request.session_id = session->config.click_event_id;
        range_request.seq = scheduled_post_burst_diag_seq(entry);
        range_request.round_index = UWB_POST_BURST_DIAG_ROUND_INDEX;
        range_request.flags = session->config.flags;
        range_request.reply_delay_uus = UWB_POST_BURST_DIAG_REPLY_DELAY_UUS;
        range_request.capture_rsl = false;
        range_request.skip_responder_report = true;
        range_request.send_clicker_diag = false;
        range_request.expect_anchor_diag = true;
        range_request.expect_anchor_diag_fragments = true;
        range_request.anchor_full_cir = stored->anchor_full_cir;
        range_request.anchor_full_cir_cap = sizeof(stored->anchor_full_cir);

        for (uint8_t diag_attempt = 0u; diag_attempt < UWB_POST_BURST_DIAG_ATTEMPTS;
             diag_attempt++) {
            int64_t attempt_target_us =
                target_us + ((int64_t)diag_attempt * UWB_POST_BURST_DIAG_RETRY_DELAY_MS *
                             1000);
            int64_t remaining_ms;

            sleep_until_us(attempt_target_us);
            remaining_ms = click_deadline_ms - k_uptime_get();
            if (remaining_ms <= CLICK_REPORT_BUILD_GUARD_MS) {
                LOG_WRN("ML post-burst diagnostic skipped: reason=click_budget anchor=0x%016llx entry=%u attempt=%u remaining_ms=%lld",
                        (unsigned long long)entry->anchor_id,
                        entry_index,
                        diag_attempt + 1u,
                        (long long)remaining_ms);
                break;
            }
            range_request.timeout_ms = MIN(UWB_POST_BURST_DIAG_TIMEOUT_MS,
                                           (uint32_t)(remaining_ms -
                                                      CLICK_REPORT_BUILD_GUARD_MS));

            memset(&range_result, 0, sizeof(range_result));
            range_result.status = RANGE_RX_TIMEOUT;
            memset(stored->anchor_full_cir, 0, sizeof(stored->anchor_full_cir));

            ret = radio_guard_uwb_start("clicker post-burst diagnostic");
            if (ret < 0) {
                LOG_WRN("ML post-burst diagnostic not started: reason=radio_guard anchor=0x%016llx entry=%u attempt=%u ret=%d",
                        (unsigned long long)entry->anchor_id,
                        entry_index,
                        diag_attempt + 1u,
                        ret);
                continue;
            }

            LOG_INF("ML post-burst diagnostic start: anchor=0x%016llx entry=%u attempt=%u/%u seq=%u timeout_ms=%u",
                    (unsigned long long)entry->anchor_id,
                    entry_index,
                    diag_attempt + 1u,
                    UWB_POST_BURST_DIAG_ATTEMPTS,
                    range_request.seq,
                    range_request.timeout_ms);
            high_debug_log_event("POST_BURST_DIAG_TX",
                                 "anchor=0x%016llx event_seq=%u entry=%u diag_attempt=%u/%u seq=%u target_us=%lld now_ms=%lld timeout_ms=%u",
                                 (unsigned long long)entry->anchor_id,
                                 session->config.click_event_id,
                                 entry_index,
                                 diag_attempt + 1u,
                                 UWB_POST_BURST_DIAG_ATTEMPTS,
                                 range_request.seq,
                                 (long long)attempt_target_us,
                                 (long long)k_uptime_get(),
                                 range_request.timeout_ms);

            ret = dwm3000_driver_range_initiator(&range_request, &range_result);
            (void)dwm3000_driver_idle();
            radio_guard_uwb_stop();

            if (range_result.exchange_started) {
                LOG_INF("ML post-burst diagnostic complete: anchor=0x%016llx entry=%u attempt=%u seq=%u ret=%d status=%s(%u) rsl_present=%u cir_present=%u anchor_full_cir=%u clicker_diag=%u",
                        (unsigned long long)entry->anchor_id,
                        entry_index,
                        diag_attempt + 1u,
                        range_request.seq,
                        ret,
                        range_status_name(range_result.status),
                        range_result.status,
                        range_result.rsl_sampled ? 1u : 0u,
                        range_result.cir_sampled ? 1u : 0u,
                        range_result.anchor_full_cir_sampled ? 1u : 0u,
                        range_result.clicker_diag_received ? 1u : 0u);
                high_debug_log_event("POST_BURST_DIAG_TX",
                                     "result=complete anchor=0x%016llx event_seq=%u entry=%u diag_attempt=%u seq=%u ret=%d status=%u rsl_present=%u cir_present=%u anchor_full_cir=%u clicker_diag=%u",
                                     (unsigned long long)entry->anchor_id,
                                     session->config.click_event_id,
                                     entry_index,
                                     diag_attempt + 1u,
                                     range_request.seq,
                                     ret,
                                     range_result.status,
                                     range_result.rsl_sampled ? 1u : 0u,
                                     range_result.cir_sampled ? 1u : 0u,
                                     range_result.anchor_full_cir_sampled ? 1u : 0u,
                                     range_result.clicker_diag_received ? 1u : 0u);
                stored->result = range_result;
                stored->valid = true;
                diag_complete = ret == 0 && range_result.status == RANGE_OK &&
                                range_result.anchor_full_cir_sampled;
                if (diag_complete) {
                    break;
                }
            } else {
                LOG_WRN("ML post-burst diagnostic failed: anchor=0x%016llx entry=%u attempt=%u seq=%u ret=%d status=%s(%u)",
                        (unsigned long long)entry->anchor_id,
                        entry_index,
                        diag_attempt + 1u,
                        range_request.seq,
                        ret,
                        range_status_name(range_result.status),
                        range_result.status);
                high_debug_log_event("POST_BURST_DIAG_TX",
                                     "result=fail anchor=0x%016llx event_seq=%u entry=%u diag_attempt=%u seq=%u ret=%d status=%u exchange_started=0",
                                     (unsigned long long)entry->anchor_id,
                                     session->config.click_event_id,
                                     entry_index,
                                     diag_attempt + 1u,
                                     range_request.seq,
                                     ret,
                                     range_result.status);
            }
        }

        if (stored->valid) {
            if (!diag_complete) {
                LOG_WRN("ML post-burst diagnostic stored incomplete: anchor=0x%016llx entry=%u seq=%u status=%s(%u) anchor_full_cir=%u",
                        (unsigned long long)entry->anchor_id,
                        entry_index,
                        range_request.seq,
                        range_status_name(stored->result.status),
                        stored->result.status,
                        stored->result.anchor_full_cir_sampled ? 1u : 0u);
            }
            ml_clicker_runtime.post_burst_diagnostics++;
        }
    }

    if (quiet_owned && gateway_ble_uwb_quiet_active()) {
        gateway_ble_exit_uwb_quiet("ml-clicker-post-burst-diagnostic-uwb");
    }
}

static void ml_clicker_emit_stored_post_burst_diagnostics(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule)
{
    if (!ml_clicker_runtime.active || session == NULL || schedule == NULL) {
        return;
    }

    for (uint16_t i = 0u; i < ml_clicker_runtime.post_burst_diagnostics; i++) {
        const struct ml_clicker_post_burst_diagnostic *stored =
            &ml_clicker_runtime.post_burst_diagnostics_storage[i];
        const struct uwb_range_schedule_entry *entry;
        int ret;

        if (!stored->valid || stored->entry_index >= schedule->selected_count) {
            continue;
        }

        entry = &schedule->entries[stored->entry_index];
        ret = ml_clicker_emit_post_burst_diagnostic_if_active(session,
                                                              schedule,
                                                              entry,
                                                              stored->entry_index,
                                                              &stored->result,
                                                              stored->anchor_full_cir);
        if (ret < 0) {
            ml_clicker_runtime.notify_failures++;
            LOG_WRN("ML stored post-burst diagnostic emit failed: index=%u/%u anchor=0x%016llx ret=%d",
                    (unsigned int)(i + 1u),
                    ml_clicker_runtime.post_burst_diagnostics,
                    (unsigned long long)entry->anchor_id,
                    ret);
        }
    }
}

static enum command_status ml_clicker_status_from_ret(int ret)
{
    if (ret == 0) {
        return COMMAND_OK;
    }
    if (ret == -ETIMEDOUT || ret == PROTO_ERR_NOT_FOUND) {
        return COMMAND_TIMEOUT;
    }
    if (ret == -EINVAL || ret == PROTO_ERR_MALFORMED) {
        return COMMAND_MALFORMED_PAYLOAD;
    }
    if (ret == -EBUSY || ret == PROTO_ERR_BUSY) {
        return COMMAND_BUSY;
    }
    return COMMAND_RADIO_ERROR;
}

static enum command_status ml_clicker_run_live_tracking(
    const struct ml_clicker_request *request)
{
    enum command_status status = COMMAND_OK;
    uint32_t laps = 0u;
    int last_ret = 0;

    if (request == NULL) {
        return COMMAND_INTERNAL_ERROR;
    }

    memset(&ml_clicker_runtime, 0, sizeof(ml_clicker_runtime));
    ml_clicker_runtime.request = *request;
    ml_clicker_runtime.timestamp_base_ms = (uint64_t)k_uptime_get();
    ml_clicker_runtime.next_packet_seq = 1u;
    ml_clicker_runtime.active = true;
    ml_clicker_discovery_slot_override = request->discovery_slot_count;
    atomic_clear(&ml_clicker_live_stop_requested);
    ml_clicker_live_tracking_touch();

    LOG_INF("ML live tracking start: command=0x%04x samples_per_anchor=%u max_anchors=%u discovery_slots=%u watchdog_ms=%u host=0x%016llx",
            (unsigned int)request->command_id,
            request->samples_per_anchor,
            request->max_anchor_count,
            request->discovery_slot_count,
            request->live_watchdog_ms,
            (unsigned long long)request->host_id);

    while (true) {
        struct uwb_clicker_session session;
        struct uwb_range_schedule_frame schedule;
        struct uwb_clicker_config config;
        uint32_t event_seq;
        uint64_t priority_id;
        int64_t click_deadline_ms;
        uint8_t attempted_count = 0u;
        bool setup_quiet = false;
        int ret;

        if (atomic_get(&ml_clicker_live_stop_requested) != 0) {
            status = COMMAND_OK;
            break;
        }
        if (!ml_clicker_live_tracking_heartbeat_ok(k_uptime_get_32(),
                                                   request->live_watchdog_ms)) {
            status = COMMAND_TIMEOUT;
            last_ret = -ETIMEDOUT;
            break;
        }

        memset(&session, 0, sizeof(session));
        memset(&schedule, 0, sizeof(schedule));
        memset(&config, 0, sizeof(config));

        event_seq = next_click_event_seq();
        priority_id = clicker_priority_id(event_seq, 1u);
        click_deadline_ms = k_uptime_get() + ML_CLICKER_COLLECTION_DEADLINE_MS;

        config.network_id = NETWORK_ID;
        config.clicker_id = DEVICE_ID;
        config.click_event_id = event_seq;
        config.nonce = clicker_nonce(event_seq);
        config.min_anchor_count = 1u;
        config.max_anchor_count = request->max_anchor_count;
        config.max_attempts = 1u;
        config.samples_per_anchor = request->samples_per_anchor;
        config.wake_channel = UWB_WAKE_CHANNEL;
        config.ranging_channel = UWB_RANGING_CHANNEL;
        config.flags = FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;

        ml_clicker_runtime.event_seq = event_seq;
        ml_clicker_runtime.burst_id = uwb_schedule_burst_id(event_seq, 1u);
        ml_clicker_runtime.timestamp_base_ms = (uint64_t)k_uptime_get();

        ret = uwb_clicker_session_start(&session, &config);
        if (ret == PROTO_OK) {
            gateway_ble_enter_uwb_quiet("ml-clicker-live-control");
            setup_quiet = true;
            ret = app_clicker_collect_uwb_attempt_with_options(&session,
                                                               priority_id,
                                                               &schedule,
                                                               true,
                                                               false);
        }
        if (setup_quiet && gateway_ble_uwb_quiet_active()) {
            gateway_ble_exit_uwb_quiet("ml-clicker-live-control");
            setup_quiet = false;
        }

        if (ret == 0) {
            ml_clicker_runtime.selected_anchors = schedule.selected_count;
            ret = app_clicker_range_scheduled_anchors(&session,
                                                      &schedule,
                                                      click_deadline_ms,
                                                      &attempted_count);
            ml_clicker_runtime.attempted_ranges += attempted_count;
            ml_clicker_flush_buffered_frames(&schedule);
        }

        if (ret < 0) {
            last_ret = ret;
            if (atomic_get(&ml_clicker_live_stop_requested) != 0) {
                status = COMMAND_OK;
                break;
            }
            if (!ml_clicker_live_tracking_heartbeat_ok(k_uptime_get_32(),
                                                       request->live_watchdog_ms)) {
                status = COMMAND_TIMEOUT;
                break;
            }
            status = ml_clicker_status_from_ret(ret);
            break;
        }

        laps++;
    }

    ml_clicker_runtime.active = false;
    ml_clicker_discovery_slot_override = 0u;
    if (status == COMMAND_OK && ml_clicker_runtime.notify_failures > 0u) {
        status = COMMAND_INTERNAL_ERROR;
    }

    LOG_INF("ML live tracking done: status=%s last_ret=%d laps=%u selected=%u attempted=%u emitted=%u buffered=%u flushed=%u notify_failures=%u",
            command_status_name(status),
            last_ret,
            laps,
            ml_clicker_runtime.selected_anchors,
            ml_clicker_runtime.attempted_ranges,
            ml_clicker_runtime.emitted_samples,
            ml_clicker_runtime.buffered_frames,
            ml_clicker_runtime.flushed_frames,
            ml_clicker_runtime.notify_failures);
    return status;
}

void ml_clicker_handle_ble_frame(const uint8_t *frame, size_t frame_len)
{
    struct proto_packet packet = {0};
    struct ml_clicker_request request = {0};
    enum command_id command_id = CMD_VENDOR_BASE;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    const uint8_t *decoded_payload = NULL;
    size_t payload_len = 0u;
    int ret;

    ret = serial_frame_decode_packet(frame,
                                     frame_len,
                                     &packet,
                                     payload,
                                     sizeof(payload),
                                     &payload_len);
    if (ret != PROTO_OK) {
        LOG_WRN("ML clicker BLE COBS frame decode failed: %d", ret);
        return;
    }

    decoded_payload = payload;
    ret = gateway_command_extract_id(decoded_payload, payload_len, &command_id);
    if (packet.msg_type != MSG_COMMAND || ret != PROTO_OK) {
        request.command = packet;
        request.host_id = packet.src_id;
        (void)ml_clicker_send_command_result(&request,
                                             COMMAND_MALFORMED_PAYLOAD,
                                             0u);
        return;
    }
	    if (command_id != CMD_ML_START_COLLECTION &&
	        command_id != CMD_ML_START_FAST_RANGING &&
	        command_id != CMD_ML_START_ANCHOR_PAIR_SURVEY &&
	        command_id != CMD_ML_START_LIVE_TRACKING &&
	        command_id != CMD_ML_LIVE_TRACKING_HEARTBEAT &&
	        command_id != CMD_ML_STOP_LIVE_TRACKING) {
	        request.command = packet;
	        request.command_id = command_id;
	        request.host_id = packet.src_id;
        (void)ml_clicker_send_command_result(&request,
                                             COMMAND_UNSUPPORTED_COMMAND,
                                             0u);
        return;
    }
    if (packet.dst_id != DEVICE_ID &&
        packet.dst_id != GATEWAY_ID &&
        packet.dst_id != MESH_BROADCAST_ID) {
        request.command = packet;
        request.command_id = command_id;
        request.host_id = packet.src_id;
        (void)ml_clicker_send_command_result(&request,
	                                             COMMAND_DENIED,
	                                             0u);
	        return;
	    }

	    request.command = packet;
	    request.command_id = command_id;
	    request.host_id = packet.src_id;

	    if (command_id == CMD_ML_LIVE_TRACKING_HEARTBEAT ||
	        command_id == CMD_ML_STOP_LIVE_TRACKING) {
	        enum command_status live_status = COMMAND_INVALID_STATE;

	        if (ml_clicker_live_tracking_active()) {
	            ml_clicker_live_tracking_touch();
	            if (command_id == CMD_ML_STOP_LIVE_TRACKING) {
	                atomic_set(&ml_clicker_live_stop_requested, 1);
	            }
	            live_status = COMMAND_OK;
	        }
	        (void)ml_clicker_send_command_result(&request, live_status, 0u);
	        return;
	    }

	    request.samples_per_anchor = CONFIG_IMEC_ML_DEFAULT_SAMPLES_PER_ANCHOR;
	    request.max_anchor_count = CONFIG_IMEC_ML_MAX_ANCHORS;
	    request.discovery_slot_count = CONFIG_IMEC_ML_DISCOVERY_SLOT_COUNT;
	    request.live_tracking = command_id == CMD_ML_START_LIVE_TRACKING;
	    request.range_only = command_id == CMD_ML_START_FAST_RANGING ||
	                         request.live_tracking;
	    request.allow_cached_discovery = request.range_only;
	    request.anchor_pair_survey = command_id == CMD_ML_START_ANCHOR_PAIR_SURVEY;
	    request.live_watchdog_ms = ML_CLICKER_LIVE_DEFAULT_WATCHDOG_MS;
	    if (request.live_tracking) {
	        request.samples_per_anchor = UWB_RANGING_REQUESTS_MAX_PER_ANCHOR;
	    }

    if (request.anchor_pair_survey) {
        request.samples_per_anchor = 1u;
        ret = 0;
    } else {
        ret = ml_clicker_read_u8_tlv(decoded_payload,
                                     payload_len,
                                     TLV_SAMPLE_COUNT,
                                     request.samples_per_anchor,
                                     1u,
                                     UWB_RANGING_REQUESTS_MAX_PER_ANCHOR,
                                     &request.samples_per_anchor);
    }
	    if (ret == 0) {
	        ret = ml_clicker_read_u8_tlv(decoded_payload,
	                                     payload_len,
                                     TLV_DISCOVERY_SLOT_COUNT,
                                     request.discovery_slot_count,
                                     request.anchor_pair_survey ?
                                     UWB_ANCHOR_PAIR_SCHEDULE_MIN_ANCHORS : 1u,
	                                     UWB_RANGE_SCHEDULE_MAX_ANCHORS,
	                                     &request.discovery_slot_count);
	    }
	    if (ret == 0 && request.live_tracking) {
	        ret = ml_clicker_read_live_watchdog_ms(decoded_payload,
	                                               payload_len,
	                                               &request.live_watchdog_ms);
	    }
    if (ret < 0) {
        (void)ml_clicker_send_command_result(&request,
                                             COMMAND_MALFORMED_PAYLOAD,
                                             0u);
        return;
    }
    request.max_anchor_count = request.discovery_slot_count;

    if (!atomic_cas(&ml_clicker_busy, 0, 1)) {
        (void)ml_clicker_send_command_result(&request, COMMAND_BUSY, 0u);
        return;
    }

    ml_clicker_pending_request = request;
    ret = app_clicker_submit_work(&ml_clicker_collect_work);
    if (ret < 0) {
        atomic_clear(&ml_clicker_busy);
        (void)ml_clicker_send_command_result(&request,
                                             COMMAND_INTERNAL_ERROR,
                                             0u);
    }
}

static void ml_clicker_collect_work_handler(struct k_work *work)
{
    struct ml_clicker_request request = ml_clicker_pending_request;
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_clicker_config config;
    enum command_status status;
    int64_t click_deadline_ms;
    uint32_t event_seq;
    uint64_t priority_id;
    uint8_t attempted_count = 0u;
    const char *mode;
    int ret;

    ARG_UNUSED(work);

    memset(&session, 0, sizeof(session));
    memset(&schedule, 0, sizeof(schedule));
    memset(&config, 0, sizeof(config));

    if (request.live_tracking) {
        status = ml_clicker_run_live_tracking(&request);
        (void)ml_clicker_send_command_result(&request, status, 0u);
        atomic_clear(&ml_clicker_busy);
        return;
    }

    event_seq = next_click_event_seq();
    priority_id = clicker_priority_id(event_seq, 1u);
    click_deadline_ms = k_uptime_get() + ML_CLICKER_COLLECTION_DEADLINE_MS;

    config.network_id = NETWORK_ID;
    config.clicker_id = DEVICE_ID;
    config.click_event_id = event_seq;
    config.nonce = clicker_nonce(event_seq);
    config.min_anchor_count = request.anchor_pair_survey ?
                              UWB_ANCHOR_PAIR_SCHEDULE_MIN_ANCHORS : 1u;
    config.max_anchor_count = request.max_anchor_count;
    config.max_attempts = 1u;
    config.samples_per_anchor = request.anchor_pair_survey ?
                                1u : request.samples_per_anchor;
    config.wake_channel = UWB_WAKE_CHANNEL;
    config.ranging_channel = UWB_RANGING_CHANNEL;
    config.flags = request.range_only ?
                   (FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY) : FLAG_DIAGNOSTIC;
    mode = request.anchor_pair_survey ? "anchor_pair_survey" :
           (request.range_only ? "fast_range_only" : "full_diagnostics");

    memset(&ml_clicker_runtime, 0, sizeof(ml_clicker_runtime));
    ml_clicker_runtime.request = request;
    ml_clicker_runtime.event_seq = event_seq;
    ml_clicker_runtime.burst_id = uwb_schedule_burst_id(event_seq, 1u);
    ml_clicker_runtime.timestamp_base_ms = (uint64_t)k_uptime_get();
    ml_clicker_runtime.next_packet_seq = 1u;
    ml_clicker_runtime.active = true;
    ml_clicker_discovery_slot_override = request.discovery_slot_count;

    LOG_INF("ML collection start: event_seq=%u command=0x%04x mode=%s samples_per_anchor=%u max_anchors=%u discovery_slots=%u host=0x%016llx",
            event_seq,
            (unsigned int)request.command_id,
            mode,
            request.samples_per_anchor,
            request.max_anchor_count,
            request.discovery_slot_count,
            (unsigned long long)request.host_id);

    gateway_ble_enter_uwb_quiet("ml-clicker-collection");
    ret = uwb_clicker_session_start(&session, &config);
    if (ret == PROTO_OK) {
        ret = app_clicker_attempt_gate(&session,
                                       event_seq,
                                       priority_id,
                                       click_deadline_ms,
                                       false,
                                       &clicker_attempt_gate_config);
    }
    if (ret == 0 && request.anchor_pair_survey) {
        ret = ml_clicker_run_anchor_pair_survey(&session,
                                                priority_id,
                                                click_deadline_ms);
    } else if (ret == 0) {
        ret = app_clicker_collect_uwb_attempt_with_options(&session,
                                                       priority_id,
                                                       &schedule,
                                                       request.allow_cached_discovery,
                                                       !request.range_only);
    }
    if (ret == 0) {
        ml_clicker_runtime.selected_anchors = schedule.selected_count;
        ret = app_clicker_range_scheduled_anchors(&session,
                                          &schedule,
                                          click_deadline_ms,
                                          &attempted_count);
        ml_clicker_runtime.attempted_ranges = attempted_count;
    }
    gateway_ble_exit_uwb_quiet("ml-clicker-collection");
    if (request.anchor_pair_survey) {
        ml_clicker_emit_stored_anchor_pair_results();
    } else {
        ml_clicker_flush_buffered_frames(&schedule);
        ml_clicker_emit_stored_post_burst_diagnostics(&session, &schedule);
    }

    ml_clicker_runtime.active = false;
    ml_clicker_discovery_slot_override = 0u;
    status = ml_clicker_status_from_ret(ret);
    if (status == COMMAND_OK && ml_clicker_runtime.notify_failures > 0u) {
        status = COMMAND_INTERNAL_ERROR;
    }

    LOG_INF("ML collection done: event_seq=%u ret=%d status=%s mode=%s cached_discovery=%u selected=%u attempted=%u emitted=%u buffered=%u flushed=%u notify_failures=%u ds_ok=%u ds_fail=%u",
            event_seq,
            ret,
            command_status_name(status),
            mode,
            ml_clicker_runtime.cached_discovery_used ? 1u : 0u,
            ml_clicker_runtime.selected_anchors,
            ml_clicker_runtime.attempted_ranges,
            ml_clicker_runtime.emitted_samples,
            ml_clicker_runtime.buffered_frames,
            ml_clicker_runtime.flushed_frames,
            ml_clicker_runtime.notify_failures,
            session.diagnostics.ds_twr_successes,
            session.diagnostics.ds_twr_failures);
    (void)ml_clicker_send_command_result(&request, status, 0u);
    atomic_clear(&ml_clicker_busy);
}
#endif


int app_ml_init(void)
{
#if defined(CONFIG_IMEC_ML_CLICKER)
    if (DEVICE_ROLE == ROLE_CLICKER) {
        int ret;

        (void)app_clicker_start_work_queue();
        k_work_init(&ml_clicker_collect_work, ml_clicker_collect_work_handler);
        k_work_init_delayable(&ml_clicker_rainbow_led_work,
                              ml_clicker_rainbow_led_work_handler);
        (void)k_work_schedule(&ml_clicker_rainbow_led_work, K_NO_WAIT);
        ret = gateway_ble_init();
        if (ret < 0) {
            LOG_ERR("ML clicker BLE PC link unavailable: %d", ret);
        }
        LOG_INF("ML clicker ready; BLE-triggered UWB collection enabled");
    }
#endif
#if defined(CONFIG_IMEC_ML_ANCHOR)
    if (DEVICE_ROLE == ROLE_ANCHOR) {
        int ret;

        k_work_init_delayable(&ml_anchor_battery_led_work,
                              ml_anchor_battery_led_work_handler);
        (void)k_work_schedule(&ml_anchor_battery_led_work, K_NO_WAIT);
        ret = gateway_ble_init();
        if (ret < 0) {
            LOG_ERR("ML anchor BLE debug log link unavailable: %d", ret);
        }
    }
#endif
    return 0;
}

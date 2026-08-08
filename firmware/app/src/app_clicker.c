#include "app_clicker.h"
#include "app_clicker_event_runtime.h"

#include "app_board.h"
#include "app_click_event_sequence.h"
#include "app_radio_recovery.h"
#include "app_config.h"
#include "app_node_comm.h"
#include "app_radio_low_power_policy.h"
#include "app_stack_workload_diag.h"
#include "app_state.h"
#include "app_wake_train_politeness.h"
#include "app_watchdog.h"
#include "button_action_handoff.h"
#include "button_wake_recovery.h"
#include "dwm3000_driver.h"
#include "dwm3000_port.h"
#include "firmware_state_machines.h"
#include "mesh_relay.h"
#include "report.h"
#include "uwb.h"
#include "uwb_ble_courtesy.h"
#include "uwb_session.h"

#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/net_buf.h>
#if defined(CONFIG_BT_LL_SOFTDEVICE_HEADERS_INCLUDE)
#include <bluetooth/hci_vs_sdc.h>
#endif
#endif
#include <hal/nrf_gpio.h>
#include <hal/nrf_power.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_clicker, LOG_LEVEL_DBG);

#define CLICKER_POLITENESS_UWB_RESTART 1
#define BLE_COURTESY_STOP_RETRY_COUNT 3u
#define BLE_COURTESY_STOP_RETRY_DELAY_MS 5u
#define CLICK_BUTTON_RECOVERY_MAX_FAILURES 8u
#define CLICK_BUTTON_RECOVERY_RETRY_MS 20u
#define CLICK_BUTTON_RECOVERY_REBOOT_DELAY_MS 1000u
#define CLICKER_STATUS_LED_CONNECT_ATTEMPTS 2u
#define CLICKER_STATUS_LED_CONNECT_RETRY_US 100u

static const struct app_clicker_attempt_gate_config clicker_attempt_gate_config = {
    .wake_adv_ms = WAKE_ADV_MS,
    .max_politeness_wait_ms = MAX_POLITENESS_WAIT_MS,
    .polite_sample_rx_ms = UWB_POLITE_SAMPLE_RX_MS,
    .polite_required_quiet_samples = UWB_POLITE_REQUIRED_QUIET_SAMPLES,
    .polite_relevant_frame_wait_ms = UWB_POLITE_RELEVANT_FRAME_WAIT_MS,
    .ble_courtesy_min_window_ms = BLE_COURTESY_MIN_WINDOW_MS,
    .ble_courtesy_peer_finish_ms = BLE_COURTESY_PEER_FINISH_MS,
    .ble_courtesy_max_defers_per_attempt = BLE_COURTESY_MAX_DEFERS_PER_ATTEMPT,
};

static const struct app_clicker_wake_train_config clicker_wake_train_config = {
    .wake_adv_ms = WAKE_ADV_MS,
    .post_wake_claimed_duration_ms = UWB_POST_WAKE_CLAIMED_DURATION_MS,
    .control_tx_timeout_ms = UWB_CONTROL_TX_TIMEOUT_MS,
};

static const struct app_clicker_range_tx_config clicker_range_tx_config = {
    .control_tx_timeout_ms = UWB_CONTROL_TX_TIMEOUT_MS,
    .prepare_range_mode_after_schedule = false,
};

#define SELF_TEST_REPORT_DELIVERY_TIMEOUT_MS CLICK_REPORT_DEADLINE_MS
#define SELF_TEST_REPORT_DELIVERY_POLL_MS 20u

BUILD_ASSERT(SELF_TEST_REPORT_DELIVERY_POLL_MS <
                 SELF_TEST_REPORT_DELIVERY_TIMEOUT_MS,
             "self-test report polling must fit its reliable-delivery bound");

static struct app_clicker_callbacks clicker_callbacks;
static struct app_clicker_event_runtime clicker_event_runtime;

int app_clicker_ble_courtesy_start(uint32_t event_seq,
                                   uint8_t attempt_index,
                                   uint64_t priority_id,
                                   uint32_t peer_finish_ms);
uint32_t app_clicker_ble_courtesy_higher_wait_ms(void);
void app_clicker_ble_courtesy_stop(void);
void app_clicker_arm_self_test_timeout(void);
void app_clicker_cancel_self_test_timeout(void);

uint32_t app_clicker_bound_delay_ms(uint32_t requested_ms,
                                    int64_t deadline_ms,
                                    uint32_t required_after_delay_ms)
{
    int64_t now_ms = k_uptime_get();
    int64_t latest_start_ms = deadline_ms - required_after_delay_ms - 1;
    int64_t available_ms = latest_start_ms - now_ms;

    if (available_ms <= 0) {
        return 0u;
    }
    if ((uint64_t)requested_ms > (uint64_t)available_ms) {
        return (uint32_t)available_ms;
    }
    return requested_ms;
}

uint32_t app_clicker_sleep_bounded(uint32_t requested_ms,
                                   int64_t deadline_ms,
                                   uint32_t required_after_delay_ms)
{
    uint32_t bounded_ms = app_clicker_bound_delay_ms(requested_ms,
                                                     deadline_ms,
                                                     required_after_delay_ms);

    if (bounded_ms > 0u) {
        k_msleep(bounded_ms);
    }
    return bounded_ms;
}

uint32_t app_clicker_apply_contention_delay(struct uwb_clicker_session *session,
                                            int64_t click_deadline_ms,
                                            uint32_t required_after_delay_ms)
{
    uint32_t requested_ms;
    uint32_t delay_ms;

    if (session == NULL) {
        return 0u;
    }

    requested_ms = uwb_clicker_contention_delay_ms(session->attempt_index,
                                                  sys_rand32_get());
    delay_ms = app_clicker_sleep_bounded(requested_ms,
                                         click_deadline_ms,
                                         required_after_delay_ms);
    uwb_clicker_note_contention_delay(session, delay_ms);
    LOG_INF("clicker contention delay: attempt=%u requested_ms=%u applied_ms=%u",
            session->attempt_index,
            requested_ms,
            delay_ms);
    return delay_ms;
}

uint32_t app_clicker_apply_retry_delay(struct uwb_clicker_session *session,
                                       int64_t click_deadline_ms,
                                       uint32_t retry_base_delay_ms,
                                       uint32_t required_after_delay_ms)
{
    uint32_t delay_ms;

    if (session == NULL) {
        return 0u;
    }

    delay_ms = app_clicker_sleep_bounded(retry_base_delay_ms,
                                         click_deadline_ms,
                                         required_after_delay_ms);
    uwb_clicker_note_retry_delay(session, delay_ms);
    LOG_INF("clicker retry base delay: attempt=%u requested_ms=%u applied_ms=%u",
            session->attempt_index,
            retry_base_delay_ms,
            delay_ms);
    return delay_ms;
}

static int clicker_sample_uwb_gate(struct uwb_clicker_session *session,
                                   uint32_t listen_ms,
                                   uint32_t *uwb_restart_wait_ms,
                                   uint32_t *sample_count,
                                   uint32_t *activity_count,
                                   uint8_t *quiet_samples,
                                   const struct app_clicker_attempt_gate_config *config)
{
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    uint16_t relevant_wait_ms = 0u;
    uint8_t frame_type = 0u;
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    bool channel_activity_detected = false;
    int ret;

    ret = dwm3000_driver_receive_frame_detailed(listen_ms,
                                                frame,
                                                sizeof(frame),
                                                &frame_len,
                                                NULL,
                                                NULL,
                                                &rx_failure);
    if (ret == 0) {
        int decode_ret = uwb_clicker_decode_politeness_wait(
            session,
            frame,
            frame_len,
            config->polite_relevant_frame_wait_ms,
            &relevant_wait_ms,
            &frame_type);

        channel_activity_detected = true;
        if (decode_ret == PROTO_OK && relevant_wait_ms > 0u) {
            if (uwb_restart_wait_ms != NULL) {
                *uwb_restart_wait_ms = relevant_wait_ms;
            }
            ret = CLICKER_POLITENESS_UWB_RESTART;
            LOG_INF("clicker relevant UWB gate packet: type=0x%02x wait_ms=%u frame_len=%u",
                    frame_type,
                    relevant_wait_ms,
                    (unsigned int)frame_len);
        } else if (decode_ret != PROTO_OK) {
            LOG_DBG("clicker ignored undecodable UWB gate packet: type=0x%02x ret=%d frame_len=%u",
                    frame_type,
                    decode_ret,
                    (unsigned int)frame_len);
        } else {
            LOG_INF("clicker UWB gate observed channel activity: type=0x%02x frame_len=%u",
                    frame_type,
                    (unsigned int)frame_len);
        }
    } else if (app_wake_train_politeness_rx_activity(ret, rx_failure)) {
        channel_activity_detected = true;
        LOG_INF("clicker UWB gate observed partial channel activity: ret=%d failure=%u",
                ret,
                (unsigned int)rx_failure);
        ret = 0;
    } else if (ret != -ETIMEDOUT) {
        LOG_WRN("clicker UWB gate aborted on hard receive failure: ret=%d failure=%u",
                ret,
                (unsigned int)rx_failure);
        return ret;
    } else {
        ret = 0;
    }
    if (sample_count != NULL) {
        (*sample_count)++;
    }
    uwb_clicker_note_politeness_sample(session, channel_activity_detected);

    if (channel_activity_detected) {
        if (quiet_samples != NULL) {
            *quiet_samples = 0u;
        }
        if (activity_count != NULL) {
            (*activity_count)++;
        }
    } else if (quiet_samples != NULL) {
        (*quiet_samples)++;
    }

    return ret;
}

static int clicker_politeness_phase(struct uwb_clicker_session *session,
                                    uint32_t event_seq,
                                    uint64_t priority_id,
                                    bool use_ble_courtesy,
                                    int64_t click_deadline_ms,
                                    uint32_t *uwb_restart_wait_ms,
                                    uint32_t *ble_defer_wait_ms,
                                    const struct app_clicker_attempt_gate_config *config)
{
    int64_t now_ms;
    int64_t deadline_ms;
    uint32_t phase_budget_ms;
    uint8_t quiet_samples = 0u;
    uint32_t sample_count = 0u;
    uint32_t activity_count = 0u;
    bool ble_started = false;
    int64_t ble_courtesy_until_ms = 0;
    int release_ret;
    int ret;

    if (session == NULL || config == NULL) {
        return -EINVAL;
    }
    now_ms = k_uptime_get();
    if (!app_wake_train_deadline_clip_delay(
            now_ms,
            click_deadline_ms,
            config->max_politeness_wait_ms,
            config->wake_adv_ms,
            &phase_budget_ms)) {
        return -ETIMEDOUT;
    }
    if (phase_budget_ms == 0u) {
        return -ETIMEDOUT;
    }
    deadline_ms = now_ms + phase_budget_ms;
    if (uwb_restart_wait_ms != NULL) {
        *uwb_restart_wait_ms = 0u;
    }
    if (ble_defer_wait_ms != NULL) {
        *ble_defer_wait_ms = 0u;
    }

    if (use_ble_courtesy) {
        ret = app_clicker_ble_courtesy_start(event_seq,
                                             session->attempt_index,
                                             priority_id,
                                             config->ble_courtesy_peer_finish_ms);
        if (ret == 0) {
            ble_started = true;
            ble_courtesy_until_ms = k_uptime_get() + config->ble_courtesy_min_window_ms;
        } else {
            LOG_WRN("BLE courtesy unavailable for attempt=%u: %d",
                    session->attempt_index,
                    ret);
        }
    }

    ret = radio_guard_uwb_start("clicker politeness sniff");
    if (ret < 0) {
        if (ble_started) {
            app_clicker_ble_courtesy_stop();
        }
        return ret;
    }
    ret = dwm3000_driver_configure_range_mode();
    if (ret < 0) {
        release_ret = app_radio_standby_with_bounded_recovery(
            "politeness configure failure");
        radio_guard_uwb_stop();
        if (ble_started) {
            app_clicker_ble_courtesy_stop();
        }
        return release_ret < 0 ? release_ret : ret;
    }
    if (k_uptime_get() >= deadline_ms) {
        release_ret = app_radio_standby_with_bounded_recovery(
            "politeness deadline");
        radio_guard_uwb_stop();
        if (ble_started) {
            app_clicker_ble_courtesy_stop();
        }
        return release_ret < 0 ? release_ret : -ETIMEDOUT;
    }

    /* BLE courtesy remains active while the external DWM3000 rearms without gaps. */
    while ((quiet_samples < config->polite_required_quiet_samples ||
            (ble_started && k_uptime_get() < ble_courtesy_until_ms)) &&
           k_uptime_get() < deadline_ms) {
        int64_t remaining_ms = deadline_ms - k_uptime_get();
        uint32_t listen_ms;

        if (remaining_ms <= 0) {
            break;
        }
        listen_ms = MIN(config->polite_sample_rx_ms, (uint32_t)remaining_ms);
        if (listen_ms == 0u) {
            break;
        }

        ret = clicker_sample_uwb_gate(session,
                                      listen_ms,
                                      uwb_restart_wait_ms,
                                      &sample_count,
                                      &activity_count,
                                      &quiet_samples,
                                      config);
        if (ret == CLICKER_POLITENESS_UWB_RESTART) {
            break;
        }
        if (ret < 0) {
            /*
             * A hard receive failure is not evidence of a quiet channel.  Stop
             * this attempt and preserve the error through radio cleanup so a
             * broken DWM3000 path cannot be treated as politeness success.
             */
            break;
        }
        if (ble_started) {
            uint32_t peer_wait_ms = app_clicker_ble_courtesy_higher_wait_ms();

            if (peer_wait_ms > 0u) {
                if (ble_defer_wait_ms != NULL) {
                    *ble_defer_wait_ms = peer_wait_ms;
                }
                ret = -EAGAIN;
                break;
            }
        }

    }
    if (ret == 0 && ble_started) {
        uint32_t peer_wait_ms = app_clicker_ble_courtesy_higher_wait_ms();

        if (peer_wait_ms > 0u) {
            if (ble_defer_wait_ms != NULL) {
                *ble_defer_wait_ms = peer_wait_ms;
            }
            ret = -EAGAIN;
        }
    }

    release_ret = app_radio_standby_with_bounded_recovery(
        "politeness complete");
    radio_guard_uwb_stop();
    if (ble_started) {
        app_clicker_ble_courtesy_stop();
    }
    if (release_ret < 0) {
        LOG_ERR("clicker politeness radio release failed: %d",
                release_ret);
        return release_ret;
    }
    if (ret == -EAGAIN) {
        LOG_INF("clicker BLE courtesy deferred attempt=%u event_seq=%u priority=%llx peer_wait_ms=%u",
                session->attempt_index,
                event_seq,
                (unsigned long long)priority_id,
                ble_defer_wait_ms != NULL ? *ble_defer_wait_ms : 0u);
        return ret;
    }
    if (ret == CLICKER_POLITENESS_UWB_RESTART) {
        LOG_INF("clicker UWB gate will restart after relevant packet wait: attempt=%u wait_ms=%u samples=%u activity=%u",
                session->attempt_index,
                uwb_restart_wait_ms != NULL ? *uwb_restart_wait_ms : 0u,
                sample_count,
                activity_count);
        return ret;
    }
    if (ret < 0) {
        LOG_ERR("clicker UWB politeness receive failed closed: attempt=%u ret=%d samples=%u activity=%u",
                session->attempt_index,
                ret,
                sample_count,
                activity_count);
        return ret;
    }
    if (quiet_samples < config->polite_required_quiet_samples) {
        LOG_WRN("clicker UWB politeness exhausted without required quiet window: quiet_samples=%u/%u samples=%u activity=%u max_wait_ms=%u; proceeding with bounded click priority",
                quiet_samples,
                config->polite_required_quiet_samples,
                sample_count,
                activity_count,
                config->max_politeness_wait_ms);
    }
    LOG_INF("clicker sampled politeness complete: quiet_samples=%u/%u samples=%u activity=%u max_wait_ms=%u",
            quiet_samples,
            config->polite_required_quiet_samples,
            sample_count,
            activity_count,
            config->max_politeness_wait_ms);
    return 0;
}

int app_clicker_attempt_gate(struct uwb_clicker_session *session,
                             uint32_t event_seq,
                             uint64_t priority_id,
                             int64_t click_deadline_ms,
                             bool use_ble_courtesy,
                             const struct app_clicker_attempt_gate_config *config)
{
    uint8_t defer_count = 0u;
    bool ble_courtesy_allowed = use_ble_courtesy;
    int ret;

    if (session == NULL || config == NULL) {
        return -EINVAL;
    }

    while (true) {
        uint32_t uwb_restart_wait_ms = 0u;
        uint32_t ble_defer_wait_ms = 0u;

        if (k_uptime_get() + config->wake_adv_ms >= click_deadline_ms) {
            return -ETIMEDOUT;
        }
        ret = clicker_politeness_phase(session,
                                       event_seq,
                                       priority_id,
                                       ble_courtesy_allowed,
                                       click_deadline_ms,
                                       &uwb_restart_wait_ms,
                                       &ble_defer_wait_ms,
                                       config);
        if (ret == CLICKER_POLITENESS_UWB_RESTART) {
            uint32_t slept_ms = app_clicker_sleep_bounded(uwb_restart_wait_ms,
                                                          click_deadline_ms,
                                                          config->wake_adv_ms);

            LOG_INF("clicker UWB gate restart: event_seq=%u attempt=%u requested_wait_ms=%u slept_ms=%u",
                    event_seq,
                    session->attempt_index,
                    uwb_restart_wait_ms,
                    slept_ms);
            continue;
        }
        if (ret != -EAGAIN) {
            break;
        }
        if (defer_count >= config->ble_courtesy_max_defers_per_attempt) {
            LOG_WRN("BLE courtesy defer cap reached: event_seq=%u attempt=%u",
                    event_seq,
                    session->attempt_index);
            ble_courtesy_allowed = false;
            continue;
        }
        defer_count++;
        if (ble_defer_wait_ms == 0u) {
            ble_defer_wait_ms = config->ble_courtesy_peer_finish_ms;
        }
        LOG_INF("BLE courtesy peer defer: event_seq=%u attempt=%u defer=%u/%u requested_wait_ms=%u",
                event_seq,
                session->attempt_index,
                defer_count,
                config->ble_courtesy_max_defers_per_attempt,
                ble_defer_wait_ms);
        (void)app_clicker_sleep_bounded(ble_defer_wait_ms,
                                        click_deadline_ms,
                                        config->wake_adv_ms);
        if (k_uptime_get() + config->wake_adv_ms >= click_deadline_ms) {
            return -ETIMEDOUT;
        }
    }
    if (ret < 0) {
        return ret;
    }

    return 0;
}

static int clicker_wake_train_sniff_activity(const char *phase,
                                             struct uwb_clicker_session *session,
                                             uint8_t retry_index,
                                             int64_t deadline_ms,
                                             uint32_t required_after_sniff_ms,
                                             bool *activity)
{
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    int release_ret;
    int ret;

    if (activity == NULL) {
        return -EINVAL;
    }
    *activity = false;

    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        return ret;
    }
    if (!app_wake_train_deadline_fits(
            k_uptime_get(),
            deadline_ms,
            u32_saturating_add(APP_WAKE_TRAIN_POLITE_SNIFF_MS,
                               required_after_sniff_ms))) {
        release_ret = app_radio_standby_with_bounded_recovery(
            "wake sniff deadline");
        return release_ret < 0 ? release_ret : -ETIMEDOUT;
    }
    ret = dwm3000_driver_sniff_activity(APP_WAKE_TRAIN_POLITE_SNIFF_MS,
                                        &rx_failure);
    release_ret = app_radio_standby_with_bounded_recovery(
        "wake sniff complete");
    if (release_ret < 0) {
        LOG_ERR("clicker wake sniff radio release failed: phase=%s ret=%d",
                phase == NULL ? "unknown" : phase,
                release_ret);
        return release_ret;
    }

    *activity = app_wake_train_politeness_rx_activity(ret, rx_failure);
    if (*activity) {
        LOG_INF("clicker wake train C5 activity during %s sniff: event_seq=%u attempt=%u retry=%u ret=%d failure=%u",
                phase == NULL ? "unknown" : phase,
                session != NULL ? session->config.click_event_id : 0u,
                session != NULL ? session->attempt_index : 0u,
                retry_index,
                ret,
                (unsigned int)rx_failure);
        return 0;
    }

    return ret == -ETIMEDOUT ? 0 : ret;
}

uint32_t app_clicker_wake_train_opportunity_tail_ms(
    const struct app_clicker_wake_train_config *config)
{
    uint64_t required_ms;

    if (config == NULL) {
        return UINT32_MAX;
    }
    required_ms = (2ull * APP_WAKE_TRAIN_POLITE_SNIFF_MS) +
                  config->wake_adv_ms +
                  config->control_tx_timeout_ms +
                  config->post_wake_claimed_duration_ms;
    return required_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)required_ms;
}

static uint32_t clicker_wake_train_after_pre_tail_ms(
    const struct app_clicker_wake_train_config *config)
{
    uint64_t required_ms;

    if (config == NULL) {
        return UINT32_MAX;
    }
    required_ms = APP_WAKE_TRAIN_POLITE_SNIFF_MS +
                  config->wake_adv_ms +
                  config->control_tx_timeout_ms +
                  config->post_wake_claimed_duration_ms;
    return required_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)required_ms;
}

static int clicker_wake_train_backoff(struct uwb_clicker_session *session,
                                      const char *phase,
                                      uint8_t retry_index,
                                      int64_t deadline_ms,
                                      uint32_t required_tail_ms)
{
    uint32_t requested_delay_ms = app_wake_train_politeness_backoff_ms(
        retry_index,
        sys_rand32_get());
    uint32_t delay_ms;

    if (!app_wake_train_deadline_clip_delay(k_uptime_get(),
                                             deadline_ms,
                                             requested_delay_ms,
                                             required_tail_ms,
                                             &delay_ms)) {
        return -ETIMEDOUT;
    }

    LOG_INF("clicker wake train deferred after C5 activity: event_seq=%u attempt=%u retry=%u/%u phase=%s requested_backoff_ms=%u backoff_ms=%u",
            session != NULL ? session->config.click_event_id : 0u,
            session != NULL ? session->attempt_index : 0u,
            (uint32_t)retry_index + 1u,
            APP_WAKE_TRAIN_POLITE_MAX_RETRIES,
            phase == NULL ? "unknown" : phase,
            requested_delay_ms,
            delay_ms);
    k_msleep(delay_ms);
    return 0;
}

static uint16_t clicker_claimed_duration_ms(
    uint16_t wake_train_ends_in_ms,
    const struct app_clicker_wake_train_config *config)
{
    uint32_t claimed_ms = (uint32_t)wake_train_ends_in_ms +
                          config->post_wake_claimed_duration_ms;

    return claimed_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)claimed_ms;
}

static int clicker_send_wake_claim_train_until(
    struct uwb_clicker_session *session,
    uint64_t priority_id,
    const struct app_clicker_wake_train_config *config,
    int64_t deadline_ms)
{
    uint8_t frame[UWB_WAKE_CLAIM_LEN];
    uint8_t polite_retry = 0u;
    uint32_t required_after_pre_ms;
    uint32_t required_tail_ms;
    int release_ret;
    int ret = -EAGAIN;

    if (session == NULL || config == NULL) {
        return -EINVAL;
    }
    required_tail_ms = app_clicker_wake_train_opportunity_tail_ms(config);
    required_after_pre_ms =
        clicker_wake_train_after_pre_tail_ms(config);

    while (polite_retry <= APP_WAKE_TRAIN_POLITE_MAX_RETRIES) {
        size_t frame_len = 0u;
        int64_t close_ms;
        uint16_t sent_count = 0u;
        bool c5_activity = false;
        const char *activity_phase = NULL;

        if (!app_wake_train_deadline_fits(k_uptime_get(),
                                           deadline_ms,
                                           required_tail_ms)) {
            LOG_INF("clicker wake train deadline cannot fit complete opportunity: event_seq=%u attempt=%u retry=%u required_ms=%u",
                    session->config.click_event_id,
                    session->attempt_index,
                    polite_retry,
                    required_tail_ms);
            return -ETIMEDOUT;
        }

        ret = radio_guard_uwb_start("clicker UWB WAKE_CLAIM train");
        if (ret < 0) {
            status_debug_note("DBG_WAKE_TRAIN_GUARD_FAIL\n");
            LOG_WRN("clicker UWB WAKE_CLAIM guard failed: ret=%d", ret);
            return ret;
        }
        status_debug_note("DBG_WAKE_TRAIN_GUARD_OK\n");

        ret = clicker_wake_train_sniff_activity("pre",
                                                session,
                                                polite_retry,
                                                deadline_ms,
                                                required_after_pre_ms,
                                                &c5_activity);
        if (ret < 0) {
            goto attempt_out;
        }
        if (c5_activity) {
            activity_phase = "pre";
            ret = -EAGAIN;
            goto attempt_out;
        }


        status_debug_note("DBG_WAKE_TRAIN_CONFIG_BEGIN\n");
        ret = dwm3000_driver_configure_wake_mode();
        if (ret < 0) {
            status_debug_note("DBG_WAKE_TRAIN_CONFIG_FAIL\n");
            LOG_WRN("clicker UWB WAKE_CLAIM wake-mode config failed: ret=%d",
                    ret);
            goto attempt_out;
        }
        status_debug_note("DBG_WAKE_TRAIN_CONFIG_OK\n");
        if (!app_wake_train_deadline_fits(k_uptime_get(),
                                           deadline_ms,
                                           required_after_pre_ms)) {
            ret = -ETIMEDOUT;
            goto attempt_out;
        }

        close_ms = k_uptime_get() + config->wake_adv_ms;
        while (k_uptime_get() < close_ms) {
            struct uwb_wake_claim_frame claim;
            int64_t remaining_ms = close_ms - k_uptime_get();
            uint16_t remaining_u16 = delay_ms_to_u16(remaining_ms);

            ret = uwb_clicker_build_wake_claim(
                session,
                priority_id,
                remaining_u16,
                remaining_u16,
                clicker_claimed_duration_ms(remaining_u16, config),
                &claim);
            if (ret != PROTO_OK) {
                status_debug_note("DBG_WAKE_TRAIN_BUILD_FAIL\n");
                LOG_WRN("clicker UWB WAKE_CLAIM build failed: proto_ret=%d",
                        ret);
                ret = -EINVAL;
                break;
            }
            ret = uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len);
            if (ret != PROTO_OK) {
                status_debug_note("DBG_WAKE_TRAIN_ENCODE_FAIL\n");
                LOG_WRN("clicker UWB WAKE_CLAIM encode failed: proto_ret=%d",
                        ret);
                ret = -EINVAL;
                break;
            }

            if (sent_count == 0u) {
                status_debug_note("DBG_WAKE_TRAIN_FIRST_SEND_BEGIN\n");
            }
            if (!app_wake_train_deadline_fits(
                    k_uptime_get(),
                    deadline_ms,
                    config->control_tx_timeout_ms +
                        APP_WAKE_TRAIN_POLITE_SNIFF_MS +
                        config->post_wake_claimed_duration_ms)) {
                ret = -ETIMEDOUT;
                break;
            }
            ret = dwm3000_driver_send_frame(frame,
                                            frame_len,
                                            config->control_tx_timeout_ms);
            if (ret < 0) {
                status_debug_note("DBG_WAKE_TRAIN_FIRST_SEND_FAIL\n");
                LOG_WRN("clicker UWB WAKE_CLAIM send failed: sent=%u ret=%d frame_len=%u",
                        sent_count,
                        ret,
                        (unsigned int)frame_len);
                break;
            }
            if (sent_count == 0u) {
                status_debug_note("DBG_WAKE_TRAIN_FIRST_SEND_OK\n");
            }
            status_debug_tx_wake_claim_sent_pulse();
            sent_count++;
            uwb_clicker_note_wake_claim_tx(session, 1u);

            if (k_uptime_get() < close_ms) {
                uint32_t jitter_us =
                    uwb_clicker_wake_claim_jitter_us(sys_rand32_get());
                int64_t remaining_after_tx_ms = close_ms - k_uptime_get();

                if (jitter_us > 0u && remaining_after_tx_ms > 0) {
                    k_busy_wait(jitter_us);
                }
            }
        }

        if (ret >= 0 && sent_count > 0u) {
            if (!app_wake_train_deadline_fits(
                    k_uptime_get(),
                    deadline_ms,
                    APP_WAKE_TRAIN_POLITE_SNIFF_MS +
                        config->post_wake_claimed_duration_ms)) {
                ret = -ETIMEDOUT;
                goto attempt_out;
            }
            ret = clicker_wake_train_sniff_activity("post",
                                                    session,
                                                    polite_retry,
                                                    deadline_ms,
                                                    config->post_wake_claimed_duration_ms,
                                                    &c5_activity);
            if (ret < 0) {
                goto attempt_out;
            }
            if (c5_activity) {
                activity_phase = "post";
                ret = -EAGAIN;
                goto attempt_out;
            }
        }

attempt_out:
        release_ret = app_radio_standby_with_bounded_recovery(
            "wake claim train");
        radio_guard_uwb_stop();
        if (release_ret < 0) {
            LOG_ERR("clicker wake train radio release failed: %d",
                    release_ret);
            ret = release_ret;
        }
        if (ret == -EAGAIN && c5_activity &&
            polite_retry < APP_WAKE_TRAIN_POLITE_MAX_RETRIES) {
            ret = clicker_wake_train_backoff(session,
                                             activity_phase,
                                             polite_retry,
                                             deadline_ms,
                                             required_tail_ms);
            if (ret < 0) {
                return ret;
            }
            polite_retry++;
            continue;
        }
        if (ret < 0) {
            LOG_WRN("clicker UWB WAKE_CLAIM train failed: sent=%u ret=%d retry=%u activity=%u phase=%s",
                    sent_count,
                    ret,
                    polite_retry,
                    c5_activity ? 1u : 0u,
                    activity_phase == NULL ? "none" : activity_phase);
            return ret;
        }

        LOG_INF("clicker UWB WAKE_CLAIM train complete: sent=%u duration_ms=%u retry=%u",
                sent_count,
                config->wake_adv_ms,
                polite_retry);
        return sent_count == 0u ? -ETIMEDOUT : 0;
    }

    return ret;
}

int app_clicker_send_wake_claim_train(
    struct uwb_clicker_session *session,
    uint64_t priority_id,
    const struct app_clicker_wake_train_config *config)
{
    return app_clicker_send_wake_claim_train_until(session,
                                                   priority_id,
                                                   config,
                                                   INT64_MAX);
}

int app_clicker_send_wake_claim_train_until(
    struct uwb_clicker_session *session,
    uint64_t priority_id,
    const struct app_clicker_wake_train_config *config,
    int64_t deadline_ms)
{
    return clicker_send_wake_claim_train_until(session,
                                               priority_id,
                                               config,
                                               deadline_ms);
}

static void clicker_log_range_schedule_entries(const struct uwb_range_schedule_frame *schedule)
{
    if (schedule == NULL) {
        return;
    }

    for (uint8_t i = 0u; i < schedule->selected_count; i++) {
        const struct uwb_range_schedule_entry *entry = &schedule->entries[i];

        LOG_INF("clicker UWB RANGE_SCHEDULE entry: order=%u/%u anchor=0x%016llx seq_base=%u samples=%u first_poll_ms=%u stride_us=%u burst_ms=%u",
                (unsigned int)(i + 1u),
                schedule->selected_count,
                (unsigned long long)entry->anchor_id,
                entry->seq,
                entry->sample_count,
                schedule->first_poll_delay_ms,
                schedule->exchange_stride_us,
                schedule->burst_window_ms);
    }
}

static uint32_t clicker_range_slot_timeout_ms(
    const struct uwb_range_schedule_frame *schedule)
{
    uint32_t stride_timeout_ms;

    if (schedule == NULL) {
        return UINT32_MAX;
    }
    stride_timeout_ms = u32_saturating_add(
        (uint32_t)ceil_us_to_ms(schedule->exchange_stride_us),
        UWB_SCHEDULE_GUARD_MS);
    return MIN(CLICK_UWB_TIMEOUT_MS, stride_timeout_ms);
}

static uint32_t clicker_range_schedule_tail_ms(
    const struct uwb_range_schedule_frame *schedule)
{
    size_t total_samples;
    uint64_t tail_us;

    if (schedule == NULL) {
        return UINT32_MAX;
    }
    total_samples = uwb_range_schedule_total_samples(schedule);
    if (total_samples == 0u) {
        return UINT32_MAX;
    }

    /*
     * The burst window starts at the first scheduled POLL, not at schedule
     * transmission. Budget from the schedule-TX completion through the last
     * complete exchange so a late setup cannot emit a schedule that the
     * clicker must abandon before its advertised burst is over.
     */
    tail_us = ((uint64_t)schedule->first_poll_delay_ms * 1000u) +
              ((uint64_t)(total_samples - 1u) *
               schedule->exchange_stride_us) +
              ((uint64_t)clicker_range_slot_timeout_ms(schedule) * 1000u);
    return tail_us > ((uint64_t)UINT32_MAX * 1000u) ?
           UINT32_MAX : (uint32_t)((tail_us + 999u) / 1000u);
}

static uint32_t clicker_range_schedule_deadline_budget_ms(
    const struct uwb_range_schedule_frame *schedule,
    const struct app_clicker_range_tx_config *config)
{
    if (config == NULL) {
        return UINT32_MAX;
    }
    return u32_saturating_add(
        config->control_tx_timeout_ms,
        u32_saturating_add(clicker_range_schedule_tail_ms(schedule),
                           CLICK_REPORT_BUILD_GUARD_MS));
}

static int clicker_send_range_schedule_until(
    const struct uwb_range_schedule_frame *schedule,
    const struct app_clicker_range_tx_config *config,
    int64_t deadline_ms,
    int64_t *schedule_tx_ms)
{
    uint8_t frame[UWB_RANGE_SCHEDULE_MAX_LEN];
    size_t frame_len = 0u;
    int64_t tx_complete_ms = -1;
    int release_ret;
    int ret;

    if (schedule_tx_ms != NULL) {
        *schedule_tx_ms = -1;
    }
    if (schedule == NULL || config == NULL) {
        return -EINVAL;
    }

    ret = uwb_encode_range_schedule(schedule, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = radio_guard_uwb_start("clicker UWB RANGE_SCHEDULE");
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        if (!app_wake_train_deadline_fits(
                k_uptime_get(),
                deadline_ms,
                clicker_range_schedule_deadline_budget_ms(schedule,
                                                          config))) {
            ret = -ETIMEDOUT;
        } else {
            ret = dwm3000_driver_send_frame(frame,
                                            frame_len,
                                            config->control_tx_timeout_ms);
            if (ret == 0) {
                tx_complete_ms = k_uptime_get();
            }
        }
    }
    if (ret == 0 && config->prepare_range_mode_after_schedule) {
        int prep_ret = dwm3000_driver_configure_range_mode();

        if (prep_ret < 0) {
            LOG_WRN("ML clicker range-mode prep after RANGE_SCHEDULE failed: %d",
                    prep_ret);
            ret = prep_ret;
        }
        release_ret = app_radio_idle_with_bounded_recovery(
            "range schedule prepared");
    } else {
        release_ret = app_radio_standby_with_bounded_recovery(
            "range schedule complete");
    }
    radio_guard_uwb_stop();
    if (ret >= 0 && release_ret < 0) {
        ret = release_ret;
    }

    if (ret < 0) {
        LOG_WRN("clicker UWB RANGE_SCHEDULE TX failed: ret=%d", ret);
        return ret;
    }
    if (schedule_tx_ms != NULL) {
        *schedule_tx_ms = tx_complete_ms;
    }
    LOG_INF("clicker UWB RANGE_SCHEDULE TX complete: selected=%u samples_per_anchor=%u",
            schedule->selected_count,
            schedule->samples_per_anchor);
    clicker_log_range_schedule_entries(schedule);
    return 0;
}

int app_clicker_send_range_release(struct uwb_clicker_session *session,
                                   uint8_t reason,
                                   const struct app_clicker_range_tx_config *config)
{
    struct uwb_range_release_frame release;
    uint8_t frame[UWB_RANGE_RELEASE_LEN];
    size_t frame_len = 0u;
    int release_ret;
    int ret;

    if (session == NULL || config == NULL) {
        return -EINVAL;
    }

    ret = uwb_clicker_build_range_release(session, reason, &release);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = uwb_encode_range_release(&release, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = radio_guard_uwb_start("clicker UWB RANGE_RELEASE");
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        ret = dwm3000_driver_send_frame(frame,
                                        frame_len,
                                        config->control_tx_timeout_ms);
    }
    release_ret = app_radio_standby_with_bounded_recovery(
        "range release complete");
    radio_guard_uwb_stop();
    if (ret >= 0 && release_ret < 0) {
        ret = release_ret;
    }

    if (ret < 0) {
        LOG_WRN("clicker UWB RANGE_RELEASE TX failed: ret=%d", ret);
        return ret;
    }
    LOG_INF("clicker UWB RANGE_RELEASE TX complete: candidates=%u min=%u reason=%u",
            release.discovered_anchor_count,
            release.min_anchor_count,
            release.reason);
    return 0;
}



static int clicker_discover_uwb_anchors_until(
    struct uwb_clicker_session *session,
    int64_t absolute_deadline_ms)
{
    struct uwb_discover_frame discover;
    uint8_t frame[UWB_DISCOVERY_REPLY_LEN];
    size_t frame_len = 0u;
    int64_t deadline_ms;
    uint32_t reply_window_ms;
    uint16_t rx_frames = 0u;
    uint16_t decoded_replies = 0u;
    uint16_t malformed_frames = 0u;
    uint16_t rejected_replies = 0u;
    int release_ret;
    int ret;
    int last_ret = -ETIMEDOUT;

    ret = uwb_clicker_build_discover(session, &discover);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
#if defined(CONFIG_IMEC_ML_CLICKER)
    if (clicker_callbacks.ml_discovery_slot_count_override != NULL) {
        uint8_t slot_count = clicker_callbacks.ml_discovery_slot_count_override();

        if (slot_count > 0u) {
            discover.discovery_slot_count = slot_count;
        }
    }
#endif
    reply_window_ms = discovery_window_ms_for_slots(discover.discovery_slot_count) +
                      UWB_DISCOVERY_RX_GUARD_MS;
    ret = uwb_encode_discover(&discover, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    if (!app_wake_train_deadline_fits(
            k_uptime_get(),
            absolute_deadline_ms,
            UWB_CONTROL_TX_TIMEOUT_MS + 1u)) {
        return -ETIMEDOUT;
    }

    ret = radio_guard_uwb_start("clicker UWB DISCOVER");
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        goto out;
    }

    if (!app_wake_train_deadline_fits(
            k_uptime_get(),
            absolute_deadline_ms,
            UWB_CONTROL_TX_TIMEOUT_MS + 1u)) {
        ret = -ETIMEDOUT;
        goto out;
    }
    ret = dwm3000_driver_send_frame(frame, frame_len, UWB_CONTROL_TX_TIMEOUT_MS);
    if (ret < 0) {
        goto out;
    }

    deadline_ms = k_uptime_get() + reply_window_ms;
    if (absolute_deadline_ms != INT64_MAX &&
        deadline_ms > absolute_deadline_ms) {
        deadline_ms = absolute_deadline_ms;
    }
    if (k_uptime_get() >= deadline_ms) {
        ret = -ETIMEDOUT;
        goto out;
    }
    while (k_uptime_get() < deadline_ms) {
        struct uwb_discovery_reply_frame reply;
        enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
        uint8_t quality = 0u;
        int64_t remaining_ms = deadline_ms - k_uptime_get();

        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      frame,
                                                      sizeof(frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      &rx_failure);
        if (ret == -ETIMEDOUT) {
            break;
        }
        if (ret < 0) {
            last_ret = ret;
            if (rx_failure == DWM3000_RX_FAILURE_NONE) {
                LOG_ERR("clicker discovery aborted on hard radio receive failure: ret=%d",
                        ret);
                goto out;
            }
            continue;
        }
        rx_frames++;

        ret = uwb_decode_discovery_reply(frame, frame_len, &reply);
        if (ret != PROTO_OK) {
            malformed_frames++;
            LOG_DBG("clicker ignored malformed UWB discovery frame: ret=%d frame_len=%u quality=%u",
                    ret,
                    (unsigned int)frame_len,
                    quality);
            last_ret = -EBADMSG;
            continue;
        }
        decoded_replies++;
        ret = uwb_clicker_note_discovery_reply(session, &reply);
        if (ret == PROTO_OK) {
#if defined(CONFIG_IMEC_ML_CLICKER)
            if (clicker_callbacks.ml_cache_note_discovery_reply != NULL) {
                clicker_callbacks.ml_cache_note_discovery_reply(&reply);
            }
#endif
            LOG_INF("clicker UWB discovery reply: anchor=0x%016llx slot=%u quality=%u status=%u candidates=%u",
                    (unsigned long long)reply.anchor_id,
                    reply.anchor_slot,
                    reply.rx_quality,
                    reply.status,
                    session->candidate_count);
        } else {
            rejected_replies++;
            LOG_DBG("clicker rejected UWB discovery reply: ret=%d anchor=0x%016llx selected_clicker=0x%016llx event_seq=%u attempt=%u status=%u quality=%u",
                    ret,
                    (unsigned long long)reply.anchor_id,
                    (unsigned long long)reply.selected_clicker_id,
                    reply.click_event_id,
                    reply.attempt_index,
                    reply.status,
                    reply.rx_quality);
        }
    }

    ret = session->candidate_count > 0u ? (int)session->candidate_count : last_ret;
    LOG_INF("clicker UWB discovery complete: rx_frames=%u decoded_replies=%u candidates=%u malformed_frames=%u rejected_replies=%u window_ms=%u ret=%d",
            rx_frames,
            decoded_replies,
            session->candidate_count,
            malformed_frames,
            rejected_replies,
            reply_window_ms,
            ret);

out:
    release_ret = app_radio_standby_with_bounded_recovery(
        "discovery complete");
    radio_guard_uwb_stop();
    if (ret >= 0 && release_ret < 0) {
        ret = release_ret;
    }
    if (ret < 0) {
    }
    return ret;
}

int app_clicker_discover_uwb_anchors(struct uwb_clicker_session *session)
{
    return app_clicker_discover_uwb_anchors_until(session, INT64_MAX);
}

int app_clicker_discover_uwb_anchors_until(
    struct uwb_clicker_session *session,
    int64_t deadline_ms)
{
    return clicker_discover_uwb_anchors_until(session, deadline_ms);
}

static int clicker_collect_uwb_attempt_with_options_until(
    struct uwb_clicker_session *session,
    uint64_t priority_id,
    struct uwb_range_schedule_frame *schedule,
    bool allow_cached_discovery,
    bool post_burst_diagnostics,
    int64_t deadline_ms,
    int64_t *schedule_tx_ms)
{
    struct app_clicker_range_tx_config range_tx_config = clicker_range_tx_config;
    int ret;
    bool used_cached_discovery = false;

    if (schedule_tx_ms != NULL) {
        *schedule_tx_ms = -1;
    }
    ret = clicker_send_wake_claim_train_until(session,
                                              priority_id,
                                              &clicker_wake_train_config,
                                              deadline_ms);
    if (ret < 0) {
        return ret;
    }

#if defined(CONFIG_IMEC_ML_CLICKER)
    if (allow_cached_discovery &&
        clicker_callbacks.ml_seed_cached_anchors != NULL) {
        ret = clicker_callbacks.ml_seed_cached_anchors(session,
                                                       session->config.max_anchor_count);
        if (ret >= 0) {
            used_cached_discovery = true;
            if (clicker_callbacks.ml_note_cached_discovery_used != NULL) {
                clicker_callbacks.ml_note_cached_discovery_used();
            }
            LOG_INF("ML fast ranging using cached anchors: count=%d fresh_ms=%u",
                    ret,
                    ML_CLICKER_FAST_CACHE_FRESH_MS);
        } else if (ret != -ENOENT) {
            LOG_WRN("ML cached anchor seed failed: ret=%d; falling back to discovery",
                    ret);
        }
    }
#else
    ARG_UNUSED(allow_cached_discovery);
    ARG_UNUSED(post_burst_diagnostics);
#endif

    if (!used_cached_discovery) {
        ret = clicker_discover_uwb_anchors_until(session, deadline_ms);
        if (ret < 0) {
            return ret;
        }
    }

    if ((session->config.flags & FLAG_COUNT_AS_CLICK) != 0u &&
        session->candidate_count > 0u &&
        session->candidate_count < session->config.min_anchor_count) {
        int release_ret;

        if (!app_wake_train_deadline_fits(
                k_uptime_get(),
                deadline_ms,
                clicker_range_tx_config.control_tx_timeout_ms)) {
            release_ret = -ETIMEDOUT;
        } else {
            release_ret = app_clicker_send_range_release(
                session,
                UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS,
                &clicker_range_tx_config);
        }
        if (release_ret < 0) {
            LOG_WRN("clicker could not release anchors after insufficient discovery: ret=%d candidates=%u min=%u",
                    release_ret,
                    session->candidate_count,
                    session->config.min_anchor_count);
        }
        (void)uwb_clicker_abort_attempt(session);
        return -ETIMEDOUT;
    }

    ret = uwb_clicker_build_range_schedule(session,
                                           UWB_RANGE_REPLY_DELAY_UUS,
                                           UWB_RANGE_FIRST_POLL_DELAY_MS,
                                           UWB_ANCHOR_RANGE_WINDOW_MS,
                                           schedule);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_NOT_FOUND ? -ETIMEDOUT : -EINVAL;
    }
#if defined(CONFIG_IMEC_ML_CLICKER)
    if (clicker_callbacks.ml_relax_range_schedule != NULL) {
        ret = clicker_callbacks.ml_relax_range_schedule(schedule, post_burst_diagnostics);
        if (ret < 0) {
            (void)uwb_clicker_abort_attempt(session);
            return ret;
        }
    }
    session->schedule = *schedule;
    if (clicker_callbacks.ml_runtime_active != NULL) {
        range_tx_config.prepare_range_mode_after_schedule =
            clicker_callbacks.ml_runtime_active();
    }
#endif

    if (!app_wake_train_deadline_fits(
            k_uptime_get(),
            deadline_ms,
            clicker_range_schedule_deadline_budget_ms(schedule,
                                                      &range_tx_config))) {
        (void)uwb_clicker_abort_attempt(session);
        return -ETIMEDOUT;
    }
    ret = clicker_send_range_schedule_until(schedule,
                                            &range_tx_config,
                                            deadline_ms,
                                            schedule_tx_ms);
    if (ret < 0) {
        (void)uwb_clicker_abort_attempt(session);
        LOG_WRN("clicker aborting UWB attempt before DS-TWR: reason=range_schedule_tx ret=%d attempt=%u retries=%u ds_fail=%u",
                ret,
                session->attempt_index,
                session->diagnostics.retries,
                session->diagnostics.ds_twr_failures);
    }
    return ret;
}

int app_clicker_collect_uwb_attempt_with_options(
    struct uwb_clicker_session *session,
    uint64_t priority_id,
    struct uwb_range_schedule_frame *schedule,
    bool allow_cached_discovery,
    bool post_burst_diagnostics)
{
    return app_clicker_collect_uwb_attempt_with_options_until(
        session,
        priority_id,
        schedule,
        allow_cached_discovery,
        post_burst_diagnostics,
        INT64_MAX,
        NULL);
}

int app_clicker_collect_uwb_attempt_with_options_until(
    struct uwb_clicker_session *session,
    uint64_t priority_id,
    struct uwb_range_schedule_frame *schedule,
    bool allow_cached_discovery,
    bool post_burst_diagnostics,
    int64_t deadline_ms,
    int64_t *schedule_tx_ms)
{
    return clicker_collect_uwb_attempt_with_options_until(
        session,
        priority_id,
        schedule,
        allow_cached_discovery,
        post_burst_diagnostics,
        deadline_ms,
        schedule_tx_ms);
}

static int clicker_idle_scheduled_range_radio(void)
{
    int ret = app_radio_idle_with_bounded_recovery(
        "scheduled range sample");

    if (ret < 0) {
        LOG_WRN("clicker DW3000 idle after scheduled sample failed: %d", ret);
    }
    return ret;
}

static int clicker_finish_scheduled_range_radio_burst(void)
{
    int ret = app_radio_standby_with_bounded_recovery(
        "scheduled range burst");

    if (ret < 0) {
        LOG_WRN("clicker DW3000 standby after scheduled burst failed: %d", ret);
    }
    radio_guard_uwb_stop();
    return ret;
}

int app_clicker_range_scheduled_anchors(struct uwb_clicker_session *session,
                                        const struct uwb_range_schedule_frame *schedule,
                                        int64_t schedule_tx_ms,
                                        int64_t click_deadline_ms,
                                        uint8_t *attempted_count)
{
    size_t total_samples;
    int last_ret = -ETIMEDOUT;
    int finish_ret;
    int ret;

    if (session == NULL || schedule == NULL || schedule_tx_ms < 0 ||
        schedule_tx_ms > k_uptime_get()) {
        return -EINVAL;
    }
    total_samples = uwb_range_schedule_total_samples(schedule);
    ret = radio_guard_uwb_start("clicker scheduled UWB range burst");
    if (ret < 0) {
        (void)uwb_clicker_abort_attempt(session);
        LOG_WRN("scheduled click DS-TWR burst not started: reason=radio_guard ret=%d attempt=%u",
                ret,
                session->attempt_index);
        return ret;
    }
    while (session->state == UWB_CLICKER_RANGING) {
        struct uwb_range_step step;
        struct dwm3000_range_request range_request;
        struct dwm3000_range_result range_result;
        struct proto_packet click_activity_packet = {0};
        int64_t target_us;
        int64_t remaining_ms;
        uint32_t slot_timeout_ms;
        uint32_t slot_deadline_budget_ms;
        int idle_ret;

        ret = uwb_clicker_next_range_step(session, &step);
        if (ret == PROTO_ERR_NOT_FOUND) {
            break;
        }
        if (ret != PROTO_OK) {
            last_ret = -EINVAL;
            break;
        }

        slot_timeout_ms = clicker_range_slot_timeout_ms(schedule);
        slot_deadline_budget_ms = u32_saturating_add(
            slot_timeout_ms, CLICK_REPORT_BUILD_GUARD_MS);
        target_us = scheduled_range_sample_target_us(schedule_tx_ms,
                                                      schedule,
                                                      step.sample_index);
        if (click_deadline_ms != INT64_MAX) {
            int64_t latest_start_ms =
                click_deadline_ms - slot_deadline_budget_ms;

            if (latest_start_ms < 0 ||
                target_us > latest_start_ms * 1000) {
                (void)uwb_clicker_record_range_result(session,
                                                      &step,
                                                      RANGE_RX_TIMEOUT);
                (void)uwb_clicker_abort_attempt(session);
                LOG_WRN("scheduled click DS-TWR not started: reason=scheduled_target_after_budget anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u target_us=%lld latest_start_ms=%lld exchange_budget_ms=%u attempt=%u ds_fail=%u",
                        (unsigned long long)step.anchor_id,
                        step.anchor_index,
                        (unsigned int)(step.sample_index + 1u),
                        (unsigned int)total_samples,
                        step.round_index,
                        step.seq,
                        (long long)target_us,
                        (long long)latest_start_ms,
                        slot_deadline_budget_ms,
                        session->attempt_index,
                        session->diagnostics.ds_twr_failures);
                last_ret = -ETIMEDOUT;
                break;
            }
        }
        sleep_until_us(target_us);

        remaining_ms = click_deadline_ms - k_uptime_get();
        if (remaining_ms < slot_deadline_budget_ms) {
            (void)uwb_clicker_record_range_result(session, &step, RANGE_RX_TIMEOUT);
            (void)uwb_clicker_abort_attempt(session);
            LOG_WRN("scheduled click DS-TWR not started: reason=click_budget anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u remaining_ms=%lld exchange_budget_ms=%u attempt=%u ds_fail=%u",
                    (unsigned long long)step.anchor_id,
                    step.anchor_index,
                    (unsigned int)(step.sample_index + 1u),
                    (unsigned int)total_samples,
                    step.round_index,
                    step.seq,
                    (long long)remaining_ms,
                    slot_deadline_budget_ms,
                    session->attempt_index,
                    session->diagnostics.ds_twr_failures);
            last_ret = -ETIMEDOUT;
            break;
        }

        memset(&range_request, 0, sizeof(range_request));
        memset(&range_result, 0, sizeof(range_result));
        range_result.status = RANGE_RX_TIMEOUT;
        range_request.initiator_id = DEVICE_ID;
        range_request.responder_id = step.anchor_id;
        range_request.network_id = session->config.network_id;
        range_request.session_nonce = session->config.nonce;
        range_request.responder_short_addr = uwb_session_short_addr_from_id(step.anchor_id);
        range_request.session_id = session->config.click_event_id;
        range_request.seq = step.seq;
        range_request.round_index = step.round_index;
        range_request.flags = session->config.flags;
        range_request.click_timestamp_ms = session->config.click_timestamp_ms;
        range_request.click_timestamp_present =
            (session->config.flags & FLAG_COUNT_AS_CLICK) != 0u;
        range_request.skip_responder_report = false;
        range_request.send_clicker_diag = false;
        range_request.expect_anchor_diag = false;
        range_request.capture_rsl = false;
        range_request.timeout_ms = slot_timeout_ms;
        click_activity_packet.src_id = range_request.initiator_id;
        click_activity_packet.dst_id = range_request.responder_id;
        click_activity_packet.session_id = range_request.session_id;
        click_activity_packet.seq = range_request.seq;
        click_activity_packet.msg_type = MSG_UWB_POLL;

        LOG_INF("scheduled click DS-TWR start: anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u timeout_ms=%u",
                (unsigned long long)step.anchor_id,
                step.anchor_index,
                (unsigned int)(step.sample_index + 1u),
                (unsigned int)total_samples,
                step.round_index,
                step.seq,
                range_request.timeout_ms);
#if defined(CONFIG_IMEC_ML_CLICKER)
        if (clicker_callbacks.ml_enter_range_quiet != NULL) {
            clicker_callbacks.ml_enter_range_quiet();
        }
#endif
        remaining_ms = click_deadline_ms - k_uptime_get();
        if (remaining_ms < slot_deadline_budget_ms) {
#if defined(CONFIG_IMEC_ML_CLICKER)
            if (clicker_callbacks.ml_exit_range_quiet != NULL) {
                clicker_callbacks.ml_exit_range_quiet();
            }
#endif
            (void)uwb_clicker_record_range_result(session,
                                                  &step,
                                                  RANGE_RX_TIMEOUT);
            (void)uwb_clicker_abort_attempt(session);
            LOG_WRN("scheduled click DS-TWR not started: reason=radio_setup_consumed_budget anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u remaining_ms=%lld exchange_budget_ms=%u attempt=%u ds_fail=%u",
                    (unsigned long long)step.anchor_id,
                    step.anchor_index,
                    (unsigned int)(step.sample_index + 1u),
                    (unsigned int)total_samples,
                    step.round_index,
                    step.seq,
                    (long long)remaining_ms,
                    slot_deadline_budget_ms,
                    session->attempt_index,
                    session->diagnostics.ds_twr_failures);
            last_ret = -ETIMEDOUT;
            break;
        }
        app_stack_workload_diag_click_activity_admit(&click_activity_packet,
                                                      1u, 0u);
        ret = dwm3000_driver_range_initiator(&range_request, &range_result);
        idle_ret = clicker_idle_scheduled_range_radio();
#if defined(CONFIG_IMEC_ML_CLICKER)
        if (clicker_callbacks.ml_exit_range_quiet != NULL) {
            clicker_callbacks.ml_exit_range_quiet();
        }
#endif
        app_stack_workload_diag_click_activity_sample(
            &click_activity_packet, 1u,
            range_result.exchange_started ? 1u : 0u);
        app_stack_workload_diag_click_activity_release(
            &click_activity_packet,
            ret == 0 && range_result.exchange_started &&
                range_result.status == RANGE_OK ? 0 : -EIO,
            0u, 0u);

        if (idle_ret < 0) {
            (void)uwb_clicker_abort_attempt(session);
            last_ret = idle_ret;
            break;
        }
        if (ret == -ECANCELED) {
            (void)uwb_clicker_abort_attempt(session);
            last_ret = ret;
            break;
        }
        if (!range_result.exchange_started) {
            enum range_status status = range_result.status;

            if (status == RANGE_OK || !range_status_valid(status)) {
                status = RANGE_RX_TIMEOUT;
            }
            (void)uwb_clicker_record_range_result(session, &step, status);
            LOG_WRN("scheduled click DS-TWR did not start: anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u ret=%d status=%s(%u)",
                    (unsigned long long)step.anchor_id,
                    step.anchor_index,
                    (unsigned int)(step.sample_index + 1u),
                    (unsigned int)total_samples,
                    step.round_index,
                    step.seq,
                    ret,
                    range_status_name(range_result.status),
                    range_result.status);
#if defined(CONFIG_IMEC_ML_CLICKER)
            if (clicker_callbacks.ml_emit_range_sample_if_active != NULL) {
                (void)clicker_callbacks.ml_emit_range_sample_if_active(session,
                                                                       schedule,
                                                                       &step,
                                                                       &range_result);
            }
            if (clicker_callbacks.ml_continue_after_range_start_failure != NULL &&
                clicker_callbacks.ml_continue_after_range_start_failure()) {
                last_ret = ret < 0 ? ret : -EIO;
                continue;
            }
#endif
            (void)uwb_clicker_abort_attempt(session);
            last_ret = ret < 0 ? ret : -EIO;
            break;
        }
        if (attempted_count != NULL) {
            (*attempted_count)++;
        }

        if (ret == 0 && range_result.status == RANGE_OK) {
            int record_ret;

            record_ret = uwb_clicker_record_range_result(session, &step, RANGE_OK);
            if (record_ret != PROTO_OK) {
                LOG_ERR("scheduled click DS-TWR state update failed: anchor=0x%016llx seq=%u ret=%d",
                        (unsigned long long)step.anchor_id,
                        step.seq,
                        record_ret);
                last_ret = -EINVAL;
                break;
            }
            LOG_INF("scheduled click DS-TWR complete: anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u distance_mm=%d quality=%u",
                    (unsigned long long)range_result.responder_id,
                    step.anchor_index,
                    (unsigned int)(step.sample_index + 1u),
                    (unsigned int)total_samples,
                    step.round_index,
                    range_result.seq,
                    range_result.distance_mm,
                    range_result.quality);
#if defined(CONFIG_IMEC_ML_CLICKER)
            if (clicker_callbacks.ml_emit_range_sample_if_active != NULL) {
                (void)clicker_callbacks.ml_emit_range_sample_if_active(session,
                                                                       schedule,
                                                                       &step,
                                                                       &range_result);
            }
#endif
            last_ret = 0;
        } else {
            enum range_status status = range_result.status;
            int record_ret;

            if (status == RANGE_OK || !range_status_valid(status)) {
                status = RANGE_INTERNAL_ERROR;
            }
            if (status == RANGE_TIMING_INVALID) {
            }
            record_ret = uwb_clicker_record_range_result(session, &step, status);
            if (record_ret != PROTO_OK) {
                LOG_ERR("scheduled click DS-TWR failure state update failed: anchor=0x%016llx seq=%u ret=%d status=%s(%u)",
                        (unsigned long long)step.anchor_id,
                        step.seq,
                        record_ret,
                        range_status_name(status),
                        status);
                last_ret = -EINVAL;
                break;
            }
            LOG_WRN("scheduled click DS-TWR failed: anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u ret=%d status=%s(%u)",
                    (unsigned long long)step.anchor_id,
                    step.anchor_index,
                    (unsigned int)(step.sample_index + 1u),
                    (unsigned int)total_samples,
                    step.round_index,
                    step.seq,
                    ret,
                    range_status_name(status),
                    status);
#if defined(CONFIG_IMEC_ML_CLICKER)
            if (clicker_callbacks.ml_emit_range_sample_if_active != NULL) {
                (void)clicker_callbacks.ml_emit_range_sample_if_active(session,
                                                                       schedule,
                                                                       &step,
                                                                       &range_result);
            }
#endif
            last_ret = ret < 0 ? ret : -EIO;
        }

#if defined(CONFIG_IMEC_ML_CLICKER)
        if (clicker_callbacks.ml_should_continue_ranging != NULL &&
            !clicker_callbacks.ml_should_continue_ranging()) {
            (void)uwb_clicker_abort_attempt(session);
            last_ret = -ECANCELED;
            break;
        }
#endif

        if (session->state == UWB_CLICKER_SUCCEEDED) {
            last_ret = 0;
            break;
        }
    }

    if (last_ret < 0 && session->state == UWB_CLICKER_RANGING) {
        (void)uwb_clicker_abort_attempt(session);
    }
    finish_ret = clicker_finish_scheduled_range_radio_burst();
    if (finish_ret < 0) {
        return finish_ret;
    }
#if defined(CONFIG_IMEC_ML_CLICKER)
    if (session->state == UWB_CLICKER_SUCCEEDED &&
        schedule->diagnostics_required != UWB_RANGE_SCHEDULE_DIAGNOSTICS_OMITTED &&
        clicker_callbacks.ml_run_post_burst_diagnostics != NULL) {
        clicker_callbacks.ml_run_post_burst_diagnostics(session,
                                                        schedule,
                                                        schedule_tx_ms,
                                                        click_deadline_ms);
    }
#endif
    if (last_ret < 0) {
        return last_ret;
    }
    return session->state == UWB_CLICKER_SUCCEEDED ? 0 : last_ret;
}

static int clicker_runtime_expect_effect(enum fw_effect_type expected)
{
    struct fw_effect effect;

    if (!app_clicker_event_runtime_take_effect(&clicker_event_runtime,
                                               &effect) ||
        effect.type != expected) {
        LOG_ERR("clicker event effect mismatch: expected=%u",
                (unsigned int)expected);
        return -EPROTO;
    }
    return 0;
}

static int clicker_runtime_click_event(enum fw_event_type type)
{
    return app_clicker_event_runtime_click_event(&clicker_event_runtime,
                                                 type,
                                                 NULL);
}

static int clicker_runtime_click_event_payload(
    enum fw_event_type type,
    const struct fw_event_payload *payload)
{
    return app_clicker_event_runtime_click_event(&clicker_event_runtime,
                                                 type,
                                                 payload);
}

static int clicker_runtime_begin_click(uint32_t event_seq)
{
    int ret;

    ret = app_clicker_event_runtime_click_start(&clicker_event_runtime,
                                                event_seq);
    if (ret < 0) {
        return ret;
    }
    ret = clicker_runtime_expect_effect(FW_EFFECT_CLICK_CREATE);
    if (ret < 0) {
        return ret;
    }
    ret = clicker_runtime_click_event(FW_EVENT_CLICK_CREATED);
    if (ret < 0) {
        return ret;
    }
    return clicker_runtime_expect_effect(FW_EFFECT_CLICK_CHECK_POLITENESS);
}

static int clicker_runtime_abort_click(void)
{
    int ret = clicker_runtime_click_event(FW_EVENT_RADIO_JOB_FAILED);

    if (ret < 0) {
        return ret;
    }
    return clicker_runtime_expect_effect(FW_EFFECT_CLICK_CLEANUP);
}

static int clicker_runtime_prepare_retry(struct uwb_clicker_session *session,
                                         int64_t click_deadline_ms)
{
    uint32_t required_retry_tail_ms;
    int ret;

    if (session == NULL) {
        return -EINVAL;
    }
    if (session->attempt_index >= session->config.max_attempts) {
        ret = clicker_runtime_click_event(FW_EVENT_RETRY_EXHAUSTED);
        if (ret < 0) {
            return ret;
        }
        ret = clicker_runtime_expect_effect(FW_EFFECT_CLICK_CLEANUP);
        return ret < 0 ? ret : -ETIMEDOUT;
    }

    ret = uwb_clicker_prepare_retry(session);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    required_retry_tail_ms = app_clicker_wake_train_opportunity_tail_ms(
        &clicker_wake_train_config);
    (void)app_clicker_apply_retry_delay(session,
                                        click_deadline_ms,
                                        UWB_RETRY_BASE_DELAY_MS,
                                        required_retry_tail_ms);
    (void)app_clicker_apply_contention_delay(session,
                                             click_deadline_ms,
                                             required_retry_tail_ms);

    ret = clicker_runtime_click_event(FW_EVENT_RETRY_ALLOWED);
    if (ret < 0) {
        return ret;
    }
    ret = clicker_runtime_expect_effect(FW_EFFECT_CLICK_CHECK_POLITENESS);
    if (ret < 0) {
        return ret;
    }
    return 0;
}

static int clicker_runtime_retry_after_failure(
    struct uwb_clicker_session *session,
    int64_t click_deadline_ms,
    int failure)
{
    struct fw_event_payload payload = {
        .flags = FW_EVENT_FLAG_RETRYABLE,
        .value = failure < 0 ? (uint32_t)(-(int64_t)failure) :
                              (uint32_t)failure,
    };
    int ret;

    ret = clicker_runtime_click_event_payload(FW_EVENT_RADIO_JOB_FAILED,
                                              &payload);
    if (ret < 0) {
        return ret;
    }
    ret = clicker_runtime_expect_effect(FW_EFFECT_START_TIMER);
    if (ret < 0) {
        return ret;
    }
    ret = clicker_runtime_prepare_retry(session, click_deadline_ms);
    return ret < 0 ? ret : 0;
}

static int clicker_build_and_send_schedule_for_event(
    struct uwb_clicker_session *session,
    struct uwb_range_schedule_frame *schedule,
    int64_t click_deadline_ms,
    int64_t *schedule_tx_ms)
{
    struct app_clicker_range_tx_config range_tx_config = clicker_range_tx_config;
    int ret;

    ret = uwb_clicker_build_range_schedule(session,
                                           UWB_RANGE_REPLY_DELAY_UUS,
                                           UWB_RANGE_FIRST_POLL_DELAY_MS,
                                           UWB_ANCHOR_RANGE_WINDOW_MS,
                                           schedule);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_NOT_FOUND ? -ETIMEDOUT : -EINVAL;
    }
#if defined(CONFIG_IMEC_ML_CLICKER)
    if (clicker_callbacks.ml_relax_range_schedule != NULL) {
        ret = clicker_callbacks.ml_relax_range_schedule(schedule, true);
        if (ret < 0) {
            (void)uwb_clicker_abort_attempt(session);
            return ret;
        }
    }
    session->schedule = *schedule;
    if (clicker_callbacks.ml_runtime_active != NULL) {
        range_tx_config.prepare_range_mode_after_schedule =
            clicker_callbacks.ml_runtime_active();
    }
#endif
    if (!app_wake_train_deadline_fits(
            k_uptime_get(),
            click_deadline_ms,
            clicker_range_schedule_deadline_budget_ms(schedule,
                                                      &range_tx_config))) {
        (void)uwb_clicker_abort_attempt(session);
        return -ETIMEDOUT;
    }
    ret = clicker_send_range_schedule_until(schedule,
                                            &range_tx_config,
                                            click_deadline_ms,
                                            schedule_tx_ms);
    if (ret < 0) {
        (void)uwb_clicker_abort_attempt(session);
    }
    return ret;
}

int app_clicker_run_normal_click(void)
{
    uint32_t event_seq;
    uint8_t attempted_count = 0u;
    uint16_t total_candidate_count = 0u;
    int64_t click_deadline_ms;
    struct uwb_clicker_session session;
    struct uwb_clicker_config config;
    int last_ret = -ETIMEDOUT;
    int ret;

    ret = app_click_event_sequence_next(&event_seq);
    if (ret < 0) {
        LOG_ERR("normal click identity allocation failed closed: %d", ret);
        return ret;
    }
    config = (struct uwb_clicker_config) {
        .network_id = NETWORK_ID,
        .clicker_id = DEVICE_ID,
        .click_event_id = event_seq,
        .click_timestamp_ms =
            app_clicker_event_runtime_button_pressed_at_ms(
                &clicker_event_runtime),
        .nonce = clicker_nonce(event_seq),
        .min_anchor_count = app_clicker_debug_min_anchor_count(),
        .max_anchor_count = app_clicker_debug_max_anchor_count(),
        .max_attempts = MAX_WAKE_ATTEMPTS,
        .samples_per_anchor = app_clicker_debug_samples_per_anchor(),
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
        .flags = app_clicker_debug_session_flags(),
    };

    BUILD_ASSERT(UWB_NORMAL_CLICK_MIN_ANCHORS <= MAX_SUCCESSFUL_ANCHORS,
                 "successful anchor result storage must cover the success threshold");
    BUILD_ASSERT(CLICK_REPORT_DEADLINE_MS <= UWB_CLICK_AGE_MAX_MS,
                 "normal click age must fit the first DS-TWR frame");

    ret = uwb_clicker_session_start(&session, &config);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = clicker_runtime_begin_click(event_seq);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("normal click started on event/state path: event_seq=%u wake_ms=%u max_attempts=%u min_unique_anchors=%u samples_per_anchor=%u",
            event_seq,
            WAKE_ADV_MS,
            MAX_WAKE_ATTEMPTS,
            config.min_anchor_count,
            config.samples_per_anchor);
    click_deadline_ms = k_uptime_get() + CLICK_REPORT_DEADLINE_MS;

    while (session.attempt_index <= session.config.max_attempts) {
        struct uwb_range_schedule_frame schedule = {0};
        int64_t schedule_tx_ms = -1;
        struct fw_event_payload payload = {0};
        uint32_t wake_claim_tx_count;
        int wake_ret;
        int range_ret;
        uint64_t priority_id = clicker_priority_id(event_seq,
                                                   session.attempt_index);

        ret = app_clicker_attempt_gate(&session,
                                       event_seq,
                                       priority_id,
                                       click_deadline_ms,
                                       true,
                                       &clicker_attempt_gate_config);
        if (ret < 0) {
            LOG_WRN("normal click attempt gate failed: event_seq=%u attempt=%u ret=%d",
                    event_seq,
                    session.attempt_index,
                    ret);
            (void)clicker_runtime_abort_click();
            return ret;
        }
        ret = clicker_runtime_click_event(FW_EVENT_CHANNEL_CLEAR);
        if (ret < 0) {
            return ret;
        }
        ret = clicker_runtime_expect_effect(FW_EFFECT_CLICK_SEND_WAKE);
        if (ret < 0) {
            return ret;
        }

        wake_claim_tx_count = session.diagnostics.wake_claim_tx_count;
        wake_ret = app_clicker_send_wake_claim_train_until(
            &session,
            priority_id,
            &clicker_wake_train_config,
            click_deadline_ms);
        if (session.diagnostics.wake_claim_tx_count != wake_claim_tx_count) {
            ret = clicker_runtime_click_event(FW_EVENT_RF_STARTED);
            if (ret < 0) {
                return ret;
            }
        }
        if (wake_ret < 0) {
            last_ret = wake_ret;
            ret = clicker_runtime_retry_after_failure(&session,
                                                     click_deadline_ms,
                                                     wake_ret);
            if (ret == 0) {
                continue;
            }
            return ret == -ETIMEDOUT ? last_ret : ret;
        }
        ret = clicker_runtime_click_event(FW_EVENT_WAKE_COMPLETED);
        if (ret < 0) {
            return ret;
        }
        ret = clicker_runtime_expect_effect(FW_EFFECT_CLICK_DISCOVER);
        if (ret < 0) {
            return ret;
        }

        ret = app_clicker_discover_uwb_anchors_until(&session,
                                                     click_deadline_ms);
        payload.count = session.candidate_count;
        payload.value = session.config.min_anchor_count;
        /* Discovery timeout and malformed/no-reply outcomes remain bounded
         * click retries, matching the previous custody behavior. */
        if (ret < 0 && session.candidate_count > 0u) {
            last_ret = ret;
            ret = clicker_runtime_retry_after_failure(&session,
                                                     click_deadline_ms,
                                                     ret);
            if (ret == 0) {
                continue;
            }
            return ret == -ETIMEDOUT ? last_ret : ret;
        }
        if (ret < 0 && session.candidate_count == 0u) {
            last_ret = ret;
            payload.count = 0u;
        }
        ret = app_clicker_event_runtime_click_event(&clicker_event_runtime,
                                                    FW_EVENT_DISCOVERY_COMPLETED,
                                                    &payload);
        if (ret < 0) {
            return ret;
        }
        if (session.candidate_count == 0u) {
            ret = clicker_runtime_expect_effect(FW_EFFECT_START_TIMER);
            if (ret < 0) {
                return ret;
            }
            ret = clicker_runtime_prepare_retry(&session,
                                                click_deadline_ms);
            if (ret == 0) {
                continue;
            }
            return ret == -ETIMEDOUT ? last_ret : ret;
        }
        if (session.candidate_count < session.config.min_anchor_count) {
            ret = clicker_runtime_expect_effect(FW_EFFECT_CLICK_SEND_RELEASE);
            if (ret < 0) {
                return ret;
            }
            ret = app_clicker_send_range_release(
                &session,
                UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS,
                &clicker_range_tx_config);
            if (ret < 0) {
                last_ret = ret;
                ret = clicker_runtime_retry_after_failure(&session,
                                                         click_deadline_ms,
                                                         ret);
                if (ret == 0) {
                    continue;
                }
                return ret == -ETIMEDOUT ? last_ret : ret;
            }
            (void)uwb_clicker_abort_attempt(&session);
            last_ret = -ETIMEDOUT;
            ret = clicker_runtime_click_event(FW_EVENT_RELEASE_COMPLETED);
            if (ret < 0) {
                return ret;
            }
            ret = clicker_runtime_expect_effect(FW_EFFECT_START_TIMER);
            if (ret < 0) {
                return ret;
            }
            ret = clicker_runtime_prepare_retry(&session,
                                                click_deadline_ms);
            if (ret == 0) {
                continue;
            }
            return ret == -ETIMEDOUT ? last_ret : ret;
        }

        ret = clicker_runtime_expect_effect(FW_EFFECT_CLICK_SEND_SCHEDULE);
        if (ret < 0) {
            return ret;
        }
        ret = clicker_build_and_send_schedule_for_event(&session,
                                                        &schedule,
                                                        click_deadline_ms,
                                                        &schedule_tx_ms);
        if (ret < 0) {
            last_ret = ret;
            ret = clicker_runtime_retry_after_failure(&session,
                                                     click_deadline_ms,
                                                     ret);
            if (ret == 0) {
                continue;
            }
            return ret == -ETIMEDOUT ? last_ret : ret;
        }
        total_candidate_count += schedule.selected_count;
        LOG_INF("normal click UWB attempt scheduled anchors: event_seq=%u attempt=%u selected=%u unique_success=%u/%u",
                event_seq,
                session.attempt_index,
                schedule.selected_count,
                session.successful_unique_count,
                session.config.min_anchor_count);
        ret = clicker_runtime_click_event(FW_EVENT_SCHEDULE_COMPLETED);
        if (ret < 0) {
            return ret;
        }
        ret = clicker_runtime_expect_effect(FW_EFFECT_CLICK_RANGE);
        if (ret < 0) {
            return ret;
        }
        range_ret = app_clicker_range_scheduled_anchors(&session,
                                                        &schedule,
                                                        schedule_tx_ms,
                                                        click_deadline_ms,
                                                        &attempted_count);
        if (range_ret == -ECANCELED) {
            (void)clicker_runtime_abort_click();
            return range_ret;
        }
        if (range_ret < 0) {
            last_ret = range_ret;
            if (session.state == UWB_CLICKER_SUCCEEDED) {
                ret = clicker_runtime_abort_click();
                if (ret < 0) {
                    return ret;
                }
                return range_ret;
            }
        }
        payload.count = range_ret == 0 &&
                            session.state == UWB_CLICKER_SUCCEEDED ?
                            session.successful_unique_count : 0u;
        payload.value = session.config.min_anchor_count;
        ret = app_clicker_event_runtime_click_event(&clicker_event_runtime,
                                                    FW_EVENT_RANGE_COMPLETED,
                                                    &payload);
        if (ret < 0) {
            return ret;
        }
        if (payload.count >= session.config.min_anchor_count &&
            session.state == UWB_CLICKER_SUCCEEDED) {
            ret = clicker_runtime_expect_effect(FW_EFFECT_CLICK_CLEANUP);
            if (ret < 0) {
                return ret;
            }
            LOG_INF("normal click completed: event_seq=%u candidates_scheduled=%u attempted_ranges=%u successful_unique_ranges=%u retries=%u sample_order=%u ds_ok=%u ds_fail=%u timing_reject=%u polite_samples=%u polite_activity=%u contention_ms=%u retry_ms=%u wake_claim_tx=%u",
                    event_seq,
                    total_candidate_count,
                    attempted_count,
                    session.successful_unique_count,
                    session.diagnostics.retries,
                    session.diagnostics.sample_order_count,
                    session.diagnostics.ds_twr_successes,
                    session.diagnostics.ds_twr_failures,
                    session.diagnostics.timing_rejections,
                    session.diagnostics.politeness_samples,
                    session.diagnostics.politeness_activity_hits,
                    session.diagnostics.contention_delay_ms,
                    session.diagnostics.retry_delay_ms,
                    session.diagnostics.wake_claim_tx_count);
            return 0;
        }
        ret = clicker_runtime_expect_effect(FW_EFFECT_START_TIMER);
        if (ret < 0) {
            return ret;
        }
        ret = clicker_runtime_prepare_retry(&session,
                                            click_deadline_ms);
        if (ret == 0) {
            continue;
        }
        return ret == -ETIMEDOUT ? last_ret : ret;
    }

    (void)clicker_runtime_abort_click();
    LOG_WRN("normal click failed: event_seq=%u candidates_scheduled=%u attempted_ranges=%u successful_unique_ranges=%u required=%u retries=%u sample_order=%u ds_ok=%u ds_fail=%u timing_reject=%u polite_samples=%u polite_activity=%u contention_ms=%u retry_ms=%u wake_claim_tx=%u",
            event_seq,
            total_candidate_count,
            attempted_count,
            session.successful_unique_count,
            session.config.min_anchor_count,
            session.diagnostics.retries,
            session.diagnostics.sample_order_count,
            session.diagnostics.ds_twr_successes,
            session.diagnostics.ds_twr_failures,
            session.diagnostics.timing_rejections,
            session.diagnostics.politeness_samples,
            session.diagnostics.politeness_activity_hits,
            session.diagnostics.contention_delay_ms,
            session.diagnostics.retry_delay_ms,
            session.diagnostics.wake_claim_tx_count);
    return last_ret;
}

int app_clicker_run_uwb_diagnostic_click(uint32_t event_seq)
{
    uint8_t attempted_count = 0u;
    int64_t click_deadline_ms = k_uptime_get() + CLICK_REPORT_DEADLINE_MS;
    int64_t schedule_tx_ms = -1;
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_clicker_config config = {
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
    int ret;

    ret = uwb_clicker_session_start(&session, &config);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = app_clicker_attempt_gate(&session,
                                   event_seq,
                                   clicker_priority_id(event_seq, 1u),
                                   click_deadline_ms,
                                   false,
                                   &clicker_attempt_gate_config);
    if (ret < 0) {
        return ret;
    }

    ret = clicker_collect_uwb_attempt_with_options_until(
        &session,
        clicker_priority_id(event_seq, 1u),
        &schedule,
        false,
        true,
        click_deadline_ms,
        &schedule_tx_ms);
    if (ret < 0) {
        return ret;
    }

    ret = app_clicker_range_scheduled_anchors(&session,
                                              &schedule,
                                              schedule_tx_ms,
                                              click_deadline_ms,
                                              &attempted_count);
    if (ret == 0 && session.state == UWB_CLICKER_SUCCEEDED) {
        LOG_INF("self-test UWB diagnostic click passed: event_seq=%u attempted=%u",
                event_seq,
                attempted_count);
        return 0;
    }
    return ret < 0 ? ret : -EIO;
}

static enum self_test_failure app_clicker_run_self_test(uint32_t event_seq)
{
    uint32_t dev_id;
    enum self_test_failure radio_failure = SELF_TEST_FAILURE_DWM3000;
    int release_ret;
    int ret;

    LOG_INF("self-test started on UWB wake path");

    ret = radio_guard_uwb_start("clicker self-test DWM probe");
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 radio ownership unavailable: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }
    ret = dwm3000_port_init();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 port init failed: %d", ret);
        goto self_test_radio_done;
    }

    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 wake failed: %d", ret);
        goto self_test_radio_done;
    }

    ret = dwm3000_port_hw_reset();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 reset failed: %d", ret);
        goto self_test_radio_done;
    }

    ret = dwm3000_driver_probe(&dev_id);
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 decadriver DEV_ID probe failed: %d", ret);
        goto self_test_radio_done;
    }

    ret = dwm3000_port_set_fast_spi();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 fast SPI config failed: %d", ret);
        goto self_test_radio_done;
    }

    LOG_INF("self-test DWM3000 decadriver DEV_ID=0x%08x; fast SPI config checked at %u Hz",
            dev_id,
            (unsigned int)dwm3000_port_current_spi_hz());
    radio_failure = SELF_TEST_FAILURE_NONE;

self_test_radio_done:
    release_ret = app_radio_standby_with_bounded_recovery(
        "self-test DWM probe");
    radio_guard_uwb_stop();
    if (release_ret < 0) {
        radio_failure = SELF_TEST_FAILURE_DWM3000;
    }
    if (radio_failure != SELF_TEST_FAILURE_NONE) {
        return radio_failure;
    }

    ret = app_clicker_run_uwb_diagnostic_click(event_seq);
    if (ret == 0) {
        return SELF_TEST_FAILURE_NONE;
    }

    LOG_WRN("self-test UWB diagnostic click failed: ret=%d", ret);
    if (ret == -ETIMEDOUT) {
        return SELF_TEST_FAILURE_NO_ANCHOR;
    }
    return SELF_TEST_FAILURE_UWB;
}

#if DEVICE_ROLE == ROLE_CLICKER
static int app_clicker_emit_self_test_report(uint32_t event_seq,
                                             enum self_test_failure failure)
{
    struct mesh_outbound outbound = {0};
    struct node_comm_terminal_event event;
    struct self_test_report_fields fields = {
        .clicker_id = DEVICE_ID,
        .event_seq = event_seq,
        .failure_code = (uint8_t)failure,
        .battery_mv = 0u,
    };
    size_t payload_len = 0u;
    uint64_t deadline_ms;
    uint32_t delivery_handle = 0u;
    uint16_t packet_seq = (uint16_t)event_seq;
    int ret;

    if (packet_seq == 0u) {
        packet_seq = 1u;
    }

    ret = report_append_self_test_tlvs(outbound.payload,
                                       sizeof(outbound.payload),
                                       &payload_len,
                                       &fields);
    if (ret != PROTO_OK) {
        LOG_WRN("self-test report TLV build failed: event_seq=%u failure=%u ret=%d",
                event_seq,
                (unsigned int)failure,
                ret);
        return -EINVAL;
    }

    ret = report_init_self_test_packet(&outbound.packet,
                                       DEVICE_ID,
                                       GATEWAY_ID,
                                       event_seq,
                                       packet_seq,
                                       (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        LOG_WRN("self-test report packet build failed: event_seq=%u failure=%u ret=%d",
                event_seq,
                (unsigned int)failure,
                ret);
        return -EINVAL;
    }

    outbound.payload_len = (uint8_t)payload_len;
    outbound.next_hop_id = GATEWAY_ID;
    outbound.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;

    deadline_ms = (uint64_t)k_uptime_get() +
                  SELF_TEST_REPORT_DELIVERY_TIMEOUT_MS;
    ret = app_node_comm_submit_reliable_uplink(
        &outbound,
        deadline_ms,
        ((uint32_t)MSG_SELF_TEST_REPORT << 16) | packet_seq,
        &delivery_handle);
    if (ret < 0) {
        LOG_WRN("self-test report reliable delivery admission failed: event_seq=%u failure=%u ret=%d",
                event_seq,
                (unsigned int)failure,
                ret);
        return ret;
    }

    while ((uint64_t)k_uptime_get() < deadline_ms) {
        if (app_node_comm_take_delivery_event_for(delivery_handle, &event)) {
            if (event.reason == NODE_COMM_TERMINAL_DELIVERED) {
                LOG_INF("self-test report gateway ACK received: event_seq=%u failure=%u attempts=%u",
                        event_seq,
                        (unsigned int)failure,
                        event.attempts_started);
                return 0;
            }
            LOG_WRN("self-test report delivery failed: event_seq=%u failure=%u terminal=%u attempts=%u",
                    event_seq,
                    (unsigned int)failure,
                    (unsigned int)event.reason,
                    event.attempts_started);
            return -EHOSTUNREACH;
        }
        k_msleep(SELF_TEST_REPORT_DELIVERY_POLL_MS);
    }

    (void)app_node_comm_abandon_delivery(delivery_handle);
    LOG_WRN("self-test report delivery timed out before low power: event_seq=%u failure=%u",
            event_seq,
            (unsigned int)failure);
    return -ETIMEDOUT;
}
#else
static int app_clicker_emit_self_test_report(uint32_t event_seq,
                                             enum self_test_failure failure)
{
    ARG_UNUSED(event_seq);
    ARG_UNUSED(failure);
    return -ENOTSUP;
}
#endif

#if defined(CONFIG_BT) && DEVICE_ROLE == ROLE_CLICKER
static bool ble_courtesy_init_attempted;
static bool ble_courtesy_available;
static bool ble_courtesy_adv_active;
static bool ble_courtesy_scan_active;
static struct k_spinlock ble_courtesy_lock;
static uint32_t ble_courtesy_higher_wait_ms;
static uint8_t ble_courtesy_adv_data[UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN];
static struct uwb_ble_courtesy_frame ble_courtesy_local;

static int clicker_ble_courtesy_set_scan_channel(void)
{
#if defined(CONFIG_BT_LL_SOFTDEVICE_HEADERS_INCLUDE)
    const sdc_hci_cmd_vs_scan_channel_map_set_t params = {
        .channel_map = {0xffu, 0xffu, 0xffu, 0xffu, 0x3fu},
    };

    return hci_vs_sdc_scan_channel_map_set(&params);
#else
    return -ENOTSUP;
#endif
}

static void clicker_ble_courtesy_note_higher_peer(uint32_t wait_ms)
{
    k_spinlock_key_t key = k_spin_lock(&ble_courtesy_lock);

    if (wait_ms > ble_courtesy_higher_wait_ms) {
        ble_courtesy_higher_wait_ms = wait_ms;
    }
    k_spin_unlock(&ble_courtesy_lock, key);
}

static void clicker_ble_courtesy_clear_higher_peer(void)
{
    k_spinlock_key_t key = k_spin_lock(&ble_courtesy_lock);

    ble_courtesy_higher_wait_ms = 0u;
    k_spin_unlock(&ble_courtesy_lock, key);
}

static bool clicker_ble_courtesy_parse_ad(struct bt_data *data, void *user_data)
{
    struct uwb_ble_courtesy_frame peer;
    int cmp;

    ARG_UNUSED(user_data);

    if (data->type != BT_DATA_MANUFACTURER_DATA) {
        return true;
    }
    if (uwb_ble_courtesy_decode(data->data, data->data_len, &peer) != PROTO_OK) {
        return true;
    }
    if (peer.network_id != NETWORK_ID || peer.clicker_id == ble_courtesy_local.clicker_id) {
        return false;
    }

    cmp = uwb_claim_precedence_compare(peer.attempt_index,
                                       peer.priority_id,
                                       peer.clicker_id,
                                       peer.click_event_id,
                                       ble_courtesy_local.attempt_index,
                                       ble_courtesy_local.priority_id,
                                       ble_courtesy_local.clicker_id,
                                       ble_courtesy_local.click_event_id);
    if (cmp > 0) {
        uint32_t wait_ms = uwb_ble_courtesy_duration_ms(peer.defer_duration_units);

        clicker_ble_courtesy_note_higher_peer(wait_ms);
        LOG_INF("BLE courtesy saw higher-precedence clicker: peer=%llx event=%u attempt=%u priority=%llx wait_ms=%u",
                (unsigned long long)peer.clicker_id,
                peer.click_event_id,
                peer.attempt_index,
                (unsigned long long)peer.priority_id,
                wait_ms);
    }
    return false;
}

static void clicker_ble_courtesy_scan_cb(const bt_addr_le_t *addr,
                                         int8_t rssi,
                                         uint8_t adv_type,
                                         struct net_buf_simple *buf)
{
    ARG_UNUSED(addr);
    ARG_UNUSED(rssi);
    ARG_UNUSED(adv_type);

    if (!ble_courtesy_scan_active) {
        return;
    }
    bt_data_parse(buf, clicker_ble_courtesy_parse_ad, NULL);
}

static int clicker_ble_courtesy_init_once(void)
{
    int disable_ret;
    int ret;

    if (ble_courtesy_available) {
        return 0;
    }
    if (ble_courtesy_init_attempted) {
        return -ENOTSUP;
    }

    ble_courtesy_init_attempted = true;
    ret = bt_enable(NULL);
    if (ret != 0 && ret != -EALREADY) {
        LOG_WRN("BLE courtesy disabled: bt_enable failed: %d", ret);
        ble_courtesy_init_attempted = false;
        return ret;
    }

    ret = clicker_ble_courtesy_set_scan_channel();
    if (ret != 0) {
        LOG_WRN("BLE courtesy disabled: scan channel 37 map failed: %d", ret);
        disable_ret = bt_disable();
        if (disable_ret == 0 || disable_ret == -EALREADY) {
            ble_courtesy_init_attempted = false;
        } else {
            LOG_WRN("BLE courtesy initialization rollback failed: %d",
                    disable_ret);
        }
        return ret;
    }

    ble_courtesy_available = true;
    LOG_INF("BLE courtesy enabled on advertising/scanning channel 37");
    return 0;
}

int app_clicker_ble_courtesy_start(uint32_t event_seq,
                                   uint8_t attempt_index,
                                   uint64_t priority_id,
                                   uint32_t peer_finish_ms)
{
    const struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BLE_COURTESY_SCAN_INTERVAL_UNITS,
        .window = BLE_COURTESY_SCAN_WINDOW_UNITS,
        .timeout = 0u,
        .interval_coded = 0u,
        .window_coded = 0u,
    };
    const struct bt_le_adv_param adv_param = {
        .id = BT_ID_DEFAULT,
        .sid = 0u,
        .secondary_max_skip = 0u,
        .options = BT_LE_ADV_OPT_USE_IDENTITY |
                   BT_LE_ADV_OPT_DISABLE_CHAN_38 |
                   BT_LE_ADV_OPT_DISABLE_CHAN_39,
        .interval_min = BLE_COURTESY_ADV_INTERVAL_MIN_UNITS,
        .interval_max = BLE_COURTESY_ADV_INTERVAL_MAX_UNITS,
        .peer = NULL,
    };
    const struct bt_data ad[] = {
        BT_DATA(BT_DATA_MANUFACTURER_DATA,
                ble_courtesy_adv_data,
                sizeof(ble_courtesy_adv_data)),
    };
    size_t written = 0u;
    int ret;

    ret = clicker_ble_courtesy_init_once();
    if (ret < 0) {
        return ret;
    }

    ble_courtesy_local.network_id = NETWORK_ID;
    ble_courtesy_local.clicker_id = DEVICE_ID;
    ble_courtesy_local.click_event_id = event_seq;
    ble_courtesy_local.attempt_index = attempt_index;
    ble_courtesy_local.priority_id = priority_id;
    ble_courtesy_local.defer_duration_units =
        uwb_ble_courtesy_duration_units_from_ms(peer_finish_ms);
    ret = uwb_ble_courtesy_encode(&ble_courtesy_local,
                                  ble_courtesy_adv_data,
                                  sizeof(ble_courtesy_adv_data),
                                  &written);
    if (ret != PROTO_OK || written != sizeof(ble_courtesy_adv_data)) {
        return -EINVAL;
    }

    clicker_ble_courtesy_clear_higher_peer();
    /* Legacy scan and advertising share the random-address state. Start the
     * identity advertiser before identity scanning so Zephyr accepts both.
     */
    ret = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0u);
    if (ret != 0) {
        LOG_WRN("BLE courtesy advertising start failed: %d", ret);
        return ret;
    }
    ble_courtesy_adv_active = true;

    ble_courtesy_scan_active = true;
    ret = bt_le_scan_start(&scan_param, clicker_ble_courtesy_scan_cb);
    if (ret != 0) {
        int stop_ret;

        LOG_WRN("BLE courtesy scan start failed: %d", ret);
        ble_courtesy_scan_active = false;
        stop_ret = bt_le_adv_stop();
        if (stop_ret == 0 || stop_ret == -EALREADY) {
            ble_courtesy_adv_active = false;
        } else {
            LOG_WRN("BLE courtesy advertising rollback failed: %d", stop_ret);
        }
        return ret;
    }
    return 0;
}

uint32_t app_clicker_ble_courtesy_higher_wait_ms(void)
{
    k_spinlock_key_t key = k_spin_lock(&ble_courtesy_lock);
    uint32_t wait_ms = ble_courtesy_higher_wait_ms;

    k_spin_unlock(&ble_courtesy_lock, key);
    return wait_ms;
}

static int clicker_ble_courtesy_stop_advertising(void)
{
    int ret = 0;

    for (uint8_t attempt = 0u;
         ble_courtesy_adv_active && attempt < BLE_COURTESY_STOP_RETRY_COUNT;
         attempt++) {
        ret = bt_le_adv_stop();
        if (ret == 0 || ret == -EALREADY) {
            ble_courtesy_adv_active = false;
            return 0;
        }
        if (attempt + 1u < BLE_COURTESY_STOP_RETRY_COUNT) {
            k_msleep(BLE_COURTESY_STOP_RETRY_DELAY_MS);
        }
    }
    if (ble_courtesy_adv_active) {
        LOG_WRN("BLE courtesy advertising stop failed after %u attempts: %d",
                BLE_COURTESY_STOP_RETRY_COUNT,
                ret);
    }
    return ret;
}

static int clicker_ble_courtesy_stop_scanning(void)
{
    int ret = 0;

    for (uint8_t attempt = 0u;
         ble_courtesy_scan_active && attempt < BLE_COURTESY_STOP_RETRY_COUNT;
         attempt++) {
        ret = bt_le_scan_stop();
        if (ret == 0 || ret == -EALREADY) {
            ble_courtesy_scan_active = false;
            return 0;
        }
        if (attempt + 1u < BLE_COURTESY_STOP_RETRY_COUNT) {
            k_msleep(BLE_COURTESY_STOP_RETRY_DELAY_MS);
        }
    }
    if (ble_courtesy_scan_active) {
        LOG_WRN("BLE courtesy scan stop failed after %u attempts: %d",
                BLE_COURTESY_STOP_RETRY_COUNT,
                ret);
    }
    return ret;
}

void app_clicker_ble_courtesy_stop(void)
{
    (void)clicker_ble_courtesy_stop_advertising();
    (void)clicker_ble_courtesy_stop_scanning();
}

int app_clicker_ble_courtesy_low_power_stop(void)
{
    int ret = 0;

    app_clicker_ble_courtesy_stop();
    if (!ble_courtesy_init_attempted) {
        return 0;
    }

    for (uint8_t attempt = 0u; attempt < BLE_COURTESY_STOP_RETRY_COUNT; attempt++) {
        ret = bt_disable();
        if (ret == 0 || ret == -EALREADY) {
            ble_courtesy_init_attempted = false;
            ble_courtesy_available = false;
            ble_courtesy_adv_active = false;
            ble_courtesy_scan_active = false;
            clicker_ble_courtesy_clear_higher_peer();
            return 0;
        }
        if (attempt + 1u < BLE_COURTESY_STOP_RETRY_COUNT) {
            k_msleep(BLE_COURTESY_STOP_RETRY_DELAY_MS);
        }
    }
    LOG_WRN("BLE courtesy disable before retained idle failed after %u attempts: %d",
            BLE_COURTESY_STOP_RETRY_COUNT,
            ret);
    return ret;
}
#else
int app_clicker_ble_courtesy_start(uint32_t event_seq,
                                   uint8_t attempt_index,
                                   uint64_t priority_id,
                                   uint32_t peer_finish_ms)
{
    ARG_UNUSED(event_seq);
    ARG_UNUSED(attempt_index);
    ARG_UNUSED(priority_id);
    ARG_UNUSED(peer_finish_ms);

    return -ENOTSUP;
}

uint32_t app_clicker_ble_courtesy_higher_wait_ms(void)
{
    return 0u;
}

void app_clicker_ble_courtesy_stop(void)
{
}

int app_clicker_ble_courtesy_low_power_stop(void)
{
    return 0;
}
#endif

#if DT_NODE_HAS_STATUS(CLICK_BUTTON_NODE, okay)
static const struct gpio_dt_spec click_button = GPIO_DT_SPEC_GET(CLICK_BUTTON_NODE, gpios);
static struct gpio_callback click_button_cb;
static struct k_work click_button_work;
static struct k_work_delayable click_button_release_work;
static struct k_work_delayable click_button_rearm_work;
static struct k_work_delayable self_test_arm_timeout_work;
static struct k_work clicker_action_work;
#define CLICK_BUTTON_PORT_NUM DT_PROP(DT_GPIO_CTLR(CLICK_BUTTON_NODE, gpios), port)
#define CLICK_BUTTON_PIN_NUM DT_GPIO_PIN(CLICK_BUTTON_NODE, gpios)
#define HAS_CLICK_BUTTON 1
#else
#define HAS_CLICK_BUTTON 0
#endif

#if (HAS_CLICK_BUTTON || defined(CONFIG_IMEC_ML_CLICKER)) && \
    DEVICE_ROLE == ROLE_CLICKER
#define HAS_CLICKER_ACTION_WORK_QUEUE 1
#else
#define HAS_CLICKER_ACTION_WORK_QUEUE 0
#endif

#if HAS_CLICKER_ACTION_WORK_QUEUE
K_THREAD_STACK_DEFINE(clicker_action_work_q_stack, CLICKER_ACTION_WORKQUEUE_STACK_SIZE);
#endif

#if HAS_CLICKER_ACTION_WORK_QUEUE
static struct k_work_q clicker_action_work_q;
static const struct k_work_queue_config clicker_action_work_q_config = {
    .name = "clicker_action",
};
static bool clicker_action_work_q_started;
#endif
#if HAS_CLICK_BUTTON
static atomic_t clicker_action_active;
static struct button_action_handoff clicker_action_handoff;
static struct k_spinlock clicker_action_handoff_lock;
static uint32_t clicker_action_watchdog_generation;
static struct k_work_delayable clicker_action_submit_retry_work;
static struct button_wake_recovery clicker_action_submit_recovery;
static struct k_spinlock click_button_edge_lock;
static bool click_button_press_pending;
static bool click_button_press_cycle_active;
static uint32_t click_button_press_at_ms;
static uint32_t clicker_low_power_transition_failures;
static bool click_button_systemoff_wake_armed;
static struct button_wake_recovery click_button_systemoff_recovery;
static struct button_wake_recovery click_button_release_recovery;
static struct button_wake_recovery click_button_rearm_recovery;
static bool click_button_callback_initialized;
static bool click_button_callback_registered;
static void click_button_recovery_reset(const char *source, int error);
static void click_button_schedule_rearm_recovery(int error,
                                                 const char *source);
#endif

int app_clicker_init(const struct app_clicker_callbacks *callbacks)
{
    if (callbacks != NULL) {
        clicker_callbacks = *callbacks;
    } else {
        clicker_callbacks = (struct app_clicker_callbacks){0};
    }
    app_clicker_event_runtime_init(&clicker_event_runtime);
    return 0;
}

bool clicker_reset_reason_was_systemoff(void)
{
#if NRF_POWER_HAS_RESETREAS
    uint32_t reset_reason = nrf_power_resetreas_get(NRF_POWER);

    if (reset_reason != 0u) {
        nrf_power_resetreas_clear(NRF_POWER, reset_reason);
    }
    return (reset_reason & NRF_POWER_RESETREAS_OFF_MASK) != 0u;
#else
    return false;
#endif
}

static uint32_t systemoff_ram_retention_mask(void)
{
    uint32_t mask = 0u;

#if defined(POWER_RAM_POWER_S0RETENTION_Msk)
    mask |= POWER_RAM_POWER_S0RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S1RETENTION_Msk)
    mask |= POWER_RAM_POWER_S1RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S2RETENTION_Msk)
    mask |= POWER_RAM_POWER_S2RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S3RETENTION_Msk)
    mask |= POWER_RAM_POWER_S3RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S4RETENTION_Msk)
    mask |= POWER_RAM_POWER_S4RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S5RETENTION_Msk)
    mask |= POWER_RAM_POWER_S5RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S6RETENTION_Msk)
    mask |= POWER_RAM_POWER_S6RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S7RETENTION_Msk)
    mask |= POWER_RAM_POWER_S7RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S8RETENTION_Msk)
    mask |= POWER_RAM_POWER_S8RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S9RETENTION_Msk)
    mask |= POWER_RAM_POWER_S9RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S10RETENTION_Msk)
    mask |= POWER_RAM_POWER_S10RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S11RETENTION_Msk)
    mask |= POWER_RAM_POWER_S11RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S12RETENTION_Msk)
    mask |= POWER_RAM_POWER_S12RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S13RETENTION_Msk)
    mask |= POWER_RAM_POWER_S13RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S14RETENTION_Msk)
    mask |= POWER_RAM_POWER_S14RETENTION_Msk;
#endif
#if defined(POWER_RAM_POWER_S15RETENTION_Msk)
    mask |= POWER_RAM_POWER_S15RETENTION_Msk;
#endif
    return mask;
}

void clicker_request_systemoff_ram_retention(void)
{
#if defined(POWER_RAM_POWER_S0RETENTION_Msk)
    uint32_t retention_mask;

    if (!IS_ENABLED(CONFIG_IMEC_CLICKER_SYSTEMOFF_RAM_RETENTION)) {
        return;
    }

    retention_mask = systemoff_ram_retention_mask();
    for (size_t block = 0u; block < ARRAY_SIZE(NRF_POWER->RAM); block++) {
        nrf_power_rampower_mask_on(NRF_POWER, (uint8_t)block, retention_mask);
    }
#endif
}

int app_clicker_start_work_queue(void)
{
#if HAS_CLICKER_ACTION_WORK_QUEUE
    if (clicker_action_work_q_started) {
        return 0;
    }

    k_work_queue_start(&clicker_action_work_q,
                       clicker_action_work_q_stack,
                       K_THREAD_STACK_SIZEOF(clicker_action_work_q_stack),
                       CLICKER_ACTION_WORKQUEUE_PRIORITY,
                       &clicker_action_work_q_config);
    clicker_action_work_q_started = true;
#endif
    return 0;
}

int app_clicker_submit_work(struct k_work *work)
{
#if HAS_CLICKER_ACTION_WORK_QUEUE
    int ret;

    if (work == NULL) {
        return -EINVAL;
    }

    ret = app_clicker_start_work_queue();
    if (ret < 0) {
        return ret;
    }
    return k_work_submit_to_queue(&clicker_action_work_q, work);
#else
    ARG_UNUSED(work);

    return -ENODEV;
#endif
}

uint8_t app_clicker_debug_min_anchor_count(void)
{
    return UWB_NORMAL_CLICK_MIN_ANCHORS;
}

uint8_t app_clicker_debug_max_anchor_count(void)
{
    return UWB_NORMAL_CLICK_MAX_ANCHORS;
}

uint8_t app_clicker_debug_samples_per_anchor(void)
{
    return UWB_RANGING_REQUESTS_MAX_PER_ANCHOR;
}

uint8_t app_clicker_debug_session_flags(void)
{
    return FLAG_COUNT_AS_CLICK;
}

static int clicker_connect_status_leds_for_action(void)
{
    int ret = 0;

    if (!clicker_systemon_retained_idle_enabled()) {
        return 0;
    }

    for (uint8_t attempt = 0u;
         attempt < CLICKER_STATUS_LED_CONNECT_ATTEMPTS;
         attempt++) {
        ret = status_leds_connect();
        if (ret == 0) {
            return 0;
        }
        if (attempt + 1u < CLICKER_STATUS_LED_CONNECT_ATTEMPTS) {
            k_busy_wait(CLICKER_STATUS_LED_CONNECT_RETRY_US);
        }
    }
    return ret;
}

static void clicker_hold_terminal_status(int ret)
{
    ARG_UNUSED(ret);
    k_msleep(STATUS_PASS_DURATION_MS);
}

void app_clicker_handle_button_action(enum button_action action)
{
    struct status_inputs status = {0};
    enum self_test_failure failure;
    uint32_t self_test_event_seq;
    bool self_test_event_allocated;
    int ret;

    if (action != BUTTON_ACTION_NONE) {
        ret = clicker_connect_status_leds_for_action();
        if (ret < 0) {
            LOG_ERR("clicker status LED reconnect failed after bounded retry: %d",
                    ret);
        }
    }

    switch (action) {
    case BUTTON_ACTION_NORMAL_CLICK:
        ret = app_clicker_run_normal_click();
        status.click_accepted = ret == 0;
        if (ret != 0) {
            status.click_failure = (ret == -ETIMEDOUT) ?
                                    CLICK_FAILURE_NO_ANCHOR :
                                    CLICK_FAILURE_INSUFFICIENT_RANGES;
        }
        status_apply(&status);
        if (ret == 0) {
            LOG_INF("normal click MVP completed");
        } else {
            LOG_WRN("normal click MVP failed: %d", ret);
        }
        clicker_hold_terminal_status(ret);
        break;
    case BUTTON_ACTION_SELF_TEST_ARMED:
        status.self_test_armed = true;
        status_apply(&status);
        app_clicker_arm_self_test_timeout();
        LOG_INF("self-test armed");
        break;
    case BUTTON_ACTION_SELF_TEST_START:
        app_clicker_cancel_self_test_timeout();
        status.self_test_running = true;
        status_apply(&status);
        ret = app_click_event_sequence_next(&self_test_event_seq);
        self_test_event_allocated = ret == 0;
        if (ret < 0) {
            LOG_ERR("self-test identity allocation failed closed: %d", ret);
            failure = SELF_TEST_FAILURE_INTERNAL;
        } else {
            failure = app_clicker_run_self_test(self_test_event_seq);
        }
        status.self_test_running = false;
        status.failure = failure;
        status.self_test_passed = failure == SELF_TEST_FAILURE_NONE;
        status_apply(&status);
        if (self_test_event_allocated) {
            (void)app_clicker_emit_self_test_report(self_test_event_seq,
                                                    failure);
        }
        clicker_hold_terminal_status(
            failure == SELF_TEST_FAILURE_NONE ? 0 : -EIO);
        break;
    case BUTTON_ACTION_SELF_TEST_CANCELLED:
        status_apply(&status);
        LOG_INF("self-test arm cancelled");
        break;
    case BUTTON_ACTION_NONE:
    default:
        break;
    }
}

#if HAS_CLICK_BUTTON
static bool click_button_latch_press(uint32_t pressed_at_ms)
{
    bool latched = false;
    k_spinlock_key_t key = k_spin_lock(&click_button_edge_lock);

    if (!click_button_press_pending && !click_button_press_cycle_active) {
        click_button_press_at_ms = pressed_at_ms;
        click_button_press_pending = true;
        latched = true;
    }
    k_spin_unlock(&click_button_edge_lock, key);
    return latched;
}

static bool click_button_press_is_pending(void)
{
    bool pending;
    k_spinlock_key_t key = k_spin_lock(&click_button_edge_lock);

    pending = click_button_press_pending;
    k_spin_unlock(&click_button_edge_lock, key);
    return pending;
}

static bool click_button_press_cycle_is_active(void)
{
    bool active;
    k_spinlock_key_t key = k_spin_lock(&click_button_edge_lock);

    active = click_button_press_cycle_active;
    k_spin_unlock(&click_button_edge_lock, key);
    return active;
}

static bool click_button_claim_press(uint32_t sampled_at_ms,
                                     uint32_t *owned_press_at_ms,
                                     bool *latched)
{
    k_spinlock_key_t key;

    if (owned_press_at_ms == NULL || latched == NULL) {
        return false;
    }

    key = k_spin_lock(&click_button_edge_lock);
    if (click_button_press_cycle_active) {
        k_spin_unlock(&click_button_edge_lock, key);
        return false;
    }

    *latched = click_button_press_pending;
    *owned_press_at_ms = click_button_press_pending ?
                         click_button_press_at_ms : sampled_at_ms;
    click_button_press_pending = false;
    click_button_press_cycle_active = true;
    k_spin_unlock(&click_button_edge_lock, key);
    return true;
}

static void click_button_finish_press_cycle(void)
{
    k_spinlock_key_t key = k_spin_lock(&click_button_edge_lock);

    click_button_press_cycle_active = false;
    k_spin_unlock(&click_button_edge_lock, key);
}

static int click_button_pressed(void)
{
    int value = gpio_pin_get_raw(click_button.port, click_button.pin);

    if (value < 0) {
        return value;
    }
    return value == 0 ? 1 : 0;
}

static void click_button_clear_latch(void)
{
#if defined(NRF_GPIO_LATCH_PRESENT)
    nrf_gpio_pin_latch_clear(NRF_GPIO_PIN_MAP(CLICK_BUTTON_PORT_NUM,
                                              CLICK_BUTTON_PIN_NUM));
#endif
}

static int click_button_gesture_handle(enum button_signal signal,
                                       uint32_t now_ms,
                                       enum button_action *action)
{
    struct fw_effect effect;
    int ret;

    if (action == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = app_clicker_event_runtime_button_signal(&clicker_event_runtime,
                                                  signal,
                                                  now_ms,
                                                  action);
    while (app_clicker_event_runtime_take_effect(&clicker_event_runtime,
                                                 &effect)) {
        /* GPIO release polling and the existing delayable work items own the
         * actual timers.  The shared button machine has already recorded the
         * requested START/CANCEL transition, so consume those effects at the
         * same serialized work boundary. */
        if (effect.type != FW_EFFECT_START_TIMER &&
            effect.type != FW_EFFECT_CANCEL_TIMER &&
            effect.type != FW_EFFECT_PUBLISH_EVENT) {
            LOG_ERR("unexpected clicker button effect: type=%u",
                    (unsigned int)effect.type);
            if (ret == 0) {
                ret = -EPROTO;
            }
        }
    }
    return ret;
}

static int click_button_configure_input(void)
{
    if (!gpio_is_ready_dt(&click_button)) {
        return -ENODEV;
    }

    return gpio_pin_configure_dt(&click_button, GPIO_INPUT);
}

static int click_button_ensure_callback_registered(void)
{
    int ret;

    if (click_button_callback_registered) {
        return 0;
    }
    if (!click_button_callback_initialized) {
        return -EAGAIN;
    }

    ret = gpio_add_callback(click_button.port, &click_button_cb);
    if (ret < 0) {
        return ret;
    }
    click_button_callback_registered = true;
    return 0;
}

static bool click_button_wait_for_release(void)
{
    int64_t released_since_ms = -1;
    uint8_t read_failures = 0u;

    for (;;) {
        int64_t now_ms = k_uptime_get();
        int pressed = click_button_pressed();

        if (pressed < 0) {
            read_failures++;
            if (read_failures >= CLICK_BUTTON_RECOVERY_MAX_FAILURES) {
                return false;
            }
            k_msleep(CLICK_BUTTON_RECOVERY_RETRY_MS);
            continue;
        }
        read_failures = 0u;
        if (pressed != 0) {
            released_since_ms = -1;
        } else {
            if (released_since_ms < 0) {
                released_since_ms = now_ms;
            }
            if ((now_ms - released_since_ms) >= CLICK_BUTTON_RELEASE_STABLE_MS) {
                return true;
            }
        }
        k_msleep(CLICK_BUTTON_RELEASE_POLL_MS);
    }
}

static int click_button_arm_idle_interrupt(void)
{
    int pressed;
    int ret;

    ret = click_button_configure_input();
    if (ret < 0) {
        return ret;
    }
    ret = click_button_ensure_callback_registered();
    if (ret < 0) {
        return ret;
    }
    ret = gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_DISABLE);
    if (ret < 0) {
        return ret;
    }
    click_button_clear_latch();
    ret = gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        return ret;
    }

    /*
     * Close the clear/arm race: a press that became active before edge
     * sensing was enabled may not produce an edge, so sample after arming and
     * hand the already-active level to the same serialized work owner.
     */
    pressed = click_button_pressed();
    if (pressed < 0) {
        (void)gpio_pin_interrupt_configure(click_button.port,
                                           click_button.pin,
                                           GPIO_INT_DISABLE);
        return pressed;
    }
    if (pressed != 0) {
        (void)click_button_latch_press(k_uptime_get_32());
    }
    if (pressed != 0 || click_button_press_is_pending()) {
        ret = k_work_submit(&click_button_work);
        if (ret < 0) {
            (void)gpio_pin_interrupt_configure(click_button.port,
                                               click_button.pin,
                                               GPIO_INT_DISABLE);
            return ret;
        }
    }
    return 0;
}

static bool clicker_capture_systemoff_button_action(enum button_action *action)
{
    enum button_action ignored = BUTTON_ACTION_NONE;
    int ret;

    if (action == NULL) {
        return false;
    }
    *action = BUTTON_ACTION_NONE;

    ret = click_button_configure_input();
    if (ret < 0) {
        LOG_WRN("click button input unavailable after wake: %d", ret);
        return false;
    }
    ret = gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_DISABLE);
    if (ret < 0) {
        LOG_WRN("click button interrupt disable after wake failed: %d", ret);
        return false;
    }
    click_button_clear_latch();

    ret = click_button_gesture_handle(BUTTON_SIGNAL_PRESS,
                                      k_uptime_get_32(),
                                      &ignored);
    if (ret != PROTO_OK) {
        return false;
    }
    if (!click_button_wait_for_release()) {
        return false;
    }
    click_button_clear_latch();

    ret = click_button_gesture_handle(BUTTON_SIGNAL_RELEASE,
                                      k_uptime_get_32(),
                                      action);
    return ret == PROTO_OK;
}

static void clicker_prepare_radio_systemoff(void)
{
    uint32_t dev_id;
    int ret;

    ret = dwm3000_port_init();
    if (ret < 0) {
        LOG_WRN("DWM3000 port init before system-off failed: %d", ret);
        (void)dwm3000_port_prepare_systemoff();
        return;
    }
    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        LOG_WRN("DWM3000 wake before system-off failed: %d", ret);
        (void)dwm3000_port_prepare_systemoff();
        return;
    }
    ret = dwm3000_port_hw_reset();
    if (ret < 0) {
        LOG_WRN("DWM3000 reset before system-off failed: %d", ret);
        (void)dwm3000_port_prepare_systemoff();
        return;
    }
    ret = dwm3000_driver_probe(&dev_id);
    if (ret < 0) {
        LOG_WRN("DWM3000 probe before system-off failed: %d", ret);
        (void)dwm3000_port_prepare_systemoff();
        return;
    }
    ret = dwm3000_driver_configure_default();
    if (ret < 0) {
        LOG_WRN("DWM3000 config before system-off failed: %d", ret);
        (void)dwm3000_port_prepare_systemoff();
        return;
    }
    ret = dwm3000_driver_standby();
    if (ret < 0) {
        LOG_WRN("DWM3000 standby before system-off failed: %d", ret);
    }
    ret = dwm3000_port_prepare_systemoff();
    if (ret < 0) {
        LOG_WRN("DWM3000 pin park before system-off failed: %d", ret);
    }
}

static void clicker_systemoff_now(void)
{
    if (!click_button_systemoff_wake_armed) {
        click_button_recovery_reset("systemoff_unarmed", -EPERM);
        return;
    }

    clicker_request_systemoff_ram_retention();
    LOG_PANIC();
    sys_poweroff();
}

static int click_button_try_arm_systemoff_wake(void)
{
    int pressed;
    int ret;

    click_button_systemoff_wake_armed = false;
    ret = click_button_configure_input();
    if (ret < 0) {
        LOG_WRN("click button wake input unavailable: %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_DISABLE);
    if (ret < 0) {
        LOG_WRN("click button wake interrupt disable failed: %d", ret);
        return ret;
    }

    pressed = click_button_pressed();
    if (pressed < 0) {
        LOG_WRN("click button wake release read failed: %d", pressed);
        return pressed;
    }
    if (pressed != 0) {
        return -EBUSY;
    }

    k_msleep(CLICK_BUTTON_RELEASE_STABLE_MS);
    pressed = click_button_pressed();
    if (pressed < 0) {
        LOG_WRN("click button stable-release read failed: %d", pressed);
        return pressed;
    }
    if (pressed != 0) {
        return -EBUSY;
    }

    click_button_clear_latch();
    ret = gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_LEVEL_LOW);
    if (ret < 0) {
        LOG_WRN("click button wake arm failed: %d", ret);
        return ret;
    }

    click_button_systemoff_wake_armed = true;
    return 0;
}

void app_clicker_enter_systemoff_idle(void)
{
    enum button_wake_recovery_action action;
    bool held_reported = false;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_CLICKER_SYSTEMOFF_IDLE) ||
        DEVICE_ROLE != ROLE_CLICKER) {
        return;
    }

    (void)battery_adc_divider_disable();
    clicker_prepare_radio_systemoff();
    status_leds_set(false, false, false);
    status_leds_disconnect();

    button_wake_recovery_init(&click_button_systemoff_recovery,
                              CLICK_BUTTON_RECOVERY_MAX_FAILURES);
    for (;;) {
        ret = click_button_try_arm_systemoff_wake();
        if (ret == 0) {
            action = button_wake_recovery_note(
                &click_button_systemoff_recovery,
                BUTTON_WAKE_OBSERVATION_ARMED);
            if (action != BUTTON_WAKE_RECOVERY_POWER_READY) {
                click_button_recovery_reset("systemoff_policy", -EPERM);
                return;
            }
            LOG_INF("clicker entering system-off idle; wake source=P0.%u physical-low",
                    (unsigned int)CLICK_BUTTON_PIN_NUM);
            clicker_systemoff_now();
            return;
        }

        action = button_wake_recovery_note(
            &click_button_systemoff_recovery,
            ret == -EBUSY ? BUTTON_WAKE_OBSERVATION_WAITING :
                            BUTTON_WAKE_OBSERVATION_FAILURE);
        if (action == BUTTON_WAKE_RECOVERY_RESET) {
            LOG_ERR("click button wake arm exhausted bounded recovery: ret=%d attempts=%u",
                    ret,
                    click_button_systemoff_recovery.consecutive_failures);
            click_button_recovery_reset("systemoff_arm", ret);
            return;
        }
        if (ret == -EBUSY) {
            if (!held_reported) {
                LOG_WRN("click button still held; keeping System ON until a wake source can be armed");
                held_reported = true;
            }
            k_msleep(CLICK_BUTTON_RELEASE_POLL_MS);
        } else {
            held_reported = false;
            k_msleep(CLICK_BUTTON_RECOVERY_RETRY_MS);
        }
    }
}

static int clicker_radio_retained_standby_transition(void)
{
    int ret = dwm3000_driver_configure_wake_mode();

    if (ret < 0) {
        return ret;
    }
    return dwm3000_driver_standby();
}

static int clicker_enter_radio_retained_standby(void)
{
    struct app_radio_low_power_policy policy;
    enum app_radio_low_power_action action;
    int first_ret;
    int recovery_ret;
    int retry_ret = 0;

    app_radio_low_power_policy_init(&policy, APP_RADIO_LOW_POWER_STANDBY);
    first_ret = clicker_radio_retained_standby_transition();
    action = app_radio_low_power_policy_note_transition(&policy, first_ret);
    if (action == APP_RADIO_LOW_POWER_COMPLETE) {
        return 0;
    }

    recovery_ret = dwm3000_driver_force_recovery();
    action = app_radio_low_power_policy_note_recovery(&policy, recovery_ret);
    if (action == APP_RADIO_LOW_POWER_RETRY) {
        retry_ret = clicker_radio_retained_standby_transition();
        action = app_radio_low_power_policy_note_transition(&policy, retry_ret);
        if (action == APP_RADIO_LOW_POWER_COMPLETE) {
            LOG_WRN("clicker DWM3000 retained standby recovered: first_ret=%d",
                    first_ret);
            return 0;
        }
    }

    if (clicker_low_power_transition_failures != UINT32_MAX) {
        clicker_low_power_transition_failures++;
    }
    LOG_ERR("clicker DWM3000 retained standby failed after bounded recovery: first_ret=%d recovery_ret=%d retry_ret=%d failures=%u",
            first_ret,
            recovery_ret,
            retry_ret,
            clicker_low_power_transition_failures);
    return recovery_ret < 0 ? recovery_ret : retry_ret;
}

static int clicker_arm_retained_idle_wake(void)
{
    int ret = -EIO;

    for (uint8_t attempt = 0u;
         attempt < CLICK_BUTTON_RECOVERY_MAX_FAILURES;
         attempt++) {
        ret = click_button_arm_idle_interrupt();
        if (ret == 0) {
            return 0;
        }
        LOG_WRN("click button retained-idle wake arm failed: ret=%d attempt=%u/%u",
                ret,
                (unsigned int)(attempt + 1u),
                CLICK_BUTTON_RECOVERY_MAX_FAILURES);
        if (clicker_action_watchdog_generation != 0u &&
            !app_watchdog_note_clicker_action_progress(
                clicker_action_watchdog_generation)) {
            return -EIO;
        }
        if (attempt + 1u < CLICK_BUTTON_RECOVERY_MAX_FAILURES) {
            k_msleep(CLICK_BUTTON_RECOVERY_RETRY_MS);
        }
    }
    return ret;
}

static void clicker_enter_systemon_retained_idle(void)
{
    bool pins_floated = false;
    int ret;

    if (!clicker_systemon_retained_idle_enabled()) {
        return;
    }

    ret = battery_adc_divider_disable();
    if (ret < 0) {
        LOG_ERR("battery ADC divider disable before retained idle failed: %d",
                ret);
        click_button_recovery_reset("retained_idle_adc", ret);
        return;
    }
    ret = app_clicker_ble_courtesy_low_power_stop();
    if (ret < 0) {
        LOG_ERR("BLE cleanup before retained idle incomplete: %d", ret);
        click_button_recovery_reset("retained_idle_ble", ret);
        return;
    }
    ret = clicker_enter_radio_retained_standby();
    if (ret < 0) {
        click_button_recovery_reset("retained_idle_radio", ret);
        return;
    }
    for (uint8_t attempt = 0u; attempt < 2u; attempt++) {
        ret = dwm3000_port_float_pins();
        if (ret == 0) {
            pins_floated = true;
            break;
        }
        if (attempt == 0u) {
            k_busy_wait(100u);
        }
    }
    if (!pins_floated) {
        LOG_ERR("DWM3000 retained-idle pin float failed after retry: %d",
                ret);
        click_button_recovery_reset("retained_idle_pins", ret);
        return;
    }
    ret = clicker_arm_retained_idle_wake();
    if (ret < 0) {
        click_button_recovery_reset("retained_idle_wake", ret);
        return;
    }
    LOG_INF("CLICKER_IDLE" " mode=system_on_retained wake_source=P0.%u button_irq=edge_to_active release_poll=1 local_command_poll=0 radio_retained=1 dwm_pins=%s",
            (unsigned int)CLICK_BUTTON_PIN_NUM,
            pins_floated ? "float" : "driven");
    LOG_INF("clicker entering retained system-on idle; wake source=P0.%u press-edge interrupt with release polling",
            (unsigned int)CLICK_BUTTON_PIN_NUM);
    status_leds_set(false, false, false);
    status_leds_disconnect();
}

void app_clicker_enter_idle(void)
{
    if (clicker_systemon_retained_idle_enabled()) {
        clicker_enter_systemon_retained_idle();
        return;
    }

    app_clicker_enter_systemoff_idle();
}

static void clicker_action_work_handler(struct k_work *work)
{
    bool idle_after_drain = false;
    k_spinlock_key_t recovery_key;

    ARG_UNUSED(work);

    (void)k_work_cancel_delayable(&clicker_action_submit_retry_work);
    recovery_key = k_spin_lock(&clicker_action_handoff_lock);
    button_wake_recovery_init(&clicker_action_submit_recovery,
                              CLICK_BUTTON_RECOVERY_MAX_FAILURES);
    k_spin_unlock(&clicker_action_handoff_lock, recovery_key);

    for (;;) {
        enum button_action action = BUTTON_ACTION_NONE;
        k_spinlock_key_t key =
            k_spin_lock(&clicker_action_handoff_lock);

        if (button_action_handoff_take(&clicker_action_handoff, &action)) {
            k_spin_unlock(&clicker_action_handoff_lock, key);
            if (!app_watchdog_note_clicker_action_progress(
                    clicker_action_watchdog_generation)) {
                click_button_recovery_reset(
                    "action_watchdog_begin", -EIO);
                return;
            }
            app_clicker_handle_button_action(action);
            if (!app_watchdog_note_clicker_action_progress(
                    clicker_action_watchdog_generation)) {
                click_button_recovery_reset(
                    "action_watchdog_end", -EIO);
                return;
            }
            idle_after_drain =
                action == BUTTON_ACTION_NORMAL_CLICK ||
                action == BUTTON_ACTION_SELF_TEST_START ||
                action == BUTTON_ACTION_SELF_TEST_CANCELLED;
            continue;
        }

        if (idle_after_drain) {
            /*
             * Keep ownership across the idle transition.  The retained FIFO
             * accepts a press that arrives after GPIO re-arm but before this
             * worker can inspect the queue again.
             */
            k_spin_unlock(&clicker_action_handoff_lock, key);
            if (!app_watchdog_note_clicker_action_progress(
                    clicker_action_watchdog_generation)) {
                click_button_recovery_reset(
                    "action_watchdog_idle_begin", -EIO);
                return;
            }
            app_clicker_enter_idle();
            if (!app_watchdog_note_clicker_action_progress(
                    clicker_action_watchdog_generation)) {
                click_button_recovery_reset(
                    "action_watchdog_idle_end", -EIO);
                return;
            }
            idle_after_drain = false;
            continue;
        }

        if (!button_action_handoff_release_if_empty(
                &clicker_action_handoff)) {
            k_spin_unlock(&clicker_action_handoff_lock, key);
            click_button_recovery_reset("action_handoff_release", -EIO);
            return;
        }
        if (clicker_action_watchdog_generation != 0u &&
            !app_watchdog_clicker_action_end(
                clicker_action_watchdog_generation)) {
            k_spin_unlock(&clicker_action_handoff_lock, key);
            click_button_recovery_reset(
                "action_watchdog_release", -EIO);
            return;
        }
        clicker_action_watchdog_generation = 0u;
        atomic_set(&clicker_action_active, 0);
        k_spin_unlock(&clicker_action_handoff_lock, key);
        return;
    }
}

static void clicker_action_submit_or_recover(const char *source)
{
    enum button_wake_recovery_action recovery_action;
    uint8_t consecutive_failures;
    uint8_t max_failures;
    k_spinlock_key_t key;
    int ret;

    ret = app_clicker_submit_work(&clicker_action_work);
    key = k_spin_lock(&clicker_action_handoff_lock);
    if (ret >= 0) {
        button_wake_recovery_init(&clicker_action_submit_recovery,
                                  CLICK_BUTTON_RECOVERY_MAX_FAILURES);
        k_spin_unlock(&clicker_action_handoff_lock, key);
        return;
    }

    recovery_action = button_wake_recovery_note(
        &clicker_action_submit_recovery,
        BUTTON_WAKE_OBSERVATION_FAILURE);
    consecutive_failures =
        clicker_action_submit_recovery.consecutive_failures;
    max_failures = clicker_action_submit_recovery.max_failures;
    k_spin_unlock(&clicker_action_handoff_lock, key);
    if (recovery_action == BUTTON_WAKE_RECOVERY_RESET) {
        click_button_recovery_reset(source, ret);
        return;
    }

    LOG_WRN("clicker button action worker submit deferred: source=%s ret=%d attempt=%u/%u",
            source == NULL ? "unknown" : source,
            ret,
            consecutive_failures,
            max_failures);
    ret = k_work_reschedule(&clicker_action_submit_retry_work,
                            K_MSEC(CLICK_BUTTON_RECOVERY_RETRY_MS));
    if (ret < 0) {
        click_button_recovery_reset("action_submit_retry_schedule", ret);
    }
}

static void clicker_action_submit_retry_work_handler(struct k_work *work)
{
    bool submit_needed;
    k_spinlock_key_t key;

    ARG_UNUSED(work);

    key = k_spin_lock(&clicker_action_handoff_lock);
    submit_needed = clicker_action_handoff.owner_active &&
                    clicker_action_handoff.count > 0u;
    k_spin_unlock(&clicker_action_handoff_lock, key);
    if (submit_needed) {
        clicker_action_submit_or_recover("action_submit_retry");
    }
}

void app_clicker_submit_button_action(enum button_action action)
{
    enum button_action_handoff_submit_result submit_result;
    uint8_t queue_depth;
    k_spinlock_key_t key;

    if (action == BUTTON_ACTION_NONE) {
        return;
    }

    key = k_spin_lock(&clicker_action_handoff_lock);
    submit_result =
        button_action_handoff_submit(&clicker_action_handoff, action);
    if (submit_result == BUTTON_ACTION_HANDOFF_START_OWNER) {
        clicker_action_watchdog_generation =
            app_watchdog_clicker_action_begin();
        if (clicker_action_watchdog_generation != 0u) {
            atomic_set(&clicker_action_active, 1);
        }
    }
    queue_depth = clicker_action_handoff.count;
    k_spin_unlock(&clicker_action_handoff_lock, key);

    if (submit_result == BUTTON_ACTION_HANDOFF_FULL) {
        LOG_ERR("clicker button action FIFO exhausted: action=%u capacity=%u",
                (unsigned int)action,
                BUTTON_ACTION_HANDOFF_CAPACITY);
        click_button_recovery_reset("action_handoff_full", -ENOSPC);
        return;
    }
    if (submit_result == BUTTON_ACTION_HANDOFF_START_OWNER &&
        clicker_action_watchdog_generation == 0u) {
        click_button_recovery_reset("action_watchdog_admission", -EIO);
        return;
    }
    if (submit_result == BUTTON_ACTION_HANDOFF_START_OWNER) {
        clicker_action_submit_or_recover("action_submit");
    } else {
    }
}

static void click_button_handle_signal_at(enum button_signal signal,
                                          uint32_t signal_at_ms,
                                          const char *source)
{
    enum button_action action;
    int ret;

    ret = click_button_gesture_handle(signal, signal_at_ms, &action);
    if (ret != PROTO_OK) {
        LOG_ERR("button FSM rejected signal %u from %s: %d",
                (unsigned int)signal,
                source == NULL ? "unknown" : source,
                ret);
        return;
    }
    app_clicker_submit_button_action(action);
}

static void click_button_handle_signal(enum button_signal signal,
                                       const char *source)
{
    click_button_handle_signal_at(signal, k_uptime_get_32(), source);
}

static void click_button_recovery_reset(const char *source, int error)
{
    LOG_ERR("click button recovery exhausted or lost its work owner: source=%s error=%d reboot_delay_ms=%u",
            source == NULL ? "unknown" : source,
            error,
            CLICK_BUTTON_RECOVERY_REBOOT_DELAY_MS);
    app_watchdog_stop_feeding();
    LOG_PANIC();
    k_msleep(CLICK_BUTTON_RECOVERY_REBOOT_DELAY_MS);
    sys_reboot(SYS_REBOOT_COLD);
    for (;;) {
        k_cpu_idle();
    }
}

static bool click_button_reschedule_or_reset(
    struct k_work_delayable *work,
    uint32_t delay_ms,
    const char *source)
{
    int ret = k_work_reschedule(work, K_MSEC(delay_ms));

    if (ret < 0) {
        click_button_recovery_reset(source, ret);
        return false;
    }
    return true;
}

static void click_button_schedule_rearm_recovery(int error,
                                                 const char *source)
{
    enum button_wake_recovery_action action;

    (void)gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_DISABLE);
    action = button_wake_recovery_note(
        &click_button_rearm_recovery,
        BUTTON_WAKE_OBSERVATION_FAILURE);
    if (action == BUTTON_WAKE_RECOVERY_RESET) {
        click_button_recovery_reset(source, error);
        return;
    }

    LOG_WRN("click button interrupt recovery scheduled: source=%s error=%d attempt=%u/%u",
            source == NULL ? "unknown" : source,
            error,
            click_button_rearm_recovery.consecutive_failures,
            click_button_rearm_recovery.max_failures);
    (void)click_button_reschedule_or_reset(
        &click_button_rearm_work,
        CLICK_BUTTON_RECOVERY_RETRY_MS,
        source);
}

static void click_button_retry_release_poll(int error)
{
    enum button_wake_recovery_action action =
        button_wake_recovery_note(&click_button_release_recovery,
                                  BUTTON_WAKE_OBSERVATION_FAILURE);

    if (action == BUTTON_WAKE_RECOVERY_RESET) {
        click_button_recovery_reset("release_poll", error);
        return;
    }

    LOG_WRN("click button release poll recovery scheduled: error=%d attempt=%u/%u",
            error,
            click_button_release_recovery.consecutive_failures,
            click_button_release_recovery.max_failures);
    (void)click_button_reschedule_or_reset(
        &click_button_release_work,
        CLICK_BUTTON_RECOVERY_RETRY_MS,
        "release_poll");
}

static void click_button_release_work_handler(struct k_work *work)
{
    int pressed;
    int ret;

    ARG_UNUSED(work);

    pressed = click_button_pressed();
    if (pressed < 0) {
        LOG_ERR("failed to poll click button release: %d", pressed);
        click_button_retry_release_poll(pressed);
        return;
    }
    (void)button_wake_recovery_note(
        &click_button_release_recovery,
        BUTTON_WAKE_OBSERVATION_WAITING);
    if (pressed != 0) {
        (void)click_button_reschedule_or_reset(
            &click_button_release_work,
            CLICK_BUTTON_RELEASE_POLL_MS,
            "release_poll_held");
        return;
    }

    click_button_clear_latch();
    click_button_handle_signal(BUTTON_SIGNAL_RELEASE, "release_poll");
    click_button_finish_press_cycle();
    ret = click_button_arm_idle_interrupt();
    if (ret < 0) {
        LOG_WRN("click button rearm after release poll failed: %d", ret);
        click_button_schedule_rearm_recovery(
            ret, "release_poll_rearm");
    }
}

static void click_button_rearm_work_handler(struct k_work *work)
{
    enum button_wake_recovery_action action;
    uint32_t press_at_ms;
    bool latched;
    int pressed;
    int ret;

    ARG_UNUSED(work);

    if (click_button_press_is_pending()) {
        ret = k_work_submit(&click_button_work);
        if (ret < 0) {
            click_button_schedule_rearm_recovery(
                ret, "rearm_latched_submit");
        }
        return;
    }
    if (click_button_press_cycle_is_active()) {
        return;
    }

    pressed = click_button_pressed();
    if (pressed < 0) {
        click_button_schedule_rearm_recovery(
            pressed, "rearm_read");
        return;
    }
    if (pressed != 0) {
        ret = gpio_pin_interrupt_configure(click_button.port,
                                           click_button.pin,
                                           GPIO_INT_DISABLE);
        if (ret < 0) {
            click_button_schedule_rearm_recovery(
                ret, "rearm_pressed_disable");
            return;
        }
        (void)button_wake_recovery_note(
            &click_button_rearm_recovery,
            BUTTON_WAKE_OBSERVATION_WAITING);
        if (!click_button_claim_press(k_uptime_get_32(),
                                      &press_at_ms,
                                      &latched)) {
            return;
        }
        if (clicker_systemon_retained_idle_enabled() &&
            atomic_get(&clicker_action_active) == 0) {
        }
        click_button_handle_signal_at(
            BUTTON_SIGNAL_PRESS,
            press_at_ms,
            latched ? "rearm_latched" : "rearm_recovery");
        (void)click_button_reschedule_or_reset(
            &click_button_release_work,
            CLICK_BUTTON_RELEASE_STABLE_MS,
            "rearm_pressed_release_poll");
        return;
    }

    ret = click_button_arm_idle_interrupt();
    if (ret < 0) {
        click_button_schedule_rearm_recovery(
            ret, "rearm_interrupt");
        return;
    }
    action = button_wake_recovery_note(
        &click_button_rearm_recovery,
        BUTTON_WAKE_OBSERVATION_ARMED);
    if (action != BUTTON_WAKE_RECOVERY_POWER_READY) {
        click_button_recovery_reset("rearm_policy", -EIO);
    }
}

static void click_button_work_handler(struct k_work *work)
{
    uint32_t press_at_ms;
    bool latched;
    int pressed;
    int ret;

    ARG_UNUSED(work);

process_pending_press:
    if (click_button_press_is_pending()) {
        ret = gpio_pin_interrupt_configure(click_button.port,
                                           click_button.pin,
                                           GPIO_INT_DISABLE);
        if (ret < 0) {
            click_button_schedule_rearm_recovery(
                ret, "irq_latched_disable");
            return;
        }
        if (!click_button_claim_press(k_uptime_get_32(),
                                      &press_at_ms,
                                      &latched)) {
            return;
        }
        if (clicker_systemon_retained_idle_enabled() &&
            atomic_get(&clicker_action_active) == 0) {
        }
        click_button_handle_signal_at(BUTTON_SIGNAL_PRESS,
                                      press_at_ms,
                                      "irq_latched");
        (void)click_button_reschedule_or_reset(
            &click_button_release_work,
            CLICK_BUTTON_RELEASE_STABLE_MS,
            "irq_latched_release_poll");
        return;
    }
    if (click_button_press_cycle_is_active()) {
        return;
    }

    pressed = click_button_pressed();
    if (pressed < 0) {
        LOG_ERR("failed to read click button: %d", pressed);
        click_button_schedule_rearm_recovery(
            pressed, "irq_read");
        return;
    }
    if (pressed != 0) {
        ret = gpio_pin_interrupt_configure(click_button.port,
                                           click_button.pin,
                                           GPIO_INT_DISABLE);
        if (ret < 0) {
            click_button_schedule_rearm_recovery(
                ret, "irq_press_disable");
            return;
        }
        if (!click_button_claim_press(k_uptime_get_32(),
                                      &press_at_ms,
                                      &latched)) {
            return;
        }
        if (clicker_systemon_retained_idle_enabled() &&
            atomic_get(&clicker_action_active) == 0) {
        }
        click_button_handle_signal_at(
            BUTTON_SIGNAL_PRESS,
            press_at_ms,
            latched ? "irq_latched_race" : "irq_sample");
        (void)click_button_reschedule_or_reset(
            &click_button_release_work,
            CLICK_BUTTON_RELEASE_STABLE_MS,
            "irq_press_release_poll");
        return;
    }

    if (click_button_press_is_pending()) {
        goto process_pending_press;
    }
    click_button_handle_signal(BUTTON_SIGNAL_RELEASE, "irq_sample");
    (void)k_work_cancel_delayable(&click_button_release_work);
    ret = click_button_arm_idle_interrupt();
    if (ret < 0) {
        LOG_WRN("click button rearm after release IRQ failed: %d", ret);
        click_button_schedule_rearm_recovery(
            ret, "release_irq_rearm");
    } else if (click_button_press_is_pending()) {
        goto process_pending_press;
    }
}

static void click_button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    int ret;

    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    (void)click_button_latch_press(k_uptime_get_32());
    ret = k_work_submit(&click_button_work);
    if (ret < 0) {
        app_watchdog_stop_feeding();
    }
}

static void self_test_arm_timeout_handler(struct k_work *work)
{
    enum button_action action;

    ARG_UNUSED(work);

    if (click_button_gesture_handle(BUTTON_SIGNAL_TICK,
                                    k_uptime_get_32(),
                                    &action) == PROTO_OK) {
        app_clicker_submit_button_action(action);
    }
}

void app_clicker_arm_self_test_timeout(void)
{
    (void)click_button_reschedule_or_reset(
        &self_test_arm_timeout_work,
        SELF_TEST_ARM_WINDOW_MS + 1u,
        "self_test_arm_timeout");
}

void app_clicker_cancel_self_test_timeout(void)
{
    (void)k_work_cancel_delayable(&self_test_arm_timeout_work);
}

int ML_CLICKER_BUTTON_UNUSED app_clicker_button_init(void)
{
    k_spinlock_key_t edge_key;
    int ret;

    k_work_init(&click_button_work, click_button_work_handler);
    k_work_init_delayable(&click_button_release_work,
                          click_button_release_work_handler);
    k_work_init_delayable(&click_button_rearm_work,
                          click_button_rearm_work_handler);
    k_work_init(&clicker_action_work, clicker_action_work_handler);
    k_work_init_delayable(&clicker_action_submit_retry_work,
                          clicker_action_submit_retry_work_handler);
    k_work_init_delayable(&self_test_arm_timeout_work,
                          self_test_arm_timeout_handler);
    button_action_handoff_init(&clicker_action_handoff);
    clicker_action_watchdog_generation = 0u;
    atomic_set(&clicker_action_active, 0);
    edge_key = k_spin_lock(&click_button_edge_lock);
    click_button_press_pending = false;
    click_button_press_cycle_active = false;
    click_button_press_at_ms = 0u;
    k_spin_unlock(&click_button_edge_lock, edge_key);
    button_wake_recovery_init(&clicker_action_submit_recovery,
                              CLICK_BUTTON_RECOVERY_MAX_FAILURES);
    button_wake_recovery_init(&click_button_release_recovery,
                              CLICK_BUTTON_RECOVERY_MAX_FAILURES);
    button_wake_recovery_init(&click_button_rearm_recovery,
                              CLICK_BUTTON_RECOVERY_MAX_FAILURES);
    if (!click_button_callback_initialized) {
        gpio_init_callback(&click_button_cb,
                           click_button_isr,
                           BIT(click_button.pin));
        click_button_callback_initialized = true;
    }

    ret = click_button_arm_idle_interrupt();
    if (ret < 0) {
        click_button_schedule_rearm_recovery(
            ret, "button_init");
        return 0;
    }

    ret = k_work_submit(&click_button_work);
    if (ret < 0) {
        click_button_schedule_rearm_recovery(
            ret, "button_init_close_race");
    }
    return 0;
}

void app_clicker_prepare_startup_idle(enum button_action *boot_action)
{
    if (boot_action == NULL) {
        return;
    }

    if (DEVICE_ROLE == ROLE_CLICKER &&
        IS_ENABLED(CONFIG_IMEC_CLICKER_SYSTEMOFF_IDLE) &&
        !clicker_systemon_retained_idle_enabled()) {
        if (clicker_reset_reason_was_systemoff()) {
            if (!clicker_capture_systemoff_button_action(boot_action)) {
                LOG_WRN("system-off wake button capture failed; returning to idle");
                app_clicker_enter_systemoff_idle();
            }
        } else {
            app_clicker_enter_systemoff_idle();
        }
    }
}
#else
void app_clicker_enter_systemoff_idle(void)
{
}

void app_clicker_enter_idle(void)
{
}

int app_clicker_button_init(void)
{
    return -ENODEV;
}

void app_clicker_arm_self_test_timeout(void)
{
}

void app_clicker_cancel_self_test_timeout(void)
{
}

void app_clicker_prepare_startup_idle(enum button_action *boot_action)
{
    ARG_UNUSED(boot_action);
}
#endif

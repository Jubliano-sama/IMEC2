#include "app_clicker.h"

#include "app_board.h"
#include "app_config.h"
#include "app_high_debug.h"
#include "app_state.h"
#include "dwm3000_driver.h"
#include "dwm3000_port.h"
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
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_clicker, LOG_LEVEL_DBG);

#define CLICKER_POLITENESS_UWB_RESTART 1

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

static const struct app_clicker_range_tx_config clicker_range_tx_config = {
    .control_tx_timeout_ms = UWB_CONTROL_TX_TIMEOUT_MS,
    .prepare_range_mode_after_schedule = false,
};

static struct app_clicker_callbacks clicker_callbacks;

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

static uint32_t clicker_sleep_until_ble_or_timeout(
    uint32_t sleep_ms,
    int64_t deadline_ms,
    const struct app_clicker_attempt_gate_config *config)
{
    uint32_t remaining_ms = sleep_ms;

    while (remaining_ms > 0u && k_uptime_get() < deadline_ms) {
        uint32_t step_ms = MIN(remaining_ms, config->ble_courtesy_poll_sleep_ms);
        int64_t deadline_remaining_ms = deadline_ms - k_uptime_get();
        uint32_t wait_ms = app_clicker_ble_courtesy_higher_wait_ms();

        if (wait_ms > 0u) {
            return wait_ms;
        }
        if (deadline_remaining_ms <= 0) {
            break;
        }
        step_ms = MIN(step_ms, (uint32_t)deadline_remaining_ms);
        if (step_ms == 0u) {
            break;
        }
        k_msleep(step_ms);
        remaining_ms -= step_ms;
    }
    return app_clicker_ble_courtesy_higher_wait_ms();
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
    bool relevant_activity_detected = false;
    int ret;

    ret = dwm3000_driver_receive_frame(listen_ms,
                                       frame,
                                       sizeof(frame),
                                       &frame_len,
                                       NULL,
                                       NULL);
    if (ret == 0) {
        int decode_ret = uwb_clicker_decode_politeness_wait(
            session,
            frame,
            frame_len,
            config->polite_relevant_frame_wait_ms,
            &relevant_wait_ms,
            &frame_type);

        relevant_activity_detected = decode_ret == PROTO_OK && relevant_wait_ms > 0u;
        if (relevant_activity_detected) {
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
            LOG_DBG("clicker ignored irrelevant UWB gate packet: type=0x%02x frame_len=%u",
                    frame_type,
                    (unsigned int)frame_len);
        }
    } else if (ret != -ETIMEDOUT) {
        LOG_DBG("clicker UWB gate receive sample failed: ret=%d", ret);
        ret = 0;
    } else {
        ret = 0;
    }
    (void)dwm3000_driver_standby();

    if (sample_count != NULL) {
        (*sample_count)++;
    }
    uwb_clicker_note_politeness_sample(session, relevant_activity_detected);

    if (relevant_activity_detected) {
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
                                    uint32_t *uwb_restart_wait_ms,
                                    uint32_t *ble_defer_wait_ms,
                                    const struct app_clicker_attempt_gate_config *config)
{
    int64_t deadline_ms = k_uptime_get() + config->max_politeness_wait_ms;
    uint8_t quiet_samples = 0u;
    uint32_t sample_count = 0u;
    uint32_t activity_count = 0u;
    bool ble_started = false;
    int64_t ble_courtesy_until_ms = 0;
    int ret;

    if (session == NULL || config == NULL) {
        return -EINVAL;
    }
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
        radio_guard_uwb_stop();
        if (ble_started) {
            app_clicker_ble_courtesy_stop();
        }
        return ret;
    }

    if (ble_started) {
        int64_t remaining_ms = deadline_ms - k_uptime_get();
        uint32_t peer_wait_ms = 0u;

        if (remaining_ms > 0) {
            ret = clicker_sample_uwb_gate(session,
                                         MIN(config->polite_sample_rx_ms,
                                             (uint32_t)remaining_ms),
                                         uwb_restart_wait_ms,
                                         &sample_count,
                                         &activity_count,
                                         &quiet_samples,
                                         config);
        }
        if (ret == 0) {
            peer_wait_ms = app_clicker_ble_courtesy_higher_wait_ms();
        }
        if (peer_wait_ms > 0u) {
            if (ble_defer_wait_ms != NULL) {
                *ble_defer_wait_ms = peer_wait_ms;
            }
            ret = -EAGAIN;
        }
        if (ret == 0 && k_uptime_get() < ble_courtesy_until_ms) {
            int64_t courtesy_remaining_ms = ble_courtesy_until_ms - k_uptime_get();
            int64_t deadline_remaining_ms = deadline_ms - k_uptime_get();

            if (courtesy_remaining_ms > 0 && deadline_remaining_ms > 0 &&
                (peer_wait_ms = clicker_sleep_until_ble_or_timeout(
                    (uint32_t)MIN(courtesy_remaining_ms, deadline_remaining_ms),
                    deadline_ms,
                    config)) > 0u) {
                if (ble_defer_wait_ms != NULL) {
                    *ble_defer_wait_ms = peer_wait_ms;
                }
                ret = -EAGAIN;
            }
        }
        if (ret == 0 && k_uptime_get() < deadline_ms) {
            remaining_ms = deadline_ms - k_uptime_get();
            ret = clicker_sample_uwb_gate(session,
                                         MIN(config->polite_sample_rx_ms,
                                             (uint32_t)remaining_ms),
                                         uwb_restart_wait_ms,
                                         &sample_count,
                                         &activity_count,
                                         &quiet_samples,
                                         config);
        }
    } else {
        while (quiet_samples < config->polite_required_quiet_samples &&
               k_uptime_get() < deadline_ms) {
            int64_t sample_start_ms = k_uptime_get();
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

            if (quiet_samples < config->polite_required_quiet_samples &&
                k_uptime_get() < deadline_ms) {
                int64_t elapsed_ms = k_uptime_get() - sample_start_ms;
                int64_t sleep_ms = (int64_t)config->polite_sample_period_ms - elapsed_ms;
                int64_t remaining_after_sample_ms = deadline_ms - k_uptime_get();

                if (sleep_ms > 0 && remaining_after_sample_ms > 0) {
                    k_msleep((uint32_t)MIN(sleep_ms, remaining_after_sample_ms));
                }
            }
        }
    }

    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();
    if (ble_started) {
        app_clicker_ble_courtesy_stop();
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

static uint16_t clicker_claimed_duration_ms(
    uint16_t wake_train_ends_in_ms,
    const struct app_clicker_wake_train_config *config)
{
    uint32_t claimed_ms = (uint32_t)wake_train_ends_in_ms +
                          config->post_wake_claimed_duration_ms;

    return claimed_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)claimed_ms;
}

int app_clicker_send_wake_claim_train(struct uwb_clicker_session *session,
                                      uint64_t priority_id,
                                      const struct app_clicker_wake_train_config *config)
{
    uint8_t frame[UWB_WAKE_CLAIM_LEN];
    size_t frame_len = 0u;
    int64_t close_ms;
    uint16_t sent_count = 0u;
    int ret;

    if (session == NULL || config == NULL) {
        return -EINVAL;
    }

    ret = radio_guard_uwb_start("clicker UWB WAKE_CLAIM train");
    if (ret < 0) {
        status_debug_note("DBG_WAKE_TRAIN_GUARD_FAIL\n");
        LOG_WRN("clicker UWB WAKE_CLAIM guard failed: ret=%d", ret);
        return ret;
    }
    status_debug_note("DBG_WAKE_TRAIN_GUARD_OK\n");
    stage1_led_phase(STAGE1_LED_PHASE_WAKE);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);

    status_debug_note("DBG_WAKE_TRAIN_CONFIG_BEGIN\n");
    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        status_debug_note("DBG_WAKE_TRAIN_CONFIG_FAIL\n");
        LOG_WRN("clicker UWB WAKE_CLAIM wake-mode config failed: ret=%d",
                ret);
        goto out;
    }
    status_debug_note("DBG_WAKE_TRAIN_CONFIG_OK\n");

    high_debug_log_event("WAKE_CLAIM_TX",
                         "phase=start event_seq=%u attempt=%u duration_ms=%u",
                         session->config.click_event_id,
                         session->attempt_index,
                         config->wake_adv_ms);
    close_ms = k_uptime_get() + config->wake_adv_ms;
    while (k_uptime_get() < close_ms) {
        struct uwb_wake_claim_frame claim;
        int64_t remaining_ms = close_ms - k_uptime_get();
        uint16_t remaining_u16 = delay_ms_to_u16(remaining_ms);

        ret = uwb_clicker_build_wake_claim(session,
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
        HIGH_DEBUG_COUNTER_INC(wake_claim_tx);
        uwb_clicker_note_wake_claim_tx(session, 1u);

        if (k_uptime_get() < close_ms) {
            uint32_t jitter_us = uwb_clicker_wake_claim_jitter_us(sys_rand32_get());
            int64_t remaining_after_tx_ms = close_ms - k_uptime_get();

            if (jitter_us > 0u && remaining_after_tx_ms > 0) {
                k_busy_wait(jitter_us);
            }
        }
    }

out:
    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();
    if (ret < 0) {
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        LOG_WRN("clicker UWB WAKE_CLAIM train failed: sent=%u ret=%d",
                sent_count,
                ret);
        return ret;
    }

    stage1_led_result(sent_count == 0u ?
                      STAGE1_LED_RESULT_TIMEOUT :
                      STAGE1_LED_RESULT_OK);
    high_debug_log_event("WAKE_CLAIM_TX",
                         "phase=done event_seq=%u attempt=%u sent=%u duration_ms=%u",
                         session->config.click_event_id,
                         session->attempt_index,
                         sent_count,
                         config->wake_adv_ms);
    LOG_INF("clicker UWB WAKE_CLAIM train complete: sent=%u duration_ms=%u",
            sent_count,
            config->wake_adv_ms);
    return sent_count == 0u ? -ETIMEDOUT : 0;
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

int app_clicker_send_range_schedule(const struct uwb_range_schedule_frame *schedule,
                                    const struct app_clicker_range_tx_config *config)
{
    uint8_t frame[UWB_RANGE_SCHEDULE_MAX_LEN];
    size_t frame_len = 0u;
    int ret;

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
    stage1_led_phase(STAGE1_LED_PHASE_SCHEDULE);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        ret = dwm3000_driver_send_frame(frame,
                                        frame_len,
                                        config->control_tx_timeout_ms);
    }
    if (ret == 0 && config->prepare_range_mode_after_schedule) {
        int prep_ret = dwm3000_driver_configure_range_mode();

        if (prep_ret < 0) {
            LOG_WRN("ML clicker range-mode prep after RANGE_SCHEDULE failed: %d",
                    prep_ret);
            ret = prep_ret;
        }
        (void)dwm3000_driver_idle();
    } else {
        (void)dwm3000_driver_standby();
    }
    radio_guard_uwb_stop();

    if (ret < 0) {
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        LOG_WRN("clicker UWB RANGE_SCHEDULE TX failed: ret=%d", ret);
        return ret;
    }
    HIGH_DEBUG_COUNTER_INC(schedules_tx);
    stage1_led_result(STAGE1_LED_RESULT_OK);
    high_debug_log_event("RANGE_SCHEDULE_TX",
                         "clicker=0x%016llx event_seq=%u attempt=%u selected=%u samples_per_anchor=%u reply_delay_uus=%u first_poll_ms=%u stride_us=%u burst_ms=%u BENCH_ONLY=%u",
                         (unsigned long long)schedule->clicker_id,
                         schedule->click_event_id,
                         schedule->attempt_index,
                         schedule->selected_count,
                         schedule->samples_per_anchor,
                         schedule->reply_delay_us,
                         schedule->first_poll_delay_ms,
                         schedule->exchange_stride_us,
                         schedule->burst_window_ms,
                         (IMEC_STAGE == 1 &&
                          IS_ENABLED(CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE)) ? 1u : 0u);
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
    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();

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

void app_clicker_run_continuous_wake_claims(
    const struct app_clicker_continuous_wake_claims_config *config)
{
    uint32_t train_count = 0u;

    if (config == NULL) {
        return;
    }

    while (true) {
        uint32_t event_seq = next_click_event_seq();
        uint64_t priority_id = clicker_priority_id(event_seq, 1u);
        struct uwb_clicker_session session;
        struct uwb_clicker_config clicker_config = {
            .network_id = NETWORK_ID,
            .clicker_id = DEVICE_ID,
            .click_event_id = event_seq,
            .nonce = clicker_nonce(event_seq),
            .min_anchor_count = config->min_anchor_count,
            .max_anchor_count = config->max_anchor_count,
            .max_attempts = config->max_attempts,
            .samples_per_anchor = config->samples_per_anchor,
            .wake_channel = config->wake_channel,
            .ranging_channel = config->ranging_channel,
            .flags = config->flags,
        };
        int ret;

        ret = uwb_clicker_session_start(&session, &clicker_config);
        if (ret != PROTO_OK) {
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("continuous WAKE_CLAIM session start failed: event_seq=%u proto_ret=%d",
                    event_seq,
                    ret);
            k_msleep(250u);
            continue;
        }

        train_count++;
        high_debug_log_event("WAKE_SPAM_START",
                             "train=%u event_seq=%u priority=0x%016llx wake_ms=%u",
                             train_count,
                             event_seq,
                             (unsigned long long)priority_id,
                             config->wake_adv_ms);
        LOG_INF("continuous WAKE_CLAIM train start: train=%u event_seq=%u",
                train_count,
                event_seq);

        ret = app_clicker_send_wake_claim_train(&session,
                                                priority_id,
                                                &config->wake_train);
        high_debug_log_event("WAKE_SPAM_DONE",
                             "train=%u event_seq=%u ret=%d tx_count=%u",
                             train_count,
                             event_seq,
                             ret,
                             session.diagnostics.wake_claim_tx_count);
        LOG_INF("continuous WAKE_CLAIM train done: train=%u event_seq=%u ret=%d tx_count=%u",
                train_count,
                event_seq,
                ret,
                session.diagnostics.wake_claim_tx_count);

        k_msleep(ret < 0 ? 250u : 20u);
    }
}

int app_clicker_discover_uwb_anchors(struct uwb_clicker_session *session)
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

    ret = radio_guard_uwb_start("clicker UWB DISCOVER");
    if (ret < 0) {
        return ret;
    }
    stage1_led_phase(STAGE1_LED_PHASE_DISCOVERY);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        goto out;
    }

    ret = dwm3000_driver_send_frame(frame, frame_len, UWB_CONTROL_TX_TIMEOUT_MS);
    if (ret < 0) {
        goto out;
    }
    HIGH_DEBUG_COUNTER_INC(discovery_tx);
    high_debug_log_event("DISCOVER_TX",
                         "event_seq=%u attempt=%u window_ms=%u",
                         session->config.click_event_id,
                         session->attempt_index,
                         reply_window_ms);

    deadline_ms = k_uptime_get() + reply_window_ms;
    while (k_uptime_get() < deadline_ms) {
        struct uwb_discovery_reply_frame reply;
        uint8_t quality = 0u;
        int64_t remaining_ms = deadline_ms - k_uptime_get();

        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      frame,
                                                      sizeof(frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      NULL);
        if (ret == -ETIMEDOUT) {
            break;
        }
        if (ret < 0) {
            last_ret = ret;
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
            HIGH_DEBUG_COUNTER_INC(discovery_reply_rx);
            stage1_led_result(STAGE1_LED_RESULT_OK);
            high_debug_log_event("DISCOVERY_REPLY_RX",
                                 "anchor=0x%016llx slot=%u quality=%u candidates=%u",
                                 (unsigned long long)reply.anchor_id,
                                 reply.anchor_slot,
                                 reply.rx_quality,
                                 session->candidate_count);
            LOG_INF("clicker UWB discovery reply: anchor=0x%016llx slot=%u quality=%u status=%u candidates=%u",
                    (unsigned long long)reply.anchor_id,
                    reply.anchor_slot,
                    reply.rx_quality,
                    reply.status,
                    session->candidate_count);
        } else {
            rejected_replies++;
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            high_debug_log_event("DISCOVERY_REPLY_RX",
                                 "anchor=0x%016llx rejected_reason=proto_%d selected_clicker=0x%016llx event_seq=%u attempt=%u",
                                 (unsigned long long)reply.anchor_id,
                                 ret,
                                 (unsigned long long)reply.selected_clicker_id,
                                 reply.click_event_id,
                                 reply.attempt_index);
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
    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();
    if (ret < 0) {
        stage1_led_result(ret == -ETIMEDOUT ?
                          STAGE1_LED_RESULT_TIMEOUT :
                          STAGE1_LED_RESULT_ERROR);
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
    struct app_clicker_range_tx_config range_tx_config = clicker_range_tx_config;
    int ret;
    bool used_cached_discovery = false;

    ret = app_clicker_send_wake_claim_train(session,
                                            priority_id,
                                            &clicker_wake_train_config);
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
        ret = app_clicker_discover_uwb_anchors(session);
        if (ret < 0) {
            return ret;
        }
    }

    if ((session->config.flags & FLAG_COUNT_AS_CLICK) != 0u &&
        session->candidate_count > 0u &&
        session->candidate_count < session->config.min_anchor_count) {
        int release_ret = app_clicker_send_range_release(
            session,
            UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS,
            &clicker_range_tx_config);

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
            return ret;
        }
    }
    session->schedule = *schedule;
    if (clicker_callbacks.ml_runtime_active != NULL) {
        range_tx_config.prepare_range_mode_after_schedule =
            clicker_callbacks.ml_runtime_active();
    }
#endif

    ret = app_clicker_send_range_schedule(schedule, &range_tx_config);
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

int app_clicker_collect_uwb_attempt(struct uwb_clicker_session *session,
                                    uint64_t priority_id,
                                    struct uwb_range_schedule_frame *schedule)
{
    return app_clicker_collect_uwb_attempt_with_options(session,
                                                       priority_id,
                                                       schedule,
                                                       false,
                                                       true);
}

static void clicker_release_scheduled_range_radio(void)
{
    int ret = dwm3000_driver_idle();

    if (ret < 0) {
        LOG_WRN("clicker DW3000 idle after scheduled sample failed: %d", ret);
    }
}

static void clicker_finish_scheduled_range_radio_burst(void)
{
    int ret = dwm3000_driver_standby();

    if (ret < 0) {
        LOG_WRN("clicker DW3000 standby after scheduled burst failed: %d", ret);
    }
}

int app_clicker_range_scheduled_anchors(struct uwb_clicker_session *session,
                                        const struct uwb_range_schedule_frame *schedule,
                                        int64_t click_deadline_ms,
                                        uint8_t *attempted_count)
{
    int64_t schedule_tx_ms = k_uptime_get();
    size_t total_samples = uwb_range_schedule_total_samples(schedule);
    int last_ret = -ETIMEDOUT;

    while (session->state == UWB_CLICKER_RANGING) {
        struct uwb_range_step step;
        struct dwm3000_range_request range_request;
        struct dwm3000_range_result range_result;
        int64_t target_us;
        int64_t remaining_ms;
        uint32_t slot_timeout_ms = CLICK_UWB_TIMEOUT_MS;
        int ret;

        ret = uwb_clicker_next_range_step(session, &step);
        if (ret == PROTO_ERR_NOT_FOUND) {
            break;
        }
        if (ret != PROTO_OK) {
            clicker_finish_scheduled_range_radio_burst();
            return -EINVAL;
        }

        target_us = scheduled_range_sample_target_us(schedule_tx_ms, schedule, step.sample_index);
        sleep_until_us(target_us);
        stage1_led_phase(STAGE1_LED_PHASE_RANGE);
        stage1_led_result(STAGE1_LED_RESULT_ACTIVE);

        remaining_ms = click_deadline_ms - k_uptime_get();
        if (remaining_ms <= CLICK_REPORT_BUILD_GUARD_MS) {
            (void)uwb_clicker_record_range_result(session, &step, RANGE_RX_TIMEOUT);
            (void)uwb_clicker_abort_attempt(session);
            stage1_led_result(STAGE1_LED_RESULT_TIMEOUT);
            LOG_WRN("scheduled click DS-TWR not started: reason=click_budget anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u remaining_ms=%lld guard_ms=%u attempt=%u ds_fail=%u",
                    (unsigned long long)step.anchor_id,
                    step.anchor_index,
                    (unsigned int)(step.sample_index + 1u),
                    (unsigned int)total_samples,
                    step.round_index,
                    step.seq,
                    (long long)remaining_ms,
                    CLICK_REPORT_BUILD_GUARD_MS,
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
        range_request.skip_responder_report = false;
        range_request.send_clicker_diag = false;
        range_request.expect_anchor_diag = false;
        range_request.capture_rsl = false;
        slot_timeout_ms = MIN(slot_timeout_ms,
                              (uint32_t)ceil_us_to_ms(schedule->exchange_stride_us) +
                              UWB_SCHEDULE_GUARD_MS);
        range_request.timeout_ms = MIN(slot_timeout_ms,
                                       (uint32_t)(remaining_ms - CLICK_REPORT_BUILD_GUARD_MS));

        ret = radio_guard_uwb_start("clicker scheduled UWB range");
        if (ret < 0) {
            (void)uwb_clicker_record_range_result(session, &step, RANGE_INTERNAL_ERROR);
            (void)uwb_clicker_abort_attempt(session);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("scheduled click DS-TWR not started: reason=radio_guard anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u ret=%d attempt=%u ds_fail=%u",
                    (unsigned long long)step.anchor_id,
                    step.anchor_index,
                    (unsigned int)(step.sample_index + 1u),
                    (unsigned int)total_samples,
                    step.round_index,
                    step.seq,
                    ret,
                    session->attempt_index,
                    session->diagnostics.ds_twr_failures);
            clicker_finish_scheduled_range_radio_burst();
            return ret;
        }
        LOG_INF("scheduled click DS-TWR start: anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u timeout_ms=%u",
                (unsigned long long)step.anchor_id,
                step.anchor_index,
                (unsigned int)(step.sample_index + 1u),
                (unsigned int)total_samples,
                step.round_index,
                step.seq,
                range_request.timeout_ms);
        HIGH_DEBUG_COUNTER_INC(ds_twr_attempts);
        high_debug_log_event("DS_TWR_POLL_TX",
                             "anchor=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u schedule_base_ms=%lld target_us=%lld now_ms=%lld late_us=%lld first_poll_ms=%u stride_us=%u timeout_ms=%u",
                             (unsigned long long)step.anchor_id,
                             session->config.click_event_id,
                             session->attempt_index,
                             uwb_schedule_burst_id(session->config.click_event_id,
                                                   session->attempt_index),
                             (unsigned int)(step.sample_index + 1u),
                             (unsigned int)total_samples,
                             step.round_index,
                             step.seq,
                             (long long)schedule_tx_ms,
                             (long long)target_us,
                             (long long)k_uptime_get(),
                             (long long)((k_uptime_get() * 1000) - target_us),
                             schedule->first_poll_delay_ms,
                             schedule->exchange_stride_us,
                             range_request.timeout_ms);
#if defined(CONFIG_IMEC_ML_CLICKER)
        if (clicker_callbacks.ml_enter_range_quiet != NULL) {
            clicker_callbacks.ml_enter_range_quiet();
        }
#endif
        ret = dwm3000_driver_range_initiator(&range_request, &range_result);
        clicker_release_scheduled_range_radio();
        radio_guard_uwb_stop();
#if defined(CONFIG_IMEC_ML_CLICKER)
        if (clicker_callbacks.ml_exit_range_quiet != NULL) {
            clicker_callbacks.ml_exit_range_quiet();
        }
#endif

        if (!range_result.exchange_started) {
            enum range_status status = range_result.status;

            if (status == RANGE_OK || !range_status_valid(status)) {
                status = RANGE_RX_TIMEOUT;
            }
            HIGH_DEBUG_COUNTER_INC(ds_twr_failures);
            (void)uwb_clicker_record_range_result(session, &step, status);
            stage1_led_result(STAGE1_LED_RESULT_TIMEOUT);
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
            high_debug_log_event("RANGE_FAIL",
                                 "anchor=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u ret=%d reason=%s exchange_started=0",
                                 (unsigned long long)step.anchor_id,
                                 session->config.click_event_id,
                                 session->attempt_index,
                                 uwb_schedule_burst_id(session->config.click_event_id,
                                                       session->attempt_index),
                                 (unsigned int)(step.sample_index + 1u),
                                 (unsigned int)total_samples,
                                 step.round_index,
                                 step.seq,
                                 ret,
                                 range_status_name(status));
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

            HIGH_DEBUG_COUNTER_INC(ds_twr_successes);
            record_ret = uwb_clicker_record_range_result(session, &step, RANGE_OK);
            if (record_ret != PROTO_OK) {
                LOG_ERR("scheduled click DS-TWR state update failed: anchor=0x%016llx seq=%u ret=%d",
                        (unsigned long long)step.anchor_id,
                        step.seq,
                        record_ret);
                clicker_finish_scheduled_range_radio_burst();
                return -EINVAL;
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
            stage1_led_result(STAGE1_LED_RESULT_OK);
            high_debug_log_event("RANGE_OK",
                                 "anchor=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u distance_mm=%d quality=%u rsl_dbm=%d rsl_present=%u",
                                 (unsigned long long)range_result.responder_id,
                                 range_result.session_id,
                                 session->attempt_index,
                                 uwb_schedule_burst_id(session->config.click_event_id,
                                                       session->attempt_index),
                                 (unsigned int)(step.sample_index + 1u),
                                 (unsigned int)total_samples,
                                 step.round_index,
                                 range_result.seq,
                                 range_result.distance_mm,
                                 range_result.quality,
                                 range_result.rsl_dbm,
                                 range_result.rsl_sampled ? 1u : 0u);
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
            HIGH_DEBUG_COUNTER_INC(ds_twr_failures);
            if (status == RANGE_TIMING_INVALID) {
                HIGH_DEBUG_COUNTER_INC(ds_twr_timing_rejects);
            }
            stage1_led_result(status == RANGE_RX_TIMEOUT ?
                              STAGE1_LED_RESULT_TIMEOUT :
                              STAGE1_LED_RESULT_ERROR);
            record_ret = uwb_clicker_record_range_result(session, &step, status);
            if (record_ret != PROTO_OK) {
                LOG_ERR("scheduled click DS-TWR failure state update failed: anchor=0x%016llx seq=%u ret=%d status=%s(%u)",
                        (unsigned long long)step.anchor_id,
                        step.seq,
                        record_ret,
                        range_status_name(status),
                        status);
                clicker_finish_scheduled_range_radio_burst();
                return -EINVAL;
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
            high_debug_log_event(status == RANGE_TIMING_INVALID ?
                                 "RANGE_TIMING_REJECT" : "RANGE_FAIL",
                                 "anchor=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u ret=%d reason=%s status=%u",
                                 (unsigned long long)step.anchor_id,
                                 session->config.click_event_id,
                                 session->attempt_index,
                                 uwb_schedule_burst_id(session->config.click_event_id,
                                                       session->attempt_index),
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
    clicker_finish_scheduled_range_radio_burst();
    return session->state == UWB_CLICKER_SUCCEEDED ? 0 : last_ret;
}

int app_clicker_run_normal_click(void)
{
    uint32_t event_seq = next_click_event_seq();
    uint8_t attempted_count = 0u;
    uint16_t total_candidate_count = 0u;
    int64_t click_deadline_ms;
    struct uwb_clicker_session session;
    struct uwb_clicker_config config = {
        .network_id = NETWORK_ID,
        .clicker_id = DEVICE_ID,
        .click_event_id = event_seq,
        .nonce = clicker_nonce(event_seq),
        .min_anchor_count = app_clicker_debug_min_anchor_count(),
        .max_anchor_count = app_clicker_debug_max_anchor_count(),
        .max_attempts = MAX_WAKE_ATTEMPTS,
        .samples_per_anchor = app_clicker_debug_samples_per_anchor(),
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
        .flags = app_clicker_debug_session_flags(),
    };
    int last_ret = -ETIMEDOUT;
    int ret;

    BUILD_ASSERT(UWB_NORMAL_CLICK_MIN_ANCHORS <= MAX_SUCCESSFUL_ANCHORS,
                 "successful anchor result storage must cover the success threshold");

    high_debug_log_event("CLICK_TRACE",
                         "point=enter event_seq=%u clicker=0x%016llx nonce=0x%016llx min=%u max=%u attempts=%u samples=%u flags=0x%02x",
                         event_seq,
                         (unsigned long long)config.clicker_id,
                         (unsigned long long)config.nonce,
                         config.min_anchor_count,
                         config.max_anchor_count,
                         config.max_attempts,
                         config.samples_per_anchor,
                         config.flags);
    stage1_click_diag("enter event_seq=%u clicker=0x%016llx nonce=0x%016llx min=%u max=%u attempts=%u samples=%u flags=0x%02x",
                      event_seq,
                      (unsigned long long)config.clicker_id,
                      (unsigned long long)config.nonce,
                      config.min_anchor_count,
                      config.max_anchor_count,
                      config.max_attempts,
                      config.samples_per_anchor,
                      config.flags);
    stage1_led_phase(STAGE1_LED_PHASE_WAKE);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);

    ret = uwb_clicker_session_start(&session, &config);
    if (ret != PROTO_OK) {
        high_debug_log_event("CLICK_TRACE",
                             "point=session_start_failed event_seq=%u proto_ret=%d",
                             event_seq,
                             ret);
        stage1_click_diag("session_start_failed event_seq=%u proto_ret=%d",
                          event_seq,
                          ret);
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        return -EINVAL;
    }
    high_debug_log_event("CLICK_TRACE",
                         "point=session_started event_seq=%u state=%u attempt=%u",
                         event_seq,
                         session.state,
                         session.attempt_index);
    stage1_click_diag("session_started event_seq=%u state=%u attempt=%u",
                      event_seq,
                      session.state,
                      session.attempt_index);

    LOG_INF("normal click started on UWB wake path: event_seq=%u wake_ms=%u max_attempts=%u min_unique_anchors=%u samples_per_anchor=%u",
            event_seq,
            WAKE_ADV_MS,
            MAX_WAKE_ATTEMPTS,
            config.min_anchor_count,
            config.samples_per_anchor);
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (CONFIG_IMEC_BENCH_STAGE == 1 &&
        IS_ENABLED(CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE)) {
        high_debug_log_event("BENCH_ONLY",
                             "stage=1 one_anchor_schedule=enabled production_min_unique_anchors=%u",
                             UWB_NORMAL_CLICK_MIN_ANCHORS);
    }
#endif

    click_deadline_ms = k_uptime_get() + CLICK_REPORT_DEADLINE_MS;

    while (session.attempt_index <= session.config.max_attempts) {
        struct uwb_range_schedule_frame schedule;
        uint64_t priority_id = clicker_priority_id(event_seq, session.attempt_index);

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
            high_debug_log_event("CLICK_TRACE",
                                 "point=attempt_gate_failed event_seq=%u attempt=%u ret=%d",
                                 event_seq,
                                 session.attempt_index,
                                 ret);
            stage1_click_diag("attempt_gate_failed event_seq=%u attempt=%u ret=%d",
                              event_seq,
                              session.attempt_index,
                              ret);
            stage1_led_result(ret == -ETIMEDOUT ?
                              STAGE1_LED_RESULT_TIMEOUT :
                              STAGE1_LED_RESULT_ERROR);
            return ret;
        }

        if (k_uptime_get() + WAKE_ADV_MS >= click_deadline_ms) {
            high_debug_log_event("CLICK_TRACE",
                                 "point=deadline_before_wake event_seq=%u attempt=%u now_ms=%lld deadline_ms=%lld wake_ms=%u",
                                 event_seq,
                                 session.attempt_index,
                                 (long long)k_uptime_get(),
                                 (long long)click_deadline_ms,
                                 WAKE_ADV_MS);
            stage1_click_diag("deadline_before_wake event_seq=%u attempt=%u now_ms=%lld deadline_ms=%lld wake_ms=%u",
                              event_seq,
                              session.attempt_index,
                              (long long)k_uptime_get(),
                              (long long)click_deadline_ms,
                              WAKE_ADV_MS);
            break;
        }

        high_debug_log_event("CLICK_TRACE",
                             "point=attempt_collect_begin event_seq=%u attempt=%u priority=0x%016llx",
                             event_seq,
                             session.attempt_index,
                             (unsigned long long)priority_id);
        stage1_click_diag("attempt_collect_begin event_seq=%u attempt=%u priority=0x%016llx",
                          event_seq,
                          session.attempt_index,
                          (unsigned long long)priority_id);
        ret = app_clicker_collect_uwb_attempt(&session, priority_id, &schedule);
        if (ret < 0) {
            last_ret = ret;
            LOG_WRN("normal click UWB attempt found no scheduled anchors: event_seq=%u attempt=%u ret=%d",
                    event_seq,
                    session.attempt_index,
                    ret);
            high_debug_log_event("CLICK_TRACE",
                                 "point=attempt_collect_failed event_seq=%u attempt=%u ret=%d state=%u unique=%u/%u",
                                 event_seq,
                                 session.attempt_index,
                                 ret,
                                 session.state,
                                 session.successful_unique_count,
                                 session.config.min_anchor_count);
            stage1_click_diag("attempt_collect_failed event_seq=%u attempt=%u ret=%d state=%u unique=%u/%u",
                              event_seq,
                              session.attempt_index,
                              ret,
                              session.state,
                              session.successful_unique_count,
                              session.config.min_anchor_count);
        } else {
            total_candidate_count += schedule.selected_count;
            LOG_INF("normal click UWB attempt scheduled anchors: event_seq=%u attempt=%u selected=%u unique_success=%u/%u",
                    event_seq,
                    session.attempt_index,
                    schedule.selected_count,
                    session.successful_unique_count,
                    session.config.min_anchor_count);

            ret = app_clicker_range_scheduled_anchors(&session,
                                                      &schedule,
                                                      click_deadline_ms,
                                                      &attempted_count);
            if (ret == 0 && session.state == UWB_CLICKER_SUCCEEDED) {
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
                high_debug_log_event("CLICK_TRACE",
                                     "point=success_after_range event_seq=%u attempt=%u selected=%u attempted=%u unique=%u/%u",
                                     event_seq,
                                     session.attempt_index,
                                     schedule.selected_count,
                                     attempted_count,
                                     session.successful_unique_count,
                                     session.config.min_anchor_count);
                stage1_click_diag("success_after_range event_seq=%u attempt=%u selected=%u attempted=%u unique=%u/%u",
                                  event_seq,
                                  session.attempt_index,
                                  schedule.selected_count,
                                  attempted_count,
                                  session.successful_unique_count,
                                  session.config.min_anchor_count);
                stage1_led_result(STAGE1_LED_RESULT_OK);
                return 0;
            }
            if (ret < 0) {
                last_ret = ret;
                high_debug_log_event("CLICK_TRACE",
                                     "point=range_failed event_seq=%u attempt=%u ret=%d state=%u attempted=%u unique=%u/%u",
                                     event_seq,
                                     session.attempt_index,
                                     ret,
                                     session.state,
                                     attempted_count,
                                     session.successful_unique_count,
                                     session.config.min_anchor_count);
                stage1_click_diag("range_failed event_seq=%u attempt=%u ret=%d state=%u attempted=%u unique=%u/%u",
                                  event_seq,
                                  session.attempt_index,
                                  ret,
                                  session.state,
                                  attempted_count,
                                  session.successful_unique_count,
                                  session.config.min_anchor_count);
            }
        }

        if (session.state == UWB_CLICKER_SUCCEEDED) {
            high_debug_log_event("CLICK_TRACE",
                                 "point=success_state event_seq=%u attempt=%u unique=%u/%u",
                                 event_seq,
                                 session.attempt_index,
                                 session.successful_unique_count,
                                 session.config.min_anchor_count);
            stage1_click_diag("success_state event_seq=%u attempt=%u unique=%u/%u",
                              event_seq,
                              session.attempt_index,
                              session.successful_unique_count,
                              session.config.min_anchor_count);
            stage1_led_result(STAGE1_LED_RESULT_OK);
            return 0;
        }
        if (session.attempt_index < session.config.max_attempts) {
            ret = uwb_clicker_prepare_retry(&session);
            if (ret != PROTO_OK) {
                high_debug_log_event("CLICK_TRACE",
                                     "point=prepare_retry_failed event_seq=%u attempt=%u proto_ret=%d",
                                     event_seq,
                                     session.attempt_index,
                                     ret);
                stage1_click_diag("prepare_retry_failed event_seq=%u attempt=%u proto_ret=%d",
                                  event_seq,
                                  session.attempt_index,
                                  ret);
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                return -EINVAL;
            }
            (void)app_clicker_apply_retry_delay(&session,
                                                click_deadline_ms,
                                                UWB_RETRY_BASE_DELAY_MS,
                                                WAKE_ADV_MS);
            (void)app_clicker_apply_contention_delay(&session,
                                                     click_deadline_ms,
                                                     WAKE_ADV_MS);
            LOG_INF("normal click retry scheduled: event_seq=%u next_attempt=%u retries=%u unique_success=%u/%u ds_ok=%u ds_fail=%u timing_reject=%u",
                    event_seq,
                    session.attempt_index,
                    session.diagnostics.retries,
                    session.successful_unique_count,
                    session.config.min_anchor_count,
                    session.diagnostics.ds_twr_successes,
                    session.diagnostics.ds_twr_failures,
                    session.diagnostics.timing_rejections);
        } else {
            break;
        }
    }

    high_debug_log_event("CLICK_TRACE",
                         "point=final_failed event_seq=%u last_ret=%d attempts=%u selected_total=%u attempted=%u unique=%u/%u state=%u",
                         event_seq,
                         last_ret,
                         session.attempt_index,
                         total_candidate_count,
                         attempted_count,
                         session.successful_unique_count,
                         session.config.min_anchor_count,
                         session.state);
    stage1_click_diag("final_failed event_seq=%u last_ret=%d attempts=%u selected_total=%u attempted=%u unique=%u/%u state=%u",
                      event_seq,
                      last_ret,
                      session.attempt_index,
                      total_candidate_count,
                      attempted_count,
                      session.successful_unique_count,
                      session.config.min_anchor_count,
                      session.state);
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
    stage1_led_result(last_ret == -ETIMEDOUT ?
                      STAGE1_LED_RESULT_TIMEOUT :
                      STAGE1_LED_RESULT_ERROR);
    return last_ret < 0 ? last_ret : -EIO;
}

int app_clicker_run_uwb_diagnostic_click(uint32_t event_seq)
{
    uint8_t attempted_count = 0u;
    int64_t click_deadline_ms = k_uptime_get() + CLICK_REPORT_DEADLINE_MS;
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

    ret = app_clicker_collect_uwb_attempt(&session,
                                          clicker_priority_id(event_seq, 1u),
                                          &schedule);
    if (ret < 0) {
        return ret;
    }

    ret = app_clicker_range_scheduled_anchors(&session,
                                              &schedule,
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
    int ret;

    LOG_INF("self-test started on UWB wake path");

    ret = dwm3000_port_init();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 port init failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 wake failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    ret = dwm3000_port_hw_reset();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 reset failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    ret = dwm3000_driver_probe(&dev_id);
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 decadriver DEV_ID probe failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    ret = dwm3000_port_set_fast_spi();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 fast SPI config failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    LOG_INF("self-test DWM3000 decadriver DEV_ID=0x%08x; fast SPI config checked at %u Hz",
            dev_id,
            (unsigned int)dwm3000_port_current_spi_hz());
    (void)dwm3000_driver_standby();

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

static int app_clicker_emit_self_test_report(uint32_t event_seq,
                                             enum self_test_failure failure)
{
    struct mesh_outbound outbound = {0};
    struct self_test_report_fields fields = {
        .clicker_id = DEVICE_ID,
        .event_seq = event_seq,
        .failure_code = (uint8_t)failure,
        .battery_mv = 0u,
    };
    size_t payload_len = 0u;
    uint16_t packet_seq = (uint16_t)event_seq;
    int ret;

    if (clicker_callbacks.send_mesh_outbound == NULL) {
        return -ENOSYS;
    }
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

    ret = clicker_callbacks.send_mesh_outbound(&outbound, "self-test-report");
    if (ret < 0) {
        LOG_WRN("self-test report UWB mesh TX failed: event_seq=%u failure=%u ret=%d",
                event_seq,
                (unsigned int)failure,
                ret);
        return ret;
    }

    LOG_INF("self-test report sent over UWB mesh: event_seq=%u failure=%u",
            event_seq,
            (unsigned int)failure);
    return 0;
}

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
        return ret;
    }

    ret = clicker_ble_courtesy_set_scan_channel();
    if (ret != 0) {
        LOG_WRN("BLE courtesy disabled: scan channel 37 map failed: %d", ret);
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
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    high_debug_log_event("BLE_TEST", "phase=init ret=%d", ret);
#endif
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
#if defined(CONFIG_IMEC_HIGH_DEBUG)
        high_debug_log_event("BLE_TEST",
                             "phase=encode ret=%d written=%u expected=%u duration_units=%u",
                             ret,
                             (unsigned int)written,
                             (unsigned int)sizeof(ble_courtesy_adv_data),
                             ble_courtesy_local.defer_duration_units);
#endif
        return -EINVAL;
    }
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    high_debug_log_event("BLE_TEST",
                         "phase=encode ret=0 written=%u duration_units=%u",
                         (unsigned int)written,
                         ble_courtesy_local.defer_duration_units);
#endif

    clicker_ble_courtesy_clear_higher_peer();
    ble_courtesy_scan_active = true;
    ret = bt_le_scan_start(&scan_param, clicker_ble_courtesy_scan_cb);
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    high_debug_log_event("BLE_TEST", "phase=scan_start ret=%d", ret);
#endif
    if (ret != 0) {
        LOG_WRN("BLE courtesy scan start failed: %d", ret);
        ble_courtesy_scan_active = false;
        return ret;
    }

    ret = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0u);
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    high_debug_log_event("BLE_TEST", "phase=adv_start ret=%d", ret);
#endif
    if (ret != 0) {
        LOG_WRN("BLE courtesy advertising start failed: %d", ret);
        (void)bt_le_scan_stop();
        ble_courtesy_scan_active = false;
        return ret;
    }
    ble_courtesy_adv_active = true;
    return 0;
}

uint32_t app_clicker_ble_courtesy_higher_wait_ms(void)
{
    k_spinlock_key_t key = k_spin_lock(&ble_courtesy_lock);
    uint32_t wait_ms = ble_courtesy_higher_wait_ms;

    k_spin_unlock(&ble_courtesy_lock, key);
    return wait_ms;
}

void app_clicker_ble_courtesy_stop(void)
{
    int ret;

    if (ble_courtesy_adv_active) {
        ret = bt_le_adv_stop();
        if (ret != 0 && ret != -EALREADY) {
            LOG_WRN("BLE courtesy advertising stop failed: %d", ret);
        }
        ble_courtesy_adv_active = false;
    }
    if (ble_courtesy_scan_active) {
        ret = bt_le_scan_stop();
        if (ret != 0 && ret != -EALREADY) {
            LOG_WRN("BLE courtesy scan stop failed: %d", ret);
        }
        ble_courtesy_scan_active = false;
    }
}

void app_clicker_ble_courtesy_low_power_stop(void)
{
    int ret;

    app_clicker_ble_courtesy_stop();
    if (!ble_courtesy_init_attempted) {
        return;
    }

    ret = bt_disable();
    if (ret != 0 && ret != -EALREADY) {
        LOG_WRN("BLE courtesy disable before retained idle failed: %d", ret);
    }
    ble_courtesy_init_attempted = false;
    ble_courtesy_available = false;
    clicker_ble_courtesy_clear_higher_peer();
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    high_debug_log_event("BLE_TEST", "phase=low_power_stop ret=%d", ret);
#endif
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

void BLE_CONNECTIVITY_TEST_UNUSED app_clicker_ble_courtesy_low_power_stop(void)
{
}
#endif

#if DT_NODE_HAS_STATUS(CLICK_BUTTON_NODE, okay) && \
    !defined(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS)
static const struct gpio_dt_spec click_button = GPIO_DT_SPEC_GET(CLICK_BUTTON_NODE, gpios);
static struct gpio_callback click_button_cb;
static struct k_work click_button_work;
static struct k_work_delayable click_button_release_work;
static struct k_work_delayable self_test_arm_timeout_work;
static struct k_work clicker_action_work;
#define CLICK_BUTTON_PORT_NUM DT_PROP(DT_GPIO_CTLR(CLICK_BUTTON_NODE, gpios), port)
#define CLICK_BUTTON_PIN_NUM DT_GPIO_PIN(CLICK_BUTTON_NODE, gpios)
#define HAS_CLICK_BUTTON 1
#else
#define HAS_CLICK_BUTTON 0
#endif

#if HAS_CLICK_BUTTON || defined(CONFIG_IMEC_ML_CLICKER)
#define HAS_CLICKER_ACTION_WORK_QUEUE 1
#else
#define HAS_CLICKER_ACTION_WORK_QUEUE 0
#endif

#if HAS_CLICKER_ACTION_WORK_QUEUE
K_THREAD_STACK_DEFINE(clicker_action_work_q_stack, CLICKER_ACTION_WORKQUEUE_STACK_SIZE);
#endif

static struct button_fsm button_fsm;
#if HAS_CLICKER_ACTION_WORK_QUEUE
static struct k_work_q clicker_action_work_q;
static const struct k_work_queue_config clicker_action_work_q_config = {
    .name = "clicker_action",
};
static bool clicker_action_work_q_started;
#endif
#if HAS_CLICK_BUTTON
static atomic_t clicker_action_active;
static enum button_action clicker_pending_action;
#endif

int app_clicker_init(const struct app_clicker_callbacks *callbacks)
{
    if (callbacks != NULL) {
        clicker_callbacks = *callbacks;
    } else {
        clicker_callbacks = (struct app_clicker_callbacks){0};
    }
    button_fsm_init(&button_fsm);
    return 0;
}

bool BLE_CONNECTIVITY_TEST_UNUSED clicker_reset_reason_was_systemoff(void)
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

void BLE_CONNECTIVITY_TEST_UNUSED clicker_request_systemoff_ram_retention(void)
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
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (CONFIG_IMEC_BENCH_STAGE == 1 &&
        IS_ENABLED(CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE)) {
        return 1u;
    }
#endif
    return UWB_NORMAL_CLICK_MIN_ANCHORS;
}

uint8_t app_clicker_debug_max_anchor_count(void)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (CONFIG_IMEC_BENCH_STAGE == 1 &&
        IS_ENABLED(CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE)) {
        return 1u;
    }
#endif
    return UWB_RANGE_SCHEDULE_MAX_ANCHORS;
}

uint8_t app_clicker_debug_samples_per_anchor(void)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (CONFIG_IMEC_BENCH_STAGE == 1 &&
        IS_ENABLED(CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE)) {
        return 1u;
    }
#endif
    return UWB_RANGING_REQUESTS_MAX_PER_ANCHOR;
}

uint8_t app_clicker_debug_session_flags(void)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (CONFIG_IMEC_BENCH_STAGE == 1 &&
        IS_ENABLED(CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE)) {
        return FLAG_DIAGNOSTIC;
    }
#endif
    return FLAG_COUNT_AS_CLICK;
}

void app_clicker_handle_button_action(enum button_action action)
{
    struct status_inputs status = {0};
    enum self_test_failure failure;
    uint32_t self_test_event_seq;
    int ret;

    switch (action) {
    case BUTTON_ACTION_NORMAL_CLICK:
#if defined(CONFIG_IMEC_HIGH_DEBUG)
        if (DEVICE_ROLE == ROLE_CLICKER && CONFIG_IMEC_BENCH_STAGE == 0) {
            ret = clicker_callbacks.run_stage0_simulated_click != NULL ?
                  clicker_callbacks.run_stage0_simulated_click() : -ENOSYS;
            status.click_accepted = ret == 0;
            status.click_failure = ret == 0 ? 0 : CLICK_FAILURE_INSUFFICIENT_RANGES;
            status_apply(&status);
            app_clicker_enter_idle();
            break;
        }
#endif
        stage1_click_trace_reset();
        high_debug_log_event("BUTTON_ACTION",
                             "action=normal_click point=before_run");
        stage1_click_diag("button before_run action=normal_click");
        ret = app_clicker_run_normal_click();
        high_debug_log_event("BUTTON_ACTION",
                             "action=normal_click point=after_run ret=%d",
                             ret);
        stage1_click_diag("button after_run ret=%d", ret);
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
        high_debug_log_event("BUTTON_ACTION",
                             "action=normal_click point=before_led_hold ret=%d",
                             ret);
        stage1_click_diag("button before_led_hold ret=%d", ret);
        stage1_led_hold_click_result(ret, 2000u);
        high_debug_log_event("BUTTON_ACTION",
                             "action=normal_click point=before_idle ret=%d",
                             ret);
        stage1_click_diag("button before_idle ret=%d", ret);
        stage1_click_trace_dump("before_idle");
        app_clicker_enter_idle();
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
        self_test_event_seq = next_click_event_seq();
#if defined(CONFIG_IMEC_HIGH_DEBUG)
        if (DEVICE_ROLE == ROLE_CLICKER && CONFIG_IMEC_BENCH_STAGE == 0) {
            ret = clicker_callbacks.run_stage0_hardware_self_test != NULL ?
                  clicker_callbacks.run_stage0_hardware_self_test() : -ENOSYS;
            failure = ret == 0 ? SELF_TEST_FAILURE_NONE : SELF_TEST_FAILURE_DWM3000;
        } else
#endif
        {
            failure = app_clicker_run_self_test(self_test_event_seq);
        }
        status.self_test_running = false;
        status.failure = failure;
        status.self_test_passed = failure == SELF_TEST_FAILURE_NONE;
        status_apply(&status);
#if defined(CONFIG_IMEC_HIGH_DEBUG)
        if (!(DEVICE_ROLE == ROLE_CLICKER && CONFIG_IMEC_BENCH_STAGE == 0))
#endif
        {
            if (clicker_callbacks.send_mesh_outbound != NULL) {
                (void)app_clicker_emit_self_test_report(self_test_event_seq,
                                                        failure);
            }
        }
        app_clicker_enter_idle();
        break;
    case BUTTON_ACTION_SELF_TEST_CANCELLED:
        status_apply(&status);
        LOG_INF("self-test arm cancelled");
        app_clicker_enter_idle();
        break;
    case BUTTON_ACTION_NONE:
    default:
        break;
    }
}

#if HAS_CLICK_BUTTON
static void clicker_notify_early_led(enum app_clicker_early_led_event event)
{
    if (clicker_callbacks.early_led != NULL) {
        clicker_callbacks.early_led(event);
    }
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

static int click_button_configure_input(void)
{
    if (!gpio_is_ready_dt(&click_button)) {
        return -ENODEV;
    }

    return gpio_pin_configure_dt(&click_button, GPIO_INPUT);
}

static bool click_button_wait_for_release(void)
{
    int64_t deadline_ms = k_uptime_get() + CLICK_BUTTON_RELEASE_TIMEOUT_MS;
    int64_t released_since_ms = -1;

    while (k_uptime_get() < deadline_ms) {
        int64_t now_ms = k_uptime_get();
        int pressed = click_button_pressed();

        if (pressed < 0) {
            return false;
        }
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

    return click_button_pressed() == 0;
}

static int click_button_arm_idle_interrupt(void)
{
    int ret;

    ret = click_button_configure_input();
    if (ret < 0) {
        return ret;
    }
    (void)gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_DISABLE);
    click_button_clear_latch();
    return gpio_pin_interrupt_configure(click_button.port,
                                        click_button.pin,
                                        GPIO_INT_EDGE_TO_ACTIVE);
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
    (void)gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_DISABLE);
    click_button_clear_latch();

    ret = button_fsm_handle(&button_fsm,
                            BUTTON_SIGNAL_PRESS,
                            k_uptime_get_32(),
                            &ignored);
    if (ret != PROTO_OK) {
        return false;
    }
    if (!click_button_wait_for_release()) {
        return false;
    }
    click_button_clear_latch();

    ret = button_fsm_handle(&button_fsm,
                            BUTTON_SIGNAL_RELEASE,
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
    clicker_request_systemoff_ram_retention();
    LOG_PANIC();
    sys_poweroff();
}

void app_clicker_enter_systemoff_idle(void)
{
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_CLICKER_SYSTEMOFF_IDLE) ||
        DEVICE_ROLE != ROLE_CLICKER) {
        return;
    }

    (void)battery_adc_divider_disable();
    clicker_prepare_radio_systemoff();
    status_leds_set(false, false, false);
    status_leds_disconnect();

    ret = click_button_configure_input();
    if (ret < 0) {
        LOG_WRN("click button wake arm unavailable: %d", ret);
        clicker_notify_early_led(APP_CLICKER_EARLY_LED_BUTTON_WAKE_INPUT_UNAVAILABLE);
        clicker_systemoff_now();
        return;
    }

    (void)gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_DISABLE);
    if (!click_button_wait_for_release()) {
        LOG_WRN("click button still held; entering system-off without wake arm");
        clicker_notify_early_led(APP_CLICKER_EARLY_LED_BUTTON_STILL_HELD_NO_WAKE_ARM);
        (void)gpio_pin_configure_dt(&click_button, GPIO_DISCONNECTED);
        clicker_systemoff_now();
        return;
    }
    click_button_clear_latch();

    ret = gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_LEVEL_LOW);
    if (ret < 0) {
        LOG_WRN("click button wake arm failed: %d", ret);
        clicker_notify_early_led(APP_CLICKER_EARLY_LED_BUTTON_WAKE_ARM_FAILED);
    }

    LOG_INF("clicker entering system-off idle; wake source=P0.%u physical-low",
            (unsigned int)CLICK_BUTTON_PIN_NUM);
    clicker_systemoff_now();
}

static void clicker_enter_systemon_retained_idle(void)
{
    bool radio_retained = false;
    int ret;

    if (!clicker_systemon_retained_idle_enabled()) {
        return;
    }

    (void)battery_adc_divider_disable();
    app_clicker_ble_courtesy_low_power_stop();
    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        LOG_WRN("DWM3000 retained-idle preconfigure failed: %d", ret);
    } else {
        ret = dwm3000_driver_standby();
        if (ret < 0) {
            LOG_WRN("DWM3000 retained-idle sleep failed: %d", ret);
        } else {
            radio_retained = true;
        }
    }
    ret = dwm3000_port_float_pins();
    if (ret < 0) {
        LOG_WRN("DWM3000 retained-idle pin float failed: %d", ret);
    }

    ret = click_button_arm_idle_interrupt();
    if (ret < 0) {
        LOG_WRN("click button retained-idle wake arm failed: %d", ret);
    }
    high_debug_log_event("CLICKER_IDLE",
                         "mode=system_on_retained wake_source=P0.%u button_irq=edge_to_active release_poll=1 local_command_poll=0 radio_retained=%u dwm_pins=float",
                         (unsigned int)CLICK_BUTTON_PIN_NUM,
                         radio_retained ? 1u : 0u);
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
    enum button_action action = clicker_pending_action;

    ARG_UNUSED(work);

    clicker_pending_action = BUTTON_ACTION_NONE;
    high_debug_log_event("BUTTON_ACTION",
                         "point=action_worker_begin action=%u",
                         (unsigned int)action);
    app_clicker_handle_button_action(action);
    high_debug_log_event("BUTTON_ACTION",
                         "point=action_worker_end action=%u",
                         (unsigned int)action);
    atomic_set(&clicker_action_active, 0);
}

static void clicker_submit_button_action(enum button_action action)
{
    int ret;

    if (action == BUTTON_ACTION_NONE) {
        return;
    }
    if (!atomic_cas(&clicker_action_active, 0, 1)) {
        high_debug_log_event("BUTTON_ACTION",
                             "point=action_drop reason=busy action=%u",
                             (unsigned int)action);
        LOG_WRN("clicker button action ignored while previous action is active: %u",
                (unsigned int)action);
        return;
    }

    clicker_pending_action = action;
    ret = app_clicker_submit_work(&clicker_action_work);
    if (ret < 0) {
        clicker_pending_action = BUTTON_ACTION_NONE;
        atomic_set(&clicker_action_active, 0);
        high_debug_log_event("BUTTON_ACTION",
                             "point=action_submit_failed action=%u ret=%d",
                             (unsigned int)action,
                             ret);
        LOG_ERR("clicker button action queue submit failed: action=%u ret=%d",
                (unsigned int)action,
                ret);
    }
}

static void click_button_handle_signal(enum button_signal signal, const char *source)
{
    enum button_action action;
    int ret;

    ret = button_fsm_handle(&button_fsm, signal, k_uptime_get_32(), &action);
    if (ret != PROTO_OK) {
        LOG_ERR("button FSM rejected signal %u from %s: %d",
                (unsigned int)signal,
                source == NULL ? "unknown" : source,
                ret);
        return;
    }
    high_debug_log_event("BUTTON_EDGE",
                         "source=%s signal=%u action=%u",
                         source == NULL ? "unknown" : source,
                         (unsigned int)signal,
                         (unsigned int)action);
    clicker_submit_button_action(action);
}

static void click_button_release_work_handler(struct k_work *work)
{
    int pressed;
    int ret;

    ARG_UNUSED(work);

    pressed = click_button_pressed();
    if (pressed < 0) {
        LOG_ERR("failed to poll click button release: %d", pressed);
        return;
    }
    if (pressed != 0) {
        (void)k_work_reschedule(&click_button_release_work,
                                K_MSEC(CLICK_BUTTON_RELEASE_POLL_MS));
        return;
    }

    click_button_clear_latch();
    click_button_handle_signal(BUTTON_SIGNAL_RELEASE, "release_poll");
    ret = click_button_arm_idle_interrupt();
    if (ret < 0) {
        LOG_WRN("click button rearm after release poll failed: %d", ret);
    }
}

static void click_button_work_handler(struct k_work *work)
{
    enum button_signal signal;
    int pressed;
    int ret;

    ARG_UNUSED(work);

    pressed = click_button_pressed();
    if (pressed < 0) {
        LOG_ERR("failed to read click button: %d", pressed);
        return;
    }
    if (pressed != 0) {
        (void)gpio_pin_interrupt_configure(click_button.port,
                                           click_button.pin,
                                           GPIO_INT_DISABLE);
    }
    if (pressed != 0 && clicker_systemon_retained_idle_enabled() &&
        atomic_get(&clicker_action_active) == 0) {
        clicker_notify_early_led(APP_CLICKER_EARLY_LED_SYSTEMON_BUTTON_PRESS);
    }

    signal = pressed != 0 ? BUTTON_SIGNAL_PRESS : BUTTON_SIGNAL_RELEASE;
    click_button_handle_signal(signal, "irq");
    if (pressed != 0) {
        (void)k_work_reschedule(&click_button_release_work,
                                K_MSEC(CLICK_BUTTON_RELEASE_STABLE_MS));
    } else {
        (void)k_work_cancel_delayable(&click_button_release_work);
        ret = click_button_arm_idle_interrupt();
        if (ret < 0) {
            LOG_WRN("click button rearm after release IRQ failed: %d", ret);
        }
    }
}

static void click_button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    (void)k_work_submit(&click_button_work);
}

static void self_test_arm_timeout_handler(struct k_work *work)
{
    enum button_action action;

    ARG_UNUSED(work);

    if (button_fsm_handle(&button_fsm, BUTTON_SIGNAL_TICK,
                               k_uptime_get_32(), &action) == PROTO_OK) {
        clicker_submit_button_action(action);
    }
}

void app_clicker_arm_self_test_timeout(void)
{
    (void)k_work_reschedule(&self_test_arm_timeout_work,
                            K_MSEC(SELF_TEST_ARM_WINDOW_MS + 1u));
}

void app_clicker_cancel_self_test_timeout(void)
{
    (void)k_work_cancel_delayable(&self_test_arm_timeout_work);
}

int ML_CLICKER_BUTTON_UNUSED app_clicker_button_init(void)
{
    int ret;

    ret = click_button_configure_input();
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_DISABLE);
    if (ret < 0) {
        return ret;
    }

    k_work_init(&click_button_work, click_button_work_handler);
    k_work_init_delayable(&click_button_release_work, click_button_release_work_handler);
    k_work_init(&clicker_action_work, clicker_action_work_handler);
    k_work_init_delayable(&self_test_arm_timeout_work, self_test_arm_timeout_handler);
    gpio_init_callback(&click_button_cb, click_button_isr, BIT(click_button.pin));
    ret = gpio_add_callback(click_button.port, &click_button_cb);
    if (ret < 0) {
        return ret;
    }

    ret = click_button_arm_idle_interrupt();
    if (ret < 0) {
        (void)gpio_remove_callback(click_button.port, &click_button_cb);
        return ret;
    }

    (void)k_work_submit(&click_button_work);
    return 0;
}

void app_clicker_prepare_startup_idle(enum button_action *boot_action)
{
    if (boot_action == NULL) {
        return;
    }

    if (DEVICE_ROLE == ROLE_CLICKER &&
        IS_ENABLED(CONFIG_IMEC_CLICKER_SYSTEMOFF_IDLE) &&
        !clicker_systemon_retained_idle_enabled() &&
        !IS_ENABLED(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS)) {
        if (clicker_reset_reason_was_systemoff()) {
            clicker_notify_early_led(APP_CLICKER_EARLY_LED_SYSTEMOFF_BUTTON_WAKE);
            if (!clicker_capture_systemoff_button_action(boot_action)) {
                LOG_WRN("system-off wake button capture failed; returning to idle");
                clicker_notify_early_led(APP_CLICKER_EARLY_LED_SYSTEMOFF_CAPTURE_FAILED);
                app_clicker_enter_systemoff_idle();
            }
        } else {
            app_clicker_enter_systemoff_idle();
        }
    }
}
#else
void BLE_CONNECTIVITY_TEST_UNUSED app_clicker_enter_systemoff_idle(void)
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

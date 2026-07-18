#include "app_anchor_survey_runtime.h"

#include "app_anchor_survey_discovery.h"
#include "app_board.h"
#include "app_config.h"
#include "app_node_comm.h"
#include "app_operation_policy.h"
#include "app_state.h"
#include "dwm3000_driver.h"
#include "status.h"
#include "survey_pair_lease.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <errno.h>

LOG_MODULE_DECLARE(app_anchor, LOG_LEVEL_DBG);

static struct app_anchor_survey_runtime_ops runtime_ops;
static bool runtime_initialized;
static uint16_t survey_sequence;
static struct k_spinlock survey_lock;
static struct survey_pair_lease pair_lease;
static struct survey_discovery_config discovery_config;
static uint32_t discovery_start_ms;
static bool pair_start_pending;
static uint32_t pair_start_delivery_handle;
static bool survey_running;
static bool discovery_pending;
static bool discovery_generation_active;
static bool discovery_report_stage_pending;
static atomic_t abort_requested;
static struct k_work_delayable survey_work;
static struct k_work_delayable pair_lease_work;

#define SURVEY_START_DELIVERY_POLL_MS 5u
#define SURVEY_NON_RF_SERVICE_POLL_MS REPORT_TX_RETRY_DELAY_MS

struct survey_rf_retry_state {
    uint32_t survey_id;
    uint32_t opportunity;
    uint16_t retry_round;
    bool valid;
};

static struct survey_rf_retry_state discovery_rf_retry;
static struct survey_rf_retry_state pair_rf_retry;

static bool pair_queueable(const struct survey_pair *pair)
{
    return pair != NULL &&
           pair->sample_count <= SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT &&
           pair->sample_count <= REPORT_TX_QUEUE_DEPTH;
}

static void schedule(k_timeout_t delay)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    if (runtime_ops.work_queue != NULL) {
        (void)k_work_reschedule_for_queue(runtime_ops.work_queue,
                                          &survey_work,
                                          delay);
    }
#else
    ARG_UNUSED(delay);
#endif
}

static void survey_rf_retry_reset(struct survey_rf_retry_state *state)
{
    if (state != NULL) {
        *state = (struct survey_rf_retry_state) {0};
    }
}

static int survey_rf_retry_delay_ms(struct survey_rf_retry_state *state,
                                    uint32_t survey_id,
                                    uint32_t opportunity,
                                    uint32_t absolute_deadline_ms,
                                    uint32_t *delay_ms_out)
{
    uint32_t remaining_ms;
    uint32_t now_ms;
    int ret;

    if (state == NULL || survey_id == 0u || delay_ms_out == NULL) {
        return -EINVAL;
    }
    now_ms = k_uptime_get_32();
    if (uptime_deadline_reached(now_ms, absolute_deadline_ms)) {
        return -ETIMEDOUT;
    }
    if (!state->valid || state->survey_id != survey_id ||
        state->opportunity != opportunity) {
        *state = (struct survey_rf_retry_state) {
            .survey_id = survey_id,
            .opportunity = opportunity,
            .valid = true,
        };
    }
    if (state->retry_round < UINT16_MAX) {
        state->retry_round++;
    }
    ret = app_node_comm_retry_identity_backoff_ms(
        DEVICE_ID,
        survey_id,
        opportunity,
        NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
        state->retry_round,
        delay_ms_out);
    if (ret < 0) {
        return ret;
    }
    remaining_ms = uptime_ms_until_deadline(now_ms, absolute_deadline_ms);
    if (*delay_ms_out > remaining_ms) {
        *delay_ms_out = remaining_ms;
    }
    status_debug_printf(
        "DBG_SURVEY_RF_DEFER survey=%u opportunity=%u round=%u delay_ms=%u deadline_ms=%u\n",
        survey_id,
        opportunity,
        state->retry_round,
        *delay_ms_out,
        absolute_deadline_ms);
    return 0;
}

static uint32_t survey_discovery_radio_deadline_ms(
    const struct survey_discovery_config *config,
    uint32_t start_ms)
{
    return start_ms + survey_discovery_duration_ms(config);
}

static uint32_t survey_discovery_radio_opportunity(void)
{
    return (uint32_t)MSG_SURVEY_DISCOVERY_START << 16;
}

static uint32_t survey_pair_radio_opportunity(
    const struct survey_pair_control_id *control_id)
{
    return ((uint32_t)MSG_COMMAND << 16) | control_id->command_seq;
}

static bool schedule_pair_rf_retry(
    const struct survey_pair_control_id *control_id,
    uint32_t absolute_deadline_ms,
    const char *reason)
{
    uint32_t retry_delay_ms = 0u;
    bool still_current;
    k_spinlock_key_t key;
    int ret;

    ret = survey_rf_retry_delay_ms(
        &pair_rf_retry,
        control_id->session_id,
        survey_pair_radio_opportunity(control_id),
        absolute_deadline_ms,
        &retry_delay_ms);
    if (ret == 0) {
        schedule(K_MSEC(retry_delay_ms));
        return true;
    }

    key = k_spin_lock(&survey_lock);
    still_current = pair_start_pending && pair_lease.start_id_valid &&
        pair_lease.start_id.session_id == control_id->session_id &&
        pair_lease.start_id.command_seq == control_id->command_seq;
    if (still_current) {
        (void)survey_pair_lease_abort(&pair_lease);
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
    }
    k_spin_unlock(&survey_lock, key);
    if (still_current) {
        (void)k_work_cancel_delayable(&pair_lease_work);
    }
    survey_rf_retry_reset(&pair_rf_retry);
    LOG_ERR("survey pair RF retry terminated: survey=%u seq=%u ret=%d reason=%s",
            control_id->session_id,
            control_id->command_seq,
            ret,
            reason == NULL ? "radio-deadline" : reason);
    return false;
}

static void abandon_pair_start_delivery(uint32_t delivery_handle,
                                        const char *reason)
{
    int ret;

    if (delivery_handle == 0u) {
        return;
    }
    ret = app_node_comm_abandon_delivery(delivery_handle);
    status_debug_printf(
        "DBG_SURVEY_PAIR_START_ABANDON handle=%u ret=%d reason=%s\n",
        delivery_handle,
        ret,
        reason == NULL ? "pair-state-release" : reason);
    if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
        LOG_WRN("survey pair start delivery abandon failed: handle=%u ret=%d reason=%s",
                delivery_handle,
                ret,
                reason == NULL ? "pair-state-release" : reason);
    }
}

void app_anchor_survey_runtime_schedule_ms(uint32_t delay_ms)
{
    schedule(K_MSEC(delay_ms));
}

uint16_t app_anchor_survey_runtime_next_sequence(void)
{
    uint16_t sequence;
    k_spinlock_key_t key = k_spin_lock(&survey_lock);

    survey_sequence++;
    if (survey_sequence == 0u) {
        survey_sequence = 1u;
    }
    sequence = survey_sequence;
    k_spin_unlock(&survey_lock, key);
    return sequence;
}

void app_anchor_survey_runtime_seed_sequence(uint16_t observed_sequence)
{
    k_spinlock_key_t key;

    if (observed_sequence == 0u) {
        return;
    }
    key = k_spin_lock(&survey_lock);
    survey_sequence = observed_sequence;
    k_spin_unlock(&survey_lock, key);
}

bool app_anchor_survey_runtime_discovery_is_pending(void)
{
    k_spinlock_key_t key;
    bool pending;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return false;
    }

    key = k_spin_lock(&survey_lock);
    pending = discovery_pending;
    k_spin_unlock(&survey_lock, key);
    return pending;
}

bool app_anchor_survey_runtime_abort_requested(void)
{
    return atomic_get(&abort_requested) != 0;
}

enum app_anchor_survey_discovery_admission
app_anchor_survey_runtime_admit_discovery(uint32_t survey_id)
{
    enum app_anchor_survey_discovery_admission admission;
    k_spinlock_key_t key;

    if (DEVICE_ROLE != ROLE_ANCHOR || survey_id == 0u) {
        return APP_ANCHOR_SURVEY_DISCOVERY_BUSY;
    }

    key = k_spin_lock(&survey_lock);
    if (!discovery_generation_active) {
        discovery_generation_active = true;
        discovery_config = (struct survey_discovery_config) {
            .survey_id = survey_id,
        };
        admission = APP_ANCHOR_SURVEY_DISCOVERY_ACCEPTED;
    } else if (discovery_config.survey_id == survey_id) {
        admission = APP_ANCHOR_SURVEY_DISCOVERY_DUPLICATE;
    } else {
        admission = APP_ANCHOR_SURVEY_DISCOVERY_BUSY;
    }
    k_spin_unlock(&survey_lock, key);
    return admission;
}

void app_anchor_survey_runtime_queue_discovery(
    const struct survey_discovery_config *config,
    uint32_t start_ms)
{
    k_spinlock_key_t key;

    if (config == NULL || config->survey_id == 0u) {
        return;
    }

    key = k_spin_lock(&survey_lock);
    if (discovery_generation_active &&
        discovery_config.survey_id == config->survey_id) {
        discovery_config = *config;
        discovery_start_ms = start_ms;
        discovery_pending = true;
        atomic_set(&abort_requested, 0);
    }
    k_spin_unlock(&survey_lock, key);
}

void app_anchor_survey_runtime_handle_pair_prepare(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct survey_pair pair = {0};
    struct survey_pair_control_id control_id;
    enum survey_pair_lease_decision decision;
    enum command_status status = COMMAND_OK;
    uint32_t lease_remaining_ms = 0u;
    uint16_t round_id = SURVEY_LEGACY_ROUND_ID;
    uint8_t reason = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        packet == NULL ||
        packet->msg_type != MSG_SURVEY_PAIR_PREPARE ||
        packet->dst_id != DEVICE_ID ||
        packet->src_id != GATEWAY_ID) {
        return;
    }

    ret = app_operation_policy_apply_payload(payload, payload_len, 0u, NULL);
    if (ret < 0) {
        status = COMMAND_MALFORMED_PAYLOAD;
        reason = EBADMSG;
    }
    if (ret == 0) {
        ret = survey_extract_pair_tlvs(payload, payload_len, &pair);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_id_extract_tlv(payload, payload_len, &round_id);
    }
    if (ret != PROTO_OK || packet->session_id != pair.survey_id) {
        status = COMMAND_MALFORMED_PAYLOAD;
        reason = (uint8_t)(ret == PROTO_OK ? 1u : -ret);
    } else if (pair.initiator_id != DEVICE_ID && pair.responder_id != DEVICE_ID) {
        status = COMMAND_DENIED;
        reason = 2u;
    } else if (!pair_queueable(&pair)) {
        status = COMMAND_DENIED;
        reason = 4u;
    } else {
        k_spinlock_key_t key = k_spin_lock(&survey_lock);

        control_id = (struct survey_pair_control_id) {
            .session_id = packet->session_id,
            .command_seq = packet->seq,
        };
        decision = survey_pair_lease_prepare_round(
            &pair_lease,
            &pair,
            round_id,
            &control_id,
            k_uptime_get_32(),
            SURVEY_PAIR_PREPARED_LEASE_MS);
        if (decision == SURVEY_PAIR_LEASE_BUSY) {
            status = COMMAND_BUSY;
            reason = 3u;
        } else if (decision == SURVEY_PAIR_LEASE_ACCEPTED ||
                   decision == SURVEY_PAIR_LEASE_DUPLICATE ||
                   decision == SURVEY_PAIR_LEASE_SUPERSEDED) {
            lease_remaining_ms = survey_pair_lease_remaining_ms(
                &pair_lease, k_uptime_get_32());
            atomic_set(&abort_requested, 0);
        } else {
            status = COMMAND_INVALID_STATE;
            reason = decision == SURVEY_PAIR_LEASE_EXPIRED ? 5u : 4u;
        }
        k_spin_unlock(&survey_lock, key);
    }

    if (lease_remaining_ms > 0u) {
        (void)k_work_reschedule(&pair_lease_work,
                                K_MSEC(lease_remaining_ms));
    }

    ret = runtime_ops.send_command_result(packet,
                                          CMD_SURVEY_PREPARE_PAIR,
                                          status,
                                          reason,
                                          NULL,
                                          0u);
    status_debug_printf(
        "DBG_SURVEY_PAIR_PREPARE_RX survey=%u round=%u seq=%u initiator=0x%016llx "
        "responder=0x%016llx samples=%u status=%u reason=%u result_ret=%d\n",
        packet->session_id,
        round_id,
        packet->seq,
        (unsigned long long)pair.initiator_id,
        (unsigned long long)pair.responder_id,
        pair.sample_count,
        status,
        reason,
        ret);
    if (ret < 0) {
        LOG_WRN("survey pair prepare result TX failed: status=%u ret=%d",
                status,
                ret);
        return;
    }

    LOG_INF("survey pair prepare handled: survey=%u responder=0x%016llx samples=%u status=%u reason=%u",
            pair.survey_id,
            (unsigned long long)pair.responder_id,
            pair.sample_count,
            status,
            reason);
}

static int run_pair_initiator(const struct survey_pair *pair)
{
    int last_ret = 0;

    for (uint16_t sample_index = 0u;
         sample_index < pair->sample_count &&
         !app_anchor_survey_runtime_abort_requested();
         sample_index++) {
        struct dwm3000_range_request request = {0};
        struct dwm3000_range_result result = {0};
        int ret = -ETIMEDOUT;

        request.initiator_id = pair->initiator_id;
        request.responder_id = pair->responder_id;
        request.network_id = NETWORK_ID;
        request.session_nonce = survey_sample_nonce(pair, sample_index);
        request.responder_short_addr =
            uwb_session_short_addr_from_id(pair->responder_id);
        request.session_id = pair->survey_id;
        request.seq = survey_sample_seq(sample_index);
        request.flags = FLAG_DIAGNOSTIC;
        request.timeout_ms = SURVEY_PAIR_INITIATOR_TIMEOUT_MS;
        /*
         * The responder report carries the link RSL. Reading optional RX
         * diagnostics here would run between RESP reception and the delayed
         * FINAL, where the clicker path deliberately performs no extra SPI.
         */
        request.capture_rsl = false;
        result.status = RANGE_RX_TIMEOUT;

        LOG_INF("survey DS-TWR initiator sample start: survey=%u responder=0x%016llx sample=%u/%u seq=%u",
                pair->survey_id,
                (unsigned long long)pair->responder_id,
                (unsigned int)(sample_index + 1u),
                pair->sample_count,
                request.seq);
        ret = dwm3000_driver_range_initiator(&request, &result);
        if (result.initiator_id == 0u) {
            result.initiator_id = pair->initiator_id;
        }
        if (result.responder_id == 0u) {
            result.responder_id = pair->responder_id;
        }
        result.session_id = pair->survey_id;
        result.seq = request.seq;
        result.flags = FLAG_DIAGNOSTIC;
        if (result.status == RANGE_OK && ret < 0) {
            result.status = RANGE_INTERNAL_ERROR;
        }

        if (ret == 0 && result.status == RANGE_OK) {
            LOG_INF("survey DS-TWR initiator sample complete: survey=%u responder=0x%016llx sample=%u/%u distance_mm=%d quality=%u rsl=%d rsl_present=%u clock=%d clock_present=%u carrier=%d carrier_present=%u",
                    pair->survey_id,
                    (unsigned long long)result.responder_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    result.distance_mm,
                    result.quality,
                    result.rsl_dbm,
                    result.rsl_sampled ? 1u : 0u,
                    result.clock_offset_raw,
                    result.clock_offset_sampled ? 1u : 0u,
                    result.carrier_integrator,
                    result.carrier_integrator_sampled ? 1u : 0u);
        } else {
            last_ret = ret < 0 ? ret : -EIO;
            LOG_WRN("survey DS-TWR initiator sample failed: survey=%u responder=0x%016llx sample=%u/%u ret=%d status=%s(%u)",
                    pair->survey_id,
                    (unsigned long long)pair->responder_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    ret,
                    range_status_name(result.status),
                    result.status);
        }

        ret = runtime_ops.queue_sample_result(pair,
                                              sample_index,
                                              DEVICE_ID,
                                              &result);
        if (ret < 0) {
            LOG_WRN("survey sample result queue failed: survey=%u sample=%u ret=%d",
                    pair->survey_id,
                    sample_index,
                    ret);
            last_ret = ret;
        }
        if (sample_index + 1u < pair->sample_count) {
            k_msleep(SURVEY_PAIR_SAMPLE_GAP_MS);
        }
    }

    return last_ret;
}

static int run_pair_responder(const struct survey_pair *pair)
{
    int last_ret = 0;

    for (uint16_t sample_index = 0u;
         sample_index < pair->sample_count &&
         !app_anchor_survey_runtime_abort_requested();
         sample_index++) {
        struct dwm3000_range_request expected = {0};
        struct dwm3000_range_result result = {0};
        int64_t deadline_ms;
        int ret = -ETIMEDOUT;

        expected.initiator_id = pair->initiator_id;
        expected.responder_id = pair->responder_id;
        expected.network_id = NETWORK_ID;
        expected.session_nonce = survey_sample_nonce(pair, sample_index);
        expected.responder_short_addr = local_uwb_short_addr();
        expected.session_id = pair->survey_id;
        expected.seq = survey_sample_seq(sample_index);
        expected.flags = FLAG_DIAGNOSTIC;
        expected.capture_rsl = sample_index == 0u;
        result.status = RANGE_RX_TIMEOUT;

        deadline_ms = k_uptime_get() + SURVEY_PAIR_RESPONDER_WINDOW_MS;
        LOG_INF("survey DS-TWR responder listen: survey=%u initiator=0x%016llx sample=%u/%u seq=%u",
                pair->survey_id,
                (unsigned long long)pair->initiator_id,
                (unsigned int)(sample_index + 1u),
                pair->sample_count,
                expected.seq);
        while (k_uptime_get() < deadline_ms &&
               !app_anchor_survey_runtime_abort_requested()) {
            uint32_t remaining_ms =
                (uint32_t)MAX(1, deadline_ms - k_uptime_get());

            ret = dwm3000_driver_responder_poll_expected(DEVICE_ID,
                                                         &expected,
                                                         remaining_ms,
                                                         &result);
            if (ret == -EAGAIN) {
                continue;
            }
            break;
        }

        if (ret == 0 && result.status == RANGE_OK) {
            LOG_INF("survey DS-TWR responder sample complete: survey=%u initiator=0x%016llx sample=%u/%u distance_mm=%d quality=%u rsl=%d rsl_present=%u clock=%d clock_present=%u carrier=%d carrier_present=%u",
                    pair->survey_id,
                    (unsigned long long)result.initiator_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    result.distance_mm,
                    result.quality,
                    result.rsl_dbm,
                    result.rsl_sampled ? 1u : 0u,
                    result.clock_offset_raw,
                    result.clock_offset_sampled ? 1u : 0u,
                    result.carrier_integrator,
                    result.carrier_integrator_sampled ? 1u : 0u);
        } else {
            last_ret = ret < 0 ? ret : -EIO;
            if (result.status == RANGE_OK || !range_status_valid(result.status)) {
                result.status = ret == -ETIMEDOUT ?
                                RANGE_RX_TIMEOUT : RANGE_INTERNAL_ERROR;
            }
            LOG_WRN("survey DS-TWR responder sample failed: survey=%u initiator=0x%016llx sample=%u/%u ret=%d status=%s(%u)",
                    pair->survey_id,
                    (unsigned long long)pair->initiator_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    ret,
                    range_status_name(result.status),
                    result.status);
        }

        ret = runtime_ops.queue_sample_result(pair,
                                              sample_index,
                                              DEVICE_ID,
                                              &result);
        if (ret < 0) {
            LOG_WRN("survey responder sample result queue failed: survey=%u sample=%u ret=%d",
                    pair->survey_id,
                    sample_index,
                    ret);
            last_ret = ret;
        }
    }

    return last_ret;
}

static bool pair_start_delivery_ready(void)
{
    struct node_comm_terminal_event event;
    struct survey_pair_control_id control_id = {0};
    uint32_t delivery_handle;
    bool delivery_confirmed = false;
    bool ready = false;
    bool still_current;
    k_spinlock_key_t key;

    key = k_spin_lock(&survey_lock);
    if (!pair_start_pending) {
        k_spin_unlock(&survey_lock, key);
        return false;
    }
    if (survey_pair_lease_ready_snapshot(&pair_lease, NULL)) {
        k_spin_unlock(&survey_lock, key);
        return true;
    }
    delivery_handle = pair_start_delivery_handle;
    if (pair_lease.start_id_valid) {
        control_id = pair_lease.start_id;
    }
    k_spin_unlock(&survey_lock, key);

    if (delivery_handle == 0u) {
        return false;
    }
    if (!app_node_comm_take_delivery_event_for(delivery_handle, &event)) {
        schedule(K_MSEC(SURVEY_START_DELIVERY_POLL_MS));
        return false;
    }

    key = k_spin_lock(&survey_lock);
    still_current = pair_start_pending &&
        pair_start_delivery_handle == delivery_handle &&
        pair_lease.phase == SURVEY_PAIR_LEASE_START_PENDING &&
        pair_lease.start_id_valid &&
        pair_lease.start_id.session_id == control_id.session_id &&
        pair_lease.start_id.command_seq == control_id.command_seq;
    if (still_current) {
        pair_start_delivery_handle = 0u;
        if (event.reason == NODE_COMM_TERMINAL_DELIVERED) {
            delivery_confirmed = survey_pair_lease_release_start(&pair_lease,
                                                                 &control_id);
            ready = delivery_confirmed &&
                    survey_pair_lease_ready_snapshot(&pair_lease, NULL);
        } else {
            (void)survey_pair_lease_abort(&pair_lease);
            pair_start_pending = false;
        }
    }
    k_spin_unlock(&survey_lock, key);

    if (!still_current) {
        LOG_WRN("survey start delivery terminal event was stale: handle=%u reason=%u",
                delivery_handle, (unsigned int)event.reason);
    } else if (!delivery_confirmed) {
        LOG_WRN("survey start result was not delivered; pair start cancelled: handle=%u reason=%u attempts=%u",
                delivery_handle,
                (unsigned int)event.reason,
                event.attempts_started);
    } else if (!ready) {
        LOG_INF("survey start result gateway-confirmed; waiting for matching round GO: handle=%u attempts=%u",
                delivery_handle,
                event.attempts_started);
    } else {
        LOG_INF("survey start result gateway-confirmed before DS-TWR: handle=%u attempts=%u",
                delivery_handle, event.attempts_started);
    }
    return ready;
}

static void finish_discovery_without_radio(
    const struct survey_discovery_config *config,
    uint32_t start_ms,
    int run_ret,
    const char *reason)
{
    k_spinlock_key_t key;
    int report_ret;

    report_ret = app_anchor_survey_discovery_stage_empty_report(config,
                                                                 start_ms);
    status_debug_printf(
        "DBG_SURVEY_DISCOVERY_NO_RADIO survey=%u run_ret=%d report_ret=%d reason=%s\n",
        config->survey_id,
        run_ret,
        report_ret,
        reason == NULL ? "radio-deadline" : reason);
    runtime_ops.report_schedule(0u);
    survey_rf_retry_reset(&discovery_rf_retry);
    key = k_spin_lock(&survey_lock);
    survey_running = false;
    discovery_report_stage_pending = report_ret < 0;
    discovery_generation_active = report_ret < 0;
    k_spin_unlock(&survey_lock, key);
    if (report_ret < 0) {
        schedule(K_MSEC(SURVEY_NON_RF_SERVICE_POLL_MS));
    }
}

static void survey_work_handler(struct k_work *work)
{
    struct survey_pair pair;
    struct survey_pair_control_id pair_control_id = {0};
    struct survey_discovery_config pending_discovery = {0};
    uint32_t pending_discovery_start_ms = 0u;
    uint32_t pair_deadline_ms = 0u;
    bool as_responder;
    bool report_durable = false;
    bool run_discovery = false;
    bool retry_report_stage = false;
    int64_t uwb_window_start_ms;
    k_spinlock_key_t key;
    int low_power_ret;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    (void)app_anchor_survey_discovery_retry_report();
    key = k_spin_lock(&survey_lock);
    if (discovery_report_stage_pending) {
        pending_discovery = discovery_config;
        retry_report_stage = true;
    }
    if (discovery_pending) {
        pending_discovery = discovery_config;
        pending_discovery_start_ms = discovery_start_ms;
        discovery_pending = false;
        survey_running = true;
        run_discovery = true;
    }
    k_spin_unlock(&survey_lock, key);

    if (retry_report_stage) {
        (void)app_anchor_survey_discovery_retry_report();
        if (app_anchor_survey_discovery_report_staged(
                pending_discovery.survey_id)) {
            key = k_spin_lock(&survey_lock);
            if (discovery_report_stage_pending &&
                discovery_config.survey_id == pending_discovery.survey_id) {
                discovery_report_stage_pending = false;
                discovery_generation_active = false;
            }
            k_spin_unlock(&survey_lock, key);
            runtime_ops.report_schedule(0u);
        } else {
            schedule(K_MSEC(SURVEY_NON_RF_SERVICE_POLL_MS));
        }
        return;
    }

    if (run_discovery) {
        uint32_t discovery_deadline_ms = survey_discovery_radio_deadline_ms(
            &pending_discovery, pending_discovery_start_ms);
        uint32_t retry_delay_ms = 0u;

        if (uptime_deadline_reached(k_uptime_get_32(),
                                    discovery_deadline_ms)) {
            finish_discovery_without_radio(&pending_discovery,
                                           pending_discovery_start_ms,
                                           -ETIMEDOUT,
                                           "radio-deadline");
            return;
        }
        app_node_comm_stop_role_scan();
        ret = radio_guard_uwb_start("survey discovery");
        if (ret < 0) {
            int retry_ret;

            app_node_comm_restart_role_scan();
            retry_ret = survey_rf_retry_delay_ms(
                &discovery_rf_retry,
                pending_discovery.survey_id,
                survey_discovery_radio_opportunity(),
                discovery_deadline_ms,
                &retry_delay_ms);
            if (retry_ret < 0 ||
                app_anchor_survey_runtime_abort_requested()) {
                if (!app_anchor_survey_runtime_abort_requested()) {
                    finish_discovery_without_radio(
                        &pending_discovery,
                        pending_discovery_start_ms,
                        retry_ret,
                        "radio-deferral-terminal");
                } else {
                    survey_rf_retry_reset(&discovery_rf_retry);
                    key = k_spin_lock(&survey_lock);
                    survey_running = false;
                    discovery_generation_active = false;
                    k_spin_unlock(&survey_lock, key);
                }
                return;
            }
            key = k_spin_lock(&survey_lock);
            discovery_pending = true;
            survey_running = false;
            k_spin_unlock(&survey_lock, key);
            schedule(K_MSEC(retry_delay_ms));
            return;
        }
        survey_rf_retry_reset(&discovery_rf_retry);

        runtime_ops.set_uwb_busy(true);
        uwb_window_start_ms = k_uptime_get();
        ret = app_anchor_survey_discovery_run(&pending_discovery,
                                              pending_discovery_start_ms);
        report_durable = ret >= 0;
        if (ret < 0 &&
            !app_anchor_survey_runtime_abort_requested()) {
            int report_ret = app_anchor_survey_discovery_stage_empty_report(
                &pending_discovery, pending_discovery_start_ms);

            report_durable = report_ret == 0 ||
                app_anchor_survey_discovery_report_staged(
                    pending_discovery.survey_id);
            status_debug_printf(
                "DBG_SURVEY_DISCOVERY_FAILSAFE_REPORT survey=%u run_ret=%d report_ret=%d\n",
                pending_discovery.survey_id, ret, report_ret);
        }
        low_power_ret = runtime_ops.enter_low_power(
            app_radio_low_power_mode_for_connection(
                runtime_ops.connected_radio_active()),
            "survey-discovery-exit");
        if (ret >= 0 && low_power_ret < 0) {
            ret = low_power_ret;
        }
        runtime_ops.note_uwb_awake_since(uwb_window_start_ms, 0u);
        runtime_ops.set_uwb_busy(false);
        radio_guard_uwb_stop();
        app_node_comm_restart_role_scan();
        (void)runtime_ops.start_uwb_scan();
        (void)app_anchor_survey_discovery_retry_report();
        runtime_ops.report_schedule(0u);
        survey_rf_retry_reset(&discovery_rf_retry);
        key = k_spin_lock(&survey_lock);
        survey_running = false;
        discovery_report_stage_pending = ret < 0 && !report_durable;
        discovery_generation_active = discovery_report_stage_pending;
        k_spin_unlock(&survey_lock, key);
        if (discovery_report_stage_pending) {
            schedule(K_MSEC(SURVEY_NON_RF_SERVICE_POLL_MS));
        }
        LOG_INF("survey discovery run finished: survey=%u ret=%d",
                pending_discovery.survey_id,
                ret);
        return;
    }

    if (!pair_start_delivery_ready()) {
        return;
    }

    key = k_spin_lock(&survey_lock);
    if (survey_pair_lease_expire(&pair_lease, k_uptime_get_32())) {
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
    }
    if (!pair_start_pending || !pair_lease.start_id_valid ||
        !survey_pair_lease_ready_snapshot(&pair_lease, &pair)) {
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
        k_spin_unlock(&survey_lock, key);
        survey_rf_retry_reset(&pair_rf_retry);
        return;
    }
    pair_control_id = pair_lease.start_id;
    pair_deadline_ms = pair_lease.prepared_deadline_ms;
    as_responder = pair.responder_id == DEVICE_ID;
    k_spin_unlock(&survey_lock, key);

    if (anchor_uwb_window_active() ||
        app_anchor_survey_runtime_discovery_is_pending() ||
        runtime_ops.relay_tx_active()) {
        (void)schedule_pair_rf_retry(&pair_control_id,
                                     pair_deadline_ms,
                                     "radio-owner-busy");
        return;
    }

    if (runtime_ops.report_queue_used() + pair.sample_count >
        REPORT_TX_QUEUE_DEPTH) {
        runtime_ops.report_schedule(0u);
        schedule(K_MSEC(SURVEY_NON_RF_SERVICE_POLL_MS));
        return;
    }

    app_node_comm_stop_role_scan();
    ret = radio_guard_uwb_start(as_responder ? "survey responder DS-TWR" :
                                               "survey initiator DS-TWR");
    if (ret < 0) {
        bool reschedule;

        app_node_comm_restart_role_scan();
        key = k_spin_lock(&survey_lock);
        reschedule = pair_start_pending &&
                     !app_anchor_survey_runtime_abort_requested() &&
                     survey_pair_lease_ready_snapshot(&pair_lease, NULL);
        k_spin_unlock(&survey_lock, key);
        if (reschedule) {
            (void)schedule_pair_rf_retry(&pair_control_id,
                                         pair_deadline_ms,
                                         "radio-guard-busy");
        }
        return;
    }
    survey_rf_retry_reset(&pair_rf_retry);

    key = k_spin_lock(&survey_lock);
    if (survey_pair_lease_expire(&pair_lease, k_uptime_get_32())) {
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
    }
    if (!pair_start_pending ||
        !survey_pair_lease_mark_running(&pair_lease, &pair)) {
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
        k_spin_unlock(&survey_lock, key);
        radio_guard_uwb_stop();
        app_node_comm_restart_role_scan();
        return;
    }
    pair_start_pending = false;
    pair_start_delivery_handle = 0u;
    survey_running = true;
    k_spin_unlock(&survey_lock, key);
    (void)k_work_cancel_delayable(&pair_lease_work);

    runtime_ops.set_uwb_busy(true);
    uwb_window_start_ms = k_uptime_get();
    if (as_responder) {
        ret = run_pair_responder(&pair);
    } else {
        ret = run_pair_initiator(&pair);
    }
    low_power_ret = runtime_ops.enter_low_power(
        app_radio_low_power_mode_for_connection(
            runtime_ops.connected_radio_active()),
        "survey-pair-exit");
    if (ret >= 0 && low_power_ret < 0) {
        ret = low_power_ret;
    }
    runtime_ops.note_uwb_awake_since(uwb_window_start_ms, 0u);
    runtime_ops.set_uwb_busy(false);
    radio_guard_uwb_stop();
    app_node_comm_restart_role_scan();
    runtime_ops.report_schedule(0u);
    key = k_spin_lock(&survey_lock);
    survey_running = false;
    (void)survey_pair_lease_finish(&pair_lease);
    k_spin_unlock(&survey_lock, key);

    LOG_INF("survey pair run finished: survey=%u role=%s ret=%d aborted=%u",
            pair.survey_id,
            as_responder ? "responder" : "initiator",
            ret,
            app_anchor_survey_runtime_abort_requested() ? 1u : 0u);
}

int app_anchor_survey_runtime_start_pair_from_command(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    enum command_status *status,
    uint8_t *reason)
{
    struct survey_pair pair = {0};
    struct survey_pair_control_id control_id;
    enum survey_pair_lease_decision decision;
    bool as_responder;
    uint32_t superseded_delivery_handle = 0u;
    uint16_t round_id = SURVEY_LEGACY_ROUND_ID;
    k_spinlock_key_t key;
    int ret;

    if (status == NULL || reason == NULL) {
        return -EINVAL;
    }

    ret = app_operation_policy_apply_payload(payload, payload_len, 0u, NULL);
    if (ret == 0) {
        ret = survey_extract_pair_tlvs(payload, payload_len, &pair);
    }
    if (ret == PROTO_OK) {
        ret = survey_round_id_extract_tlv(payload, payload_len, &round_id);
    }
    if (ret != PROTO_OK || packet == NULL) {
        *status = COMMAND_MALFORMED_PAYLOAD;
        *reason = (uint8_t)(ret == PROTO_OK ? 1u : -ret);
        return -EINVAL;
    }
    if (pair.initiator_id != DEVICE_ID && pair.responder_id != DEVICE_ID) {
        *status = COMMAND_DENIED;
        *reason = 2u;
        return -EINVAL;
    }
    if (!pair_queueable(&pair)) {
        *status = COMMAND_DENIED;
        *reason = 4u;
        return -EINVAL;
    }
    if (anchor_uwb_window_active()) {
        *status = COMMAND_BUSY;
        *reason = 3u;
        return -EBUSY;
    }
    key = k_spin_lock(&survey_lock);
    control_id = (struct survey_pair_control_id) {
        .session_id = packet->session_id,
        .command_seq = packet->seq,
    };
    decision = survey_pair_lease_start_round(&pair_lease,
                                             &pair,
                                             round_id,
                                             &control_id,
                                             k_uptime_get_32());
    if (decision == SURVEY_PAIR_LEASE_BUSY) {
        *status = COMMAND_BUSY;
        *reason = 3u;
        k_spin_unlock(&survey_lock, key);
        return -EBUSY;
    }
    if (decision != SURVEY_PAIR_LEASE_ACCEPTED &&
        decision != SURVEY_PAIR_LEASE_DUPLICATE) {
        k_spin_unlock(&survey_lock, key);
        *status = COMMAND_INVALID_STATE;
        *reason = decision == SURVEY_PAIR_LEASE_EXPIRED ? 5u : 4u;
        return -EINVAL;
    }

    as_responder = pair.responder_id == DEVICE_ID;
    if (decision == SURVEY_PAIR_LEASE_ACCEPTED) {
        superseded_delivery_handle = pair_start_delivery_handle;
        pair_start_pending = true;
        pair_start_delivery_handle = 0u;
        atomic_set(&abort_requested, 0);
    }
    k_spin_unlock(&survey_lock, key);
    abandon_pair_start_delivery(superseded_delivery_handle,
                                "accepted-new-start");
    *status = COMMAND_OK;
    *reason = 0u;
    LOG_INF("survey pair start %s: survey=%u round=%u initiator=0x%016llx responder=0x%016llx samples=%u local_role=%s",
            decision == SURVEY_PAIR_LEASE_ACCEPTED ? "accepted" : "duplicate",
            pair.survey_id,
            round_id,
            (unsigned long long)pair.initiator_id,
            (unsigned long long)pair.responder_id,
            pair.sample_count,
            as_responder ? "responder" : "initiator");
    return 0;
}

int app_anchor_survey_runtime_go_round_from_command(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    enum command_status *status,
    uint8_t *reason)
{
    struct survey_round_go go = {0};
    enum survey_pair_lease_decision decision;
    bool ready;
    k_spinlock_key_t key;
    int ret;

    if (packet == NULL || payload == NULL || status == NULL || reason == NULL ||
        packet->msg_type != MSG_COMMAND || packet->src_id != GATEWAY_ID ||
        packet->dst_id != MESH_BROADCAST_ID) {
        return -EINVAL;
    }
    ret = survey_round_go_from_tlvs(payload, payload_len, &go);
    if (ret != PROTO_OK || packet->session_id != go.survey_id) {
        *status = COMMAND_MALFORMED_PAYLOAD;
        *reason = (uint8_t)(ret == PROTO_OK ? 1u : -ret);
        return -EINVAL;
    }

    key = k_spin_lock(&survey_lock);
    decision = survey_pair_lease_go(&pair_lease,
                                    go.survey_id,
                                    go.round_id,
                                    k_uptime_get_32());
    ready = pair_start_pending &&
            survey_pair_lease_ready_snapshot(&pair_lease, NULL);
    k_spin_unlock(&survey_lock, key);

    if (decision != SURVEY_PAIR_LEASE_ACCEPTED &&
        decision != SURVEY_PAIR_LEASE_DUPLICATE) {
        *status = decision == SURVEY_PAIR_LEASE_BUSY ? COMMAND_BUSY :
                  COMMAND_INVALID_STATE;
        *reason = decision == SURVEY_PAIR_LEASE_EXPIRED ? 5u : 4u;
        return decision == SURVEY_PAIR_LEASE_BUSY ? -EBUSY : -ESTALE;
    }
    *status = COMMAND_OK;
    *reason = 0u;
    if (ready) {
        schedule(K_NO_WAIT);
    }
    LOG_INF("survey round GO %s: survey=%u round=%u ready=%u",
            decision == SURVEY_PAIR_LEASE_ACCEPTED ? "accepted" : "duplicate",
            go.survey_id,
            go.round_id,
            ready ? 1u : 0u);
    return 0;
}

int app_anchor_survey_runtime_bind_pair_start_delivery(
    const struct proto_packet *command,
    uint32_t delivery_handle)
{
    bool bound = false;
    k_spinlock_key_t key;

    if (command == NULL || command->session_id == 0u ||
        command->seq == 0u || delivery_handle == 0u) {
        return -EINVAL;
    }

    key = k_spin_lock(&survey_lock);
    if (pair_start_pending &&
        pair_lease.phase == SURVEY_PAIR_LEASE_START_PENDING &&
        pair_lease.start_id_valid &&
        pair_lease.start_id.session_id == command->session_id &&
        pair_lease.start_id.command_seq == command->seq) {
        if (pair_start_delivery_handle == 0u ||
            pair_start_delivery_handle == delivery_handle) {
            pair_start_delivery_handle = delivery_handle;
            bound = true;
        }
    }
    k_spin_unlock(&survey_lock, key);

    if (!bound) {
        return -ESTALE;
    }
    schedule(K_NO_WAIT);
    return 0;
}

bool app_anchor_survey_runtime_cancel_pair_start(
    const struct proto_packet *command)
{
    bool cancelled = false;
    uint32_t delivery_handle = 0u;
    k_spinlock_key_t key;

    if (command == NULL) {
        return false;
    }
    key = k_spin_lock(&survey_lock);
    if (pair_start_pending &&
        pair_lease.phase == SURVEY_PAIR_LEASE_START_PENDING &&
        pair_lease.start_id_valid &&
        pair_lease.start_id.session_id == command->session_id &&
        pair_lease.start_id.command_seq == command->seq) {
        cancelled = survey_pair_lease_abort(&pair_lease);
        delivery_handle = pair_start_delivery_handle;
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
    }
    k_spin_unlock(&survey_lock, key);
    if (cancelled) {
        (void)k_work_cancel_delayable(&pair_lease_work);
        abandon_pair_start_delivery(delivery_handle, "cancel-pair-start");
    }
    return cancelled;
}

static void pair_lease_work_handler(struct k_work *work)
{
    struct survey_pair expired_pair = {0};
    enum survey_pair_lease_phase expired_phase = SURVEY_PAIR_LEASE_IDLE;
    uint32_t delivery_handle = 0u;
    bool expired;
    k_spinlock_key_t key;

    ARG_UNUSED(work);

    key = k_spin_lock(&survey_lock);
    if (pair_lease.phase == SURVEY_PAIR_LEASE_PREPARED ||
        pair_lease.phase == SURVEY_PAIR_LEASE_START_PENDING) {
        expired_pair = pair_lease.pair;
        expired_phase = pair_lease.phase;
    }
    expired = survey_pair_lease_expire(&pair_lease, k_uptime_get_32());
    if (expired) {
        delivery_handle = pair_start_delivery_handle;
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
    }
    k_spin_unlock(&survey_lock, key);

    if (expired) {
        abandon_pair_start_delivery(delivery_handle, "pair-lease-expired");
        LOG_WRN("survey pair lease expired: phase=%u survey=%u initiator=0x%016llx responder=0x%016llx",
                (unsigned int)expired_phase,
                expired_pair.survey_id,
                (unsigned long long)expired_pair.initiator_id,
                (unsigned long long)expired_pair.responder_id);
    }
}

void app_anchor_survey_runtime_abort_pair(void)
{
    bool pair_active;
    uint32_t delivery_handle;
    k_spinlock_key_t key;

    key = k_spin_lock(&survey_lock);
    pair_active = pair_lease.phase != SURVEY_PAIR_LEASE_IDLE;
    if (pair_active) {
        atomic_set(&abort_requested, 1);
        (void)survey_pair_lease_abort(&pair_lease);
    }
    delivery_handle = pair_start_delivery_handle;
    pair_start_pending = false;
    pair_start_delivery_handle = 0u;
    k_spin_unlock(&survey_lock, key);
    (void)k_work_cancel_delayable(&pair_lease_work);
    abandon_pair_start_delivery(delivery_handle, "abort-pair");
    LOG_INF("survey pair state abort requested: active=%u",
            pair_active ? 1u : 0u);
}

bool app_anchor_survey_runtime_abort_pair_matching(
    const struct survey_pair *pair,
    uint32_t session_id)
{
    bool matched;
    uint32_t delivery_handle = 0u;
    k_spinlock_key_t key;

    if (pair == NULL) {
        return false;
    }
    key = k_spin_lock(&survey_lock);
    matched = survey_pair_lease_abort_matching(&pair_lease,
                                               pair,
                                               session_id);
    if (matched) {
        atomic_set(&abort_requested, 1);
        delivery_handle = pair_start_delivery_handle;
        pair_start_pending = false;
        pair_start_delivery_handle = 0u;
    }
    k_spin_unlock(&survey_lock, key);
    if (matched) {
        (void)k_work_cancel_delayable(&pair_lease_work);
        abandon_pair_start_delivery(delivery_handle, "targeted-abort");
    }
    LOG_INF("survey pair targeted abort: matched=%u survey=%u initiator=0x%016llx responder=0x%016llx",
            matched ? 1u : 0u,
            pair->survey_id,
            (unsigned long long)pair->initiator_id,
            (unsigned long long)pair->responder_id);
    return matched;
}

int app_anchor_survey_runtime_init(
    const struct app_anchor_survey_runtime_ops *ops)
{
    if (ops == NULL || ops->send_command_result == NULL ||
        ops->enter_low_power == NULL || ops->set_uwb_busy == NULL ||
        ops->note_uwb_awake_since == NULL || ops->start_uwb_scan == NULL ||
        ops->queue_sample_result == NULL ||
        ops->report_queue_used == NULL || ops->report_schedule == NULL ||
        ops->relay_tx_active == NULL ||
        ops->connected_radio_active == NULL) {
        return -EINVAL;
    }
#if DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    if (ops->work_queue == NULL) {
        return -EINVAL;
    }
#endif

    runtime_ops = *ops;
    runtime_initialized = true;
    return 0;
}

int app_anchor_survey_runtime_start(void)
{
    if (!runtime_initialized) {
        return -EINVAL;
    }

    k_work_init_delayable(&survey_work, survey_work_handler);
    survey_pair_lease_reset(&pair_lease);
    pair_start_pending = false;
    pair_start_delivery_handle = 0u;
    discovery_pending = false;
    discovery_generation_active = false;
    discovery_report_stage_pending = false;
    survey_rf_retry_reset(&discovery_rf_retry);
    survey_rf_retry_reset(&pair_rf_retry);
    k_work_init_delayable(&pair_lease_work, pair_lease_work_handler);
    return 0;
}

#include "app_anchor_survey_discovery.h"

#include "app_config.h"
#include "app_board.h"
#include "app_high_debug.h"
#include "app_mesh_local_delivery.h"
#include "app_mesh_persistence.h"
#include "app_mesh_report.h"
#include "app_node_comm.h"
#include "app_stack_workload_diag.h"
#include "app_state.h"
#include "dwm3000_driver.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "route.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_DECLARE(app_anchor, LOG_LEVEL_DBG);

static struct app_anchor_survey_discovery_ops discovery_ops;
static bool discovery_initialized;
static uint16_t survey_delivery_retry_round;
static uint32_t survey_delivery_handle;
static bool survey_delivery_comm_owned;
static struct app_mesh_local_delivery_identity survey_delivery_comm_identity;
static bool survey_delivery_comm_identity_valid;
static struct mesh_outbound survey_report_stage_retry_outbound;
static uint32_t survey_report_stage_retry_generation;
static bool survey_report_stage_retry_valid;

#define SURVEY_DELIVERY_EVENT_POLL_MS 100u

static struct app_mesh_local_delivery *survey_delivery_instance(void)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    static struct app_mesh_local_delivery delivery;

    return &delivery;
#else
    return NULL;
#endif
}

#if DEVICE_ROLE == ROLE_ANCHOR
K_MUTEX_DEFINE(survey_delivery_lock);
#define SURVEY_DELIVERY_LOCK() k_mutex_lock(&survey_delivery_lock, K_FOREVER)
#define SURVEY_DELIVERY_UNLOCK() k_mutex_unlock(&survey_delivery_lock)
#else
#define SURVEY_DELIVERY_LOCK() do { } while (0)
#define SURVEY_DELIVERY_UNLOCK() do { } while (0)
#endif

static void schedule_work_ms(uint32_t delay_ms)
{
    if (discovery_ops.schedule_work_ms != NULL) {
        discovery_ops.schedule_work_ms(delay_ms);
    }
}

/* Caller holds survey_delivery_lock when the anchor role is active. */
static uint32_t survey_delivery_next_retry_delay_ms(
    const struct mesh_outbound *outbound)
{
    uint32_t delay_ms = SURVEY_DISCOVERY_REPORT_RETRY_INITIAL_MS;

    if (survey_delivery_retry_round < UINT16_MAX) {
        survey_delivery_retry_round++;
    }
    if (outbound == NULL ||
        app_node_comm_retry_backoff_ms(
            outbound, NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
            survey_delivery_retry_round, &delay_ms) < 0) {
        delay_ms = SURVEY_DISCOVERY_REPORT_RETRY_MAX_MS;
    }
    if (outbound != NULL) {
        status_debug_printf(
            "DBG_SURVEY_REPORT_BACKOFF survey=%u seq=%u round=%u ms=%u\n",
            outbound->packet.session_id,
            outbound->packet.seq,
            survey_delivery_retry_round,
            delay_ms);
    }
    return delay_ms;
}

static bool abort_requested(void)
{
    return discovery_ops.abort_requested != NULL &&
           discovery_ops.abort_requested();
}

static bool survey_delivery_identity_equal(
    const struct app_mesh_local_delivery_identity *left,
    const struct app_mesh_local_delivery_identity *right)
{
    return left != NULL && right != NULL &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->msg_type == right->msg_type;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int survey_delivery_save(
    void *ctx,
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    ARG_UNUSED(ctx);
    return app_mesh_persistence_save_local_delivery(snapshot);
}

static int survey_delivery_clear(void *ctx)
{
    ARG_UNUSED(ctx);
    return app_mesh_persistence_clear_local_delivery();
}

static int survey_delivery_attempt_begin(const struct proto_packet *packet,
                                         uint8_t *attempt_token)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct app_mesh_local_delivery_identity identity;
    int ret;

    if (packet == NULL || attempt_token == NULL || delivery == NULL) {
        return -EINVAL;
    }
    SURVEY_DELIVERY_LOCK();
    if (!app_mesh_local_delivery_active(delivery) ||
        app_mesh_local_delivery_outbound(delivery) == NULL) {
        SURVEY_DELIVERY_UNLOCK();
        return -ENOENT;
    }
    app_mesh_local_delivery_identity_from_outbound(
        app_mesh_local_delivery_outbound(delivery), &identity);
    if (!app_mesh_local_delivery_identity_matches(&identity, packet)) {
        SURVEY_DELIVERY_UNLOCK();
        return -ESTALE;
    }
    if (delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_STARTING ||
        delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_TRACKED) {
        ret = app_mesh_local_delivery_note_attempt_released(
            delivery,
            delivery->snapshot.attempt_token,
            APP_MESH_LOCAL_DELIVERY_RETRY);
        if (ret < 0) {
            SURVEY_DELIVERY_UNLOCK();
            return ret;
        }
    }
    ret = app_mesh_local_delivery_begin_attempt(delivery, attempt_token);
    SURVEY_DELIVERY_UNLOCK();
    return ret;
}

static int survey_delivery_attempt_complete(const struct proto_packet *packet,
                                            uint8_t attempt_token,
                                            bool rf_started)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct app_mesh_local_delivery_identity identity;
    uint16_t attempts_remaining;
    int ret;

    if (packet == NULL || attempt_token == 0u || delivery == NULL) {
        return -EINVAL;
    }
    SURVEY_DELIVERY_LOCK();
    if (!app_mesh_local_delivery_active(delivery) ||
        app_mesh_local_delivery_outbound(delivery) == NULL) {
        SURVEY_DELIVERY_UNLOCK();
        return -ENOENT;
    }
    app_mesh_local_delivery_identity_from_outbound(
        app_mesh_local_delivery_outbound(delivery), &identity);
    if (!app_mesh_local_delivery_identity_matches(&identity, packet) ||
        delivery->snapshot.attempt_token != attempt_token) {
        SURVEY_DELIVERY_UNLOCK();
        return -ESTALE;
    }
    ret = rf_started ?
        app_mesh_local_delivery_note_attempt_sent(delivery, attempt_token) :
        app_mesh_local_delivery_note_attempt_not_sent(
            delivery, attempt_token, APP_MESH_LOCAL_DELIVERY_RETRY);
    attempts_remaining = app_mesh_local_delivery_attempts_available(delivery);
    SURVEY_DELIVERY_UNLOCK();
    if (ret == 0 && rf_started) {
        app_stack_workload_diag_anchor_survey_sample(packet, 1u, 1u);
        status_debug_printf(
            "DBG_SURVEY_REPORT_RF_ATTEMPT survey=%u seq=%u token=%u remaining=%u\n",
            packet->session_id,
            packet->seq,
            attempt_token,
            attempts_remaining);
    }
    return ret;
}
#endif

int app_anchor_survey_discovery_init(
    const struct app_anchor_survey_discovery_ops *ops)
{
    if (ops == NULL || ops->abort_requested == NULL ||
        ops->abort_pair == NULL || ops->preempt_radio == NULL ||
        ops->admit_start == NULL || ops->queue_start == NULL ||
        ops->schedule_work_ms == NULL ||
        ops->next_sequence == NULL || ops->seed_sequence == NULL) {
        return -EINVAL;
    }

    discovery_ops = *ops;
    discovery_initialized = true;
#if DEVICE_ROLE == ROLE_ANCHOR
    {
        const struct app_mesh_local_delivery_ops delivery_ops = {
            .save = survey_delivery_save,
            .clear = survey_delivery_clear,
        };

        SURVEY_DELIVERY_LOCK();
        app_mesh_local_delivery_init(survey_delivery_instance(), &delivery_ops);
        survey_delivery_retry_round = 0u;
        survey_delivery_handle = 0u;
        survey_delivery_comm_owned = false;
        memset(&survey_delivery_comm_identity, 0,
               sizeof(survey_delivery_comm_identity));
        survey_delivery_comm_identity_valid = false;
        memset(&survey_report_stage_retry_outbound, 0,
               sizeof(survey_report_stage_retry_outbound));
        survey_report_stage_retry_generation = 0u;
        survey_report_stage_retry_valid = false;
        SURVEY_DELIVERY_UNLOCK();
        {
            const struct app_node_comm_durable_attempt_ops attempt_ops = {
                .begin = survey_delivery_attempt_begin,
                .complete = survey_delivery_attempt_complete,
            };

            if (app_node_comm_register_durable_attempt_ops(&attempt_ops) < 0) {
                return -EIO;
            }
        }
    }
#endif
    return 0;
}

int app_anchor_survey_discovery_restore(bool *restored)
{
    struct app_mesh_local_delivery_recovery recovery = {0};
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    int ret;

    if (restored == NULL) {
        return -EINVAL;
    }
    *restored = false;
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return 0;
    }
    if (!discovery_initialized || delivery == NULL) {
        return -EACCES;
    }

    SURVEY_DELIVERY_LOCK();
    survey_delivery_handle = 0u;
    survey_delivery_comm_owned = false;
    memset(&survey_delivery_comm_identity, 0,
           sizeof(survey_delivery_comm_identity));
    survey_delivery_comm_identity_valid = false;
    ret = app_mesh_persistence_restore_local_delivery(&delivery->snapshot);
    ret = app_mesh_local_delivery_recover(delivery,
                                          &delivery->snapshot,
                                          ret,
                                          &recovery);
    if (recovery.restored) {
        const struct mesh_outbound *outbound =
            app_mesh_local_delivery_outbound(delivery);
        uint16_t attempts_used = APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS -
            delivery->snapshot.attempts_remaining;
        int rebase_ret = app_mesh_local_delivery_rebase_after_boot(
            delivery, k_uptime_get_32());

        survey_delivery_retry_round = attempts_used;
        if (outbound != NULL) {
            discovery_ops.seed_sequence(outbound->packet.seq);
        }
        if (rebase_ret < 0) {
            LOG_WRN("survey delivery boot-time rebase persistence deferred: ret=%d",
                    rebase_ret);
            status_debug_printf(
                "DBG_SURVEY_DELIVERY_REBASE_DEFER ret=%d now=%u\n",
                rebase_ret,
                k_uptime_get_32());
        }
    }
    *restored = recovery.restored || recovery.retry_required;
    if (recovery.quarantined) {
        LOG_ERR("survey delivery journal invalid/unavailable; continuing quarantined: restore=%d clear=%d",
                recovery.source_error,
                recovery.clear_error);
        status_debug_printf("DBG_SURVEY_DELIVERY_JOURNAL_QUARANTINE restore=%d clear=%d\n",
                            recovery.source_error,
                            recovery.clear_error);
    }
    SURVEY_DELIVERY_UNLOCK();
    return ret;
}

void app_anchor_survey_delivery_gateway_confirmed(const struct proto_packet *packet)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    uint32_t completed_handle = 0u;
    bool ack_committed = false;

    if (DEVICE_ROLE != ROLE_ANCHOR || packet == NULL || delivery == NULL) {
        return;
    }
    SURVEY_DELIVERY_LOCK();
    if (!app_mesh_local_delivery_active(delivery)) {
        SURVEY_DELIVERY_UNLOCK();
        return;
    }
    if (app_mesh_local_delivery_note_ack(delivery, packet) < 0) {
        LOG_ERR("survey delivery ACK journal commit failed");
    } else {
        ack_committed = true;
        if (survey_delivery_comm_owned &&
            survey_delivery_comm_identity_valid &&
            app_mesh_local_delivery_identity_matches(
                &survey_delivery_comm_identity, packet)) {
            completed_handle = survey_delivery_handle;
            survey_delivery_handle = 0u;
            survey_delivery_comm_owned = false;
            memset(&survey_delivery_comm_identity, 0,
                   sizeof(survey_delivery_comm_identity));
            survey_delivery_comm_identity_valid = false;
        }
        survey_delivery_retry_round = 0u;
        app_stack_workload_diag_anchor_survey_sample(packet, 0u, 0u);
        app_stack_workload_diag_anchor_survey_release(packet, 0, 0u, 0u);
        status_debug_printf("DBG_SURVEY_REPORT_ACK anchor=0x%016llx survey=%u seq=%u\n",
                            (unsigned long long)DEVICE_ID,
                            packet->session_id,
                            packet->seq);
        LOG_INF("survey delivery gateway ACK committed: survey=%u seq=%u",
                packet->session_id, packet->seq);
    }
    SURVEY_DELIVERY_UNLOCK();
    if (completed_handle != 0u) {
        (void)app_node_comm_auto_reap_delivery(completed_handle);
    }
    if (ack_committed) {
        schedule_work_ms(0u);
    }
}

void app_anchor_survey_delivery_transport_released(
    const struct proto_packet *packet,
    bool preempted)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct app_mesh_local_delivery_identity identity;
    uint8_t attempt_token;
    uint32_t retry_delay_ms;

    if (DEVICE_ROLE != ROLE_ANCHOR || packet == NULL || delivery == NULL) {
        return;
    }
    SURVEY_DELIVERY_LOCK();
    if (!app_mesh_local_delivery_active(delivery)) {
        SURVEY_DELIVERY_UNLOCK();
        return;
    }
    app_mesh_local_delivery_identity_from_outbound(
        &delivery->snapshot.outbound, &identity);
    if (!app_mesh_local_delivery_identity_matches(&identity, packet)) {
        SURVEY_DELIVERY_UNLOCK();
        return;
    }
    if (delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_STARTING ||
        delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_TRACKED) {
        attempt_token = delivery->snapshot.attempt_token;
        (void)app_mesh_local_delivery_note_attempt_released(
            delivery, attempt_token,
            preempted ? APP_MESH_LOCAL_DELIVERY_PREEMPTED :
                        APP_MESH_LOCAL_DELIVERY_RETRY);
    }
    retry_delay_ms = survey_delivery_next_retry_delay_ms(
        app_mesh_local_delivery_outbound(delivery));
    SURVEY_DELIVERY_UNLOCK();
    schedule_work_ms(retry_delay_ms);
}

static int prepare_discovery_report(
    uint32_t survey_id,
    const struct survey_reachability_entry *entries,
    size_t entry_count,
    uint32_t earliest_tx_ms,
    enum command_status status)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct mesh_outbound outbound = {0};
    size_t report_payload_len = 0u;
    int ret;

    if (survey_id == 0u || (entries == NULL && entry_count != 0u) ||
        delivery == NULL || discovery_ops.next_sequence == NULL) {
        return -EINVAL;
    }

    ret = survey_append_reach_report_tlvs(outbound.payload,
                                          sizeof(outbound.payload),
                                          &report_payload_len,
                                          survey_id,
                                          DEVICE_ID,
                                          entries,
                                          entry_count);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = tlv_append_u16(outbound.payload,
                         sizeof(outbound.payload),
                         &report_payload_len,
                         TLV_COMMAND_STATUS,
                         (uint16_t)status);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = survey_init_discovery_report_packet(&outbound.packet,
                                              DEVICE_ID,
                                              GATEWAY_ID,
                                              survey_id,
                                              discovery_ops.next_sequence(),
                                              (uint8_t)report_payload_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound.payload_len = (uint8_t)report_payload_len;
    outbound.earliest_tx_ms = earliest_tx_ms;

    SURVEY_DELIVERY_LOCK();
    if (survey_report_stage_retry_valid) {
        ret = -EBUSY;
    } else {
        ret = app_mesh_local_delivery_stage(delivery, &outbound, survey_id);
        if (ret < 0 && !app_mesh_local_delivery_active(delivery)) {
            survey_report_stage_retry_outbound = outbound;
            survey_report_stage_retry_generation = survey_id;
            survey_report_stage_retry_valid = true;
        }
    }
    if (ret == 0) {
        survey_delivery_retry_round = 0u;
    }
    SURVEY_DELIVERY_UNLOCK();
    if (ret == 0) {
        app_stack_workload_diag_anchor_survey_admit(&outbound.packet, 1u, 1u);
        status_debug_printf("DBG_SURVEY_REPORT_STAGED anchor=0x%016llx survey=%u seq=%u peers=%u\n",
                            (unsigned long long)DEVICE_ID,
                            outbound.packet.session_id,
                            outbound.packet.seq,
                            (unsigned int)entry_count);
    }
    return ret;
}

int app_anchor_survey_discovery_stage_empty_report(
    const struct survey_discovery_config *config,
    uint32_t start_ms)
{
    uint32_t report_delay_ms = 0u;
    uint8_t report_slot;
    int ret;

    if (config == NULL ||
        survey_discovery_config_validate(config) != PROTO_OK) {
        return -EINVAL;
    }
    report_slot = local_survey_discovery_slot(config->slot_count);
    ret = survey_discovery_report_delay_ms(config, report_slot,
                                           SURVEY_RESULT_MESH_SLOT_MS,
                                           &report_delay_ms);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = prepare_discovery_report(config->survey_id, NULL, 0u,
                                   start_ms + report_delay_ms,
                                   COMMAND_RADIO_ERROR);
    if (ret == 0) {
        status_debug_printf(
            "DBG_SURVEY_EMPTY_REPORT_STAGED survey=%u run_retained=1\n",
            config->survey_id);
    }
    return ret;
}

/* Caller holds survey_delivery_lock when the anchor role is active. */
static bool survey_delivery_matches_locked(
    const struct app_mesh_local_delivery *delivery,
    const struct mesh_outbound *outbound)
{
    struct app_mesh_local_delivery_identity identity;

    if (!app_mesh_local_delivery_active(delivery) || outbound == NULL ||
        app_mesh_local_delivery_outbound(delivery) == NULL) {
        return false;
    }
    app_mesh_local_delivery_identity_from_outbound(
        app_mesh_local_delivery_outbound(delivery), &identity);
    return app_mesh_local_delivery_identity_matches(&identity,
                                                     &outbound->packet);
}

static uint64_t survey_delivery_deadline_ms(
    const struct mesh_outbound *outbound)
{
    const struct route_candidate *selected =
        route_selected(&mesh_runtime.upstream);
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint64_t delay_ms = 0u;
    uint8_t gateway_hop_count = SURVEY_DEFAULT_TTL;
    uint32_t custody_ms;

    if (selected != NULL && selected->hop_count < SURVEY_DEFAULT_TTL) {
        gateway_hop_count = selected->hop_count + 1u;
    }
    custody_ms = survey_discovery_report_custody_ms(gateway_hop_count);

    if (outbound != NULL && outbound->earliest_tx_ms != 0u &&
        !uptime_deadline_reached((uint32_t)now_ms,
                                 outbound->earliest_tx_ms)) {
        delay_ms = uptime_ms_until_deadline((uint32_t)now_ms,
                                            outbound->earliest_tx_ms);
    }
    if (UINT64_MAX - now_ms < delay_ms ||
        UINT64_MAX - now_ms - delay_ms < custody_ms) {
        return UINT64_MAX;
    }
    return now_ms + delay_ms + custody_ms;
}

static int survey_delivery_poll_comm_result(void)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct app_mesh_local_delivery_identity owned_identity = {0};
    struct node_comm_terminal_event event;
    struct mesh_outbound outbound = {0};
    uint32_t handle;
    uint32_t retry_delay_ms = 0u;
    bool current_delivery = false;
    bool discard_complete = false;
    int persist_ret = 0;

    SURVEY_DELIVERY_LOCK();
    if (!survey_delivery_comm_owned) {
        SURVEY_DELIVERY_UNLOCK();
        return -ENOENT;
    }
    handle = survey_delivery_handle;
    owned_identity = survey_delivery_comm_identity;
    SURVEY_DELIVERY_UNLOCK();

    if (handle == 0u) {
        schedule_work_ms(SURVEY_DELIVERY_EVENT_POLL_MS);
        return -EINPROGRESS;
    }
    if (!app_node_comm_take_delivery_event_for(handle, &event)) {
        SURVEY_DELIVERY_LOCK();
        current_delivery = survey_delivery_comm_owned &&
                           survey_delivery_handle == handle &&
                           survey_delivery_comm_identity_valid;
        SURVEY_DELIVERY_UNLOCK();
        if (current_delivery) {
            schedule_work_ms(SURVEY_DELIVERY_EVENT_POLL_MS);
            return -EINPROGRESS;
        }
        return 0;
    }

    SURVEY_DELIVERY_LOCK();
    if (!survey_delivery_comm_owned || survey_delivery_handle != handle ||
        !survey_delivery_comm_identity_valid ||
        !survey_delivery_identity_equal(&survey_delivery_comm_identity,
                                        &owned_identity)) {
        SURVEY_DELIVERY_UNLOCK();
        return 0;
    }
    survey_delivery_comm_owned = false;
    survey_delivery_handle = 0u;
    if (app_mesh_local_delivery_active(delivery) &&
        app_mesh_local_delivery_outbound(delivery) != NULL &&
        app_mesh_local_delivery_identity_matches(
            &owned_identity,
            &app_mesh_local_delivery_outbound(delivery)->packet)) {
        outbound = *app_mesh_local_delivery_outbound(delivery);
        current_delivery = true;
    }
    memset(&survey_delivery_comm_identity, 0,
           sizeof(survey_delivery_comm_identity));
    survey_delivery_comm_identity_valid = false;
    if (current_delivery && event.reason == NODE_COMM_TERMINAL_CANCELLED) {
        retry_delay_ms = survey_delivery_next_retry_delay_ms(&outbound);
    } else if (current_delivery &&
               event.reason != NODE_COMM_TERMINAL_DELIVERED) {
        persist_ret = app_mesh_local_delivery_note_failed(delivery);
        if (persist_ret == 0) {
            persist_ret = app_mesh_local_delivery_discard_failed(delivery);
            discard_complete = persist_ret == 0;
        }
    }
    SURVEY_DELIVERY_UNLOCK();

    if (!current_delivery) {
        return event.reason == NODE_COMM_TERMINAL_DELIVERED ? 0 : -ESTALE;
    }
    if (event.reason == NODE_COMM_TERMINAL_DELIVERED) {
        app_anchor_survey_delivery_gateway_confirmed(&outbound.packet);
        return 0;
    }
    if (event.reason == NODE_COMM_TERMINAL_CANCELLED) {
        schedule_work_ms(retry_delay_ms);
        return -EAGAIN;
    }
    if (persist_ret < 0) {
        LOG_ERR("survey delivery terminal journal release failed: survey=%u seq=%u reason=%u ret=%d",
                outbound.packet.session_id, outbound.packet.seq,
                (unsigned int)event.reason, persist_ret);
        schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
        return -EAGAIN;
    }
    if (discard_complete) {
        survey_delivery_retry_round = 0u;
        app_stack_workload_diag_anchor_survey_release(
            &outbound.packet, -ETIMEDOUT, event.attempts_started, 0u);
        status_debug_printf(
            "DBG_SURVEY_REPORT_TERMINAL survey=%u seq=%u reason=%u attempts=%u\n",
            outbound.packet.session_id,
            outbound.packet.seq,
            (unsigned int)event.reason,
            event.attempts_started);
    }
    return -ETIMEDOUT;
}

int app_anchor_survey_discovery_retry_report(void)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct mesh_outbound outbound;
    uint32_t delivery_handle = 0u;
    uint32_t stale_handle = 0u;
    uint32_t retry_delay_ms = 0u;
    uint64_t absolute_deadline_ms;
    bool submission_owned;
    bool still_current;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || delivery == NULL) {
        return 0;
    }
    ret = survey_delivery_poll_comm_result();
    if (ret != -ENOENT) {
        return ret;
    }
    SURVEY_DELIVERY_LOCK();
    if (survey_report_stage_retry_valid &&
        !app_mesh_local_delivery_active(delivery)) {
        ret = app_mesh_local_delivery_stage(
            delivery,
            &survey_report_stage_retry_outbound,
            survey_report_stage_retry_generation);
        if (ret == 0) {
            status_debug_printf(
                "DBG_SURVEY_REPORT_STAGE_RETRY_OK survey=%u seq=%u\n",
                survey_report_stage_retry_outbound.packet.session_id,
                survey_report_stage_retry_outbound.packet.seq);
            memset(&survey_report_stage_retry_outbound, 0,
                   sizeof(survey_report_stage_retry_outbound));
            survey_report_stage_retry_generation = 0u;
            survey_report_stage_retry_valid = false;
            survey_delivery_retry_round = 0u;
        } else {
            uint32_t survey_id = survey_report_stage_retry_generation;

            SURVEY_DELIVERY_UNLOCK();
            status_debug_printf(
                "DBG_SURVEY_REPORT_STAGE_RETRY_WAIT survey=%u ret=%d\n",
                survey_id,
                ret);
            schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return -EAGAIN;
        }
    }
    if (!app_mesh_local_delivery_active(delivery)) {
        SURVEY_DELIVERY_UNLOCK();
        return 0;
    }
    if (delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_RECOVERY_WAIT) {
        struct app_mesh_local_delivery_snapshot recovered_snapshot = {0};
        struct app_mesh_local_delivery_recovery recovery = {0};

        ret = app_mesh_persistence_restore_local_delivery(&recovered_snapshot);
        (void)app_mesh_local_delivery_recover(delivery,
                                              &recovered_snapshot,
                                              ret,
                                              &recovery);
        if (recovery.quarantined) {
            status_debug_printf("DBG_SURVEY_DELIVERY_JOURNAL_QUARANTINE restore=%d clear=%d\n",
                                recovery.source_error,
                                recovery.clear_error);
        }
        if (recovery.retry_required) {
            SURVEY_DELIVERY_UNLOCK();
            schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return -EAGAIN;
        }
        if (!app_mesh_local_delivery_active(delivery)) {
            SURVEY_DELIVERY_UNLOCK();
            return 0;
        }
    }
    if (app_mesh_local_delivery_outbound(delivery) == NULL) {
        SURVEY_DELIVERY_UNLOCK();
        return -EINVAL;
    }
    outbound = *app_mesh_local_delivery_outbound(delivery);
    if (delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_FAILED) {
        ret = app_mesh_local_delivery_discard_failed(delivery);
        SURVEY_DELIVERY_UNLOCK();
        if (ret < 0) {
            LOG_ERR("survey discovery failed-report custody release failed: survey=%u seq=%u ret=%d",
                    outbound.packet.session_id, outbound.packet.seq, ret);
            schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return -EAGAIN;
        }
        survey_delivery_retry_round = 0u;
        LOG_ERR("survey discovery report delivery exhausted and released: survey=%u seq=%u",
                outbound.packet.session_id, outbound.packet.seq);
        app_stack_workload_diag_anchor_survey_release(&outbound.packet,
                                                       -ETIMEDOUT, 0u, 0u);
        return -ETIMEDOUT;
    }
    if (delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_STARTING ||
        delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_TRACKED) {
        ret = app_mesh_local_delivery_note_attempt_released(
            delivery, delivery->snapshot.attempt_token,
            APP_MESH_LOCAL_DELIVERY_RETRY);
        if (ret < 0) {
            SURVEY_DELIVERY_UNLOCK();
            return ret;
        }
        retry_delay_ms = survey_delivery_next_retry_delay_ms(
            app_mesh_local_delivery_outbound(delivery));
        SURVEY_DELIVERY_UNLOCK();
        schedule_work_ms(retry_delay_ms);
        return -EAGAIN;
    }
    if (app_mesh_local_delivery_attempts_available(delivery) == 0u) {
        ret = app_mesh_local_delivery_note_failed(delivery);
        if (ret < 0) {
            SURVEY_DELIVERY_UNLOCK();
            return ret;
        }
        ret = app_mesh_local_delivery_discard_failed(delivery);
        SURVEY_DELIVERY_UNLOCK();
        if (ret < 0) {
            LOG_ERR("survey discovery failed-report custody release failed: survey=%u seq=%u ret=%d",
                    outbound.packet.session_id, outbound.packet.seq, ret);
            schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return -EAGAIN;
        }
        survey_delivery_retry_round = 0u;
        LOG_ERR("survey discovery report delivery exhausted and released: survey=%u seq=%u",
                outbound.packet.session_id, outbound.packet.seq);
        app_stack_workload_diag_anchor_survey_release(&outbound.packet,
                                                       -ETIMEDOUT, 0u, 0u);
        return -ETIMEDOUT;
    }
    survey_delivery_comm_owned = true;
    survey_delivery_handle = 0u;
    app_mesh_local_delivery_identity_from_outbound(
        &outbound, &survey_delivery_comm_identity);
    survey_delivery_comm_identity_valid = true;
    SURVEY_DELIVERY_UNLOCK();

    absolute_deadline_ms = survey_delivery_deadline_ms(&outbound);
    ret = app_node_comm_submit_delivery(
        &outbound,
        NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
        absolute_deadline_ms,
        outbound.packet.session_id,
        &delivery_handle);

    SURVEY_DELIVERY_LOCK();
    submission_owned = survey_delivery_comm_owned &&
                       survey_delivery_handle == 0u &&
                       survey_delivery_comm_identity_valid &&
                       app_mesh_local_delivery_identity_matches(
                           &survey_delivery_comm_identity,
                           &outbound.packet);
    still_current = submission_owned &&
                    survey_delivery_matches_locked(delivery, &outbound);
    if (ret == 0 && still_current) {
        survey_delivery_handle = delivery_handle;
    } else {
        if (ret == 0) {
            stale_handle = delivery_handle;
        }
        if (submission_owned) {
            survey_delivery_comm_owned = false;
            memset(&survey_delivery_comm_identity, 0,
                   sizeof(survey_delivery_comm_identity));
            survey_delivery_comm_identity_valid = false;
            if (still_current) {
                retry_delay_ms = survey_delivery_next_retry_delay_ms(
                    app_mesh_local_delivery_outbound(delivery));
            }
        }
    }
    SURVEY_DELIVERY_UNLOCK();

    if (stale_handle != 0u) {
        (void)app_node_comm_abandon_delivery(stale_handle);
        return 0;
    }
    if (ret < 0) {
        if (still_current) {
            schedule_work_ms(retry_delay_ms == 0u ?
                             SURVEY_DISCOVERY_REPORT_RETRY_MAX_MS :
                             retry_delay_ms);
        }
        return still_current ? -EAGAIN : 0;
    }
    status_debug_printf(
        "DBG_SURVEY_REPORT_COMM_SUBMIT survey=%u seq=%u handle=%u deadline=%llu\n",
        outbound.packet.session_id,
        outbound.packet.seq,
        delivery_handle,
        (unsigned long long)absolute_deadline_ms);
    schedule_work_ms(SURVEY_DELIVERY_EVENT_POLL_MS);
    return 0;
}

bool app_anchor_survey_discovery_report_staged(uint32_t survey_id)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    bool staged = false;

    if (DEVICE_ROLE != ROLE_ANCHOR || delivery == NULL || survey_id == 0u) {
        return false;
    }
    SURVEY_DELIVERY_LOCK();
    staged = app_mesh_local_delivery_active(delivery) &&
             delivery->snapshot.generation == survey_id;
    SURVEY_DELIVERY_UNLOCK();
    return staged;
}

void app_anchor_survey_discovery_handle_start(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct survey_discovery_config config = {0};
    struct survey_discovery_timing timing = {0};
    uint32_t received_at_ms;
    uint32_t now_ms;
    uint32_t schedule_delay_ms;
    uint32_t start_at_ms;
    enum app_anchor_survey_discovery_admission admission;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || !discovery_initialized ||
        packet == NULL || packet->msg_type != MSG_SURVEY_DISCOVERY_START ||
        packet->dst_id != MESH_BROADCAST_ID || packet->src_id != GATEWAY_ID) {
        return;
    }
    received_at_ms = k_uptime_get_32();

    ret = survey_extract_discovery_start_tlvs(payload, payload_len, &config);
    if (ret != PROTO_OK || packet->session_id != config.survey_id) {
        LOG_WRN("survey discovery start rejected: ret=%d session=%u survey=%u",
                ret, packet->session_id, config.survey_id);
        return;
    }
    ret = survey_discovery_timing_from_age(&config, packet->message_age_ms, &timing);
    if (ret != PROTO_OK || timing.expired) {
        LOG_WRN("survey discovery start stale: ret=%d survey=%u age_ms=%u duration_ms=%u",
                ret, config.survey_id, packet->message_age_ms,
                timing.duration_ms);
        return;
    }
    ret = survey_discovery_start_at_ms(&timing, received_at_ms, &start_at_ms);
    if (ret != PROTO_OK) {
        LOG_WRN("survey discovery start timing rejected: ret=%d survey=%u age_ms=%u",
                ret, config.survey_id, packet->message_age_ms);
        return;
    }

    SURVEY_DELIVERY_LOCK();
    if (app_mesh_local_delivery_active(delivery)) {
        uint32_t pending_survey_id = delivery->snapshot.generation;

        SURVEY_DELIVERY_UNLOCK();
        if (pending_survey_id == config.survey_id) {
            LOG_INF("duplicate survey discovery start retained pending report custody: survey=%u",
                    config.survey_id);
            schedule_work_ms(0u);
            return;
        }
        LOG_WRN("survey discovery start rejected while earlier report custody is pending: requested=%u pending=%u",
                config.survey_id, pending_survey_id);
        schedule_work_ms(0u);
        return;
    } else {
        SURVEY_DELIVERY_UNLOCK();
    }

    admission = discovery_ops.admit_start(config.survey_id);
    if (admission == APP_ANCHOR_SURVEY_DISCOVERY_DUPLICATE) {
        LOG_INF("duplicate survey discovery start ignored while generation is active: survey=%u",
                config.survey_id);
        return;
    }
    if (admission != APP_ANCHOR_SURVEY_DISCOVERY_ACCEPTED) {
        LOG_WRN("survey discovery start rejected while another generation is active: survey=%u",
                config.survey_id);
        return;
    }

    discovery_ops.abort_pair();
    discovery_ops.preempt_radio(config.survey_id);
    now_ms = k_uptime_get_32();
    discovery_ops.queue_start(&config, start_at_ms);
    if (uptime_deadline_reached(now_ms, start_at_ms) ||
        uptime_ms_until_deadline(now_ms, start_at_ms) <=
            SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS) {
        schedule_delay_ms = 0u;
    } else {
        schedule_delay_ms = uptime_ms_until_deadline(now_ms, start_at_ms) -
                            SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS;
    }
    schedule_work_ms(schedule_delay_ms);
    status_debug_printf("DBG_SURVEY_DISCOVERY_SCHEDULED survey=%u age=%u start=%u delay=%u\n",
                        config.survey_id,
                        packet->message_age_ms,
                        start_at_ms,
                        schedule_delay_ms);
    LOG_INF("survey discovery scheduled: survey=%u start_delay_ms=%u age_ms=%u wait_ms=%u worker_delay_ms=%u prep_budget_ms=%u processing_ms=%u slot_ms=%u slots=%u",
            config.survey_id, config.start_delay_ms, packet->message_age_ms,
            timing.wait_ms, schedule_delay_ms,
            SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS,
            now_ms - received_at_ms,
            config.slot_ms, config.slot_count);
}

static int8_t survey_quality_to_rssi(uint8_t quality)
{
    uint8_t bounded_quality = quality > 100u ? 100u : quality;

    return (int8_t)(-100 + (int)bounded_quality / 2);
}

static bool survey_peer_reportable(uint64_t peer_id)
{
    return mesh_id_is_unicast(peer_id) && peer_id != DEVICE_ID &&
           peer_id != GATEWAY_ID;
}

static bool survey_probe_slot_matches_any_opportunity(
    const struct survey_discovery_config *config,
    const struct uwb_survey_discovery_probe_frame *probe,
    uint8_t *opportunity_out)
{
    if (config == NULL || probe == NULL) {
        return false;
    }
    for (uint8_t opportunity = 0u;
         opportunity < config->round_count;
         opportunity++) {
        if (probe->anchor_slot == survey_discovery_opportunity_slot(
                                      probe->anchor_id,
                                      config->survey_id,
                                      opportunity,
                                      config->slot_count)) {
            if (opportunity_out != NULL) {
                *opportunity_out = opportunity;
            }
            return true;
        }
    }
    return false;
}

static void survey_add_reach_entry(struct survey_reachability_entry *entries,
                                   size_t entry_cap,
                                   size_t *entry_count,
                                   uint64_t peer_id,
                                   uint8_t quality)
{
    if (entries == NULL || entry_count == NULL ||
        !survey_peer_reportable(peer_id) || *entry_count >= entry_cap) {
        return;
    }

    if (quality > 100u) {
        quality = 100u;
    }
    for (size_t i = 0u; i < *entry_count; i++) {
        if (entries[i].peer_id == peer_id) {
            if (quality > entries[i].quality) {
                entries[i].quality = quality;
                entries[i].rssi_dbm = survey_quality_to_rssi(quality);
            }
            return;
        }
    }

    entries[*entry_count].peer_id = peer_id;
    entries[*entry_count].quality = quality;
    entries[*entry_count].rssi_dbm = survey_quality_to_rssi(quality);
    (*entry_count)++;
}

static int receive_survey_probes_until(
    const struct survey_discovery_config *config,
    uint32_t deadline_ms,
    struct survey_reachability_entry *entries,
    size_t entry_cap,
    size_t *entry_count)
{
    while (!uptime_deadline_reached(k_uptime_get_32(), deadline_ms) &&
           !abort_requested()) {
        struct uwb_survey_discovery_probe_frame probe = {0};
        uint8_t frame[UWB_SURVEY_DISCOVERY_PROBE_LEN];
        size_t frame_len = 0u;
        uint8_t quality = 0u;
        uint8_t opportunity = 0u;
        int8_t rsl_dbm = 0;
        enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
        uint32_t remaining_ms = uptime_ms_until_deadline(k_uptime_get_32(),
                                                         deadline_ms);
        int ret;

        if (remaining_ms == 0u) {
            break;
        }
        ret = dwm3000_driver_receive_frame_continuous(
            remaining_ms, frame, sizeof(frame), &frame_len, &quality,
            &rsl_dbm, &rx_failure);
        if (ret == -ECANCELED) {
            return ret;
        }
        if (ret == -ETIMEDOUT) {
            break;
        }
        if (ret != 0 ||
            uwb_decode_survey_discovery_probe(frame, frame_len, &probe) != PROTO_OK ||
            probe.network_id != NETWORK_ID ||
            probe.survey_id != config->survey_id ||
            probe.anchor_id == DEVICE_ID ||
            probe.slot_count != config->slot_count ||
            !survey_probe_slot_matches_any_opportunity(config, &probe,
                                                       &opportunity)) {
            continue;
        }
        if (quality > 100u) {
            quality = 100u;
        }
        survey_add_reach_entry(entries, entry_cap, entry_count,
                               probe.anchor_id, quality);
        for (size_t i = 0u; i < *entry_count; i++) {
            if (entries[i].peer_id == probe.anchor_id &&
                quality >= entries[i].quality) {
                entries[i].rssi_dbm = rsl_dbm;
                break;
            }
        }
        high_debug_log_event("SURVEY_DISCOVERY_PROBE_RX",
                             "survey=%u opportunity=%u peer=0x%016llx slot=%u quality=%u rsl=%d peers=%u",
                             config->survey_id, opportunity,
                             (unsigned long long)probe.anchor_id,
                             probe.anchor_slot, quality, rsl_dbm,
                             (unsigned int)*entry_count);
    }
    return 0;
}

static int send_local_survey_probe(
    const struct survey_discovery_config *config,
    uint8_t opportunity,
    uint8_t *slot_out,
    bool *rf_started)
{
    struct uwb_survey_discovery_probe_frame probe = {
        .network_id = NETWORK_ID,
        .survey_id = config->survey_id,
        .anchor_id = DEVICE_ID,
        .slot_count = config->slot_count,
        .flags = FLAG_DIAGNOSTIC,
    };
    uint8_t frame[UWB_SURVEY_DISCOVERY_PROBE_LEN];
    size_t frame_len = 0u;
    int ret;

    probe.anchor_slot = survey_discovery_opportunity_slot(
        DEVICE_ID, config->survey_id, opportunity, config->slot_count);
    if (slot_out != NULL) {
        *slot_out = probe.anchor_slot;
    }
    ret = uwb_encode_survey_discovery_probe(&probe, frame, sizeof(frame),
                                            &frame_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    return dwm3000_driver_send_frame_tracked(
        frame, frame_len, SURVEY_DISCOVERY_TX_TIMEOUT_MS, rf_started);
}

int app_anchor_survey_discovery_run(
    const struct survey_discovery_config *config,
    uint32_t start_ms)
{
    struct survey_reachability_entry entries[SURVEY_REACH_MAX_ENTRIES] = {0};
    size_t entry_count = 0u;
    uint32_t report_delay_ms = 0u;
    uint8_t report_slot;
    int ret;

    if (!discovery_initialized ||
        survey_discovery_config_validate(config) != PROTO_OK) {
        return -EINVAL;
    }

    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        return ret;
    }
    if (!uptime_deadline_reached(k_uptime_get_32(), start_ms)) {
        ret = dwm3000_driver_idle();
        if (ret < 0) {
            return ret;
        }
        sleep_until_ms(start_ms);
    }

    report_slot = local_survey_discovery_slot(config->slot_count);
    {
        uint32_t discovery_duration_ms =
            survey_discovery_duration_ms(config);
        uint32_t discovery_deadline_ms = start_ms + discovery_duration_ms;
        uint32_t tx_budget_ms = survey_discovery_probe_tx_budget_ms();

        if (discovery_duration_ms == 0u ||
            tx_budget_ms == UINT32_MAX) {
            return -EINVAL;
        }

        for (uint8_t opportunity = 0u;
             opportunity < config->round_count &&
             !abort_requested();
             opportunity++) {
            struct survey_discovery_attempt_schedule nominal = {0};
            uint32_t opportunity_start_ms;
            uint32_t opportunity_tx_ms;
            uint32_t opportunity_end_ms;
            uint32_t relative_now_ms;
            uint8_t probe_slot = 0u;
            bool rf_started = false;

            ret = survey_discovery_schedule_attempt(config, DEVICE_ID,
                                                    opportunity, 0u,
                                                    &nominal);
            if (ret != PROTO_OK) {
                return mesh_errno_from_proto(ret);
            }
            opportunity_start_ms = start_ms + nominal.window_start_ms;
            opportunity_tx_ms = start_ms + nominal.tx_ms;
            opportunity_end_ms = start_ms + nominal.window_end_ms;

            if (!uptime_deadline_reached(k_uptime_get_32(),
                                         opportunity_start_ms)) {
                sleep_until_ms(opportunity_start_ms);
            }
            if (!uptime_deadline_reached(k_uptime_get_32(),
                                         opportunity_tx_ms)) {
                ret = receive_survey_probes_until(
                    config, opportunity_tx_ms, entries,
                    ARRAY_SIZE(entries), &entry_count);
                if (ret < 0) {
                    return ret;
                }
            }
            relative_now_ms = k_uptime_get_32() - start_ms;
            if (relative_now_ms <= nominal.latest_tx_start_ms) {
                ret = send_local_survey_probe(config, opportunity,
                                              &probe_slot, &rf_started);
                high_debug_log_event(
                    "SURVEY_DISCOVERY_PROBE_TX",
                    "survey=%u round=%u rounds=%u slot=%u slot_ms=%u rf_started=%u completion_ret=%d",
                    config->survey_id, opportunity, config->round_count,
                    probe_slot, config->slot_ms,
                    rf_started ? 1u : 0u, ret);
                if (ret < 0) {
                    LOG_WRN("survey discovery probe TX failed: survey=%u round=%u rounds=%u slot=%u rf_started=%u ret=%d",
                            config->survey_id, opportunity,
                            config->round_count, probe_slot,
                            rf_started ? 1u : 0u, ret);
                }
                ret = dwm3000_driver_ensure_wake_mode();
                if (ret < 0) {
                    return ret;
                }
            } else {
                LOG_WRN("survey discovery probe slot elapsed before TX: survey=%u round=%u rounds=%u",
                        config->survey_id, opportunity, config->round_count);
            }
            ret = receive_survey_probes_until(
                config, opportunity_end_ms, entries,
                ARRAY_SIZE(entries), &entry_count);
            if (ret < 0) {
                return ret;
            }
        }

        if (abort_requested()) {
            return -ECANCELED;
        }
        if (!uptime_deadline_reached(k_uptime_get_32(),
                                     discovery_deadline_ms)) {
            ret = receive_survey_probes_until(
                config, discovery_deadline_ms, entries,
                ARRAY_SIZE(entries), &entry_count);
            if (ret < 0) {
                return ret;
            }
        }
    }

    ret = survey_discovery_report_delay_ms(config, report_slot,
                                           SURVEY_RESULT_MESH_SLOT_MS,
                                           &report_delay_ms);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = prepare_discovery_report(config->survey_id, entries, entry_count,
                                   start_ms + report_delay_ms,
                                   COMMAND_OK);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("survey discovery complete: survey=%u report_slot=%u peers=%u report_delay_ms=%u rounds=%u",
            config->survey_id, report_slot, (unsigned int)entry_count,
            report_delay_ms, config->round_count);
    return 0;
}

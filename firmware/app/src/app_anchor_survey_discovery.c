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

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>

LOG_MODULE_DECLARE(app_anchor, LOG_LEVEL_DBG);

static struct app_anchor_survey_discovery_ops discovery_ops;
static bool discovery_initialized;
static uint16_t survey_delivery_retry_round;

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
#endif

int app_anchor_survey_discovery_init(
    const struct app_anchor_survey_discovery_ops *ops)
{
    if (ops == NULL || ops->abort_requested == NULL ||
        ops->abort_pair == NULL || ops->preempt_radio == NULL ||
        ops->queue_start == NULL || ops->schedule_work_ms == NULL ||
        ops->next_sequence == NULL) {
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
        SURVEY_DELIVERY_UNLOCK();
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
    ret = app_mesh_persistence_restore_local_delivery(&delivery->snapshot);
    ret = app_mesh_local_delivery_recover(delivery,
                                          &delivery->snapshot,
                                          ret,
                                          &recovery);
    if (recovery.restored) {
        uint16_t attempts_used = APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS -
            delivery->snapshot.attempts_remaining;

        survey_delivery_retry_round = attempts_used;
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
    attempt_token = delivery->snapshot.attempt_token;
    (void)app_mesh_local_delivery_note_attempt_released(
        delivery, attempt_token,
        preempted ? APP_MESH_LOCAL_DELIVERY_PREEMPTED :
                    APP_MESH_LOCAL_DELIVERY_RETRY);
    retry_delay_ms = survey_delivery_next_retry_delay_ms(
        app_mesh_local_delivery_outbound(delivery));
    SURVEY_DELIVERY_UNLOCK();
    schedule_work_ms(retry_delay_ms);
}

static int prepare_discovery_report(
    uint32_t survey_id,
    const struct survey_reachability_entry *entries,
    size_t entry_count,
    uint32_t earliest_tx_ms)
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
    ret = app_mesh_local_delivery_stage(delivery, &outbound, survey_id);
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
                                   start_ms + report_delay_ms);
    if (ret == 0) {
        status_debug_printf(
            "DBG_SURVEY_EMPTY_REPORT_STAGED survey=%u run_retained=1\n",
            config->survey_id);
    }
    return ret;
}

int app_anchor_survey_discovery_retry_report(void)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct mesh_outbound outbound;
    uint8_t attempt_token = 0u;
    uint16_t attempts_remaining = 0u;
    uint32_t retry_delay_ms = 0u;
    bool rf_sent = false;
    int persist_ret = 0;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || delivery == NULL) {
        return 0;
    }
    SURVEY_DELIVERY_LOCK();
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
        if (mesh_relay_tx_active(&mesh_runtime) || anchor_uwb_window_active()) {
            SURVEY_DELIVERY_UNLOCK();
            schedule_work_ms(SURVEY_DELIVERY_EVENT_POLL_MS);
            return -EINPROGRESS;
        }
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
    if (mesh_relay_tx_active(&mesh_runtime) || anchor_uwb_window_active()) {
        retry_delay_ms = survey_delivery_next_retry_delay_ms(&outbound);
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
    SURVEY_DELIVERY_UNLOCK();

    ret = mesh_owned_tracked_tx_preflight(&outbound,
                                          "survey-discovery-report",
                                          APP_MESH_ROUTE_WAIT_TX_OWNER_DURABLE_LOCAL,
                                          outbound.packet.session_id);
    if (ret < 0) {
        SURVEY_DELIVERY_LOCK();
        if (app_mesh_local_delivery_active(delivery)) {
            retry_delay_ms = survey_delivery_next_retry_delay_ms(
                app_mesh_local_delivery_outbound(delivery));
        }
        SURVEY_DELIVERY_UNLOCK();
        schedule_work_ms(retry_delay_ms == 0u ?
                         SURVEY_DISCOVERY_REPORT_RETRY_MAX_MS :
                         retry_delay_ms);
        return -EAGAIN;
    }

    SURVEY_DELIVERY_LOCK();
    if (!app_mesh_local_delivery_active(delivery)) {
        SURVEY_DELIVERY_UNLOCK();
        return 0;
    }
    if (delivery->snapshot.outbound.packet.session_id !=
            outbound.packet.session_id ||
        delivery->snapshot.outbound.packet.seq != outbound.packet.seq) {
        SURVEY_DELIVERY_UNLOCK();
        return -ESTALE;
    }
    if (mesh_relay_tx_active(&mesh_runtime) || anchor_uwb_window_active()) {
        retry_delay_ms = survey_delivery_next_retry_delay_ms(
            app_mesh_local_delivery_outbound(delivery));
        SURVEY_DELIVERY_UNLOCK();
        schedule_work_ms(retry_delay_ms);
        return -EAGAIN;
    }
    if (delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_BLOCKED_LIVE) {
        ret = app_mesh_local_delivery_resume_blocked_attempt(
            delivery, &attempt_token);
    } else {
        ret = app_mesh_local_delivery_begin_attempt(delivery, &attempt_token);
    }
    if (ret < 0) {
        SURVEY_DELIVERY_UNLOCK();
        return ret;
    }
    outbound = *app_mesh_local_delivery_outbound(delivery);
    SURVEY_DELIVERY_UNLOCK();

    ret = app_node_comm_start_owned_delivery(&outbound,
                                             "survey-discovery-report",
                                             &rf_sent);

    SURVEY_DELIVERY_LOCK();
    if (!app_mesh_local_delivery_active(delivery)) {
        SURVEY_DELIVERY_UNLOCK();
        return 0;
    }
    if (delivery->snapshot.attempt_token != attempt_token) {
        SURVEY_DELIVERY_UNLOCK();
        return -ESTALE;
    }
    if (rf_sent) {
        persist_ret = app_mesh_local_delivery_note_attempt_sent(
            delivery, attempt_token);
    } else {
        persist_ret = app_mesh_local_delivery_note_attempt_blocked(
            delivery, attempt_token);
        if (persist_ret == 0) {
            retry_delay_ms = survey_delivery_next_retry_delay_ms(
                app_mesh_local_delivery_outbound(delivery));
        }
    }
    attempts_remaining = app_mesh_local_delivery_attempts_available(delivery);
    SURVEY_DELIVERY_UNLOCK();

    if (persist_ret < 0) {
        LOG_ERR("survey delivery attempt resolution failed: %d", persist_ret);
        return persist_ret;
    }
    if (rf_sent) {
        app_stack_workload_diag_anchor_survey_sample(&outbound.packet, 1u, 1u);
        LOG_INF("survey discovery report RF attempt started: survey=%u seq=%u remaining=%u",
                outbound.packet.session_id, outbound.packet.seq,
                attempts_remaining);
    }
    schedule_work_ms(rf_sent ? SURVEY_DELIVERY_EVENT_POLL_MS : retry_delay_ms);
    return ret == 0 ? 0 : -EAGAIN;
}

void app_anchor_survey_discovery_handle_start(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct survey_discovery_config config = {0};
    struct survey_discovery_timing timing = {0};
    struct proto_packet superseded_packet = {0};
    bool superseded_packet_valid = false;
    uint32_t received_at_ms;
    uint32_t now_ms;
    uint32_t schedule_delay_ms;
    uint32_t start_at_ms;
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
        const struct mesh_outbound *pending =
            app_mesh_local_delivery_outbound(delivery);
        uint32_t pending_survey_id = delivery->snapshot.generation;

        if (pending != NULL) {
            superseded_packet = pending->packet;
            superseded_packet_valid = true;
        }
        ret = app_mesh_local_delivery_supersede(delivery, config.survey_id);
        if (ret == 0) {
            survey_delivery_retry_round = 0u;
        }

        SURVEY_DELIVERY_UNLOCK();
        if (ret == -EALREADY) {
            LOG_INF("duplicate survey discovery start retained pending report custody: survey=%u",
                    config.survey_id);
            schedule_work_ms(0u);
            return;
        }
        if (ret < 0) {
            LOG_ERR("survey discovery start could not supersede obsolete report custody: requested=%u pending=%u ret=%d",
                    config.survey_id, pending_survey_id, ret);
            schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return;
        }
        if (superseded_packet_valid) {
            app_stack_workload_diag_anchor_survey_release(
                &superseded_packet, -ESTALE, 0u, 0u);
            status_debug_printf("DBG_SURVEY_REPORT_SUPERSEDED anchor=0x%016llx old_survey=%u old_seq=%u new_survey=%u\n",
                                (unsigned long long)DEVICE_ID,
                                superseded_packet.session_id,
                                superseded_packet.seq,
                                config.survey_id);
        }
        LOG_WRN("obsolete survey report custody superseded by new gateway survey: requested=%u pending=%u",
                config.survey_id, pending_survey_id);
    } else {
        SURVEY_DELIVERY_UNLOCK();
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

static int schedule_survey_probe_retry(
    const struct survey_discovery_config *config,
    uint32_t retry_origin_ms,
    uint32_t absolute_deadline_ms,
    struct survey_discovery_probe_attempt *attempt)
{
    int ret;

    ret = survey_discovery_probe_attempt_defer(
        attempt, config, DEVICE_ID, retry_origin_ms, absolute_deadline_ms);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_BUSY ? -ETIMEDOUT : mesh_errno_from_proto(ret);
    }
    status_debug_printf(
        "DBG_SURVEY_PROBE_DEFER survey=%u opportunity=%u round=%u delay_ms=%u deadline_ms=%u\n",
        config->survey_id,
        attempt->opportunity,
        attempt->retry_round,
        attempt->due_ms - retry_origin_ms,
        absolute_deadline_ms);
    return 0;
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
         opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
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

static void receive_survey_probes_until(
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
        struct survey_discovery_probe_attempt
            retry_states[SURVEY_DISCOVERY_OPPORTUNITY_COUNT] = {0};
        uint32_t discovery_duration_ms =
            survey_discovery_duration_ms(config);
        uint32_t nominal_duration_ms = discovery_duration_ms / 2u;
        uint32_t reserve_start_ms = start_ms + nominal_duration_ms;
        uint32_t discovery_deadline_ms = start_ms + discovery_duration_ms;
        uint32_t tx_budget_ms = survey_discovery_probe_tx_budget_ms();
        uint8_t deferred_mask = 0u;

        if (discovery_duration_ms == 0u || nominal_duration_ms == 0u ||
            tx_budget_ms == UINT32_MAX) {
            return -EINVAL;
        }
        for (uint8_t opportunity = 0u;
             opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
             opportunity++) {
            if (survey_discovery_probe_attempt_begin(
                    &retry_states[opportunity], opportunity) != PROTO_OK) {
                return -EINVAL;
            }
        }

        for (uint8_t opportunity = 0u;
             opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT &&
             !abort_requested();
             opportunity++) {
            struct survey_discovery_attempt_schedule nominal = {0};
            uint32_t opportunity_start_ms;
            uint32_t opportunity_tx_ms;
            uint32_t opportunity_end_ms;
            uint32_t retry_origin_ms;
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
                receive_survey_probes_until(
                    config, opportunity_tx_ms, entries,
                    ARRAY_SIZE(entries), &entry_count);
            }
            relative_now_ms = k_uptime_get_32() - start_ms;
            if (relative_now_ms <= nominal.latest_tx_start_ms) {
                ret = send_local_survey_probe(config, opportunity,
                                              &probe_slot, &rf_started);
                if (rf_started) {
                    if (survey_discovery_probe_attempt_note_rf_started(
                            &retry_states[opportunity]) != PROTO_OK) {
                        return -EINVAL;
                    }
                    high_debug_log_event(
                        "SURVEY_DISCOVERY_PROBE_TX",
                        "survey=%u opportunity=%u slot=%u slot_ms=%u deferred=0 completion_ret=%d",
                        config->survey_id, opportunity, probe_slot,
                        config->slot_ms, ret);
                }
                if (ret < 0) {
                    LOG_WRN("survey discovery probe TX failed: survey=%u opportunity=%u slot=%u deferred=0 rf_started=%u ret=%d",
                            config->survey_id, opportunity, probe_slot,
                            rf_started ? 1u : 0u, ret);
                }
                ret = dwm3000_driver_ensure_wake_mode();
                if (ret < 0) {
                    return ret;
                }
            }
            if (!rf_started) {
                uint32_t now_ms = k_uptime_get_32();

                deferred_mask |= (uint8_t)(1u << opportunity);
                retry_origin_ms = uptime_deadline_reached(now_ms,
                                                          reserve_start_ms) ?
                                  now_ms : reserve_start_ms;
                ret = schedule_survey_probe_retry(
                    config, retry_origin_ms, discovery_deadline_ms,
                    &retry_states[opportunity]);
                if (ret < 0 && ret != -ETIMEDOUT) {
                    return ret;
                }
            }
            receive_survey_probes_until(
                config, opportunity_end_ms, entries,
                ARRAY_SIZE(entries), &entry_count);
        }

        while (deferred_mask != 0u && !abort_requested() &&
               !uptime_deadline_reached(k_uptime_get_32(),
                                        discovery_deadline_ms)) {
            uint32_t now_ms = k_uptime_get_32();
            uint32_t next_delay_ms = UINT32_MAX;
            uint8_t next_opportunity = SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
            uint8_t probe_slot = 0u;
            bool rf_started = false;

            for (uint8_t opportunity = 0u;
                 opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
                 opportunity++) {
                uint32_t delay_ms;

                if (!retry_states[opportunity].pending) {
                    continue;
                }
                delay_ms = uptime_deadline_reached(
                               now_ms, retry_states[opportunity].due_ms) ?
                           0u : uptime_ms_until_deadline(
                                    now_ms,
                                    retry_states[opportunity].due_ms);
                if (delay_ms < next_delay_ms) {
                    next_delay_ms = delay_ms;
                    next_opportunity = opportunity;
                }
            }
            if (next_opportunity >= SURVEY_DISCOVERY_OPPORTUNITY_COUNT) {
                break;
            }
            if (next_delay_ms != 0u) {
                receive_survey_probes_until(
                    config, retry_states[next_opportunity].due_ms, entries,
                    ARRAY_SIZE(entries), &entry_count);
            }
            now_ms = k_uptime_get_32();
            if (abort_requested() ||
                uptime_ms_until_deadline(now_ms, discovery_deadline_ms) <=
                    tx_budget_ms) {
                break;
            }

            ret = send_local_survey_probe(config, next_opportunity,
                                          &probe_slot, &rf_started);
            if (rf_started) {
                if (survey_discovery_probe_attempt_note_rf_started(
                        &retry_states[next_opportunity]) != PROTO_OK) {
                    return -EINVAL;
                }
                deferred_mask &= (uint8_t)~(1u << next_opportunity);
                high_debug_log_event(
                    "SURVEY_DISCOVERY_PROBE_TX",
                    "survey=%u opportunity=%u slot=%u slot_ms=%u deferred=1 retry_round=%u completion_ret=%d",
                    config->survey_id, next_opportunity, probe_slot,
                    config->slot_ms,
                    retry_states[next_opportunity].retry_round, ret);
            } else {
                uint32_t retry_origin_ms = k_uptime_get_32();
                int retry_ret = schedule_survey_probe_retry(
                    config, retry_origin_ms, discovery_deadline_ms,
                    &retry_states[next_opportunity]);

                if (retry_ret < 0 && retry_ret != -ETIMEDOUT) {
                    return retry_ret;
                }
            }
            if (ret < 0) {
                LOG_WRN("survey discovery probe TX failed: survey=%u opportunity=%u slot=%u deferred=1 round=%u rf_started=%u ret=%d",
                        config->survey_id, next_opportunity, probe_slot,
                        retry_states[next_opportunity].retry_round,
                        rf_started ? 1u : 0u, ret);
            }
            ret = dwm3000_driver_ensure_wake_mode();
            if (ret < 0) {
                return ret;
            }
        }
        if (!abort_requested() &&
            !uptime_deadline_reached(k_uptime_get_32(),
                                     discovery_deadline_ms)) {
            receive_survey_probes_until(
                config, discovery_deadline_ms, entries,
                ARRAY_SIZE(entries), &entry_count);
        }
        if (!abort_requested() &&
            survey_discovery_probe_real_attempt_count(
                retry_states, ARRAY_SIZE(retry_states)) !=
                SURVEY_DISCOVERY_OPPORTUNITY_COUNT) {
            LOG_ERR("survey discovery deadline ended before four real attempts: survey=%u attempted=%u deferred_mask=0x%02x",
                    config->survey_id,
                    (unsigned int)survey_discovery_probe_real_attempt_count(
                        retry_states, ARRAY_SIZE(retry_states)),
                    deferred_mask);
        }
    }

    ret = survey_discovery_report_delay_ms(config, report_slot,
                                           SURVEY_RESULT_MESH_SLOT_MS,
                                           &report_delay_ms);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = prepare_discovery_report(config->survey_id, entries, entry_count,
                                   start_ms + report_delay_ms);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("survey discovery complete: survey=%u report_slot=%u peers=%u report_delay_ms=%u opportunities=%u",
            config->survey_id, report_slot, (unsigned int)entry_count,
            report_delay_ms, SURVEY_DISCOVERY_OPPORTUNITY_COUNT);
    return 0;
}

#include "app_anchor_survey_discovery.h"

#include "app_config.h"
#include "app_board.h"
#include "app_high_debug.h"
#include "app_mesh_local_delivery.h"
#include "app_mesh_persistence.h"
#include "app_mesh_report.h"
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

static bool abort_requested(void)
{
    return discovery_ops.abort_requested != NULL &&
           discovery_ops.abort_requested();
}

static uint32_t discovery_worker_delay_ms(
    const struct survey_discovery_timing *timing)
{
    if (timing == NULL || !timing->pending ||
        timing->wait_ms <= SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS) {
        return 0u;
    }

    return timing->wait_ms - SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS;
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
        LOG_INF("survey delivery gateway ACK committed");
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
    SURVEY_DELIVERY_UNLOCK();
    schedule_work_ms(0u);
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
    SURVEY_DELIVERY_UNLOCK();
    return ret;
}

int app_anchor_survey_discovery_retry_report(void)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct mesh_outbound outbound;
    uint8_t attempt_token = 0u;
    uint16_t attempts_remaining = 0u;
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
        LOG_ERR("survey discovery report delivery exhausted: survey=%u seq=%u",
                outbound.packet.session_id, outbound.packet.seq);
        SURVEY_DELIVERY_UNLOCK();
        return -ETIMEDOUT;
    }
    if (mesh_relay_tx_active(&mesh_runtime) || anchor_uwb_window_active()) {
        SURVEY_DELIVERY_UNLOCK();
        schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
        return -EAGAIN;
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
    }
    if (app_mesh_local_delivery_attempts_available(delivery) == 0u) {
        ret = app_mesh_local_delivery_note_failed(delivery);
        if (ret < 0) {
            SURVEY_DELIVERY_UNLOCK();
            return ret;
        }
        LOG_ERR("survey discovery report delivery exhausted: survey=%u seq=%u",
                outbound.packet.session_id, outbound.packet.seq);
        SURVEY_DELIVERY_UNLOCK();
        return -ETIMEDOUT;
    }
    SURVEY_DELIVERY_UNLOCK();

    ret = mesh_owned_tracked_tx_preflight(&outbound,
                                          "survey-discovery-report",
                                          APP_MESH_ROUTE_WAIT_TX_OWNER_DURABLE_LOCAL,
                                          outbound.packet.session_id);
    if (ret < 0) {
        schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
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
        SURVEY_DELIVERY_UNLOCK();
        schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
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

    ret = mesh_start_owned_tracked_tx(&outbound,
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
    }
    attempts_remaining = app_mesh_local_delivery_attempts_available(delivery);
    SURVEY_DELIVERY_UNLOCK();

    if (persist_ret < 0) {
        LOG_ERR("survey delivery attempt resolution failed: %d", persist_ret);
        return persist_ret;
    }
    if (rf_sent) {
        LOG_INF("survey discovery report RF attempt started: survey=%u seq=%u remaining=%u",
                outbound.packet.session_id, outbound.packet.seq,
                attempts_remaining);
    }
    schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
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
    uint32_t now_ms;
    uint32_t schedule_delay_ms;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || !discovery_initialized ||
        packet == NULL || packet->msg_type != MSG_SURVEY_DISCOVERY_START ||
        packet->dst_id != MESH_BROADCAST_ID || packet->src_id != GATEWAY_ID) {
        return;
    }

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

    SURVEY_DELIVERY_LOCK();
    if (app_mesh_local_delivery_active(delivery)) {
        uint32_t pending_survey_id =
            delivery->snapshot.outbound.packet.session_id;

        SURVEY_DELIVERY_UNLOCK();
        LOG_WRN("survey discovery start deferred by pending report custody: requested=%u pending=%u",
                config.survey_id, pending_survey_id);
        return;
    }
    SURVEY_DELIVERY_UNLOCK();

    discovery_ops.abort_pair();
    discovery_ops.preempt_radio(config.survey_id);
    now_ms = k_uptime_get_32();
    schedule_delay_ms = discovery_worker_delay_ms(&timing);
    discovery_ops.queue_start(
        &config,
        timing.pending ? now_ms + timing.wait_ms : now_ms - timing.elapsed_ms);
    schedule_work_ms(schedule_delay_ms);
    LOG_INF("survey discovery scheduled: survey=%u start_delay_ms=%u age_ms=%u wait_ms=%u worker_delay_ms=%u prep_budget_ms=%u slot_ms=%u slots=%u",
            config.survey_id, config.start_delay_ms, packet->message_age_ms,
            timing.wait_ms, schedule_delay_ms,
            SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS,
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
    uint8_t opportunity,
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
            probe.anchor_slot != survey_discovery_opportunity_slot(
                                     probe.anchor_id, config->survey_id,
                                     opportunity, config->slot_count)) {
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
        uint8_t deferred_mask = 0u;
        uint8_t actual_attempt_count = 0u;

        for (uint8_t phase = 0u; phase < 2u; phase++) {
            for (uint8_t opportunity = 0u;
                 opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT &&
                 !abort_requested();
                 opportunity++) {
                struct uwb_survey_discovery_probe_frame probe = {
                    .network_id = NETWORK_ID,
                    .survey_id = config->survey_id,
                    .anchor_id = DEVICE_ID,
                    .slot_count = config->slot_count,
                    .flags = FLAG_DIAGNOSTIC,
                };
                struct survey_discovery_attempt_schedule nominal = {0};
                struct survey_discovery_attempt_schedule attempt = {0};
                uint8_t frame[UWB_SURVEY_DISCOVERY_PROBE_LEN];
                size_t frame_len = 0u;
                uint32_t opportunity_start_ms;
                uint32_t opportunity_tx_ms;
                uint32_t opportunity_end_ms;
                uint32_t relative_now_ms;
                bool should_tx;

                ret = survey_discovery_schedule_attempt(config, DEVICE_ID,
                                                        opportunity, 0u,
                                                        &nominal);
                if (ret != PROTO_OK) {
                    return mesh_errno_from_proto(ret);
                }
                if (phase == 0u) {
                    attempt = nominal;
                    should_tx = true;
                } else {
                    ret = survey_discovery_schedule_attempt(
                        config, DEVICE_ID, opportunity,
                        nominal.latest_tx_start_ms + 1u, &attempt);
                    if (ret != PROTO_OK || !attempt.deferred) {
                        return ret == PROTO_OK ? -EINVAL :
                               mesh_errno_from_proto(ret);
                    }
                    should_tx = (deferred_mask & (1u << opportunity)) != 0u;
                }
                probe.anchor_slot = survey_discovery_opportunity_slot(
                    DEVICE_ID, config->survey_id, opportunity,
                    config->slot_count);
                opportunity_start_ms = start_ms + attempt.window_start_ms;
                opportunity_tx_ms = start_ms + attempt.tx_ms;
                opportunity_end_ms = start_ms + attempt.window_end_ms;

                if (!uptime_deadline_reached(k_uptime_get_32(),
                                             opportunity_start_ms)) {
                    sleep_until_ms(opportunity_start_ms);
                }
                if (!uptime_deadline_reached(k_uptime_get_32(),
                                             opportunity_tx_ms)) {
                    receive_survey_probes_until(
                        config, opportunity, opportunity_tx_ms, entries,
                        ARRAY_SIZE(entries), &entry_count);
                }
                relative_now_ms = k_uptime_get_32() - start_ms;
                if (should_tx &&
                    relative_now_ms > attempt.latest_tx_start_ms) {
                    if (phase == 0u) {
                        deferred_mask |= (uint8_t)(1u << opportunity);
                        should_tx = false;
                    } else {
                        return -EBUSY;
                    }
                }

                if (should_tx) {
                    ret = uwb_encode_survey_discovery_probe(
                        &probe, frame, sizeof(frame), &frame_len);
                    if (ret != PROTO_OK) {
                        return mesh_errno_from_proto(ret);
                    }
                    ret = dwm3000_driver_send_frame(
                        frame, frame_len, SURVEY_DISCOVERY_TX_TIMEOUT_MS);
                    actual_attempt_count++;
                    if (ret < 0) {
                        LOG_WRN("survey discovery probe TX failed: survey=%u opportunity=%u slot=%u deferred=%u ret=%d",
                                config->survey_id, opportunity,
                                probe.anchor_slot, phase, ret);
                    } else {
                        high_debug_log_event(
                            "SURVEY_DISCOVERY_PROBE_TX",
                            "survey=%u opportunity=%u slot=%u slot_ms=%u deferred=%u",
                            config->survey_id, opportunity,
                            probe.anchor_slot, config->slot_ms, phase);
                    }
                    ret = dwm3000_driver_ensure_wake_mode();
                    if (ret < 0) {
                        return ret;
                    }
                }
                receive_survey_probes_until(
                    config, opportunity, opportunity_end_ms, entries,
                    ARRAY_SIZE(entries), &entry_count);
            }
        }
        if (!abort_requested() &&
            actual_attempt_count != SURVEY_DISCOVERY_OPPORTUNITY_COUNT) {
            LOG_ERR("survey discovery attempt invariant failed: survey=%u attempted=%u deferred_mask=0x%02x",
                    config->survey_id, actual_attempt_count, deferred_mask);
            return -EBUSY;
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

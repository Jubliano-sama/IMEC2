#include "app_anchor_survey_discovery.h"

#include "app_config.h"
#include "app_board.h"
#include "app_mesh_local_delivery.h"
#include "app_mesh_report.h"
#include "app_node_comm.h"
#include "app_stack_workload_diag.h"
#include "app_state.h"
#include "app_watchdog.h"
#include "dwm3000_driver.h"
#include "mesh.h"
#include "mesh_relay.h"

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
static uint32_t survey_delivery_failed_abandon_handle;
static bool survey_delivery_comm_owned;
static struct app_mesh_local_delivery_identity survey_delivery_comm_identity;
static bool survey_delivery_comm_identity_valid;
static struct mesh_outbound survey_report_stage_retry_outbound;
static uint32_t survey_report_stage_retry_generation;
static bool survey_report_stage_retry_valid;
static uint64_t survey_delivery_generation_floor;
static bool survey_delivery_retirement_in_progress;

#define SURVEY_DELIVERY_EVENT_POLL_MS 100u
#define SURVEY_DISCOVERY_REPORT_SOURCE_DEADLINE_MS UINT64_MAX
#define SURVEY_DISCOVERY_RADIO_PROGRESS_SLICE_MS \
    (APP_WATCHDOG_PROGRESS_LEASE_MS / 2u)

BUILD_ASSERT(SURVEY_DISCOVERY_RADIO_PROGRESS_SLICE_MS > 0u &&
                 SURVEY_DISCOVERY_RADIO_PROGRESS_SLICE_MS <
                     APP_WATCHDOG_PROGRESS_LEASE_MS,
             "survey receive slices must refresh the radio lease before expiry");

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

static int schedule_work_ms(uint32_t delay_ms)
{
    if (discovery_ops.schedule_work_ms != NULL) {
        int ret = discovery_ops.schedule_work_ms(delay_ms);

        if (ret < 0) {
            LOG_ERR("survey runtime work scheduling failed: delay_ms=%u ret=%d",
                    delay_ms,
                    ret);
            /*
             * Every caller has already published either retained RAM report
             * custody or an admitted survey generation. With no worker owner
             * left, continued watchdog feeds would turn a rejected reschedule
             * into a permanent protocol stall.
             */
            app_watchdog_stop_feeding();
        }
        return ret < 0 ? ret : 0;
    }
    return -ENODEV;
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
            "DBG_SURVEY_REPORT_BACKOFF boot=%u seq=%u round=%u ms=%u\n",
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
    return app_mesh_local_delivery_identity_equal(left, right);
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int survey_delivery_save(
    void *ctx,
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    /* Volatile owner adapter: the snapshot already lives in bounded RAM. */
    ARG_UNUSED(ctx);
    ARG_UNUSED(snapshot);
    return 0;
}

static int survey_delivery_clear(void *ctx)
{
    /* Volatile owner adapter: app_mesh_local_delivery clears its RAM state. */
    ARG_UNUSED(ctx);
    return 0;
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
    if (!app_mesh_local_delivery_occupied(delivery)) {
        SURVEY_DELIVERY_UNLOCK();
        return -ENOENT;
    }
    app_mesh_local_delivery_identity_from_outbound(
        &delivery->snapshot.outbound, &identity);
    if (!app_mesh_local_delivery_identity_matches(&identity, packet) ||
        (!app_mesh_local_delivery_ack_committed(delivery) &&
         delivery->snapshot.attempt_token != attempt_token)) {
        SURVEY_DELIVERY_UNLOCK();
        return -ESTALE;
    }
    /*
     * Direct gateway ACK handling can commit the RAM owner while
     * the communication facade is still unwinding the backend call. The ACK
     * proves RF started, so completion of that exact token is already
     * satisfied and must release the facade's backend guard.
     */
    if (app_mesh_local_delivery_ack_committed(delivery)) {
        SURVEY_DELIVERY_UNLOCK();
        return 0;
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
            "DBG_SURVEY_REPORT_RF_ATTEMPT boot=%u seq=%u token=%u remaining=%u\n",
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
        ops->boot_incarnation == NULL ||
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
        survey_delivery_handle = 0u;
        survey_delivery_failed_abandon_handle = 0u;
        survey_delivery_comm_owned = false;
        memset(&survey_delivery_comm_identity, 0,
               sizeof(survey_delivery_comm_identity));
        survey_delivery_comm_identity_valid = false;
        memset(&survey_report_stage_retry_outbound, 0,
               sizeof(survey_report_stage_retry_outbound));
        survey_report_stage_retry_generation = 0u;
        survey_report_stage_retry_valid = false;
        survey_delivery_generation_floor = 0u;
        survey_delivery_retirement_in_progress = false;
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

static bool survey_delivery_semantic_digest_matches(
    const struct app_mesh_local_delivery *delivery,
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    const struct mesh_outbound *outbound;
    uint8_t stored_digest[SEMANTIC_DIGEST_SHA256_LEN];

    if (delivery == NULL || packet == NULL || semantic_digest == NULL ||
        !app_mesh_local_delivery_occupied(delivery)) {
        return false;
    }
    outbound = &delivery->snapshot.outbound;
    return outbound->packet.flags == packet->flags &&
           outbound->packet.payload_len == packet->payload_len &&
           mesh_packet_semantic_digest(&outbound->packet,
                                       outbound->payload,
                                       outbound->payload_len,
                                       stored_digest) &&
           semantic_digest_equal(stored_digest,
                                 semantic_digest,
                                 sizeof(stored_digest));
}

int app_anchor_survey_delivery_gateway_confirmed(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct app_mesh_local_delivery_identity identity;
    bool newly_committed = false;
    bool ack_committed = false;
    int schedule_ret = 0;
    int ret;

    if (packet == NULL || semantic_digest == NULL) {
        return -EINVAL;
    }
    if (DEVICE_ROLE != ROLE_ANCHOR || delivery == NULL) {
        return -ENOENT;
    }
    SURVEY_DELIVERY_LOCK();
    if (!app_mesh_local_delivery_occupied(delivery)) {
        SURVEY_DELIVERY_UNLOCK();
        return -ENOENT;
    }
    app_mesh_local_delivery_identity_from_outbound(
        &delivery->snapshot.outbound, &identity);
    if (!app_mesh_local_delivery_identity_matches(&identity, packet)) {
        SURVEY_DELIVERY_UNLOCK();
        return -ESTALE;
    }
    if (!survey_delivery_semantic_digest_matches(delivery,
                                                 packet,
                                                 semantic_digest)) {
        SURVEY_DELIVERY_UNLOCK();
        return -EBADMSG;
    }
    newly_committed = !app_mesh_local_delivery_ack_committed(delivery);
    ret = app_mesh_local_delivery_commit_ack(
        delivery, packet, semantic_digest);
    if (ret < 0) {
        LOG_ERR("survey delivery RAM-owner ACK commit failed: boot=%u seq=%u ret=%d",
                packet->session_id, packet->seq, ret);
    } else {
        ack_committed = true;
        survey_delivery_retry_round = 0u;
    }
    SURVEY_DELIVERY_UNLOCK();
    if (newly_committed && ack_committed) {
        app_stack_workload_diag_anchor_survey_sample(packet, 0u, 0u);
        app_stack_workload_diag_anchor_survey_release(packet, 0, 0u, 0u);
        status_debug_printf(
            "DBG_SURVEY_REPORT_ACK anchor=0x%016llx boot=%u seq=%u\n",
            (unsigned long long)DEVICE_ID,
            packet->session_id,
            packet->seq);
        LOG_INF("survey delivery gateway ACK committed: boot=%u seq=%u",
                packet->session_id, packet->seq);
    }
    if (ack_committed) {
        schedule_ret = schedule_work_ms(0u);
    } else {
        schedule_ret = schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
    }
    return ret < 0 ? ret : schedule_ret;
}

int app_anchor_survey_delivery_transport_released(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    bool preempted)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct app_mesh_local_delivery_identity identity;
    uint8_t attempt_token;
    uint32_t retry_delay_ms;
    int ret = 0;

    if (packet == NULL || semantic_digest == NULL) {
        return -EINVAL;
    }
    if (DEVICE_ROLE != ROLE_ANCHOR || delivery == NULL ||
        packet->msg_type != MSG_SURVEY_DISCOVERY_REPORT) {
        return 0;
    }
    SURVEY_DELIVERY_LOCK();
    if (!app_mesh_local_delivery_occupied(delivery)) {
        SURVEY_DELIVERY_UNLOCK();
        return 0;
    }
    app_mesh_local_delivery_identity_from_outbound(
        &delivery->snapshot.outbound, &identity);
    if (!app_mesh_local_delivery_identity_matches(&identity, packet)) {
        SURVEY_DELIVERY_UNLOCK();
        return -ESTALE;
    }
    if (!survey_delivery_semantic_digest_matches(delivery,
                                                 packet,
                                                 semantic_digest)) {
        SURVEY_DELIVERY_UNLOCK();
        return -EBADMSG;
    }
    if (delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_STARTING ||
        delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_TRACKED) {
        attempt_token = delivery->snapshot.attempt_token;
        ret = app_mesh_local_delivery_note_attempt_released(
            delivery, attempt_token,
            preempted ? APP_MESH_LOCAL_DELIVERY_PREEMPTED :
                        APP_MESH_LOCAL_DELIVERY_RETRY);
    }
    retry_delay_ms = survey_delivery_next_retry_delay_ms(
        app_mesh_local_delivery_outbound(delivery));
    SURVEY_DELIVERY_UNLOCK();
    if (ret < 0) {
        return ret;
    }
    return schedule_work_ms(retry_delay_ms);
}

static int prepare_discovery_report(
    uint64_t operation_generation,
    uint32_t survey_id,
    const struct survey_reachability_entry *entries,
    size_t entry_count,
    uint32_t earliest_tx_ms,
    enum command_status status)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct mesh_outbound outbound = {0};
    uint32_t boot_incarnation = 0u;
    uint16_t sequence;
    size_t report_payload_len = 0u;
    int ret;

    const uint32_t operation_session_id =
        survey_operation_session_id(operation_generation);

    if (operation_session_id == 0u || survey_id == 0u ||
        (entries == NULL && entry_count != 0u) ||
        delivery == NULL || discovery_ops.next_sequence == NULL) {
        return -EINVAL;
    }

    ret = discovery_ops.boot_incarnation(&boot_incarnation);
    if (ret < 0) {
        return ret;
    }
    if (boot_incarnation == 0u) {
        return -EIO;
    }
    sequence = discovery_ops.next_sequence();
    if (sequence == 0u) {
        LOG_ERR("survey discovery report sequence exhausted; reboot required");
        return -EOVERFLOW;
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
    ret = survey_operation_generation_append_tlv(
        outbound.payload,
        sizeof(outbound.payload),
        &report_payload_len,
        operation_generation);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = tlv_append_u32(outbound.payload,
                         sizeof(outbound.payload),
                         &report_payload_len,
                         TLV_NODE_BOOT_COUNTER,
                         boot_incarnation);
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
                                              operation_generation,
                                              boot_incarnation,
                                              sequence,
                                              (uint8_t)report_payload_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound.payload_len = (uint8_t)report_payload_len;
    outbound.earliest_tx_ms = earliest_tx_ms;
    outbound.earliest_tx_valid = true;

    SURVEY_DELIVERY_LOCK();
    if (operation_generation < survey_delivery_generation_floor) {
        ret = -ECANCELED;
    } else if (survey_delivery_retirement_in_progress ||
               survey_report_stage_retry_valid) {
        ret = -EBUSY;
    } else {
        ret = app_mesh_local_delivery_stage(delivery,
                                            &outbound,
                                            operation_session_id);
        if (ret < 0 && !app_mesh_local_delivery_active(delivery)) {
            survey_report_stage_retry_outbound = outbound;
            survey_report_stage_retry_generation = operation_session_id;
            survey_report_stage_retry_valid = true;
        }
    }
    if (ret == 0) {
        survey_delivery_retry_round = 0u;
    }
    SURVEY_DELIVERY_UNLOCK();
    if (ret == 0) {
        app_stack_workload_diag_anchor_survey_admit(&outbound.packet, 1u, 1u);
        status_debug_printf("DBG_SURVEY_REPORT_STAGED anchor=0x%016llx boot=%u seq=%u peers=%u\n",
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
    ret = prepare_discovery_report(config->operation_generation,
                                   config->survey_id, NULL, 0u,
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

    if (!app_mesh_local_delivery_occupied(delivery) || outbound == NULL) {
        return false;
    }
    app_mesh_local_delivery_identity_from_outbound(
        &delivery->snapshot.outbound, &identity);
    return app_mesh_local_delivery_identity_matches_outbound(
        &identity, outbound);
}

/* Caller holds survey_delivery_lock when the anchor role is active. */
static int survey_delivery_retain_for_retry_locked(
    struct app_mesh_local_delivery *delivery)
{
    uint8_t state;
    int ret;

    if (!app_mesh_local_delivery_active(delivery)) {
        return -ENOENT;
    }
    state = delivery->snapshot.state;
    if (state == APP_MESH_LOCAL_DELIVERY_STARTING ||
        state == APP_MESH_LOCAL_DELIVERY_TRACKED) {
        ret = app_mesh_local_delivery_note_attempt_released(
            delivery,
            delivery->snapshot.attempt_token,
            APP_MESH_LOCAL_DELIVERY_RETRY);
        if (ret < 0) {
            return ret;
        }
    } else if (state == APP_MESH_LOCAL_DELIVERY_BLOCKED_LIVE) {
        ret = app_mesh_local_delivery_note_attempt_not_sent(
            delivery,
            delivery->snapshot.attempt_token,
            APP_MESH_LOCAL_DELIVERY_RETRY);
        if (ret < 0) {
            return ret;
        }
    } else if (state != APP_MESH_LOCAL_DELIVERY_RETRY) {
        ret = app_mesh_local_delivery_note_state(
            delivery, APP_MESH_LOCAL_DELIVERY_RETRY);
        if (ret < 0) {
            return ret;
        }
    }
    if (delivery->snapshot.attempts_remaining == 0u) {
        return app_mesh_local_delivery_rearm_attempts(delivery);
    }
    return 0;
}

static int survey_delivery_poll_comm_result(void)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct app_mesh_local_delivery_identity owned_identity = {0};
    struct node_comm_terminal_event event;
    struct mesh_outbound outbound = {0};
    uint32_t handle;
    uint32_t retry_delay_ms = 0u;
    bool accepted = false;
    bool current_delivery = false;
    bool terminal_taken = false;
    int state_ret = 0;

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
    if (!app_node_comm_peek_delivery_event_for(handle, &event)) {
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
    if (app_mesh_local_delivery_occupied(delivery) &&
        app_mesh_local_delivery_identity_matches(
            &owned_identity,
            &delivery->snapshot.outbound.packet)) {
        outbound = delivery->snapshot.outbound;
        current_delivery = true;
    }
    accepted = current_delivery &&
        (app_mesh_local_delivery_ack_committed(delivery) ||
         event.reason == NODE_COMM_TERMINAL_DELIVERED);
    if (current_delivery && !accepted) {
        state_ret = survey_delivery_retain_for_retry_locked(delivery);
    }
    SURVEY_DELIVERY_UNLOCK();

    if (!current_delivery) {
        return -ESTALE;
    }
    if (event.reason == NODE_COMM_TERMINAL_DELIVERED) {
        uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];

        /*
         * A terminal DELIVERED event is read-only until the exact local
         * RAM owner has entered ACK_COMMITTED. If that state transition
         * fails, the node-communication record and terminal event remain
         * untouched for the checked retry.
         */
        if (!mesh_packet_semantic_digest(&outbound.packet,
                                         outbound.payload,
                                         outbound.payload_len,
                                         semantic_digest) ||
            app_anchor_survey_delivery_gateway_confirmed(
                &outbound.packet, semantic_digest) < 0) {
            (void)schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return -EAGAIN;
        }
        SURVEY_DELIVERY_LOCK();
        current_delivery = survey_delivery_comm_owned &&
                           survey_delivery_handle == handle &&
                           survey_delivery_comm_identity_valid &&
                           survey_delivery_identity_equal(
                               &survey_delivery_comm_identity,
                               &owned_identity) &&
                           app_mesh_local_delivery_ack_committed(delivery) &&
                           app_mesh_local_delivery_identity_matches(
                               &owned_identity,
                               &delivery->snapshot.outbound.packet);
        SURVEY_DELIVERY_UNLOCK();
        if (!current_delivery) {
            (void)schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return -EAGAIN;
        }
    } else if (!accepted && state_ret < 0) {
        LOG_ERR("survey delivery retry-state commit failed: survey=%u seq=%u reason=%u ret=%d",
                outbound.packet.session_id, outbound.packet.seq,
                (unsigned int)event.reason, state_ret);
        (void)schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
        return -EAGAIN;
    }

    terminal_taken = app_node_comm_take_delivery_event_for(handle, &event);
    if (!terminal_taken) {
        (void)schedule_work_ms(SURVEY_DELIVERY_EVENT_POLL_MS);
        return -EAGAIN;
    }

    SURVEY_DELIVERY_LOCK();
    if (!survey_delivery_comm_owned || survey_delivery_handle != handle ||
        !survey_delivery_comm_identity_valid ||
        !survey_delivery_identity_equal(&survey_delivery_comm_identity,
                                        &owned_identity)) {
        SURVEY_DELIVERY_UNLOCK();
        app_watchdog_stop_feeding();
        return -ESTALE;
    }
    survey_delivery_comm_owned = false;
    survey_delivery_handle = 0u;
    memset(&survey_delivery_comm_identity, 0,
           sizeof(survey_delivery_comm_identity));
    survey_delivery_comm_identity_valid = false;
    if (accepted) {
        state_ret = app_mesh_local_delivery_cleanup_ack(delivery);
    } else {
        retry_delay_ms = survey_delivery_next_retry_delay_ms(&outbound);
    }
    SURVEY_DELIVERY_UNLOCK();

    if (accepted) {
        if (state_ret < 0) {
            LOG_WRN("survey delivery ACK cleanup deferred: survey=%u seq=%u ret=%d",
                    outbound.packet.session_id, outbound.packet.seq,
                    state_ret);
            (void)schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
        }
        return 0;
    }
    LOG_WRN("survey discovery report retained after transport terminal: boot=%u seq=%u reason=%u attempts=%u",
            outbound.packet.session_id,
            outbound.packet.seq,
            (unsigned int)event.reason,
            event.attempts_started);
    status_debug_printf(
        "DBG_SURVEY_REPORT_TERMINAL boot=%u seq=%u reason=%u attempts=%u retained=%u\n",
        outbound.packet.session_id,
        outbound.packet.seq,
        (unsigned int)event.reason,
        event.attempts_started,
        1u);
    (void)schedule_work_ms(retry_delay_ms);
    return -EAGAIN;
}

static int survey_delivery_service_failed_abandon(void)
{
    uint32_t handle;
    int ret;

    SURVEY_DELIVERY_LOCK();
    handle = survey_delivery_failed_abandon_handle;
    SURVEY_DELIVERY_UNLOCK();
    if (handle == 0u) {
        return 0;
    }

    ret = app_node_comm_abandon_delivery(handle);
    if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
        LOG_ERR("survey discovery orphan cleanup retry failed: handle=%u ret=%d",
                handle,
                ret);
        app_watchdog_stop_feeding();
        return ret;
    }
    SURVEY_DELIVERY_LOCK();
    if (survey_delivery_failed_abandon_handle == handle) {
        survey_delivery_failed_abandon_handle = 0u;
    }
    SURVEY_DELIVERY_UNLOCK();
    return 0;
}

static int survey_delivery_service_retirement(void)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct app_mesh_local_delivery_identity identity = {0};
    struct mesh_outbound outbound = {0};
    uint64_t owned_generation = 0u;
    uint64_t retirement_floor = 0u;
    uint32_t handle = 0u;
    int handle_state;
    int ret;

    SURVEY_DELIVERY_LOCK();
    if (!survey_delivery_retirement_in_progress) {
        SURVEY_DELIVERY_UNLOCK();
        return -ENOENT;
    }
    if (delivery == NULL || !app_mesh_local_delivery_occupied(delivery)) {
        survey_delivery_retirement_in_progress = false;
        SURVEY_DELIVERY_UNLOCK();
        return 0;
    }
    outbound = delivery->snapshot.outbound;
    retirement_floor = survey_delivery_generation_floor;
    ret = survey_operation_generation_extract_tlv(
        outbound.payload, outbound.payload_len, &owned_generation);
    if (ret != PROTO_OK || owned_generation == 0u ||
        owned_generation >= retirement_floor) {
        SURVEY_DELIVERY_UNLOCK();
        if (ret != PROTO_OK || owned_generation == 0u) {
            app_watchdog_stop_feeding();
            return -EBADMSG;
        }
        return -ESTALE;
    }
    app_mesh_local_delivery_identity_from_outbound(&outbound, &identity);
    if (survey_delivery_comm_owned) {
        if (!survey_delivery_comm_identity_valid ||
            !survey_delivery_identity_equal(&survey_delivery_comm_identity,
                                            &identity)) {
            SURVEY_DELIVERY_UNLOCK();
            app_watchdog_stop_feeding();
            return -EBADMSG;
        }
        handle = survey_delivery_handle;
        if (handle == 0u) {
            SURVEY_DELIVERY_UNLOCK();
            (void)schedule_work_ms(SURVEY_DELIVERY_EVENT_POLL_MS);
            return -EINPROGRESS;
        }
    } else if (survey_delivery_handle != 0u) {
        SURVEY_DELIVERY_UNLOCK();
        app_watchdog_stop_feeding();
        return -EBADMSG;
    }
    SURVEY_DELIVERY_UNLOCK();

    if (handle != 0u) {
        ret = app_node_comm_abandon_delivery(handle);
        if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
            (void)schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return ret;
        }
        handle_state = app_node_comm_delivery_handle_state(handle);
        if (handle_state < 0) {
            (void)schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return handle_state;
        }
        if (handle_state > 0) {
            (void)schedule_work_ms(SURVEY_DELIVERY_EVENT_POLL_MS);
            return -EINPROGRESS;
        }
    }

    SURVEY_DELIVERY_LOCK();
    if (!survey_delivery_retirement_in_progress ||
        !app_mesh_local_delivery_identity_matches_outbound(
            &identity, &delivery->snapshot.outbound) ||
        (handle != 0u &&
         (!survey_delivery_comm_owned ||
          survey_delivery_handle != handle ||
          !survey_delivery_comm_identity_valid ||
          !survey_delivery_identity_equal(&survey_delivery_comm_identity,
                                          &identity)))) {
        SURVEY_DELIVERY_UNLOCK();
        return -ESTALE;
    }
    if (handle != 0u) {
        survey_delivery_comm_owned = false;
        survey_delivery_handle = 0u;
        memset(&survey_delivery_comm_identity, 0,
               sizeof(survey_delivery_comm_identity));
        survey_delivery_comm_identity_valid = false;
    }
    ret = app_mesh_local_delivery_cancel(delivery);
    if (ret == 0) {
        survey_delivery_retry_round = 0u;
        survey_delivery_retirement_in_progress = false;
    }
    SURVEY_DELIVERY_UNLOCK();
    if (ret < 0) {
        (void)schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
        return ret;
    }
    status_debug_printf(
        "DBG_SURVEY_REPORT_SUPERSEDE_RETIRED old_generation=%llu new_generation=%llu boot=%u seq=%u\n",
        (unsigned long long)owned_generation,
        (unsigned long long)retirement_floor,
        outbound.packet.session_id,
        outbound.packet.seq);
    return 0;
}

int app_anchor_survey_discovery_retry_report(void)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    struct mesh_outbound outbound;
    uint32_t delivery_handle = 0u;
    uint32_t stale_handle = 0u;
    uint32_t retry_delay_ms = 0u;
    uint64_t absolute_deadline_ms =
        SURVEY_DISCOVERY_REPORT_SOURCE_DEADLINE_MS;
    bool submission_owned;
    bool still_current;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || delivery == NULL) {
        return 0;
    }
    ret = survey_delivery_service_failed_abandon();
    if (ret < 0) {
        return ret;
    }
    ret = survey_delivery_service_retirement();
    if (ret != -ENOENT) {
        return ret;
    }
    ret = survey_delivery_poll_comm_result();
    if (ret != -ENOENT) {
        return ret;
    }
    SURVEY_DELIVERY_LOCK();
    if (app_mesh_local_delivery_ack_committed(delivery)) {
        ret = app_mesh_local_delivery_cleanup_ack(delivery);
        SURVEY_DELIVERY_UNLOCK();
        if (ret < 0) {
            LOG_WRN("survey delivery committed ACK cleanup retry failed: ret=%d",
                    ret);
            (void)schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return -EAGAIN;
        }
        return 0;
    }
    if (survey_report_stage_retry_valid &&
        !app_mesh_local_delivery_active(delivery)) {
        ret = app_mesh_local_delivery_stage(
            delivery,
            &survey_report_stage_retry_outbound,
            survey_report_stage_retry_generation);
        if (ret == 0) {
            status_debug_printf(
                "DBG_SURVEY_REPORT_STAGE_RETRY_OK boot=%u seq=%u\n",
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
    if (app_mesh_local_delivery_outbound(delivery) == NULL) {
        SURVEY_DELIVERY_UNLOCK();
        return -EINVAL;
    }
    outbound = *app_mesh_local_delivery_outbound(delivery);
    if (outbound.earliest_tx_valid) {
        uint32_t now_ms = k_uptime_get_32();
        uint32_t not_before_delay_ms =
            outbound.earliest_tx_ms - now_ms;

        ret = app_mesh_local_delivery_retire_elapsed_not_before(
            delivery, now_ms);
        if (ret == -EAGAIN) {
            if (not_before_delay_ms >
                SURVEY_DISCOVERY_REPORT_RETRY_MAX_MS) {
                not_before_delay_ms =
                    SURVEY_DISCOVERY_REPORT_RETRY_MAX_MS;
            }
            SURVEY_DELIVERY_UNLOCK();
            (void)schedule_work_ms(not_before_delay_ms == 0u ?
                                   1u : not_before_delay_ms);
            return -EINPROGRESS;
        }
        if (ret < 0) {
            SURVEY_DELIVERY_UNLOCK();
            LOG_ERR("survey discovery response-slot retirement failed: boot=%u seq=%u ret=%d",
                    outbound.packet.session_id,
                    outbound.packet.seq,
                    ret);
            (void)schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return -EAGAIN;
        }
        /*
         * The initial response slot is source scheduling metadata, not part of
         * the immutable packet. Every retry starts from the cleared transport
         * envelope before it can wait behind an
         * unrelated reliable owner.
         */
        outbound = *app_mesh_local_delivery_outbound(delivery);
    }
    ret = mesh_report_active_owner_matches_outbound(&outbound);
    if (ret > 0) {
        SURVEY_DELIVERY_UNLOCK();
        mesh_report_wake_active_outbox("survey-discovery-owner");
        schedule_work_ms(SURVEY_DELIVERY_EVENT_POLL_MS);
        return 0;
    }
    if (ret < 0) {
        SURVEY_DELIVERY_UNLOCK();
        LOG_ERR("survey discovery active-owner collision: boot=%u seq=%u ret=%d",
                outbound.packet.session_id,
                outbound.packet.seq,
                ret);
        app_watchdog_stop_feeding();
        schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
        return ret;
    }
    if (delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_FAILED) {
        ret = survey_delivery_retain_for_retry_locked(delivery);
        SURVEY_DELIVERY_UNLOCK();
        if (ret < 0) {
            LOG_ERR("survey discovery failed-report custody retention failed: boot=%u seq=%u ret=%d",
                    outbound.packet.session_id, outbound.packet.seq, ret);
            schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return -EAGAIN;
        }
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
        retry_delay_ms = survey_delivery_next_retry_delay_ms(
            app_mesh_local_delivery_outbound(delivery));
        SURVEY_DELIVERY_UNLOCK();
        schedule_work_ms(retry_delay_ms);
        return -EAGAIN;
    }
    if (app_mesh_local_delivery_attempts_available(delivery) == 0u) {
        ret = survey_delivery_retain_for_retry_locked(delivery);
        SURVEY_DELIVERY_UNLOCK();
        if (ret < 0) {
            LOG_ERR("survey discovery attempt rearm failed: boot=%u seq=%u ret=%d",
                    outbound.packet.session_id, outbound.packet.seq, ret);
            schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
            return -EAGAIN;
        }
        schedule_work_ms(REPORT_TX_RETRY_DELAY_MS);
        return -EAGAIN;
    }
    survey_delivery_comm_owned = true;
    survey_delivery_handle = 0u;
    app_mesh_local_delivery_identity_from_outbound(
        &outbound, &survey_delivery_comm_identity);
    survey_delivery_comm_identity_valid = true;
    SURVEY_DELIVERY_UNLOCK();

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
                       app_mesh_local_delivery_identity_matches_outbound(
                           &survey_delivery_comm_identity,
                           &outbound);
    still_current = submission_owned &&
                    !survey_delivery_retirement_in_progress &&
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
        ret = app_node_comm_abandon_delivery(stale_handle);
        if (ret < 0 && ret != -ENOENT && ret != -EALREADY) {
            uint32_t retained_handle;

            SURVEY_DELIVERY_LOCK();
            if (survey_delivery_failed_abandon_handle == 0u ||
                survey_delivery_failed_abandon_handle == stale_handle) {
                survey_delivery_failed_abandon_handle = stale_handle;
            }
            retained_handle = survey_delivery_failed_abandon_handle;
            SURVEY_DELIVERY_UNLOCK();
            LOG_ERR("survey discovery stale submission abandonment failed: handle=%u ret=%d retained=%u",
                    stale_handle,
                    ret,
                    retained_handle);
            app_watchdog_stop_feeding();
            return ret;
        }
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
        "DBG_SURVEY_REPORT_COMM_SUBMIT boot=%u seq=%u handle=%u deadline=%llu\n",
        outbound.packet.session_id,
        outbound.packet.seq,
        delivery_handle,
        (unsigned long long)absolute_deadline_ms);
    schedule_work_ms(SURVEY_DELIVERY_EVENT_POLL_MS);
    return 0;
}

bool app_anchor_survey_discovery_report_staged(uint32_t operation_session_id)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    bool staged = false;

    if (DEVICE_ROLE != ROLE_ANCHOR || delivery == NULL ||
        operation_session_id == 0u) {
        return false;
    }
    SURVEY_DELIVERY_LOCK();
    staged = app_mesh_local_delivery_active(delivery) &&
             delivery->snapshot.generation == operation_session_id;
    SURVEY_DELIVERY_UNLOCK();
    return staged;
}

int app_anchor_survey_discovery_report_custody_status(
    uint64_t operation_generation)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    uint64_t owned_generation = 0u;
    int ret;

    if (delivery == NULL || operation_generation == 0u) {
        return -EINVAL;
    }

    SURVEY_DELIVERY_LOCK();
    if (survey_report_stage_retry_valid) {
        ret = survey_operation_generation_extract_tlv(
            survey_report_stage_retry_outbound.payload,
            survey_report_stage_retry_outbound.payload_len,
            &owned_generation);
        SURVEY_DELIVERY_UNLOCK();
        if (ret != PROTO_OK) {
            return -EBADMSG;
        }
        return operation_generation == owned_generation ?
            -EALREADY : -EBUSY;
    }
    if (!app_mesh_local_delivery_occupied(delivery)) {
        SURVEY_DELIVERY_UNLOCK();
        return 0;
    }
    ret = survey_operation_generation_extract_tlv(
        delivery->snapshot.outbound.payload,
        delivery->snapshot.outbound.payload_len,
        &owned_generation);
    SURVEY_DELIVERY_UNLOCK();
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    return operation_generation == owned_generation ? -EALREADY : -EBUSY;
}

int app_anchor_survey_discovery_supersede_before(
    uint64_t operation_generation,
    bool *retirement_pending)
{
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    uint64_t active_generation = 0u;
    uint64_t retry_generation = 0u;
    bool pending = false;
    int ret;

    if (operation_generation == 0u || (uint32_t)operation_generation == 0u ||
        retirement_pending == NULL) {
        return -EINVAL;
    }
    *retirement_pending = false;
    if (DEVICE_ROLE != ROLE_ANCHOR || delivery == NULL) {
        return 0;
    }

    SURVEY_DELIVERY_LOCK();
    if (operation_generation < survey_delivery_generation_floor) {
        SURVEY_DELIVERY_UNLOCK();
        return -ESTALE;
    }
    if (survey_delivery_retirement_in_progress &&
        !app_mesh_local_delivery_occupied(delivery)) {
        survey_delivery_retirement_in_progress = false;
    }
    if (survey_delivery_comm_owned &&
        !app_mesh_local_delivery_occupied(delivery)) {
        SURVEY_DELIVERY_UNLOCK();
        app_watchdog_stop_feeding();
        return -EBADMSG;
    }
    if (survey_report_stage_retry_valid) {
        ret = survey_operation_generation_extract_tlv(
            survey_report_stage_retry_outbound.payload,
            survey_report_stage_retry_outbound.payload_len,
            &retry_generation);
        if (ret != PROTO_OK || retry_generation == 0u) {
            SURVEY_DELIVERY_UNLOCK();
            app_watchdog_stop_feeding();
            return -EBADMSG;
        }
        if (retry_generation > operation_generation) {
            SURVEY_DELIVERY_UNLOCK();
            return -ESTALE;
        }
    }
    if (app_mesh_local_delivery_occupied(delivery)) {
        ret = survey_operation_generation_extract_tlv(
            delivery->snapshot.outbound.payload,
            delivery->snapshot.outbound.payload_len,
            &active_generation);
        if (ret != PROTO_OK || active_generation == 0u) {
            SURVEY_DELIVERY_UNLOCK();
            app_watchdog_stop_feeding();
            return -EBADMSG;
        }
        if (active_generation > operation_generation) {
            SURVEY_DELIVERY_UNLOCK();
            return -ESTALE;
        }
    }

    survey_delivery_generation_floor = operation_generation;
    if (survey_report_stage_retry_valid &&
        retry_generation < operation_generation) {
        status_debug_printf(
            "DBG_SURVEY_REPORT_SUPERSEDE_STAGE old_generation=%llu new_generation=%llu seq=%u\n",
            (unsigned long long)retry_generation,
            (unsigned long long)operation_generation,
            survey_report_stage_retry_outbound.packet.seq);
        memset(&survey_report_stage_retry_outbound, 0,
               sizeof(survey_report_stage_retry_outbound));
        survey_report_stage_retry_generation = 0u;
        survey_report_stage_retry_valid = false;
    }
    if (app_mesh_local_delivery_occupied(delivery) &&
        active_generation < operation_generation) {
        survey_delivery_retirement_in_progress = true;
        pending = true;
        status_debug_printf(
            "DBG_SURVEY_REPORT_SUPERSEDE_OWNED old_generation=%llu new_generation=%llu boot=%u seq=%u handle=%u\n",
            (unsigned long long)active_generation,
            (unsigned long long)operation_generation,
            delivery->snapshot.outbound.packet.session_id,
            delivery->snapshot.outbound.packet.seq,
            survey_delivery_handle);
    }
    if (survey_delivery_failed_abandon_handle != 0u) {
        pending = true;
    }
    SURVEY_DELIVERY_UNLOCK();

    *retirement_pending = pending;
    if (pending) {
        (void)schedule_work_ms(0u);
        return -EINPROGRESS;
    }
    return 0;
}

void app_anchor_survey_discovery_handle_start(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
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
        packet->flags != FLAG_DIAGNOSTIC ||
        packet->dst_id != MESH_BROADCAST_ID || packet->src_id != GATEWAY_ID) {
        return;
    }
    received_at_ms = k_uptime_get_32();

    ret = survey_extract_discovery_start_tlvs(payload, payload_len, &config);
    if (ret != PROTO_OK || config.operation_generation == 0u ||
        packet->session_id !=
            survey_operation_session_id(config.operation_generation)) {
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

    admission = discovery_ops.admit_start(&config);
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
    if (uptime_deadline_reached(now_ms, start_at_ms) ||
        uptime_ms_until_deadline(now_ms, start_at_ms) <=
            SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS) {
        schedule_delay_ms = 0u;
    } else {
        schedule_delay_ms = uptime_ms_until_deadline(now_ms, start_at_ms) -
                            SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS;
    }
    ret = discovery_ops.queue_start(&config,
                                    start_at_ms,
                                    schedule_delay_ms);
    if (ret < 0) {
        LOG_ERR("survey discovery start scheduling rejected after admission: survey=%u ret=%d",
                config.survey_id,
                ret);
        return;
    }
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
                                   int8_t rssi_dbm,
                                   uint8_t quality)
{
    struct survey_reachability_entry candidate;

    if (entries == NULL || entry_count == NULL ||
        !survey_peer_reportable(peer_id)) {
        return;
    }

    if (quality > 100u) {
        quality = 100u;
    }
    candidate = (struct survey_reachability_entry) {
        .peer_id = peer_id,
        .rssi_dbm = rssi_dbm,
        .quality = quality,
    };
    (void)survey_reachability_entry_retain(entries, entry_cap, entry_count,
                                           &candidate);
}

static bool survey_discovery_rx_outcome_is_functional(
    int ret,
    enum dwm3000_rx_failure failure)
{
    return ret == 0 ||
           ret == -ETIMEDOUT ||
           failure != DWM3000_RX_FAILURE_NONE;
}

static int receive_survey_probes_until(
    const struct survey_discovery_config *config,
    uint32_t deadline_ms,
    struct survey_reachability_entry *entries,
    size_t entry_cap,
    size_t *entry_count,
    bool *functional_radio_outcome)
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
        uint32_t receive_ms;
        int ret;

        if (remaining_ms == 0u) {
            break;
        }
        receive_ms = MIN(remaining_ms,
                         SURVEY_DISCOVERY_RADIO_PROGRESS_SLICE_MS);
        ret = dwm3000_driver_receive_frame_continuous(
            receive_ms, frame, sizeof(frame), &frame_len, &quality,
            &rsl_dbm, &rx_failure);
        if (survey_discovery_rx_outcome_is_functional(ret, rx_failure)) {
            if (functional_radio_outcome != NULL) {
                *functional_radio_outcome = true;
            }
            app_watchdog_note_radio_progress();
        } else if (ret < 0) {
            return ret;
        }
        if (ret == -ETIMEDOUT) {
            if (receive_ms < remaining_ms) {
                continue;
            }
            break;
        }
        if (ret != 0 ||
            uwb_decode_survey_discovery_probe(frame, frame_len, &probe) != PROTO_OK ||
            probe.network_id != NETWORK_ID ||
            probe.survey_id != config->survey_id ||
            probe.operation_generation != config->operation_generation ||
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
                               probe.anchor_id, rsl_dbm, quality);
    }
    return 0;
}

static int send_local_survey_probe(
    const struct survey_discovery_config *config,
    uint8_t opportunity,
    uint64_t absolute_tx_deadline_ms,
    uint8_t *slot_out,
    bool *rf_started)
{
    struct uwb_survey_discovery_probe_frame probe = {
        .network_id = NETWORK_ID,
        .survey_id = config->survey_id,
        .operation_generation = config->operation_generation,
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
    {
        struct dwm3000_tx_observation observation = {0};

        ret = dwm3000_driver_send_frame_tracked_until(
            frame,
            frame_len,
            SURVEY_DISCOVERY_TX_TIMEOUT_MS,
            absolute_tx_deadline_ms,
            &observation);
        if (rf_started != NULL) {
            *rf_started = observation.rf_started;
        }
        return ret;
    }
}

static uint64_t survey_expand_future_uptime32(uint32_t deadline_ms)
{
    uint64_t now_ms = (uint64_t)k_uptime_get();
    int32_t remaining_ms =
        (int32_t)(deadline_ms - (uint32_t)now_ms);

    if (remaining_ms <= 0) {
        return now_ms;
    }
    return now_ms + (uint32_t)remaining_ms;
}

int app_anchor_survey_discovery_run(
    const struct survey_discovery_config *config,
    uint32_t start_ms,
    bool *functional_radio_outcome)
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
    if (functional_radio_outcome != NULL) {
        *functional_radio_outcome = false;
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
            uint32_t latest_tx_deadline_ms;
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
                    ARRAY_SIZE(entries), &entry_count,
                    functional_radio_outcome);
                if (ret < 0) {
                    return ret;
                }
            }
            relative_now_ms = k_uptime_get_32() - start_ms;
            if (relative_now_ms <= nominal.latest_tx_start_ms) {
                latest_tx_deadline_ms =
                    start_ms + nominal.latest_tx_start_ms + 1u;
                ret = send_local_survey_probe(
                    config,
                    opportunity,
                    survey_expand_future_uptime32(
                        latest_tx_deadline_ms),
                    &probe_slot,
                    &rf_started);
                if (ret < 0) {
                    LOG_WRN("survey discovery probe TX failed: survey=%u round=%u rounds=%u slot=%u rf_started=%u ret=%d",
                            config->survey_id, opportunity,
                            config->round_count, probe_slot,
                            rf_started ? 1u : 0u, ret);
                } else if (functional_radio_outcome != NULL) {
                    *functional_radio_outcome = true;
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
                ARRAY_SIZE(entries), &entry_count,
                functional_radio_outcome);
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
                ARRAY_SIZE(entries), &entry_count,
                functional_radio_outcome);
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
    ret = prepare_discovery_report(config->operation_generation,
                                   config->survey_id, entries, entry_count,
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

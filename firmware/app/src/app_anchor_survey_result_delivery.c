#include "app_anchor_survey_result_delivery.h"

#include "app_board.h"
#include "app_config.h"
#include "app_mesh_local_delivery.h"
#include "app_mesh_persistence.h"
#include "app_node_comm.h"
#include "app_watchdog.h"
#include "survey.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_DECLARE(app_anchor, LOG_LEVEL_DBG);

#define SURVEY_PAIR_RESULT_EVENT_POLL_MS 100u
#define SURVEY_PAIR_RESULT_RETRY_MS 50u
#define SURVEY_PAIR_RESULT_SOURCE_DEADLINE_MS UINT64_MAX

struct survey_pair_result_delivery_slot {
    struct app_mesh_local_delivery delivery;
    uint32_t handle;
    uint8_t index;
    bool admission_in_progress;
};

static struct app_anchor_survey_result_delivery_ops result_delivery_ops;
static struct survey_pair_result_delivery_slot result_delivery_slots[
    APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS];
static bool result_delivery_initialized;

BUILD_ASSERT(APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS ==
                 SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
             "durable result slots must cover one maximum survey endpoint burst");
BUILD_ASSERT(APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY >=
                 SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
             "node communication must reserve one complete survey endpoint burst");

#if DEVICE_ROLE == ROLE_ANCHOR
K_MUTEX_DEFINE(result_delivery_lock);
#define RESULT_DELIVERY_LOCK() \
    k_mutex_lock(&result_delivery_lock, K_FOREVER)
#define RESULT_DELIVERY_UNLOCK() k_mutex_unlock(&result_delivery_lock)
#else
#define RESULT_DELIVERY_LOCK() do { } while (0)
#define RESULT_DELIVERY_UNLOCK() do { } while (0)
#endif

static int result_delivery_schedule(uint32_t delay_ms)
{
    int ret;

    if (result_delivery_ops.schedule_work_ms == NULL) {
        return -ENODEV;
    }
    ret = result_delivery_ops.schedule_work_ms(delay_ms);
    if (ret < 0) {
        LOG_ERR("survey pair result work scheduling failed: delay=%u ret=%d",
                delay_ms, ret);
        app_watchdog_stop_feeding();
    }
    return ret;
}

static int result_delivery_abandon_handle(uint32_t handle,
                                          const char *reason)
{
    int ret;

    if (handle == 0u) {
        return 0;
    }
    ret = app_node_comm_abandon_delivery(handle);
    if (ret < 0) {
        LOG_ERR("survey pair result duplicate handle abandon failed closed: handle=%u reason=%s ret=%d",
                handle,
                reason == NULL ? "unknown" : reason,
                ret);
        app_watchdog_stop_feeding();
    }
    return ret;
}

static int result_delivery_save(
    void *ctx,
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    const struct survey_pair_result_delivery_slot *slot = ctx;

    if (slot == NULL) {
        return -EINVAL;
    }
    return app_mesh_persistence_save_survey_pair_result_delivery(
        slot->index, snapshot);
}

static int result_delivery_clear(void *ctx)
{
    const struct survey_pair_result_delivery_slot *slot = ctx;

    if (slot == NULL) {
        return -EINVAL;
    }
    return app_mesh_persistence_clear_survey_pair_result_delivery(
        slot->index);
}

static bool result_delivery_slot_matches(
    const struct survey_pair_result_delivery_slot *slot,
    const struct proto_packet *packet)
{
    struct app_mesh_local_delivery_identity identity;

    if (slot == NULL || packet == NULL ||
        !app_mesh_local_delivery_occupied(&slot->delivery)) {
        return false;
    }
    app_mesh_local_delivery_identity_from_outbound(
        &slot->delivery.snapshot.outbound, &identity);
    return app_mesh_local_delivery_identity_matches(&identity, packet);
}

static struct survey_pair_result_delivery_slot *
result_delivery_find_slot_locked(const struct proto_packet *packet)
{
    for (size_t i = 0u;
         i < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         i++) {
        if (result_delivery_slot_matches(&result_delivery_slots[i],
                                         packet)) {
            return &result_delivery_slots[i];
        }
    }
    return NULL;
}

static struct survey_pair_result_delivery_slot *
result_delivery_find_empty_slot_locked(void)
{
    for (size_t i = 0u;
         i < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         i++) {
        if (!app_mesh_local_delivery_occupied(
                &result_delivery_slots[i].delivery)) {
            return &result_delivery_slots[i];
        }
    }
    return NULL;
}

static bool result_delivery_any_occupied_locked(void)
{
    for (size_t i = 0u;
         i < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         i++) {
        if (app_mesh_local_delivery_occupied(
                &result_delivery_slots[i].delivery)) {
            return true;
        }
    }
    return false;
}

static bool result_delivery_same_packet(
    const struct survey_pair_result_delivery_slot *slot,
    const struct proto_packet *packet)
{
    return result_delivery_slot_matches(slot, packet);
}

static bool result_delivery_semantic_digest_matches(
    const struct survey_pair_result_delivery_slot *slot,
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    const struct mesh_outbound *outbound;
    uint8_t stored_digest[SEMANTIC_DIGEST_SHA256_LEN];

    if (slot == NULL || packet == NULL || semantic_digest == NULL ||
        !app_mesh_local_delivery_occupied(&slot->delivery)) {
        return false;
    }
    outbound = &slot->delivery.snapshot.outbound;
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

static void result_delivery_prepare_transport_envelope(
    struct mesh_outbound *outbound)
{
    if (outbound == NULL) {
        return;
    }
    /*
     * Waiting in the bounded source journal or behind nodecomm's reliable
     * single-flight owner is pre-transport custody.  Do not age a packet that
     * has never entered RF; the mesh transport stamps queued_at exactly once
     * when it first has to retain the packet for a route or retry.
     */
    outbound->queued_at_ms = 0u;
    outbound->queued_at_valid = false;
    outbound->earliest_tx_ms = 0u;
    outbound->earliest_tx_valid = false;
}

/* Caller holds result_delivery_lock. */
static int result_delivery_retain_for_retry_locked(
    struct survey_pair_result_delivery_slot *slot)
{
    uint8_t state;
    int ret;

    if (slot == NULL ||
        !app_mesh_local_delivery_active(&slot->delivery)) {
        return -ENOENT;
    }
    state = slot->delivery.snapshot.state;
    if (state == APP_MESH_LOCAL_DELIVERY_STARTING ||
        state == APP_MESH_LOCAL_DELIVERY_TRACKED) {
        ret = app_mesh_local_delivery_note_attempt_released(
            &slot->delivery,
            slot->delivery.snapshot.attempt_token,
            APP_MESH_LOCAL_DELIVERY_RETRY);
        if (ret < 0) {
            return ret;
        }
    } else if (state == APP_MESH_LOCAL_DELIVERY_BLOCKED_LIVE) {
        ret = app_mesh_local_delivery_note_attempt_not_sent(
            &slot->delivery,
            slot->delivery.snapshot.attempt_token,
            APP_MESH_LOCAL_DELIVERY_RETRY);
        if (ret < 0) {
            return ret;
        }
    } else if (state != APP_MESH_LOCAL_DELIVERY_RETRY) {
        ret = app_mesh_local_delivery_note_state(
            &slot->delivery, APP_MESH_LOCAL_DELIVERY_RETRY);
        if (ret < 0) {
            return ret;
        }
    }
    if (slot->delivery.snapshot.attempts_remaining == 0u) {
        return app_mesh_local_delivery_rearm_attempts(&slot->delivery);
    }
    return 0;
}

static int result_delivery_attempt_begin(
    const struct proto_packet *packet,
    uint8_t *attempt_token)
{
    struct survey_pair_result_delivery_slot *slot;
    int ret;

    if (packet == NULL || attempt_token == NULL ||
        packet->msg_type != MSG_SURVEY_PAIR_RESULT) {
        return -ENOENT;
    }
    RESULT_DELIVERY_LOCK();
    slot = result_delivery_find_slot_locked(packet);
    if (slot == NULL ||
        !app_mesh_local_delivery_active(&slot->delivery)) {
        RESULT_DELIVERY_UNLOCK();
        return -ENOENT;
    }
    if (slot->delivery.snapshot.state ==
            APP_MESH_LOCAL_DELIVERY_STARTING ||
        slot->delivery.snapshot.state ==
            APP_MESH_LOCAL_DELIVERY_TRACKED) {
        ret = app_mesh_local_delivery_note_attempt_released(
            &slot->delivery,
            slot->delivery.snapshot.attempt_token,
            APP_MESH_LOCAL_DELIVERY_RETRY);
        if (ret < 0) {
            RESULT_DELIVERY_UNLOCK();
            return ret;
        }
    }
    ret = app_mesh_local_delivery_begin_attempt(
        &slot->delivery, attempt_token);
    RESULT_DELIVERY_UNLOCK();
    return ret;
}

static int result_delivery_attempt_complete(
    const struct proto_packet *packet,
    uint8_t attempt_token,
    bool rf_started)
{
    struct survey_pair_result_delivery_slot *slot;
    int ret;

    if (packet == NULL || attempt_token == 0u ||
        packet->msg_type != MSG_SURVEY_PAIR_RESULT) {
        return -ENOENT;
    }
    RESULT_DELIVERY_LOCK();
    slot = result_delivery_find_slot_locked(packet);
    if (slot == NULL) {
        RESULT_DELIVERY_UNLOCK();
        return -ENOENT;
    }
    /*
     * A direct gateway ACK can durably commit while the communication facade
     * is still unwinding the backend call. The ACK proves that RF started, so
     * completing that backend token is idempotent and must not strand the
     * facade's backend-release guard.
     */
    if (app_mesh_local_delivery_ack_committed(&slot->delivery)) {
        RESULT_DELIVERY_UNLOCK();
        return 0;
    }
    if (!app_mesh_local_delivery_active(&slot->delivery) ||
        slot->delivery.snapshot.attempt_token != attempt_token) {
        RESULT_DELIVERY_UNLOCK();
        return -ESTALE;
    }
    ret = rf_started ?
        app_mesh_local_delivery_note_attempt_sent(
            &slot->delivery, attempt_token) :
        app_mesh_local_delivery_note_attempt_not_sent(
            &slot->delivery,
            attempt_token,
            APP_MESH_LOCAL_DELIVERY_RETRY);
    RESULT_DELIVERY_UNLOCK();
    return ret;
}

int app_anchor_survey_result_delivery_init(
    const struct app_anchor_survey_result_delivery_ops *ops)
{
    if (ops == NULL || ops->schedule_work_ms == NULL ||
        ops->active_owner_matches_outbound == NULL ||
        ops->resume_restored_outbox == NULL) {
        return -EINVAL;
    }
    result_delivery_ops = *ops;
    result_delivery_initialized = true;
#if DEVICE_ROLE == ROLE_ANCHOR
    RESULT_DELIVERY_LOCK();
    for (uint8_t i = 0u;
         i < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         i++) {
        struct app_mesh_local_delivery_ops delivery_ops = {
            .save = result_delivery_save,
            .clear = result_delivery_clear,
            .ctx = &result_delivery_slots[i],
        };

        memset(&result_delivery_slots[i], 0,
               sizeof(result_delivery_slots[i]));
        result_delivery_slots[i].index = i;
        app_mesh_local_delivery_init(
            &result_delivery_slots[i].delivery, &delivery_ops);
    }
    RESULT_DELIVERY_UNLOCK();
    {
        const struct app_node_comm_durable_attempt_ops attempt_ops = {
            .begin = result_delivery_attempt_begin,
            .complete = result_delivery_attempt_complete,
        };
        int ret = app_node_comm_register_durable_attempt_ops(&attempt_ops);

        if (ret < 0) {
            return ret;
        }
    }
#endif
    return 0;
}

int app_anchor_survey_result_delivery_restore(bool *restored)
{
    bool any_restored = false;

    if (restored == NULL) {
        return -EINVAL;
    }
    *restored = false;
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return 0;
    }
    if (!result_delivery_initialized) {
        return -EACCES;
    }

    RESULT_DELIVERY_LOCK();
    for (uint8_t i = 0u;
         i < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         i++) {
        struct app_mesh_local_delivery_snapshot snapshot = {0};
        struct survey_pair_result_delivery_slot *slot =
            &result_delivery_slots[i];
        int ret = app_mesh_persistence_restore_survey_pair_result_delivery(
            i, &snapshot);

        slot->handle = 0u;
        if (ret == 0) {
            continue;
        }
        if (ret < 0) {
            RESULT_DELIVERY_UNLOCK();
            LOG_ERR("survey pair result journal restore failed closed: slot=%u ret=%d",
                    i, ret);
            return ret;
        }
        ret = app_mesh_local_delivery_restore(&slot->delivery, &snapshot);
        if (ret < 0 &&
            !app_mesh_local_delivery_ack_committed(&slot->delivery)) {
            RESULT_DELIVERY_UNLOCK();
            return ret;
        }
        if (app_mesh_local_delivery_active(&slot->delivery)) {
            if (slot->delivery.snapshot.state ==
                    APP_MESH_LOCAL_DELIVERY_STARTING ||
                slot->delivery.snapshot.state ==
                    APP_MESH_LOCAL_DELIVERY_TRACKED) {
                ret = app_mesh_local_delivery_note_attempt_released(
                    &slot->delivery,
                    slot->delivery.snapshot.attempt_token,
                    APP_MESH_LOCAL_DELIVERY_RETRY);
                if (ret < 0) {
                    RESULT_DELIVERY_UNLOCK();
                    return ret;
                }
            }
            ret = app_mesh_local_delivery_rebase_after_boot(
                &slot->delivery, k_uptime_get_32());
            if (ret < 0) {
                RESULT_DELIVERY_UNLOCK();
                return ret;
            }
        }
        any_restored =
            any_restored ||
            app_mesh_local_delivery_occupied(&slot->delivery);
    }
    RESULT_DELIVERY_UNLOCK();
    *restored = any_restored;
    return 0;
}

int app_anchor_survey_result_delivery_start(void)
{
    if (!result_delivery_initialized) {
        return -EACCES;
    }
    return app_anchor_survey_result_delivery_occupied_count() == 0u ?
        0 : result_delivery_schedule(0u);
}

size_t app_anchor_survey_result_delivery_occupied_count(void)
{
    size_t count = 0u;

    if (DEVICE_ROLE != ROLE_ANCHOR || !result_delivery_initialized) {
        return 0u;
    }
    RESULT_DELIVERY_LOCK();
    for (size_t i = 0u;
         i < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         i++) {
        if (app_mesh_local_delivery_occupied(
                &result_delivery_slots[i].delivery)) {
            count++;
        }
    }
    RESULT_DELIVERY_UNLOCK();
    return count;
}

int app_anchor_survey_result_delivery_stage_reserved(
    uint32_t delivery_reservation_token,
    const struct mesh_outbound *outbound)
{
    struct survey_pair_result_delivery_slot *slot;
    struct mesh_outbound durable_outbound;
    uint64_t absolute_deadline_ms;
    uint32_t handle = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || !result_delivery_initialized ||
        delivery_reservation_token == 0u || outbound == NULL ||
        outbound->packet.msg_type != MSG_SURVEY_PAIR_RESULT ||
        outbound->payload_len != outbound->packet.payload_len ||
        outbound->payload_len > APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN) {
        return -EINVAL;
    }
    durable_outbound = *outbound;
    result_delivery_prepare_transport_envelope(&durable_outbound);
    absolute_deadline_ms = SURVEY_PAIR_RESULT_SOURCE_DEADLINE_MS;

    RESULT_DELIVERY_LOCK();
    slot = result_delivery_find_empty_slot_locked();
    if (slot == NULL) {
        RESULT_DELIVERY_UNLOCK();
        return -ENOSPC;
    }
    ret = app_mesh_local_delivery_stage(
        &slot->delivery,
        &durable_outbound,
        durable_outbound.packet.session_id);
    if (ret == 0) {
        /*
         * Communication admission runs outside this mutex because it can
         * schedule the mesh owner immediately.  Keep the exact staged slot
         * privately owned until that call returns so a concurrent service
         * pass cannot submit the same durable packet through a second handle.
         */
        slot->admission_in_progress = true;
    }
    RESULT_DELIVERY_UNLOCK();
    if (ret < 0) {
        return ret;
    }

    ret = app_node_comm_commit_durable_reliable_uplink_reservation(
        delivery_reservation_token,
        &durable_outbound,
        absolute_deadline_ms,
        ((uint32_t)MSG_SURVEY_PAIR_RESULT << 16) |
            durable_outbound.packet.seq,
        &handle);
    RESULT_DELIVERY_LOCK();
    if (ret == 0 &&
        slot->admission_in_progress &&
        result_delivery_same_packet(slot, &durable_outbound.packet) &&
        slot->handle == 0u) {
        slot->handle = handle;
    } else if (ret == 0) {
        ret = -ESTALE;
    }
    slot->admission_in_progress = false;
    RESULT_DELIVERY_UNLOCK();

    if (ret == 0) {
        status_debug_printf(
            "DBG_SURVEY_PAIR_RESULT_STAGED survey=%u seq=%u slot=%u handle=%u deadline=%llu\n",
            durable_outbound.packet.session_id,
            durable_outbound.packet.seq,
            slot->index,
            handle,
            (unsigned long long)absolute_deadline_ms);
        (void)result_delivery_schedule(SURVEY_PAIR_RESULT_EVENT_POLL_MS);
        return 0;
    }
    if (handle != 0u) {
        /*
         * A successful communication admission that lost slot ownership is
         * still a live handle.  It must be explicitly abandoned before the
         * staged source record is retried, otherwise two transports can own
         * the same semantic result.
         */
        int abandon_ret =
            result_delivery_abandon_handle(handle, "initial-admission-race");

        if (abandon_ret < 0) {
            ret = abandon_ret;
        }
    }

    /*
     * The exact record remains durable when communication admission fails.
     * Runtime production stops on this return and the worker retries the
     * staged record after the caller cancels the unconsumed reservation.
     */
    (void)result_delivery_schedule(SURVEY_PAIR_RESULT_RETRY_MS);
    return ret;
}

int app_anchor_survey_result_delivery_cancel_reservations(
    uint32_t *delivery_reservation_tokens,
    size_t delivery_reservation_count,
    const char *reason)
{
    int first_error = 0;

    if (delivery_reservation_tokens == NULL) {
        return -EINVAL;
    }
    for (size_t i = 0u; i < delivery_reservation_count; i++) {
        uint32_t token = delivery_reservation_tokens[i];
        int ret;

        if (token == 0u) {
            continue;
        }
        ret = app_node_comm_cancel_durable_reliable_uplink_reservation(
            token);
        delivery_reservation_tokens[i] = 0u;
        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
        if (ret < 0) {
            LOG_ERR("survey result reservation cancellation failed closed: token=%u index=%u reason=%s ret=%d",
                    token,
                    (unsigned int)i,
                    reason == NULL ? "unknown" : reason,
                    ret);
        }
    }
    if (first_error < 0) {
        /*
         * Reservations are RAM-owned by node communication. Losing the only
         * token after a rejected cancellation would permanently reduce the
         * four-record admission capacity, so force bounded reboot recovery.
         */
        app_watchdog_stop_feeding();
    }
    return first_error;
}

static int result_delivery_service_terminal(
    struct survey_pair_result_delivery_slot *slot,
    uint32_t handle,
    const struct proto_packet *packet,
    const struct node_comm_terminal_event *event)
{
    struct app_mesh_local_delivery_identity identity;
    struct node_comm_terminal_event consumed;
    bool accepted;
    int ret = 0;

    RESULT_DELIVERY_LOCK();
    if (slot->handle != handle ||
        !result_delivery_same_packet(slot, packet)) {
        RESULT_DELIVERY_UNLOCK();
        return -ESTALE;
    }
    accepted =
        app_mesh_local_delivery_ack_committed(&slot->delivery) ||
        event->reason == NODE_COMM_TERMINAL_DELIVERED;
    if (accepted &&
        !app_mesh_local_delivery_ack_committed(&slot->delivery)) {
        app_mesh_local_delivery_identity_from_outbound(
            &slot->delivery.snapshot.outbound, &identity);
        ret = app_mesh_local_delivery_commit_ack(
            &slot->delivery, packet, identity.semantic_digest);
    } else if (!accepted) {
        ret = result_delivery_retain_for_retry_locked(slot);
    }
    RESULT_DELIVERY_UNLOCK();
    if (ret < 0) {
        (void)result_delivery_schedule(
            SURVEY_PAIR_RESULT_RETRY_MS);
        return ret;
    }

    if (!app_node_comm_take_delivery_event_for(handle, &consumed)) {
        (void)result_delivery_schedule(SURVEY_PAIR_RESULT_EVENT_POLL_MS);
        return -EAGAIN;
    }

    RESULT_DELIVERY_LOCK();
    if (slot->handle != handle ||
        !result_delivery_same_packet(slot, packet)) {
        RESULT_DELIVERY_UNLOCK();
        app_watchdog_stop_feeding();
        return -ESTALE;
    }
    slot->handle = 0u;
    if (accepted) {
        ret = app_mesh_local_delivery_cleanup_ack(&slot->delivery);
    }
    RESULT_DELIVERY_UNLOCK();

    if (ret < 0 || !accepted) {
        (void)result_delivery_schedule(SURVEY_PAIR_RESULT_RETRY_MS);
    }
    if (!accepted) {
        LOG_WRN("survey pair result retained after transport terminal: survey=%u seq=%u reason=%u attempts=%u",
                packet->session_id, packet->seq,
                (unsigned int)event->reason, event->attempts_started);
        status_debug_printf(
            "DBG_SURVEY_PAIR_RESULT_TERMINAL survey=%u seq=%u reason=%u attempts=%u retained=%u\n",
            packet->session_id,
            packet->seq,
            (unsigned int)event->reason,
            event->attempts_started,
            1u);
        return -EAGAIN;
    }
    return ret;
}

static int result_delivery_service_slot(
    struct survey_pair_result_delivery_slot *slot)
{
    struct node_comm_terminal_event event;
    struct mesh_outbound outbound;
    uint64_t absolute_deadline_ms;
    uint32_t reservation_token = 0u;
    uint32_t handle;
    uint8_t state;
    int ret;

    RESULT_DELIVERY_LOCK();
    if (!app_mesh_local_delivery_occupied(&slot->delivery)) {
        RESULT_DELIVERY_UNLOCK();
        return 0;
    }
    if (slot->admission_in_progress) {
        RESULT_DELIVERY_UNLOCK();
        (void)result_delivery_schedule(SURVEY_PAIR_RESULT_EVENT_POLL_MS);
        return -EINPROGRESS;
    }
    handle = slot->handle;
    state = slot->delivery.snapshot.state;
    outbound = slot->delivery.snapshot.outbound;
    RESULT_DELIVERY_UNLOCK();

    if (handle == 0u &&
        state != APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED) {
        ret = result_delivery_ops.active_owner_matches_outbound(&outbound);
        if (ret > 0) {
            result_delivery_ops.resume_restored_outbox(
                "survey-pair-result-owner");
            (void)result_delivery_schedule(
                SURVEY_PAIR_RESULT_EVENT_POLL_MS);
            return -EINPROGRESS;
        }
        if (ret < 0) {
            LOG_ERR("survey pair result active-owner collision: survey=%u seq=%u ret=%d",
                    outbound.packet.session_id,
                    outbound.packet.seq,
                    ret);
            app_watchdog_stop_feeding();
            (void)result_delivery_schedule(SURVEY_PAIR_RESULT_RETRY_MS);
            return ret;
        }
    }

    if (handle != 0u) {
        if (!app_node_comm_peek_delivery_event_for(handle, &event)) {
            (void)result_delivery_schedule(
                SURVEY_PAIR_RESULT_EVENT_POLL_MS);
            return -EINPROGRESS;
        }
        return result_delivery_service_terminal(
            slot, handle, &outbound.packet, &event);
    }

    if (state == APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED) {
        RESULT_DELIVERY_LOCK();
        ret = result_delivery_same_packet(slot, &outbound.packet) ?
            app_mesh_local_delivery_cleanup_ack(&slot->delivery) :
            -ESTALE;
        RESULT_DELIVERY_UNLOCK();
        if (ret < 0) {
            (void)result_delivery_schedule(SURVEY_PAIR_RESULT_RETRY_MS);
        }
        return ret;
    }
    if (state == APP_MESH_LOCAL_DELIVERY_FAILED) {
        RESULT_DELIVERY_LOCK();
        ret = result_delivery_same_packet(slot, &outbound.packet) ?
            result_delivery_retain_for_retry_locked(slot) :
            -ESTALE;
        RESULT_DELIVERY_UNLOCK();
        if (ret < 0) {
            (void)result_delivery_schedule(SURVEY_PAIR_RESULT_RETRY_MS);
            return ret;
        }
        LOG_WRN("survey pair result restored from terminal state for retry: survey=%u seq=%u",
                outbound.packet.session_id,
                outbound.packet.seq);
        state = APP_MESH_LOCAL_DELIVERY_RETRY;
    }
    if (state == APP_MESH_LOCAL_DELIVERY_STARTING ||
        state == APP_MESH_LOCAL_DELIVERY_TRACKED) {
        RESULT_DELIVERY_LOCK();
        ret = result_delivery_same_packet(slot, &outbound.packet) ?
            app_mesh_local_delivery_note_attempt_released(
                &slot->delivery,
                slot->delivery.snapshot.attempt_token,
                APP_MESH_LOCAL_DELIVERY_RETRY) :
            -ESTALE;
        RESULT_DELIVERY_UNLOCK();
        if (ret < 0) {
            (void)result_delivery_schedule(SURVEY_PAIR_RESULT_RETRY_MS);
            return ret;
        }
    }

    RESULT_DELIVERY_LOCK();
    if (!result_delivery_same_packet(slot, &outbound.packet) ||
        slot->handle != 0u) {
        RESULT_DELIVERY_UNLOCK();
        return -ESTALE;
    }
    if (app_mesh_local_delivery_attempts_available(
            &slot->delivery) == 0u) {
        ret = result_delivery_retain_for_retry_locked(slot);
        RESULT_DELIVERY_UNLOCK();
        if (ret < 0) {
            return ret;
        }
        (void)result_delivery_schedule(SURVEY_PAIR_RESULT_RETRY_MS);
        return -EAGAIN;
    }
    if (slot->admission_in_progress) {
        RESULT_DELIVERY_UNLOCK();
        (void)result_delivery_schedule(SURVEY_PAIR_RESULT_EVENT_POLL_MS);
        return -EINPROGRESS;
    }
    outbound = slot->delivery.snapshot.outbound;
    slot->admission_in_progress = true;
    RESULT_DELIVERY_UNLOCK();

    result_delivery_prepare_transport_envelope(&outbound);
    absolute_deadline_ms = SURVEY_PAIR_RESULT_SOURCE_DEADLINE_MS;

    ret = app_node_comm_reserve_durable_reliable_uplinks(
        1u, &reservation_token, 1u);
    if (ret < 0) {
        RESULT_DELIVERY_LOCK();
        if (result_delivery_same_packet(slot, &outbound.packet)) {
            slot->admission_in_progress = false;
        }
        RESULT_DELIVERY_UNLOCK();
        (void)result_delivery_schedule(SURVEY_PAIR_RESULT_RETRY_MS);
        return ret;
    }
    handle = 0u;
    ret = app_node_comm_commit_durable_reliable_uplink_reservation(
        reservation_token,
        &outbound,
        absolute_deadline_ms,
        ((uint32_t)MSG_SURVEY_PAIR_RESULT << 16) |
            outbound.packet.seq,
        &handle);
    RESULT_DELIVERY_LOCK();
    if (ret == 0 &&
        result_delivery_same_packet(slot, &outbound.packet) &&
        slot->admission_in_progress &&
        slot->handle == 0u) {
        slot->handle = handle;
    } else if (ret == 0) {
        ret = -ESTALE;
    }
    if (slot->admission_in_progress) {
        slot->admission_in_progress = false;
    }
    RESULT_DELIVERY_UNLOCK();
    if (ret < 0) {
        int cleanup_ret;

        if (handle != 0u) {
            cleanup_ret = result_delivery_abandon_handle(
                handle, "retry-admission-race");
        } else {
            uint32_t token = reservation_token;

            cleanup_ret =
                app_anchor_survey_result_delivery_cancel_reservations(
                    &token, 1u, "retry-admission-failed");
        }
        if (cleanup_ret < 0) {
            ret = cleanup_ret;
        }
        (void)result_delivery_schedule(SURVEY_PAIR_RESULT_RETRY_MS);
        return ret;
    }
    (void)result_delivery_schedule(SURVEY_PAIR_RESULT_EVENT_POLL_MS);
    return 0;
}

int app_anchor_survey_result_delivery_service(void)
{
    int first_error = 0;

    if (DEVICE_ROLE != ROLE_ANCHOR || !result_delivery_initialized) {
        return 0;
    }
    for (size_t i = 0u;
         i < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         i++) {
        int ret = result_delivery_service_slot(&result_delivery_slots[i]);

        if (ret < 0 && ret != -EINPROGRESS &&
            first_error == 0) {
            first_error = ret;
        }
    }
    return first_error;
}

int app_anchor_survey_result_delivery_gateway_confirmed(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct survey_pair_result_delivery_slot *slot;
    int schedule_ret = 0;
    int ret;

    if (packet == NULL || semantic_digest == NULL) {
        return -EINVAL;
    }
    if (DEVICE_ROLE != ROLE_ANCHOR || !result_delivery_initialized ||
        packet->msg_type != MSG_SURVEY_PAIR_RESULT) {
        return -ENOENT;
    }
    RESULT_DELIVERY_LOCK();
    slot = result_delivery_find_slot_locked(packet);
    if (slot == NULL) {
        ret = !result_delivery_any_occupied_locked() ?
            -ENOENT : -ESTALE;
    } else if (!result_delivery_semantic_digest_matches(slot,
                                                        packet,
                                                        semantic_digest)) {
        ret = -EBADMSG;
    } else {
        ret = app_mesh_local_delivery_commit_ack(
            &slot->delivery, packet, semantic_digest);
    }
    RESULT_DELIVERY_UNLOCK();
    if (ret == 0) {
        status_debug_printf(
            "DBG_SURVEY_PAIR_RESULT_ACK survey=%u seq=%u slot=%u\n",
            packet->session_id, packet->seq, slot->index);
        schedule_ret = result_delivery_schedule(0u);
    } else if (ret != -ENOENT) {
        LOG_ERR("survey pair result ACK persistence failed: survey=%u seq=%u ret=%d",
                packet->session_id, packet->seq, ret);
        schedule_ret =
            result_delivery_schedule(SURVEY_PAIR_RESULT_RETRY_MS);
    }
    return ret < 0 ? ret : schedule_ret;
}

int app_anchor_survey_result_delivery_transport_released(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    bool preempted)
{
    struct survey_pair_result_delivery_slot *slot;
    int ret = 0;

    if (packet == NULL || semantic_digest == NULL) {
        return -EINVAL;
    }
    if (DEVICE_ROLE != ROLE_ANCHOR || !result_delivery_initialized ||
        packet->msg_type != MSG_SURVEY_PAIR_RESULT) {
        return 0;
    }
    RESULT_DELIVERY_LOCK();
    slot = result_delivery_find_slot_locked(packet);
    if (slot == NULL) {
        ret = result_delivery_any_occupied_locked() ? -ESTALE : 0;
    } else if (!result_delivery_semantic_digest_matches(slot,
                                                        packet,
                                                        semantic_digest)) {
        ret = -EBADMSG;
    } else if (slot->delivery.snapshot.state ==
                   APP_MESH_LOCAL_DELIVERY_STARTING ||
               slot->delivery.snapshot.state ==
                   APP_MESH_LOCAL_DELIVERY_TRACKED) {
        ret = app_mesh_local_delivery_note_attempt_released(
            &slot->delivery,
            slot->delivery.snapshot.attempt_token,
            preempted ? APP_MESH_LOCAL_DELIVERY_PREEMPTED :
                        APP_MESH_LOCAL_DELIVERY_RETRY);
    }
    RESULT_DELIVERY_UNLOCK();
    if (ret < 0) {
        return ret;
    }
    /*
     * Persist the attempt release before the mesh primary owner is deleted.
     * Terminal polling owns the later bounded retry/failure decision.
     */
    return result_delivery_schedule(0u);
}

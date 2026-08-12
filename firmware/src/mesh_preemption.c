#include "mesh_preemption.h"

#include "protocol.h"

#include <string.h>

static uint32_t pending_message_age_at(const struct mesh_pending_tx *pending,
                                       uint32_t now_ms)
{
    uint32_t elapsed_ms;

    if (pending == NULL) {
        return 0u;
    }
    elapsed_ms = now_ms - pending->queued_at_ms;
    if (UINT32_MAX - pending->packet.message_age_ms < elapsed_ms) {
        return UINT32_MAX;
    }
    return pending->packet.message_age_ms + elapsed_ms;
}

static bool pending_expired_at(const struct mesh_pending_tx *pending,
                               uint32_t now_ms)
{
    uint64_t expiry_ms;

    if (pending == NULL) {
        return true;
    }

    expiry_ms = (uint64_t)mesh_relay_outbox_expiry_s_for_packet(
        &pending->packet, pending->payload, pending->payload_len) * 1000u;
    return (uint64_t)pending_message_age_at(pending, now_ms) >= expiry_ms;
}

static bool pending_click_transferable(const struct mesh_pending_tx *pending,
                                       uint32_t now_ms)
{
    if (pending == NULL || pending->gateway_ack_confirm_pending ||
        pending->gateway_ack_recovery_flags != 0u ||
        pending->gateway_ack_forward_pending ||
        (pending->state != MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
         pending->state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF)) {
        return false;
    }

    return !pending_expired_at(pending, now_ms);
}

static void copy_pending_click_report(const struct mesh_pending_tx *pending,
                                      uint32_t now_ms,
                                      struct mesh_click_preempt_plan *plan)
{
    if (pending == NULL || plan == NULL) {
        return;
    }

    plan->click_report.packet = pending->packet;
    plan->click_report.packet.message_age_ms =
        pending_message_age_at(pending, now_ms);
    plan->click_report.payload_len = pending->payload_len;
    plan->click_report.radio_channel = pending->radio_channel;
    plan->click_report.next_hop_id = pending->next_hop_id;
    plan->click_report.queued_at_ms = now_ms;
    plan->click_report.earliest_tx_ms =
        pending->state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF ?
            pending->retry_after_ms : now_ms;
    plan->click_report.queued_at_valid = true;
    plan->click_report.earliest_tx_valid = true;
    if (plan->click_report.payload_len > 0u) {
        memcpy(plan->click_report.payload,
               pending->payload,
               plan->click_report.payload_len);
    }
    plan->transfer_local_click = true;
}

int mesh_prepare_click_preemption(struct mesh_relay *relay,
                                  uint64_t local_id,
                                  uint32_t now_ms,
                                  struct mesh_click_preempt_plan *plan)
{
    bool pending_local_click_report;

    if (relay == NULL || plan == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(plan, 0, sizeof(*plan));
    if (!mesh_relay_tx_active(relay)) {
        return PROTO_OK;
    }
    if (pending_expired_at(&relay->pending, now_ms)) {
        /* The timeout owner terminalizes this record; a new physical click
         * must not turn expiry into a retry or let stale custody veto it. */
        return PROTO_OK;
    }

    pending_local_click_report =
        relay->pending.packet.msg_type == MSG_CLICK_REPORT &&
        relay->pending.packet.src_id == local_id;
    if (pending_local_click_report) {
        if (pending_click_transferable(&relay->pending, now_ms)) {
            copy_pending_click_report(&relay->pending, now_ms, plan);
        }
        /* Terminal, ACK-confirm, and expired local clicks retain their
         * existing owner; they must never become ordinary retry traffic. */
        return PROTO_OK;
    }
    if (mesh_relay_can_defer_tx(relay)) {
        plan->defer_active_tx = true;
        plan->schedule_timeout = true;
    }

    /* Non-deferrable work remains owned by the active runtime. */
    return PROTO_OK;
}

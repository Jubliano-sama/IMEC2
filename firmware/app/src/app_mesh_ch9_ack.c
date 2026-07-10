#include "app_mesh_ch9_ack.h"

#include <string.h>

static int ack_payload_contains_packet(const struct proto_packet *ack_packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint32_t requested_session_id,
                                       uint16_t requested_seq,
                                       bool *contains)
{
    const uint8_t *seq_value = NULL;
    const uint8_t *session_value = NULL;
    uint8_t seq_value_len = 0u;
    uint8_t session_value_len = 0u;
    uint8_t seq_count;
    int ret;
    int session_ret;

    if (contains == NULL) {
        return PROTO_ERR_ARG;
    }
    *contains = false;

    ret = tlv_find(payload, payload_len, TLV_MESH_ACK_SEQ_LIST,
                   &seq_value, &seq_value_len);
    if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    session_ret = tlv_find(payload, payload_len, TLV_MESH_ACK_SESSION_LIST,
                           &session_value, &session_value_len);
    if (session_ret != PROTO_OK && session_ret != PROTO_ERR_NOT_FOUND) {
        return session_ret;
    }

    if (ret == PROTO_OK) {
        if ((seq_value_len % sizeof(uint16_t)) != 0u) {
            return PROTO_ERR_MALFORMED;
        }
        seq_count = seq_value_len / sizeof(uint16_t);
        if (session_ret == PROTO_OK &&
            session_value_len != seq_count * sizeof(uint32_t)) {
            return PROTO_ERR_MALFORMED;
        }

        for (uint8_t i = 0u; i < seq_count; i++) {
            uint16_t seq = proto_get_u16_le(&seq_value[i * sizeof(uint16_t)]);

            if (seq != requested_seq) {
                continue;
            }
            if (session_ret == PROTO_OK) {
                uint32_t session_id =
                    proto_get_u32_le(&session_value[i * sizeof(uint32_t)]);

                if (session_id != requested_session_id) {
                    continue;
                }
            }
            *contains = true;
            return PROTO_OK;
        }
        return PROTO_OK;
    }

    ret = tlv_find(payload, payload_len, TLV_REQUESTED_MSG_SEQ,
                   &seq_value, &seq_value_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if (seq_value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }
    if (proto_get_u16_le(seq_value) == requested_seq &&
        ack_packet != NULL &&
        ack_packet->session_id == requested_session_id) {
        *contains = true;
    }
    return PROTO_OK;
}

int app_mesh_ch9_tx_ack_apply(const struct proto_packet *ack_packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              struct app_mesh_ch9_tx_ack_entry *entries,
                              uint8_t entry_count,
                              struct app_mesh_ch9_tx_ack_result *result)
{
    struct app_mesh_ch9_tx_ack_result local_result;

    if (ack_packet == NULL || payload == NULL ||
        (entry_count > 0u && entries == NULL)) {
        return PROTO_ERR_ARG;
    }
    if (ack_packet->msg_type != MSG_GATEWAY_ACK &&
        ack_packet->msg_type != MSG_MESH_HOP_ACK) {
        return PROTO_ERR_MALFORMED;
    }

    memset(&local_result, 0, sizeof(local_result));

    for (uint8_t i = 0u; i < entry_count; i++) {
        bool contains = false;
        int ret;

        if (entries[i].acked) {
            continue;
        }

        ret = ack_payload_contains_packet(ack_packet,
                                          payload,
                                          payload_len,
                                          entries[i].session_id,
                                          entries[i].seq,
                                          &contains);
        if (ret != PROTO_OK) {
            return ret;
        }
        if (contains) {
            entries[i].acked = true;
            local_result.acked_now++;
        }
    }

    for (uint8_t i = 0u; i < entry_count; i++) {
        if (!entries[i].acked) {
            local_result.unacked_count++;
        }
    }
    local_result.any_match = local_result.acked_now > 0u;
    local_result.all_acked = local_result.any_match &&
                             local_result.unacked_count == 0u;

    if (result != NULL) {
        *result = local_result;
    }
    return PROTO_OK;
}

int app_mesh_ch9_tx_requeue_unacked(struct app_mesh_ch9_tx_retry_entry *entries,
                                    uint8_t entry_count,
                                    uint32_t now_ms,
                                    const struct app_mesh_ch9_tx_retry_ops *ops,
                                    struct app_mesh_ch9_tx_retry_result *result)
{
    struct app_mesh_ch9_tx_retry_result local_result;

    if ((entry_count > 0u && entries == NULL) ||
        ops == NULL ||
        ops->put == NULL ||
        ops->queue_used == NULL) {
        return PROTO_ERR_ARG;
    }

    memset(&local_result, 0, sizeof(local_result));
    local_result.queued_before = ops->queue_used(ops->ctx);

    for (uint8_t i = 0u; i < entry_count; i++) {
        if (entries[i].acked) {
            continue;
        }

        entries[i].outbound.queued_at_ms = now_ms;
        if (ops->put(&entries[i].outbound, ops->ctx) == 0) {
            entries[i].acked = true;
            local_result.requeued++;
        } else {
            local_result.retained++;
        }
    }

    local_result.queued_after = ops->queue_used(ops->ctx);
    if (result != NULL) {
        *result = local_result;
    }
    return PROTO_OK;
}

bool app_mesh_ch9_tx_should_track_ack(const struct proto_packet *packet,
                                      bool relay_collection_result_active)
{
    if (packet == NULL) {
        return false;
    }

    return packet->msg_type != MSG_COMMAND_RESULT ||
           !relay_collection_result_active;
}

bool app_mesh_ch9_tx_should_track_sent(const struct mesh_outbound *sent,
                                       uint64_t local_id)
{
    if (sent == NULL || local_id == 0u) {
        return false;
    }

    return sent->radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
           sent->next_hop_id != 0u &&
           sent->next_hop_id != local_id &&
           sent->next_hop_id == sent->packet.dst_id &&
           sent->packet.dst_id != local_id &&
           sent->packet.msg_type != MSG_COMMAND_RESULT &&
           sent->packet.msg_type != MSG_RESULT_BUNDLE &&
           (sent->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u;
}

bool app_mesh_ch9_core_ack_wait_active(const struct mesh_pending_tx *pending,
                                       bool relay_tx_active)
{
    return relay_tx_active && pending != NULL &&
           pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
           pending->radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
           pending->next_hop_id != 0u;
}

uint8_t app_mesh_ch9_tx_max_in_flight(const struct proto_packet *packet,
                                      uint64_t next_hop_id,
                                      uint8_t configured_max)
{
    if (configured_max == 0u) {
        return 0u;
    }
    if (packet == NULL || next_hop_id == 0u) {
        return configured_max;
    }

    if ((packet->flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u &&
        (next_hop_id != packet->dst_id ||
         packet->msg_type == MSG_COMMAND_RESULT ||
         packet->msg_type == MSG_RESULT_BUNDLE)) {
        return 1u;
    }

    return configured_max;
}

bool app_mesh_ch9_tx_requires_tracked_single(const struct proto_packet *packet,
                                             uint64_t next_hop_id,
                                             uint8_t configured_max)
{
    return packet != NULL && configured_max > 0u &&
           app_mesh_ch9_tx_max_in_flight(packet,
                                         next_hop_id,
                                         configured_max) == 1u;
}

bool app_mesh_ch9_retry_next_local_tx_prepare_ms(
    const struct mesh_event_timing *timing,
    uint16_t minimum_guard_ms,
    uint32_t *prepare_ms)
{
    struct mesh_event_timing next;
    uint32_t guard_ms;

    if (timing == NULL || prepare_ms == NULL ||
        timing->event_interval_ms == 0u ||
        mesh_event_timing_local_tx_slot(timing)) {
        return false;
    }

    next = *timing;
    next.next_event_time_ms += next.event_interval_ms;
    next.event_counter++;
    if (!mesh_event_timing_local_tx_slot(&next)) {
        return false;
    }

    guard_ms = next.guard_ms > minimum_guard_ms ?
               next.guard_ms : minimum_guard_ms;
    *prepare_ms = next.next_event_time_ms > guard_ms ?
                  next.next_event_time_ms - guard_ms : 1u;
    return true;
}

bool app_mesh_ch9_wait_plan_retry_delay_ms(uint32_t now_ms,
                                          uint32_t event_start_ms,
                                          uint16_t minimum_guard_ms,
                                          uint32_t *delay_ms)
{
    uint32_t prepare_ms;
    int32_t delta_ms;

    if (event_start_ms == 0u || delay_ms == NULL) {
        return false;
    }

    prepare_ms = event_start_ms - minimum_guard_ms;
    delta_ms = (int32_t)(prepare_ms - now_ms);
    *delay_ms = delta_ms > 0 ? (uint32_t)delta_ms : 1u;
    return true;
}

bool app_mesh_ch9_tx_timeout_counts_gateway_failure(
    const struct mesh_outbound *outbound,
    uint64_t next_hop_id,
    uint64_t gateway_id)
{
    if (outbound == NULL || gateway_id == 0u) {
        return false;
    }

    return outbound->radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
           outbound->packet.dst_id == gateway_id &&
           next_hop_id == gateway_id &&
           (outbound->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u;
}

enum app_mesh_ch9_timeout_pressure_action
app_mesh_ch9_timeout_pressure_decide(const struct mesh_outbound *outbound,
                                     bool anchor_role,
                                     bool downstream_reserved,
                                     uint64_t local_id)
{
    if (outbound == NULL || !anchor_role || !downstream_reserved ||
        local_id == 0u) {
        return APP_MESH_CH9_TIMEOUT_RETRY;
    }
    if (outbound->packet.src_id != local_id) {
        return APP_MESH_CH9_TIMEOUT_DROP_TRANSIT;
    }
    if (outbound->packet.msg_type == MSG_CLICK_REPORT ||
        outbound->packet.msg_type == MSG_COMMAND_RESULT) {
        return APP_MESH_CH9_TIMEOUT_PREEMPT_FOR_LOCAL;
    }
    return APP_MESH_CH9_TIMEOUT_DEFER_LOCAL;
}

bool app_mesh_direct_gateway_ack_matches(const struct mesh_outbound *sent,
                                         const struct proto_packet *ack_packet,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         uint64_t previous_hop_id,
                                         uint64_t gateway_id)
{
    bool contains = false;

    if (sent == NULL || ack_packet == NULL || gateway_id == 0u ||
        sent->packet.src_id == 0u) {
        return false;
    }

    if (previous_hop_id != gateway_id ||
        ack_packet->msg_type != MSG_GATEWAY_ACK ||
        ack_packet->src_id != gateway_id ||
        ack_packet->dst_id != sent->packet.src_id) {
        return false;
    }

    return ack_payload_contains_packet(ack_packet,
                                       payload,
                                       payload_len,
                                       sent->packet.session_id,
                                       sent->packet.seq,
                                       &contains) == PROTO_OK &&
           contains;
}

bool app_mesh_ch9_ack_complete_should_close_timing(
    const struct app_mesh_ch9_ack_complete_state *state)
{
    if (state == NULL || !state->route_test_enabled) {
        return false;
    }

    /*
     * ACK completion only closes the finite payload/ACK attempt. The repeating
     * channel-9 timing remains supervised until explicit policy, replacement,
     * or missed-event expiry clears it.
     */
    return false;
}

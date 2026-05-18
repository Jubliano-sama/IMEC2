#include "mesh_relay.h"

#include "mesh.h"

#include <string.h>

#define LEGACY_MSG_ROUTE_ADV 0x33u
#define LEGACY_MSG_ROUTE_STATUS 0x34u

static bool id_is_unicast(uint64_t id)
{
    return id != MESH_BROADCAST_ID;
}

static uint16_t relay_next_seq(struct mesh_relay *relay)
{
    relay->next_seq++;
    if (relay->next_seq == 0u) {
        relay->next_seq = 1u;
    }
    return relay->next_seq;
}

static void result_reset(struct mesh_relay_result *result)
{
    memset(result, 0, sizeof(*result));
    result->status = PROTO_OK;
}

static int append_route_discovery_tlvs(uint8_t *payload,
                                       size_t payload_cap,
                                       size_t *offset,
                                       uint64_t origin_id,
                                       uint64_t target_id,
                                       uint32_t route_epoch,
                                       uint8_t hop_count,
                                       uint8_t quality)
{
    int ret;

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_INITIATOR_ID, origin_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_RESPONDER_ID, target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_ROUTE_EPOCH, route_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_HOP_COUNT, hop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, quality);
}

static int find_u64_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint64_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u64_le(tlv_value);
    return PROTO_OK;
}

static int find_u32_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint32_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u32_le(tlv_value);
    return PROTO_OK;
}

static int find_u8_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint8_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, type, &tlv_value, &tlv_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (tlv_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = tlv_value[0];
    return PROTO_OK;
}

static uint8_t combined_quality(uint8_t advertised_quality, uint8_t link_quality)
{
    if (advertised_quality > 100u) {
        advertised_quality = 100u;
    }
    if (link_quality > 100u) {
        link_quality = 100u;
    }
    if (link_quality == 0u) {
        return advertised_quality;
    }
    if (advertised_quality == 0u) {
        return link_quality;
    }
    return advertised_quality < link_quality ? advertised_quality : link_quality;
}

static uint16_t downlink_effective_cost(const struct mesh_downlink_entry *entry)
{
    return (uint16_t)((uint16_t)entry->hop_count * 100u + (uint16_t)(100u - entry->quality));
}

static bool downlink_is_better(const struct mesh_downlink_entry *candidate,
                               const struct mesh_downlink_entry *selected);

static int downlink_index(const struct mesh_relay *relay, uint64_t target_id)
{
    int selected_index = -1;
    const struct mesh_downlink_entry *selected = NULL;

    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        const struct mesh_downlink_entry *entry = &relay->downlinks[i];

        if (!entry->valid || entry->target_id != target_id) {
            continue;
        }
        if (downlink_is_better(entry, selected)) {
            selected = entry;
            selected_index = (int)i;
        }
    }
    return selected_index;
}

static int downlink_exact_index(const struct mesh_relay *relay,
                                uint64_t target_id,
                                uint64_t gateway_id,
                                uint64_t next_hop_id)
{
    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        const struct mesh_downlink_entry *entry = &relay->downlinks[i];

        if (entry->valid &&
            entry->target_id == target_id &&
            entry->gateway_id == gateway_id &&
            entry->next_hop_id == next_hop_id) {
            return (int)i;
        }
    }
    return -1;
}

static bool downlink_is_better(const struct mesh_downlink_entry *candidate,
                               const struct mesh_downlink_entry *selected)
{
    uint16_t candidate_cost;
    uint16_t selected_cost;

    if (selected == NULL) {
        return true;
    }
    if (candidate->route_epoch != selected->route_epoch) {
        return candidate->route_epoch > selected->route_epoch;
    }
    candidate_cost = downlink_effective_cost(candidate);
    selected_cost = downlink_effective_cost(selected);
    if (candidate_cost != selected_cost) {
        return candidate_cost < selected_cost;
    }
    if (candidate->quality != selected->quality) {
        return candidate->quality > selected->quality;
    }
    if (candidate->hop_count != selected->hop_count) {
        return candidate->hop_count < selected->hop_count;
    }
    if (candidate->last_seen_ms != selected->last_seen_ms) {
        return candidate->last_seen_ms > selected->last_seen_ms;
    }
    return candidate->next_hop_id < selected->next_hop_id;
}

static int upsert_downlink(struct mesh_relay *relay, const struct mesh_downlink_entry *entry)
{
    int index;
    int free_index = -1;
    int replace_index = 0;
    const struct mesh_downlink_entry *replace = NULL;

    if (!id_is_unicast(entry->target_id) ||
        !id_is_unicast(entry->next_hop_id) ||
        !id_is_unicast(entry->gateway_id) ||
        entry->target_id == relay->local_id ||
        entry->quality > 100u) {
        return PROTO_ERR_MALFORMED;
    }

    index = downlink_exact_index(relay,
                                 entry->target_id,
                                 entry->gateway_id,
                                 entry->next_hop_id);
    if (index >= 0) {
        relay->downlinks[index] = *entry;
        relay->downlinks[index].valid = true;
        return PROTO_OK;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        if (!relay->downlinks[i].valid) {
            free_index = (int)i;
            break;
        }
        if (replace == NULL || downlink_is_better(replace, &relay->downlinks[i])) {
            replace = &relay->downlinks[i];
            replace_index = (int)i;
        }
    }

    if (free_index < 0 && !downlink_is_better(entry, replace)) {
        return PROTO_ERR_NO_SPACE;
    }
    index = free_index >= 0 ? free_index : replace_index;
    relay->downlinks[index] = *entry;
    relay->downlinks[index].valid = true;
    return PROTO_OK;
}

static bool duplicate_tracked(const struct proto_packet *packet)
{
    (void)packet;
    return true;
}

static void duplicate_expire_stale(struct mesh_relay *relay, uint32_t now_ms)
{
    struct mesh_duplicate_entry *entry;

    for (uint8_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        entry = &relay->duplicates[i];
        if (entry->valid && (uint32_t)(now_ms - entry->last_seen_ms) > ROUTE_DEDUP_WINDOW_MS) {
            entry->valid = false;
        }
    }
}

static bool duplicate_seen(struct mesh_relay *relay,
                           const struct proto_packet *packet,
                           uint32_t now_ms)
{
    struct mesh_duplicate_entry *entry;

    if (!duplicate_tracked(packet)) {
        return false;
    }

    duplicate_expire_stale(relay, now_ms);
    for (uint8_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        entry = &relay->duplicates[i];
        if (entry->valid &&
            entry->msg_type == packet->msg_type &&
            entry->src_id == packet->src_id &&
            entry->dst_id == packet->dst_id &&
            entry->session_id == packet->session_id &&
            entry->seq == packet->seq) {
            return true;
        }
    }
    return false;
}

static void duplicate_store(struct mesh_relay *relay,
                            const struct proto_packet *packet,
                            uint32_t now_ms)
{
    struct mesh_duplicate_entry *entry;

    if (!duplicate_tracked(packet)) {
        return;
    }

    duplicate_expire_stale(relay, now_ms);
    entry = &relay->duplicates[relay->duplicate_next];
    entry->msg_type = packet->msg_type;
    entry->src_id = packet->src_id;
    entry->dst_id = packet->dst_id;
    entry->session_id = packet->session_id;
    entry->last_seen_ms = now_ms;
    entry->seq = packet->seq;
    entry->valid = true;
    relay->duplicate_next = (uint8_t)((relay->duplicate_next + 1u) % MESH_RELAY_DUP_CACHE_SIZE);
}

static int requested_seq_from_ack(const uint8_t *payload, size_t payload_len, uint16_t *requested_seq)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    ret = tlv_find(payload, payload_len, TLV_REQUESTED_MSG_SEQ, &value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *requested_seq = proto_get_u16_le(value);
    return PROTO_OK;
}

static void outbound_from_pending(const struct mesh_pending_tx *pending,
                                  struct mesh_outbound *out)
{
    out->packet = pending->packet;
    if (pending->payload_len > 0u) {
        memcpy(out->payload, pending->payload, pending->payload_len);
    }
    out->payload_len = pending->payload_len;
    out->radio_channel = pending->radio_channel;
    out->next_hop_id = pending->next_hop_id;
}

static void pending_set_deadlines(struct mesh_pending_tx *pending, uint32_t now_ms)
{
    pending->gateway_ack_deadline_ms = now_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS;
}

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool pending_ack_matches(const struct mesh_relay *relay,
                                const struct proto_packet *packet,
                                uint16_t requested_seq)
{
    const struct mesh_pending_tx *pending = &relay->pending;

    return pending->state != MESH_RELAY_TX_IDLE &&
           packet->dst_id == relay->local_id &&
           packet->session_id == pending->packet.session_id &&
           requested_seq == pending->packet.seq;
}

static void refresh_downlink(struct mesh_relay *relay,
                             uint64_t target_id,
                             uint64_t next_hop_id,
                             uint32_t now_ms)
{
    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        struct mesh_downlink_entry *entry = &relay->downlinks[i];

        if (entry->valid &&
            entry->target_id == target_id &&
            entry->next_hop_id == next_hop_id) {
            entry->last_seen_ms = now_ms;
            return;
        }
    }
}

static int build_gateway_ack(struct mesh_relay *relay,
                             const struct proto_packet *packet,
                             uint64_t previous_hop_id,
                             struct mesh_outbound *out)
{
    uint64_t next_hop_id = previous_hop_id;
    size_t payload_len = 0u;
    int ret;

    ret = mesh_append_requested_seq(out->payload, sizeof(out->payload), &payload_len, packet->seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_init_gateway_ack(&out->packet,
                                relay->local_id,
                                packet->src_id,
                                packet->session_id,
                                relay_next_seq(relay),
                                (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }

    if (mesh_relay_select_next_hop(relay, packet->src_id, &next_hop_id) != PROTO_OK) {
        next_hop_id = previous_hop_id;
    }
    if (!id_is_unicast(next_hop_id)) {
        return PROTO_ERR_NOT_FOUND;
    }

    out->payload_len = (uint8_t)payload_len;
    out->next_hop_id = next_hop_id;
    return PROTO_OK;
}

static int handle_local_ack(struct mesh_relay *relay,
                            const struct proto_packet *packet,
                            const uint8_t *payload,
                            size_t payload_len,
                            uint32_t now_ms,
                            struct mesh_relay_result *result)
{
    uint16_t requested_seq = 0u;
    int ret;

    if (packet->dst_id != relay->local_id || packet->msg_type != MSG_GATEWAY_ACK) {
        return PROTO_OK;
    }

    ret = requested_seq_from_ack(payload, payload_len, &requested_seq);
    if (ret != PROTO_OK) {
        result->status = ret;
        return ret;
    }
    if (!pending_ack_matches(relay, packet, requested_seq)) {
        return PROTO_OK;
    }

    if (relay->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
        packet->src_id == relay->gateway_id) {
        result->actions |= MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED;
        relay->pending.state = MESH_RELAY_TX_IDLE;
        route_record_success_at(&relay->upstream, now_ms);
    }

    return PROTO_OK;
}

static int build_forward(const struct mesh_relay *relay,
                         const struct proto_packet *packet,
                         const uint8_t *payload,
                         size_t payload_len,
                         struct mesh_outbound *out)
{
    uint64_t next_hop_id = 0u;
    int ret;

    if (packet->ttl == 0u) {
        return PROTO_ERR_STALE;
    }
    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    if (payload_len > 0u) {
        memcpy(out->payload, payload, payload_len);
    }
    out->payload_len = (uint8_t)payload_len;
    out->next_hop_id = next_hop_id;
    return PROTO_OK;
}

static bool packet_needs_forward(const struct mesh_relay *relay, const struct proto_packet *packet)
{
    return packet->dst_id != relay->local_id && packet->dst_id != MESH_BROADCAST_ID;
}

static bool packet_requires_channel9_payload_event(const struct proto_packet *packet)
{
    if (packet == NULL) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_GATEWAY_ACK:
    case MSG_COMMAND:
    case MSG_COMMAND_RESULT:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_PAIR_RESULT:
        return true;
    default:
        return false;
    }
}

static bool broadcast_time_sync_command(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    if (packet == NULL ||
        packet->msg_type != MSG_COMMAND ||
        payload == NULL ||
        tlv_find(payload, payload_len, TLV_COMMAND_ID, &value, &value_len) != PROTO_OK ||
        value_len != sizeof(uint16_t)) {
        return false;
    }

    return proto_get_u16_le(value) == CMD_SYNC_TIME;
}

static bool broadcast_packet_needs_forward(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len)
{
    return packet->dst_id == MESH_BROADCAST_ID &&
           packet->ttl > 0u &&
           (packet->msg_type == MSG_SURVEY_REACH_REQ ||
            broadcast_time_sync_command(packet, payload, payload_len));
}

static int build_broadcast_forward(const struct proto_packet *packet,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   struct mesh_outbound *out)
{
    if (!broadcast_packet_needs_forward(packet, payload, payload_len)) {
        return PROTO_ERR_STALE;
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    if (payload_len > 0u) {
        memcpy(out->payload, payload, payload_len);
    }
    out->payload_len = (uint8_t)payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    return PROTO_OK;
}

static bool local_delivery_needs_response(const struct mesh_relay *relay,
                                          const struct proto_packet *packet)
{
    if (packet->dst_id != relay->local_id) {
        return false;
    }
    if (relay->role == MESH_RELAY_ROLE_GATEWAY &&
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u) {
        return true;
    }
    return relay->role == MESH_RELAY_ROLE_ANCHOR && packet->msg_type == MSG_COMMAND;
}

static int add_gateway_ack_action(struct mesh_relay *relay,
                                  const struct proto_packet *packet,
                                  uint64_t previous_hop_id,
                                  struct mesh_relay_result *result)
{
    int ret;

    if (relay->role != MESH_RELAY_ROLE_GATEWAY ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return PROTO_OK;
    }

    ret = build_gateway_ack(relay, packet, previous_hop_id, &result->gateway_ack);
    if (ret == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_GATEWAY_ACK;
    } else if (result->status == PROTO_OK) {
        result->status = ret;
    }
    return ret;
}

static int upsert_reactive_route(struct mesh_relay *relay,
                                 uint64_t target_id,
                                 uint64_t next_hop_id,
                                 uint32_t route_epoch,
                                 uint8_t advertised_hop_count,
                                 uint8_t quality,
                                 uint32_t now_ms)
{
    if (!id_is_unicast(target_id) ||
        !id_is_unicast(next_hop_id) ||
        next_hop_id == relay->local_id ||
        advertised_hop_count == UINT8_MAX ||
        quality > 100u) {
        return PROTO_ERR_MALFORMED;
    }
    if (target_id == relay->local_id) {
        return PROTO_OK;
    }

    if (target_id == relay->gateway_id) {
        struct route_candidate candidate = {
            .next_hop_id = next_hop_id,
            .gateway_id = target_id,
            .route_epoch = route_epoch,
            .last_seen_ms = now_ms,
            .hop_count = advertised_hop_count,
            .link_quality = quality,
            .valid = true,
        };

        if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
            return PROTO_OK;
        }
        return route_upsert_candidate(&relay->upstream, &candidate);
    }

    if (advertised_hop_count + 1u == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    struct mesh_downlink_entry entry = {
        .target_id = target_id,
        .next_hop_id = next_hop_id,
        .gateway_id = relay->gateway_id,
        .route_epoch = route_epoch,
        .last_seen_ms = now_ms,
        .hop_count = advertised_hop_count + 1u,
        .quality = quality,
        .valid = true,
    };

    return upsert_downlink(relay, &entry);
}

static int parse_route_discovery_tlvs(const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t *origin_id,
                                      uint64_t *target_id,
                                      uint32_t *route_epoch,
                                      uint8_t *hop_count,
                                      uint8_t *quality)
{
    int ret;

    ret = find_u64_tlv(payload, payload_len, TLV_INITIATOR_ID, origin_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u64_tlv(payload, payload_len, TLV_RESPONDER_ID, target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len, TLV_ROUTE_EPOCH, route_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len, TLV_HOP_COUNT, hop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    return find_u8_tlv(payload, payload_len, TLV_QUALITY, quality);
}

static int build_route_request_forward(const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint8_t link_quality,
                                       struct mesh_outbound *out)
{
    uint64_t origin_id = 0u;
    uint64_t target_id = 0u;
    uint32_t route_epoch = 0u;
    uint8_t hop_count = 0u;
    uint8_t quality = 0u;
    size_t out_payload_len = 0u;
    int ret;

    if (packet->ttl == 0u) {
        return PROTO_ERR_STALE;
    }

    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     &origin_id,
                                     &target_id,
                                     &route_epoch,
                                     &hop_count,
                                     &quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (hop_count == UINT8_MAX) {
        return PROTO_ERR_MALFORMED;
    }

    quality = combined_quality(quality, link_quality);
    ret = append_route_discovery_tlvs(out->payload,
                                      sizeof(out->payload),
                                      &out_payload_len,
                                      origin_id,
                                      target_id,
                                      route_epoch,
                                      hop_count + 1u,
                                      quality);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    out->packet.payload_len = (uint8_t)out_payload_len;
    out->payload_len = (uint8_t)out_payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    return PROTO_OK;
}

static int build_route_reply(struct mesh_relay *relay,
                             uint64_t origin_id,
                             uint64_t target_id,
                             uint64_t next_hop_id,
                             uint32_t session_id,
                             uint32_t route_epoch,
                             struct mesh_outbound *out)
{
    size_t payload_len = 0u;
    int ret;

    if (!id_is_unicast(origin_id) ||
        !id_is_unicast(target_id) ||
        !id_is_unicast(next_hop_id) ||
        next_hop_id == relay->local_id ||
        session_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    ret = append_route_discovery_tlvs(out->payload,
                                      sizeof(out->payload),
                                      &payload_len,
                                      origin_id,
                                      target_id,
                                      route_epoch,
                                      0u,
                                      100u);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_ROUTE_REPLY;
    out->packet.flags = 0u;
    out->packet.src_id = target_id;
    out->packet.dst_id = origin_id;
    out->packet.session_id = session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = MESH_DEFAULT_TTL;
    out->packet.payload_len = (uint8_t)payload_len;
    out->payload_len = (uint8_t)payload_len;
    out->next_hop_id = next_hop_id;
    return PROTO_OK;
}

static int build_route_reply_forward(const struct mesh_relay *relay,
                                     const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint8_t link_quality,
                                     struct mesh_outbound *out)
{
    uint64_t origin_id = 0u;
    uint64_t target_id = 0u;
    uint64_t next_hop_id = 0u;
    uint32_t route_epoch = 0u;
    uint8_t hop_count = 0u;
    uint8_t quality = 0u;
    size_t out_payload_len = 0u;
    int ret;

    if (packet->ttl == 0u) {
        return PROTO_ERR_STALE;
    }

    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     &origin_id,
                                     &target_id,
                                     &route_epoch,
                                     &hop_count,
                                     &quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (hop_count == UINT8_MAX) {
        return PROTO_ERR_MALFORMED;
    }
    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    quality = combined_quality(quality, link_quality);
    ret = append_route_discovery_tlvs(out->payload,
                                      sizeof(out->payload),
                                      &out_payload_len,
                                      origin_id,
                                      target_id,
                                      route_epoch,
                                      hop_count + 1u,
                                      quality);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    out->packet.payload_len = (uint8_t)out_payload_len;
    out->payload_len = (uint8_t)out_payload_len;
    out->next_hop_id = next_hop_id;
    return PROTO_OK;
}

static int handle_route_request(struct mesh_relay *relay,
                                const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint64_t previous_hop_id,
                                uint8_t link_quality,
                                uint32_t now_ms,
                                struct mesh_relay_result *result)
{
    uint64_t origin_id = 0u;
    uint64_t target_id = 0u;
    uint32_t route_epoch = 0u;
    uint8_t hop_count = 0u;
    uint8_t quality = 0u;
    int ret;

    if (!id_is_unicast(previous_hop_id) || previous_hop_id == relay->local_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     &origin_id,
                                     &target_id,
                                     &route_epoch,
                                     &hop_count,
                                     &quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (origin_id != packet->src_id ||
        packet->dst_id != MESH_BROADCAST_ID ||
        !id_is_unicast(target_id) ||
        target_id == origin_id ||
        hop_count == UINT8_MAX) {
        return PROTO_ERR_MALFORMED;
    }

    quality = combined_quality(quality, link_quality);
    ret = upsert_reactive_route(relay,
                                origin_id,
                                previous_hop_id,
                                route_epoch,
                                hop_count,
                                quality,
                                now_ms);
    if (ret != PROTO_OK && ret != PROTO_ERR_NO_SPACE) {
        return ret;
    }

    if (target_id == relay->local_id) {
        ret = build_route_reply(relay,
                                origin_id,
                                target_id,
                                previous_hop_id,
                                packet->session_id,
                                relay->upstream.current_epoch > route_epoch ?
                                relay->upstream.current_epoch : route_epoch,
                                &result->route_reply);
        if (ret == PROTO_OK) {
            result->actions |= MESH_RELAY_ACTION_SEND_ROUTE_REPLY;
        }
        return ret;
    }

    if (mesh_relay_tx_active(relay)) {
        return PROTO_ERR_BUSY;
    }

    ret = build_route_request_forward(packet,
                                      payload,
                                      payload_len,
                                      link_quality,
                                      &result->route_request);
    if (ret == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_ROUTE_REQ;
    }
    return ret;
}

static int handle_route_reply(struct mesh_relay *relay,
                              const struct proto_packet *packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              uint64_t previous_hop_id,
                              uint8_t link_quality,
                              uint32_t now_ms,
                              struct mesh_relay_result *result)
{
    uint64_t origin_id = 0u;
    uint64_t target_id = 0u;
    uint32_t route_epoch = 0u;
    uint8_t hop_count = 0u;
    uint8_t quality = 0u;
    int ret;

    if (!id_is_unicast(previous_hop_id) || previous_hop_id == relay->local_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     &origin_id,
                                     &target_id,
                                     &route_epoch,
                                     &hop_count,
                                     &quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (packet->src_id != target_id ||
        packet->dst_id != origin_id ||
        !id_is_unicast(origin_id) ||
        !id_is_unicast(target_id) ||
        origin_id == target_id ||
        hop_count == UINT8_MAX) {
        return PROTO_ERR_MALFORMED;
    }

    quality = combined_quality(quality, link_quality);
    ret = upsert_reactive_route(relay,
                                target_id,
                                previous_hop_id,
                                route_epoch,
                                hop_count,
                                quality,
                                now_ms);
    if (ret != PROTO_OK && ret != PROTO_ERR_NO_SPACE) {
        return ret;
    }

    if (packet->dst_id == relay->local_id) {
        result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY;
        return PROTO_OK;
    }
    if (mesh_relay_tx_active(relay)) {
        return PROTO_ERR_BUSY;
    }

    ret = build_route_reply_forward(relay,
                                    packet,
                                    payload,
                                    payload_len,
                                    link_quality,
                                    &result->route_reply);
    if (ret == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_ROUTE_REPLY;
    }
    return ret;
}

void mesh_relay_init(struct mesh_relay *relay,
                     enum mesh_relay_role role,
                     uint64_t local_id,
                     uint64_t gateway_id,
                     uint32_t route_epoch)
{
    if (relay == NULL) {
        return;
    }

    memset(relay, 0, sizeof(*relay));
    relay->role = role;
    relay->local_id = local_id;
    relay->gateway_id = gateway_id;
    relay->next_seq = 1u;
    route_table_init(&relay->upstream, route_epoch);
}

const struct mesh_downlink_entry *mesh_relay_find_downlink(const struct mesh_relay *relay,
                                                           uint64_t target_id)
{
    int index;

    if (relay == NULL || !id_is_unicast(target_id)) {
        return NULL;
    }

    index = downlink_index(relay, target_id);
    return index >= 0 ? &relay->downlinks[index] : NULL;
}

uint8_t mesh_relay_expire_routes(struct mesh_relay *relay, uint32_t now_ms)
{
    if (relay == NULL) {
        return 0u;
    }

    (void)now_ms;
    return 0u;
}

int mesh_relay_select_next_hop(const struct mesh_relay *relay,
                               uint64_t dst_id,
                               uint64_t *next_hop_id)
{
    const struct route_candidate *upstream;
    const struct mesh_downlink_entry *downlink;

    if (relay == NULL || next_hop_id == NULL || !id_is_unicast(dst_id)) {
        return PROTO_ERR_ARG;
    }
    if (dst_id == relay->local_id) {
        return PROTO_ERR_MALFORMED;
    }

    if (dst_id == relay->gateway_id) {
        if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
            return PROTO_ERR_MALFORMED;
        }
        upstream = route_selected(&relay->upstream);
        if (upstream == NULL) {
            return PROTO_ERR_NOT_FOUND;
        }
        *next_hop_id = upstream->next_hop_id;
        return PROTO_OK;
    }

    downlink = mesh_relay_find_downlink(relay, dst_id);
    if (downlink == NULL) {
        return PROTO_ERR_NOT_FOUND;
    }
    *next_hop_id = downlink->next_hop_id;
    return PROTO_OK;
}

static int event_timing_index(const struct mesh_relay *relay, uint64_t next_hop_id)
{
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (relay->event_timings[i].valid &&
            relay->event_timings[i].next_hop_id == next_hop_id) {
            return (int)i;
        }
    }
    return -1;
}

static int free_event_timing_index(const struct mesh_relay *relay)
{
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (!relay->event_timings[i].valid) {
            return (int)i;
        }
    }
    return -1;
}

int mesh_relay_set_channel9_timing(struct mesh_relay *relay,
                                   uint64_t next_hop_id,
                                   const struct mesh_event_timing *timing)
{
    int index;

    if (relay == NULL || timing == NULL || !id_is_unicast(next_hop_id) ||
        next_hop_id == relay->local_id ||
        timing->mesh_channel != MESH_EVENT_CHANNEL) {
        return PROTO_ERR_ARG;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index < 0) {
        index = free_event_timing_index(relay);
    }
    if (index < 0) {
        return PROTO_ERR_NO_SPACE;
    }

    relay->event_timings[index].next_hop_id = next_hop_id;
    relay->event_timings[index].timing = *timing;
    relay->event_timings[index].valid = true;
    return PROTO_OK;
}

void mesh_relay_clear_channel9_timing(struct mesh_relay *relay,
                                      uint64_t next_hop_id)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index >= 0) {
        relay->event_timings[index].valid = false;
    }
}

int mesh_relay_require_channel9_event(const struct mesh_relay *relay,
                                      uint64_t next_hop_id,
                                      const struct mesh_channel5_requirements *requirements,
                                      uint32_t now_ms,
                                      struct mesh_event_plan *plan)
{
    int index;
    int ret;

    if (relay == NULL || requirements == NULL || plan == NULL ||
        !id_is_unicast(next_hop_id)) {
        return PROTO_ERR_ARG;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index < 0) {
        return PROTO_ERR_STALE;
    }

    ret = mesh_event_plan_channel9(&relay->event_timings[index].timing,
                                   requirements,
                                   now_ms,
                                   plan);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (plan->action == MESH_EVENT_PLAN_START ||
        plan->action == MESH_EVENT_PLAN_CLIP) {
        return PROTO_OK;
    }
    if (plan->action == MESH_EVENT_PLAN_REFRESH_CONTACT_CH5) {
        return PROTO_ERR_STALE;
    }
    return PROTO_ERR_BUSY;
}

uint8_t mesh_relay_expire_channel9_timings(struct mesh_relay *relay,
                                           uint32_t now_ms)
{
    uint8_t expired = 0u;

    if (relay == NULL) {
        return 0u;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];

        if (entry->valid && !mesh_event_timing_usable(&entry->timing, now_ms)) {
            entry->valid = false;
            expired++;
        }
    }

    return expired;
}

int mesh_relay_build_route_request(struct mesh_relay *relay,
                                   uint64_t target_id,
                                   struct mesh_outbound *out,
                                   uint32_t now_ms)
{
    uint32_t route_epoch;
    uint32_t session_id;
    size_t payload_len = 0u;
    int ret;

    if (relay == NULL || out == NULL ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(target_id) ||
        target_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }

    (void)mesh_relay_expire_routes(relay, now_ms);
    memset(out, 0, sizeof(*out));

    route_epoch = relay->upstream.current_epoch;
    session_id = now_ms != 0u ? now_ms : route_epoch;
    if (session_id == 0u) {
        session_id = 1u;
    }

    ret = append_route_discovery_tlvs(out->payload,
                                      sizeof(out->payload),
                                      &payload_len,
                                      relay->local_id,
                                      target_id,
                                      route_epoch,
                                      0u,
                                      100u);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_ROUTE_REQ;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = MESH_BROADCAST_ID;
    out->packet.session_id = session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = MESH_DEFAULT_TTL;
    out->packet.payload_len = (uint8_t)payload_len;
    out->payload_len = (uint8_t)payload_len;
    out->next_hop_id = MESH_BROADCAST_ID;
    return PROTO_OK;
}

int mesh_relay_append_status_tlvs(const struct mesh_relay *relay,
                                  uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset)
{
    const struct route_candidate *selected;
    int ret;

    if (relay == NULL || payload == NULL || offset == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_GATEWAY_ID, relay->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
        ret = tlv_append_u32(payload,
                             payload_cap,
                             offset,
                             TLV_ROUTE_EPOCH,
                             relay->upstream.current_epoch);
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = tlv_append_u8(payload, payload_cap, offset, TLV_HOP_COUNT, 0u);
        if (ret != PROTO_OK) {
            return ret;
        }
        return tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, 100u);
    }

    selected = route_selected(&relay->upstream);
    if (selected == NULL || selected->hop_count == UINT8_MAX) {
        return tlv_append_u8(payload,
                             payload_cap,
                             offset,
                             TLV_REASON,
                             (uint8_t)(-PROTO_ERR_NOT_FOUND));
    }

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_NEXT_HOP_ID, selected->next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_ROUTE_EPOCH, selected->route_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_HOP_COUNT, selected->hop_count + 1u);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, selected->link_quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload, payload_cap, offset, TLV_RETRY_COUNT, selected->failure_count);
}

bool mesh_relay_tx_active(const struct mesh_relay *relay)
{
    return relay != NULL && relay->pending.state != MESH_RELAY_TX_IDLE;
}

void mesh_relay_cancel_tx(struct mesh_relay *relay)
{
    if (relay != NULL) {
        memset(&relay->pending, 0, sizeof(relay->pending));
    }
}

int mesh_relay_start_tx(struct mesh_relay *relay,
                        const struct proto_packet *packet,
                        const uint8_t *payload,
                        size_t payload_len,
                        uint32_t now_ms,
                        struct mesh_outbound *out)
{
    uint64_t next_hop_id = 0u;
    bool wait_gateway_ack;
    int ret;

    if (relay == NULL || packet == NULL || out == NULL ||
        (payload_len > 0u && payload == NULL) ||
        payload_len > PACKET_MAX_PAYLOAD_LEN ||
        packet->payload_len != payload_len ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        packet->ttl == 0u ||
        !id_is_unicast(packet->src_id) ||
        !id_is_unicast(packet->dst_id) ||
        packet->dst_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }
    if (mesh_relay_tx_active(relay)) {
        return PROTO_ERR_MALFORMED;
    }

    (void)mesh_relay_expire_routes(relay, now_ms);
    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    wait_gateway_ack = packet->src_id == relay->local_id &&
                       (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u;

    memset(&relay->pending, 0, sizeof(relay->pending));
    relay->pending.state = wait_gateway_ack ? MESH_RELAY_TX_WAIT_GATEWAY_ACK :
                                              MESH_RELAY_TX_IDLE;
    relay->pending.packet = *packet;
    if (payload_len > 0u) {
        memcpy(relay->pending.payload, payload, payload_len);
    }
    relay->pending.payload_len = (uint8_t)payload_len;
    relay->pending.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    relay->pending.next_hop_id = next_hop_id;
    pending_set_deadlines(&relay->pending, now_ms);
    outbound_from_pending(&relay->pending, out);
    return PROTO_OK;
}

int mesh_relay_start_channel9_tx(struct mesh_relay *relay,
                                 const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 const struct mesh_channel5_requirements *requirements,
                                 uint32_t now_ms,
                                 struct mesh_event_plan *plan,
                                 struct mesh_outbound *out)
{
    uint64_t next_hop_id = 0u;
    int ret;

    if (relay == NULL || packet == NULL || plan == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!packet_requires_channel9_payload_event(packet)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = mesh_relay_select_next_hop(relay, packet->dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    (void)mesh_relay_expire_channel9_timings(relay, now_ms);
    ret = mesh_relay_require_channel9_event(relay,
                                            next_hop_id,
                                            requirements,
                                            now_ms,
                                            plan);
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = mesh_relay_start_tx(relay,
                              packet,
                              payload,
                              payload_len,
                              now_ms,
                              out);
    if (ret == PROTO_OK) {
        out->radio_channel = MESH_EVENT_CHANNEL;
        if (mesh_relay_tx_active(relay) &&
            relay->pending.packet.session_id == packet->session_id &&
            relay->pending.packet.seq == packet->seq) {
            relay->pending.radio_channel = MESH_EVENT_CHANNEL;
        }
    }
    return ret;
}

void mesh_relay_note_channel9_success(struct mesh_relay *relay,
                                      uint64_t next_hop_id,
                                      uint32_t event_start_ms)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index >= 0) {
        mesh_event_note_success(&relay->event_timings[index].timing, event_start_ms);
    }
}

void mesh_relay_note_channel9_rx(struct mesh_relay *relay,
                                 uint64_t next_hop_id,
                                 uint32_t planned_event_start_ms,
                                 uint32_t observed_packet_ms)
{
    int index;

    if (relay == NULL || !id_is_unicast(next_hop_id)) {
        return;
    }

    index = event_timing_index(relay, next_hop_id);
    if (index >= 0) {
        mesh_event_note_observed_packet(&relay->event_timings[index].timing,
                                        planned_event_start_ms,
                                        observed_packet_ms);
    }
}

void mesh_relay_note_tx_sent(struct mesh_relay *relay,
                             const struct mesh_outbound *out,
                             uint32_t now_ms)
{
    if (relay == NULL || out == NULL || !id_is_unicast(out->next_hop_id)) {
        return;
    }

    if (out->packet.dst_id == relay->gateway_id) {
        route_refresh_selected_at(&relay->upstream, now_ms);
    } else if (out->packet.dst_id != MESH_BROADCAST_ID) {
        refresh_downlink(relay, out->packet.dst_id, out->next_hop_id, now_ms);
    }
}

void mesh_relay_note_delivery_failure(struct mesh_relay *relay,
                                      uint64_t dst_id)
{
    int index;

    if (relay == NULL || !id_is_unicast(dst_id) || dst_id == relay->local_id) {
        return;
    }

    if (dst_id == relay->gateway_id) {
        if (relay->role != MESH_RELAY_ROLE_GATEWAY) {
            (void)route_record_failure(&relay->upstream, ROUTE_FAILURE_GATEWAY_ACK);
        }
        return;
    }

    index = downlink_index(relay, dst_id);
    if (index >= 0) {
        relay->downlinks[index].valid = false;
    }
}

int mesh_relay_tick(struct mesh_relay *relay,
                    uint32_t now_ms,
                    struct mesh_relay_result *result)
{
    enum route_delivery_action action;
    uint64_t next_hop_id = 0u;

    if (relay == NULL || result == NULL) {
        return PROTO_ERR_ARG;
    }

    result_reset(result);
    (void)mesh_relay_expire_routes(relay, now_ms);
    if (!mesh_relay_tx_active(relay)) {
        return PROTO_OK;
    }

    if (relay->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
        deadline_reached(now_ms, relay->pending.gateway_ack_deadline_ms)) {
        action = route_record_failure(&relay->upstream, ROUTE_FAILURE_GATEWAY_ACK);
        if (action == ROUTE_DELIVERY_DISCOVER ||
            mesh_relay_select_next_hop(relay, relay->pending.packet.dst_id, &next_hop_id) != PROTO_OK) {
            relay->pending.state = MESH_RELAY_TX_IDLE;
            result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED;
            result->status = PROTO_ERR_NOT_FOUND;
            return PROTO_OK;
        }
        relay->pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
        relay->pending.next_hop_id = next_hop_id;
        pending_set_deadlines(&relay->pending, now_ms);
        outbound_from_pending(&relay->pending, &result->retransmit);
        result->actions |= MESH_RELAY_ACTION_RETRANSMIT;
    }

    return PROTO_OK;
}

int mesh_relay_handle_rx(struct mesh_relay *relay,
                         const struct proto_packet *packet,
                         const uint8_t *payload,
                         size_t payload_len,
                         uint64_t previous_hop_id,
                         uint8_t link_quality,
                         uint32_t now_ms,
                         struct mesh_relay_result *result)
{
    bool duplicate;
    int ret;

    if (relay == NULL || packet == NULL || result == NULL ||
        (payload_len > 0u && payload == NULL) ||
        payload_len > PACKET_MAX_PAYLOAD_LEN ||
        packet->payload_len != payload_len ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(packet->src_id) ||
        packet->src_id == relay->local_id) {
        return PROTO_ERR_ARG;
    }

    result_reset(result);
    (void)mesh_relay_expire_routes(relay, now_ms);

    if (packet->msg_type == MSG_GATEWAY_ACK && packet->dst_id == relay->local_id) {
        (void)handle_local_ack(relay, packet, payload, payload_len, now_ms, result);
        return PROTO_OK;
    }

    duplicate = duplicate_seen(relay, packet, now_ms);
    if (duplicate) {
        if (packet->dst_id == relay->local_id) {
            (void)add_gateway_ack_action(relay, packet, previous_hop_id, result);
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }
        if (packet->dst_id == MESH_BROADCAST_ID) {
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }

        if (mesh_relay_tx_active(relay)) {
            result->actions |= MESH_RELAY_ACTION_DROP;
            result->status = PROTO_ERR_BUSY;
            return PROTO_OK;
        }

        ret = build_forward(relay, packet, payload, payload_len, &result->forward);
        if (ret == PROTO_OK) {
            result->actions |= MESH_RELAY_ACTION_FORWARD;
            result->status = PROTO_ERR_STALE;
            return PROTO_OK;
        }

        result->actions |= MESH_RELAY_ACTION_DROP;
        result->status = ret;
        return PROTO_OK;
    }

    if (packet->msg_type != MSG_ROUTE_REQ &&
        packet->msg_type != MSG_ROUTE_REPLY &&
        (packet_needs_forward(relay, packet) || local_delivery_needs_response(relay, packet)) &&
        mesh_relay_tx_active(relay)) {
        result->status = PROTO_ERR_BUSY;
        result->actions |= MESH_RELAY_ACTION_DROP;
        return PROTO_OK;
    }

    if (packet->msg_type == LEGACY_MSG_ROUTE_ADV ||
        packet->msg_type == LEGACY_MSG_ROUTE_STATUS) {
        result->status = PROTO_ERR_MALFORMED;
        result->actions |= MESH_RELAY_ACTION_DROP;
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_ROUTE_REQ && packet->dst_id == MESH_BROADCAST_ID) {
        ret = handle_route_request(relay,
                                   packet,
                                   payload,
                                   payload_len,
                                   previous_hop_id,
                                   link_quality,
                                   now_ms,
                                   result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        } else {
            duplicate_store(relay, packet, now_ms);
        }
        return PROTO_OK;
    }

    if (packet->msg_type == MSG_ROUTE_REPLY && packet->dst_id != MESH_BROADCAST_ID) {
        ret = handle_route_reply(relay,
                                 packet,
                                 payload,
                                 payload_len,
                                 previous_hop_id,
                                 link_quality,
                                 now_ms,
                                 result);
        if (ret != PROTO_OK) {
            result->status = ret;
            result->actions |= MESH_RELAY_ACTION_DROP;
        } else {
            duplicate_store(relay, packet, now_ms);
        }
        return PROTO_OK;
    }

    if (packet->dst_id == relay->local_id) {
        duplicate_store(relay, packet, now_ms);
        result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
        (void)add_gateway_ack_action(relay, packet, previous_hop_id, result);
        return PROTO_OK;
    }

    if (packet->dst_id == MESH_BROADCAST_ID) {
        duplicate_store(relay, packet, now_ms);
        result->actions |= MESH_RELAY_ACTION_DELIVER_LOCAL;
        if (!mesh_relay_tx_active(relay)) {
            ret = build_broadcast_forward(packet, payload, payload_len, &result->forward);
            if (ret == PROTO_OK) {
                result->actions |= MESH_RELAY_ACTION_FORWARD;
            }
        } else if (broadcast_packet_needs_forward(packet, payload, payload_len)) {
            result->status = PROTO_ERR_BUSY;
        }
        return PROTO_OK;
    }

    ret = build_forward(relay, packet, payload, payload_len, &result->forward);
    if (ret == PROTO_OK) {
        duplicate_store(relay, packet, now_ms);
        result->actions |= MESH_RELAY_ACTION_FORWARD;
        return PROTO_OK;
    }

    result->status = ret;
    result->actions |= MESH_RELAY_ACTION_DROP;
    return PROTO_OK;
}

#line 3932 __BASE_FILE__
static int parse_route_discovery_tlvs(const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t request_id,
                                      uint32_t timing_reference_ms,
                                      struct route_discovery_fields *fields)
{
    int ret;

    ret = find_u64_tlv(payload, payload_len, TLV_INITIATOR_ID, &fields->origin_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u64_tlv(payload, payload_len, TLV_RESPONDER_ID, &fields->target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len, TLV_ROUTE_EPOCH, &fields->route_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len, TLV_HOP_COUNT, &fields->hop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len, TLV_QUALITY, &fields->quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len,
                      TLV_RELAY_CAPACITY_STATE, &fields->relay_capacity_state);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->relay_capacity_state = RELAY_CAP_UNKNOWN;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    if (fields->relay_capacity_state > RELAY_CAP_BLACK) {
        return PROTO_ERR_MALFORMED;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_QUEUE_FREE_HINT, &fields->queue_free_hint);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->queue_free_hint = UINT16_MAX;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len,
                      TLV_CHANNEL9_BUSY_HINT, &fields->channel9_busy_hint);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->channel9_busy_hint = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                       &fields->capacity_validity_interval_ms);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->capacity_validity_interval_ms = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    if (fields->relay_capacity_state == RELAY_CAP_UNKNOWN) {
        fields->queue_free_hint = 0u;
        fields->channel9_busy_hint = 0u;
        fields->capacity_validity_interval_ms = 0u;
    }

    ret = find_u32_tlv(payload, payload_len, TLV_FLOOD_EPOCH_ID,
                       &fields->flood_epoch_id);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->flood_epoch_id = request_id;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_FLOOD_PROFILE_VERSION,
                       &fields->flood_profile_version);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->flood_profile_version = MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len, TLV_SLOT_SEED, &fields->slot_seed);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->slot_seed = route_discovery_slot_seed(fields->origin_id,
                                                      fields->target_id,
                                                      request_id,
                                                      fields->route_epoch);
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len, TLV_ROUTE_REQUEST_FLAGS, &fields->request_flags);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->request_flags = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    if ((fields->request_flags & ~MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED) != 0u) {
        return PROTO_ERR_MALFORMED;
    }
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_ROUTE_REPLY_RX_DELAY_MS,
                       &fields->route_reply_rx_delay_ms);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->route_reply_rx_delay_ms = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_REPLY_NONCE, &fields->reply_nonce);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->reply_nonce = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_METRIC_CRC, &fields->metric_crc);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->metric_crc = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_event_timing_from_tlvs_at(&fields->proposed_channel9_timing,
                                         payload,
                                         payload_len,
                                         timing_reference_ms,
                                         true);
    if (ret == PROTO_OK) {
        fields->proposed_channel9_timing_valid = true;
        fields->timing_reference_ms = timing_reference_ms;
    } else if (ret == PROTO_ERR_NOT_FOUND) {
        memset(&fields->proposed_channel9_timing,
               0,
               sizeof(fields->proposed_channel9_timing));
        fields->proposed_channel9_timing_valid = false;
        fields->timing_reference_ms = 0u;
    } else {
        return ret;
    }
    return PROTO_OK;
}

static int parse_route_reply_ack_tlvs(const uint8_t *payload,
                                      size_t payload_len,
                                      struct route_reply_ack_fields *fields)
{
    int ret;

    ret = find_u64_tlv(payload, payload_len, TLV_INITIATOR_ID, &fields->origin_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u64_tlv(payload, payload_len, TLV_RESPONDER_ID, &fields->target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len, TLV_FLOOD_EPOCH_ID,
                       &fields->flood_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_REPLY_NONCE, &fields->reply_nonce);
    if (ret != PROTO_OK) {
        return ret;
    }
    return find_u16_tlv(payload, payload_len, TLV_METRIC_CRC, &fields->metric_crc);
}

static int append_gateway_route_adv_tlvs(uint8_t *payload,
                                         size_t payload_cap,
                                         size_t *offset,
                                         const struct gateway_route_adv_fields *fields)
{
    const struct flood_control_fields flood_control = {
        .random_backoff_max_ms = fields->random_backoff_max_ms,
        .random_backoff_slot_ms = fields->random_backoff_slot_ms,
        .retry_count = fields->flood_retry_count,
    };
    int ret;

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_GATEWAY_ID, fields->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_GATEWAY_EPOCH, fields->gateway_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_GATEWAY_ROUTE_SEQ, fields->gateway_route_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_HOP_COUNT, fields->hop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset,
                        TLV_PATH_QUALITY_MIN, fields->path_quality_min);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_ACCUMULATED_COST, fields->route_cost);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset,
                        TLV_RELAY_CAPACITY_STATE, fields->gateway_capacity_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                         fields->capacity_validity_interval_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_FLOOD_PROFILE_VERSION, fields->flood_profile_version);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_FLOOD_EPOCH_ID, fields->flood_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_SLOT_SEED, fields->slot_seed);
    if (ret != PROTO_OK) {
        return ret;
    }
    return append_flood_control_tlvs(payload,
                                     payload_cap,
                                     offset,
                                     &flood_control,
                                     fields->flood_packet_age_ms);
}

static int parse_gateway_route_adv_tlvs(const uint8_t *payload,
                                        size_t payload_len,
                                        struct gateway_route_adv_fields *fields)
{
    int ret;

    ret = find_u64_tlv(payload, payload_len, TLV_GATEWAY_ID, &fields->gateway_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_GATEWAY_EPOCH, &fields->gateway_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len,
                       TLV_GATEWAY_ROUTE_SEQ, &fields->gateway_route_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len, TLV_HOP_COUNT, &fields->hop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len,
                      TLV_PATH_QUALITY_MIN, &fields->path_quality_min);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload, payload_len, TLV_ACCUMULATED_COST, &fields->route_cost);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u8_tlv(payload, payload_len,
                      TLV_RELAY_CAPACITY_STATE, &fields->gateway_capacity_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u16_tlv(payload,
                       payload_len,
                       TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                       &fields->capacity_validity_interval_ms);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->capacity_validity_interval_ms = 0u;
    } else if (ret != PROTO_OK) {
        return ret;
    }
    if (fields->gateway_capacity_state == RELAY_CAP_UNKNOWN) {
        fields->capacity_validity_interval_ms = 0u;
    }
    ret = find_u16_tlv(payload, payload_len,
                       TLV_FLOOD_PROFILE_VERSION, &fields->flood_profile_version);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len,
                       TLV_FLOOD_EPOCH_ID, &fields->flood_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = find_u32_tlv(payload, payload_len, TLV_SLOT_SEED, &fields->slot_seed);
    if (ret != PROTO_OK) {
        return ret;
    }

    {
        struct flood_control_fields flood_control;

        ret = parse_flood_control_tlvs(payload, payload_len, &flood_control);
        if (ret != PROTO_OK) {
            return ret;
        }
        fields->random_backoff_max_ms = flood_control.random_backoff_max_ms;
        fields->random_backoff_slot_ms = flood_control.random_backoff_slot_ms;
        fields->flood_retry_count = flood_control.retry_count;
    }

    ret = find_u32_tlv(payload,
                       payload_len,
                       TLV_FLOOD_PACKET_AGE_MS,
                       &fields->flood_packet_age_ms);
    if (ret == PROTO_ERR_NOT_FOUND) {
        fields->flood_packet_age_ms = 0u;
        return PROTO_OK;
    }
    return ret;
}

static int build_route_reply_ack(struct mesh_relay *relay,
                                 const struct proto_packet *packet,
                                 const struct route_discovery_fields *fields,
                                 uint64_t previous_hop_id,
                                 struct mesh_outbound *out)
{
    size_t payload_len = 0u;
    int ret;

    if (fields == NULL ||
        packet->msg_type != MSG_ROUTE_REPLY ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        fields->reply_nonce == 0u ||
        fields->metric_crc == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    ret = tlv_append_u64(out->payload, sizeof(out->payload), &payload_len,
                         TLV_INITIATOR_ID, fields->origin_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(out->payload, sizeof(out->payload), &payload_len,
                         TLV_RESPONDER_ID, fields->target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(out->payload, sizeof(out->payload), &payload_len,
                         TLV_FLOOD_EPOCH_ID, fields->flood_epoch_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(out->payload, sizeof(out->payload), &payload_len,
                         TLV_REPLY_NONCE, fields->reply_nonce);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(out->payload, sizeof(out->payload), &payload_len,
                         TLV_METRIC_CRC, fields->metric_crc);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_ROUTE_REPLY_ACK;
    out->packet.flags = 0u;
    out->packet.src_id = relay->local_id;
    out->packet.dst_id = previous_hop_id;
    out->packet.session_id = packet->session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = 1u;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = previous_hop_id;
    return PROTO_OK;
}

static int complete_route_reply_identity(const struct proto_packet *packet,
                                         struct route_discovery_fields *fields)
{
    uint16_t expected_nonce;

    expected_nonce = mesh_route_reply_nonce(fields->origin_id,
                                            fields->target_id,
                                            packet->session_id,
                                            fields->flood_epoch_id);
    if (fields->reply_nonce != 0u && fields->reply_nonce != expected_nonce) {
        return PROTO_ERR_MALFORMED;
    }
    if (fields->reply_nonce == 0u) {
        fields->reply_nonce = expected_nonce;
    }
    if (fields->metric_crc == 0u) {
        fields->metric_crc = route_reply_metric_crc(fields);
    }
    return PROTO_OK;
}

static void add_route_reply_ack_action(struct mesh_relay *relay,
                                       const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint64_t previous_hop_id,
                                       struct mesh_relay_result *result)
{
    struct route_discovery_fields fields = {0};

    if (packet->msg_type != MSG_ROUTE_REPLY ||
        parse_route_discovery_tlvs(payload,
                                   payload_len,
                                   packet->session_id,
                                   0u,
                                   &fields) != PROTO_OK) {
        return;
    }
    if (packet->src_id != fields.target_id ||
        packet->dst_id != fields.origin_id ||
        !id_is_unicast(fields.origin_id) ||
        !id_is_unicast(fields.target_id) ||
        fields.origin_id == fields.target_id ||
        fields.flood_epoch_id == 0u) {
        return;
    }

    if (complete_route_reply_identity(packet, &fields) != PROTO_OK) {
        return;
    }
    if (build_route_reply_ack(relay,
                              packet,
                              &fields,
                              previous_hop_id,
                              &result->route_reply_ack) == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK;
    }
}

static int handle_route_reply_ack(struct mesh_relay *relay,
                                  const struct proto_packet *packet,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  uint64_t previous_hop_id,
                                  struct mesh_relay_result *result)
{
    struct route_reply_ack_fields fields = {0};
    int ret;

    if (!id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        packet->dst_id != relay->local_id ||
        packet->src_id != previous_hop_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = parse_route_reply_ack_tlvs(payload, payload_len, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!id_is_unicast(fields.origin_id) ||
        !id_is_unicast(fields.target_id) ||
        fields.origin_id == fields.target_id ||
        fields.flood_epoch_id == 0u ||
        fields.reply_nonce == 0u ||
        fields.metric_crc == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    result->actions |= MESH_RELAY_ACTION_ROUTE_REPLY_ACKED;
    return PROTO_OK;
}

static bool gateway_route_adv_fields_valid(const struct gateway_route_adv_fields *fields)
{
    if (fields == NULL ||
        !id_is_unicast(fields->gateway_id) ||
        fields->flood_epoch_id == 0u ||
        fields->gateway_route_seq == 0u ||
        fields->flood_profile_version == 0u ||
        fields->path_quality_min > 100u ||
        fields->gateway_capacity_state > RELAY_CAP_BLACK ||
        (fields->gateway_capacity_state != RELAY_CAP_UNKNOWN &&
         fields->capacity_validity_interval_ms == 0u) ||
        fields->route_cost != gateway_route_cost(fields->hop_count, fields->path_quality_min)) {
        return false;
    }
    return true;
}

static int build_gateway_route_adv_forward(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint8_t link_quality,
                                           uint64_t local_id,
                                           uint32_t now_ms,
                                           uint32_t random_value,
                                           struct mesh_outbound *out)
{
    struct gateway_route_adv_fields fields = {0};
    struct flood_control_fields flood_control;
    size_t out_payload_len = 0u;
    int ret;

    if (packet->ttl <= 1u) {
        return PROTO_ERR_STALE;
    }

    ret = parse_gateway_route_adv_tlvs(payload, payload_len, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!gateway_route_adv_fields_valid(&fields) ||
        fields.hop_count == UINT8_MAX) {
        return PROTO_ERR_MALFORMED;
    }

    fields.path_quality_min = combined_quality(fields.path_quality_min, link_quality);
    fields.hop_count++;
    fields.route_cost = gateway_route_cost(fields.hop_count, fields.path_quality_min);
    fields.flood_packet_age_ms = packet->message_age_ms;
    ret = append_gateway_route_adv_tlvs(out->payload,
                                        sizeof(out->payload),
                                        &out_payload_len,
                                        &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    out->packet.payload_len = (uint16_t)out_payload_len;
    out->payload_len = (uint16_t)out_payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->queued_at_ms = now_ms;
    out->flood_retry_count = fields.flood_retry_count;
    flood_control.random_backoff_max_ms = fields.random_backoff_max_ms;
    flood_control.random_backoff_slot_ms = fields.random_backoff_slot_ms;
    flood_control.retry_count = fields.flood_retry_count;
    out->earliest_tx_ms = now_ms + flood_forward_total_delay_ms(local_id,
                                                                fields.slot_seed,
                                                                fields.hop_count,
                                                                &flood_control,
                                                                random_value);
    return PROTO_OK;
}

static int handle_gateway_route_adv(struct mesh_relay *relay,
                                    const struct proto_packet *packet,
                                    const uint8_t *payload,
                                    size_t payload_len,
                                    uint64_t previous_hop_id,
                                    uint8_t link_quality,
                                    uint32_t now_ms,
                                    uint32_t random_value,
                                    struct mesh_relay_result *result)
{
    struct gateway_route_adv_fields fields = {0};
    uint8_t path_quality;
    int ret;
    int route_ret;

    if (!id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        packet->dst_id != MESH_BROADCAST_ID) {
        return PROTO_ERR_MALFORMED;
    }

    ret = parse_gateway_route_adv_tlvs(payload, payload_len, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!gateway_route_adv_fields_valid(&fields) ||
        packet->src_id != fields.gateway_id ||
        fields.gateway_id != relay->gateway_id) {
        return PROTO_ERR_MALFORMED;
    }

    path_quality = combined_quality(fields.path_quality_min, link_quality);
    route_ret = upsert_reactive_route(relay,
                                      fields.gateway_id,
                                      previous_hop_id,
                                      fields.gateway_epoch,
                                      fields.hop_count,
                                      path_quality,
                                      fields.gateway_capacity_state,
                                      UINT16_MAX,
                                      0u,
                                      fields.capacity_validity_interval_ms,
                                      now_ms,
                                      NULL,
                                      0u);
    if (route_ret != PROTO_OK &&
        route_ret != PROTO_ERR_NO_SPACE) {
        return route_ret;
    }
    if (route_ret == PROTO_OK && relay->route_discovery.active &&
        relay->route_discovery.target_id == fields.gateway_id) {
        mesh_relay_note_route_discovery_ready(relay, fields.gateway_id);
        result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY;
    }

    ret = build_gateway_route_adv_forward(packet,
                                          payload,
                                          payload_len,
                                          link_quality,
                                          relay->local_id,
                                          now_ms,
                                          random_value,
                                          &result->gateway_route_adv);
    if (ret == PROTO_OK) {
        result->actions |= MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV;
    } else if (ret != PROTO_ERR_STALE) {
        return ret;
    }

    return PROTO_OK;
}

static int build_route_reply(struct mesh_relay *relay,
                             const struct route_discovery_fields *request,
                             uint64_t next_hop_id,
                             uint32_t session_id,
                             uint32_t route_epoch,
                             struct mesh_outbound *out)
{
    struct route_discovery_fields fields;
    const struct route_candidate *selected = NULL;
    size_t payload_len = 0u;
    int ret;

    if (request == NULL ||
        !id_is_unicast(request->origin_id) ||
        !id_is_unicast(request->target_id) ||
        !id_is_unicast(next_hop_id) ||
        next_hop_id == relay->local_id ||
        session_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    fields = *request;
    fields.route_epoch = route_epoch;
    if (request->target_id == relay->gateway_id &&
        request->target_id != relay->local_id) {
        selected = route_selected(&relay->upstream);
    }
    if (selected != NULL && selected->hop_count < UINT8_MAX) {
        fields.hop_count = selected->hop_count + 1u;
        fields.quality = selected->link_quality;
    } else {
        fields.hop_count = 0u;
        fields.quality = 100u;
    }
    fields.relay_capacity_state = relay_current_capacity_state(relay);
    fields.queue_free_hint = relay_current_queue_free_hint(relay);
    fields.channel9_busy_hint = relay_active_channel9_timing_count(relay);
    fields.capacity_validity_interval_ms =
        relay_current_capacity_validity_interval_ms(relay);
    fields.request_flags = 0u;
    fields.route_reply_rx_delay_ms = 0u;
    fields.reply_nonce = mesh_route_reply_nonce(fields.origin_id,
                                                fields.target_id,
                                                session_id,
                                                fields.flood_epoch_id);
    fields.metric_crc = route_reply_metric_crc(&fields);

    memset(out, 0, sizeof(*out));
    ret = append_route_discovery_tlvs(out->payload,
                                      sizeof(out->payload),
                                      &payload_len,
                                      &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet.msg_type = MSG_ROUTE_REPLY;
    out->packet.flags = 0u;
    out->packet.src_id = request->target_id;
    out->packet.dst_id = request->origin_id;
    out->packet.session_id = session_id;
    out->packet.seq = relay_next_seq(relay);
    out->packet.ttl = MESH_DEFAULT_TTL;
    out->packet.payload_len = (uint16_t)payload_len;
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = next_hop_id;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return PROTO_OK;
}

static int build_route_reply_forward(const struct mesh_relay *relay,
                                     const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint8_t link_quality,
                                     uint32_t now_ms,
                                     struct mesh_outbound *out)
{
    struct route_discovery_fields fields = {0};
    const struct mesh_downlink_entry *reverse_route;
    uint64_t next_hop_id = 0u;
    size_t out_payload_len = 0u;
    int ret;

    if (packet->ttl == 0u) {
        return PROTO_ERR_STALE;
    }

    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     packet->session_id,
                                     0u,
                                     &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (fields.hop_count == UINT8_MAX) {
        return PROTO_ERR_MALFORMED;
    }
    ret = complete_route_reply_identity(packet, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    reverse_route = downlink_for_discovery(relay,
                                           fields.origin_id,
                                           fields.target_id,
                                           packet->session_id,
                                           fields.flood_epoch_id,
                                           fields.reply_nonce);
    if (reverse_route == NULL) {
        return PROTO_ERR_NOT_FOUND;
    }
    next_hop_id = reverse_route->next_hop_id;

    fields.quality = combined_quality(fields.quality, link_quality);
    fields.hop_count++;
    fields.proposed_channel9_timing_valid = false;
    fields.timing_reference_ms = 0u;
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];

        if (entry->valid && entry->next_hop_id == next_hop_id) {
            fields.proposed_channel9_timing = entry->timing;
            fields.proposed_channel9_timing_valid = true;
            fields.timing_reference_ms = now_ms;
            break;
        }
    }
    fields.metric_crc = route_reply_metric_crc(&fields);
    ret = append_route_discovery_tlvs(out->payload,
                                      sizeof(out->payload),
                                      &out_payload_len,
                                      &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet = *packet;
    out->packet.ttl = packet->ttl - 1u;
    out->packet.payload_len = (uint16_t)out_payload_len;
    out->payload_len = (uint16_t)out_payload_len;
    out->next_hop_id = next_hop_id;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return PROTO_OK;
}

static int handle_route_request(struct mesh_relay *relay,
                                const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint64_t previous_hop_id,
                                uint8_t link_quality,
                                uint32_t now_ms,
                                uint32_t random_value,
                                struct mesh_relay_result *result,
                                bool duplicate_packet)
{
    struct route_discovery_fields fields = {0};
    struct flood_seen_entry *flood_entry = NULL;
    const struct route_candidate *selected = NULL;
    bool first_seen = false;
    bool best_updated = false;
    bool install_reply_timing = false;
    int ret;

    if (!id_is_unicast(previous_hop_id) || previous_hop_id == relay->local_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     packet->session_id,
                                     now_ms,
                                     &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (fields.origin_id != packet->src_id ||
        packet->dst_id != MESH_BROADCAST_ID ||
        !id_is_unicast(fields.target_id) ||
        fields.target_id == fields.origin_id ||
        fields.hop_count == UINT8_MAX ||
        fields.flood_epoch_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
        return PROTO_ERR_STALE;
    }
    if (relay->role == MESH_RELAY_ROLE_ANCHOR &&
        relay_active_channel9_timing_count(relay) > 0u) {
        return PROTO_ERR_BUSY;
    }

    fields.quality = combined_quality(fields.quality, link_quality);
    flood_entry = route_solicit_flood_note(relay,
                                           &fields,
                                           packet->session_id,
                                           previous_hop_id,
                                           now_ms,
                                           &first_seen,
                                           &best_updated);
    ret = upsert_reactive_route(relay,
                                fields.origin_id,
                                previous_hop_id,
                                fields.route_epoch,
                                fields.hop_count,
                                fields.quality,
                                fields.relay_capacity_state,
                                fields.queue_free_hint,
                                fields.channel9_busy_hint,
                                fields.capacity_validity_interval_ms,
                                now_ms,
                                &fields,
                                packet->session_id);
    if (ret != PROTO_OK && ret != PROTO_ERR_NO_SPACE) {
        return ret;
    }
    install_reply_timing = (ret == PROTO_OK &&
                            fields.proposed_channel9_timing_valid);

    selected = route_selected(&relay->upstream);

    if (fields.target_id == relay->local_id ||
        (fields.target_id == relay->gateway_id && selected != NULL)) {
        uint32_t reply_epoch = fields.route_epoch;

        if (selected != NULL && selected->route_epoch > reply_epoch) {
            reply_epoch = selected->route_epoch;
        } else if (relay->upstream.current_epoch > reply_epoch) {
            reply_epoch = relay->upstream.current_epoch;
        }
        ret = build_route_reply(relay,
                                &fields,
                                previous_hop_id,
                                packet->session_id,
                                reply_epoch,
                                &result->route_reply);
        if (ret == PROTO_OK) {
            result->route_reply.earliest_tx_ms = route_reply_earliest_tx_ms(
                now_ms,
                fields.route_reply_rx_delay_ms,
                random_value);
            add_route_reply_backup(relay,
                                   &fields,
                                   packet->session_id,
                                   previous_hop_id,
                                   result);
            result->actions |= MESH_RELAY_ACTION_SEND_ROUTE_REPLY;
            if (install_reply_timing) {
                struct mesh_event_timing downstream_timing = fields.proposed_channel9_timing;

                mesh_event_timing_set_local_first_slot_tx(&downstream_timing, false);
                (void)mesh_relay_set_channel9_timing_guarded_direction(
                    relay,
                    previous_hop_id,
                    &downstream_timing,
                    MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
                    MESH_RELAY_EVENT_TIMINGS,
                    NULL);
            }
        }
        if (fields.target_id == relay->local_id) {
            return ret;
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) != 0u) {
        return PROTO_OK;
    }
    if (duplicate_packet || !first_seen) {
        relay_diag_inc_u8(&relay->diagnostics.flood_suppression_count);
        if (best_updated && flood_entry != NULL &&
            flood_entry->forward_count > 0u &&
            flood_entry->forward_due_ms != 0u &&
            !deadline_reached(now_ms, flood_entry->forward_due_ms)) {
            ret = build_route_request_forward(relay->local_id,
                                              packet,
                                              &fields,
                                              now_ms,
                                              random_value,
                                              &result->route_request);
            if (ret == PROTO_OK) {
                result->route_request.earliest_tx_ms =
                    flood_entry->forward_due_ms;
                result->actions |= MESH_RELAY_ACTION_UPDATE_ROUTE_REQ;
                return PROTO_OK;
            }
            return ret;
        }
        return PROTO_ERR_STALE;
    }
    if (mesh_relay_tx_active(relay)) {
        add_busy_action(relay, packet, payload, payload_len, previous_hop_id, result);
        return PROTO_ERR_BUSY;
    }
    if (first_seen &&
        route_solicit_forward_allowed(flood_entry)) {
        ret = build_route_request_forward(relay->local_id,
                                          packet,
                                          &fields,
                                          now_ms,
                                          random_value,
                                          &result->route_request);
        if (ret == PROTO_OK) {
            flood_entry->forward_count++;
            flood_entry->forward_due_ms = result->route_request.earliest_tx_ms;
            result->actions |= MESH_RELAY_ACTION_SEND_ROUTE_REQ;
            return PROTO_OK;
        }
        if (ret != PROTO_ERR_STALE) {
            return ret;
        }
    }

    return PROTO_ERR_NOT_FOUND;
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
    struct route_discovery_fields fields = {0};
    int ret;

    if (!id_is_unicast(previous_hop_id) || previous_hop_id == relay->local_id) {
        return PROTO_ERR_MALFORMED;
    }

    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     packet->session_id,
                                     now_ms,
                                     &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (packet->src_id != fields.target_id ||
        packet->dst_id != fields.origin_id ||
        !id_is_unicast(fields.origin_id) ||
        !id_is_unicast(fields.target_id) ||
        fields.origin_id == fields.target_id ||
        fields.hop_count == UINT8_MAX ||
        fields.flood_epoch_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    ret = complete_route_reply_identity(packet, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    fields.quality = combined_quality(fields.quality, link_quality);
    ret = upsert_reactive_route(relay,
                                fields.target_id,
                                previous_hop_id,
                                fields.route_epoch,
                                fields.hop_count,
                                fields.quality,
                                fields.relay_capacity_state,
                                fields.queue_free_hint,
                                fields.channel9_busy_hint,
                                fields.capacity_validity_interval_ms,
                                now_ms,
                                NULL,
                                0u);
    if (ret != PROTO_OK && ret != PROTO_ERR_NO_SPACE) {
        return ret;
    }
    if (ret == PROTO_OK) {
        if (fields.proposed_channel9_timing_valid) {
            struct mesh_event_timing upstream_timing = fields.proposed_channel9_timing;

            mesh_event_timing_set_local_first_slot_tx(&upstream_timing, true);
            (void)mesh_relay_set_channel9_timing_guarded_direction(
                relay,
                previous_hop_id,
                &upstream_timing,
                MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
                MESH_RELAY_EVENT_TIMINGS,
                NULL);
        }
        mesh_relay_note_route_discovery_ready(relay, fields.target_id);
        add_route_reply_ack_action(relay, packet, payload, payload_len, previous_hop_id, result);
    }

    if (packet->dst_id == relay->local_id) {
        result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY;
        return PROTO_OK;
    }
    if (mesh_relay_tx_active(relay)) {
        add_busy_action(relay, packet, payload, payload_len, previous_hop_id, result);
        return PROTO_ERR_BUSY;
    }

    ret = build_route_reply_forward(relay,
                                    packet,
                                    payload,
                                    payload_len,
                                    link_quality,
                                    now_ms,
                                    &result->route_reply);
    if (ret == PROTO_OK) {
        add_route_reply_backup(relay,
                               &fields,
                               packet->session_id,
                               result->route_reply.next_hop_id,
                               result);
        result->actions |= MESH_RELAY_ACTION_SEND_ROUTE_REPLY;
    }
    return ret;
}

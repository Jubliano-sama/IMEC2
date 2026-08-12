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
    ret = mesh_route_path_from_tlvs(payload, payload_len, &fields->path);
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
    {
        const uint8_t *reply_commitment = NULL;
        uint8_t reply_commitment_len = 0u;

        ret = tlv_find_unique(payload,
                              payload_len,
                              TLV_ROUTE_REPLY_SHA256_COMMITMENT,
                              &reply_commitment,
                              &reply_commitment_len);
        if (ret == PROTO_ERR_NOT_FOUND) {
            memset(fields->reply_commitment,
                   0,
                   sizeof(fields->reply_commitment));
            fields->reply_commitment_present = false;
        } else if (ret != PROTO_OK ||
                   reply_commitment_len !=
                       SEMANTIC_DIGEST_SHA256_LEN) {
            return PROTO_ERR_MALFORMED;
        } else {
            memcpy(fields->reply_commitment,
                   reply_commitment,
                   sizeof(fields->reply_commitment));
            fields->reply_commitment_present = true;
        }
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

static bool route_discovery_required_tlvs_present(const uint8_t *payload,
                                                  size_t payload_len)
{
    uint32_t value_u32;
    uint16_t value_u16;
    uint8_t value_u8;

    return find_u8_tlv(payload,
                       payload_len,
                       TLV_RELAY_CAPACITY_STATE,
                       &value_u8) == PROTO_OK &&
           find_u16_tlv(payload,
                        payload_len,
                        TLV_QUEUE_FREE_HINT,
                        &value_u16) == PROTO_OK &&
           find_u8_tlv(payload,
                       payload_len,
                       TLV_CHANNEL9_BUSY_HINT,
                       &value_u8) == PROTO_OK &&
           find_u16_tlv(payload,
                        payload_len,
                        TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                        &value_u16) == PROTO_OK &&
           find_u32_tlv(payload,
                        payload_len,
                        TLV_FLOOD_EPOCH_ID,
                        &value_u32) == PROTO_OK &&
           find_u16_tlv(payload,
                        payload_len,
                        TLV_FLOOD_PROFILE_VERSION,
                        &value_u16) == PROTO_OK &&
           find_u32_tlv(payload,
                        payload_len,
                        TLV_SLOT_SEED,
                        &value_u32) == PROTO_OK;
}

static int parse_route_reply_ack_tlvs(const uint8_t *payload,
                                      size_t payload_len,
                                      struct route_reply_ack_fields *fields)
{
    const uint8_t *reply_commitment = NULL;
    uint8_t reply_commitment_len = 0u;
    int ret;

    if (payload == NULL || fields == NULL ||
        payload_len != MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_ROUTE_REPLY_SHA256_COMMITMENT,
                          &reply_commitment,
                          &reply_commitment_len);
    if (ret != PROTO_OK ||
        reply_commitment_len != SEMANTIC_DIGEST_SHA256_LEN) {
        return PROTO_ERR_MALFORMED;
    }
    memcpy(fields->reply_commitment,
           reply_commitment,
           sizeof(fields->reply_commitment));
    return PROTO_OK;
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
    ret = mesh_route_path_append_tlv(payload,
                                     payload_cap,
                                     offset,
                                     &fields->path);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = append_flood_control_tlvs(payload,
                                    payload_cap,
                                    offset,
                                    &flood_control,
                                    fields->flood_packet_age_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_relay_operation_policy_append_exact(
        payload,
        payload_cap,
        offset,
        fields->operation_policy_tlvs,
        fields->operation_policy_tlvs_len,
        fields->operation_policy_present);
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
    ret = mesh_route_path_from_tlvs(payload, payload_len, &fields->path);
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
    } else if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_relay_operation_policy_parse_exact(
        payload,
        payload_len,
        &fields->operation_policy,
        fields->operation_policy_tlvs,
        &fields->operation_policy_tlvs_len,
        &fields->operation_policy_present);
}

static bool gateway_route_adv_required_tlvs_present(
    const uint8_t *payload,
    size_t payload_len)
{
    uint32_t value_u32;
    uint16_t value_u16;
    uint8_t value_u8;

    return find_u16_tlv(payload,
                        payload_len,
                        TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                        &value_u16) == PROTO_OK &&
           find_u32_tlv(payload,
                        payload_len,
                        TLV_FLOOD_RANDOM_BACKOFF_MAX_MS,
                        &value_u32) == PROTO_OK &&
           find_u16_tlv(payload,
                        payload_len,
                        TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS,
                        &value_u16) == PROTO_OK &&
           find_u8_tlv(payload,
                       payload_len,
                       TLV_FLOOD_RETRY_COUNT,
                       &value_u8) == PROTO_OK &&
           find_u32_tlv(payload,
                        payload_len,
                        TLV_FLOOD_PACKET_AGE_MS,
                        &value_u32) == PROTO_OK;
}

static int validate_route_reply_commitment(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct route_discovery_fields *fields)
{
    const size_t encoded_commitment_len =
        PROTO_TLV_HEADER_LEN + SEMANTIC_DIGEST_SHA256_LEN;
    uint8_t expected[SEMANTIC_DIGEST_SHA256_LEN];
    size_t semantic_payload_len;

    if (packet == NULL || payload == NULL || fields == NULL ||
        packet->msg_type != MSG_ROUTE_REPLY ||
        !fields->reply_commitment_present ||
        payload_len < encoded_commitment_len) {
        return PROTO_ERR_MALFORMED;
    }

    semantic_payload_len = payload_len - encoded_commitment_len;
    if (payload[semantic_payload_len] !=
            TLV_ROUTE_REPLY_SHA256_COMMITMENT ||
        payload[semantic_payload_len + 1u] !=
            SEMANTIC_DIGEST_SHA256_LEN ||
        !route_reply_semantic_commitment(packet,
                                         payload,
                                         semantic_payload_len,
                                         expected) ||
        !semantic_digest_equal(expected,
                               fields->reply_commitment,
                               sizeof(expected))) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static int append_route_reply_commitment(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len)
{
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN];
    int ret;

    if (packet == NULL || payload == NULL || payload_len == NULL ||
        *payload_len > UINT16_MAX ||
        !route_reply_semantic_commitment(packet,
                                         payload,
                                         *payload_len,
                                         commitment)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = tlv_append_bytes(payload,
                           payload_cap,
                           payload_len,
                           TLV_ROUTE_REPLY_SHA256_COMMITMENT,
                           commitment,
                           sizeof(commitment));
    if (ret == PROTO_OK) {
        packet->payload_len = (uint16_t)*payload_len;
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
        !fields->reply_commitment_present) {
        return PROTO_ERR_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    ret = tlv_append_bytes(out->payload,
                           sizeof(out->payload),
                           &payload_len,
                           TLV_ROUTE_REPLY_SHA256_COMMITMENT,
                           fields->reply_commitment,
                           sizeof(fields->reply_commitment));
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

static int validate_route_reply_identity(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct route_discovery_fields *fields)
{
    uint16_t expected_nonce;
    uint16_t expected_metric_crc;

    expected_nonce = mesh_route_reply_nonce(fields->origin_id,
                                            fields->target_id,
                                            packet->session_id,
                                            fields->flood_epoch_id);
    expected_metric_crc = route_reply_metric_crc(fields);
    if (fields->reply_nonce == 0u ||
        fields->reply_nonce != expected_nonce ||
        fields->metric_crc == 0u ||
        fields->metric_crc != expected_metric_crc ||
        fields->request_flags != 0u ||
        fields->route_reply_rx_delay_ms != 0u) {
        return PROTO_ERR_MALFORMED;
    }
    return validate_route_reply_commitment(packet,
                                           payload,
                                           payload_len,
                                           fields);
}

static int validate_route_reply_packet(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint32_t now_ms,
    struct route_discovery_fields *fields)
{
    uint32_t selected_epoch;
    int ret;

    if (relay == NULL || packet == NULL || payload == NULL || fields == NULL ||
        packet->msg_type != MSG_ROUTE_REPLY ||
        packet->payload_len != payload_len ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id) {
        return PROTO_ERR_MALFORMED;
    }
    if (!mesh_route_control_payload_len_valid(packet->msg_type, payload_len)) {
        return PROTO_ERR_MALFORMED;
    }

    memset(fields, 0, sizeof(*fields));
    if (!route_discovery_required_tlvs_present(payload, payload_len)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     packet->session_id,
                                     now_ms,
                                     fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_route_control_ttl_validate(packet->msg_type,
                                          packet->ttl,
                                          fields->hop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (packet->flags != 0u ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        packet->src_id != fields->target_id ||
        packet->dst_id != fields->origin_id ||
        !id_is_unicast(fields->origin_id) ||
        !id_is_unicast(fields->target_id) ||
        fields->origin_id == fields->target_id ||
        (uint16_t)fields->route_epoch == 0u ||
        fields->hop_count == UINT8_MAX ||
        fields->flood_epoch_id == 0u ||
        fields->flood_epoch_id != packet->session_id ||
        fields->flood_profile_version !=
            MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION ||
        fields->slot_seed !=
            route_discovery_slot_seed(fields->origin_id,
                                      fields->target_id,
                                      packet->session_id,
                                      fields->route_epoch) ||
        fields->quality > 100u ||
        (fields->relay_capacity_state == RELAY_CAP_UNKNOWN ?
             (fields->queue_free_hint != 0u ||
              fields->channel9_busy_hint != 0u ||
              fields->capacity_validity_interval_ms != 0u) :
             fields->capacity_validity_interval_ms == 0u) ||
        fields->path.count != (uint8_t)(fields->hop_count + 1u) ||
        mesh_route_path_validate(&fields->path,
                                 fields->target_id,
                                 previous_hop_id) != PROTO_OK ||
        mesh_route_path_contains(&fields->path, relay->local_id)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = route_epoch_select_newer(fields->route_epoch,
                                   relay->upstream.current_epoch,
                                   &selected_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    return validate_route_reply_identity(packet,
                                         payload,
                                         payload_len,
                                         fields);
}

static int validate_route_request_packet(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint32_t now_ms,
    struct route_discovery_fields *fields)
{
    uint32_t selected_epoch;
    int ret;

    if (relay == NULL || packet == NULL || payload == NULL || fields == NULL ||
        packet->msg_type != MSG_ROUTE_REQ ||
        packet->payload_len != payload_len ||
        packet->flags != 0u ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        !mesh_route_control_payload_len_valid(packet->msg_type, payload_len)) {
        return PROTO_ERR_MALFORMED;
    }

    memset(fields, 0, sizeof(*fields));
    if (!route_discovery_required_tlvs_present(payload, payload_len)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = parse_route_discovery_tlvs(payload,
                                     payload_len,
                                     packet->session_id,
                                     now_ms,
                                     fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_route_control_ttl_validate(packet->msg_type,
                                          packet->ttl,
                                          fields->hop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (packet->src_id != fields->origin_id ||
        packet->dst_id != MESH_BROADCAST_ID ||
        !id_is_unicast(fields->origin_id) ||
        !id_is_unicast(fields->target_id) ||
        fields->origin_id == fields->target_id ||
        (uint16_t)fields->route_epoch == 0u ||
        fields->hop_count == UINT8_MAX ||
        fields->quality > 100u ||
        fields->flood_epoch_id == 0u ||
        fields->flood_epoch_id != packet->session_id ||
        fields->flood_profile_version !=
            MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION ||
        (fields->relay_capacity_state == RELAY_CAP_UNKNOWN ?
             (fields->queue_free_hint != 0u ||
              fields->channel9_busy_hint != 0u ||
              fields->capacity_validity_interval_ms != 0u) :
             fields->capacity_validity_interval_ms == 0u) ||
        fields->slot_seed !=
            route_discovery_slot_seed(fields->origin_id,
                                      fields->target_id,
                                      packet->session_id,
                                      fields->route_epoch) ||
        fields->reply_nonce != 0u ||
        fields->metric_crc != 0u ||
        fields->reply_commitment_present ||
        fields->path.count != (uint8_t)(fields->hop_count + 1u) ||
        mesh_route_path_validate(&fields->path,
                                 fields->origin_id,
                                 previous_hop_id) != PROTO_OK ||
        mesh_route_path_contains(&fields->path, relay->local_id)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = route_epoch_select_newer(fields->route_epoch,
                                   relay->upstream.current_epoch,
                                   &selected_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (selected_epoch != fields->route_epoch) {
        /*
         * A reverse route learned from an older request would remain
         * selectable after this node had committed a newer gateway
         * generation.  The requester must first learn the current epoch;
         * never retain or rebroadcast mixed-epoch discovery state.
         */
        return PROTO_ERR_STALE;
    }
    return PROTO_OK;
}

int mesh_relay_validate_route_request(const struct mesh_relay *relay,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id,
                                      uint32_t now_ms)
{
    struct route_discovery_fields fields;

    return validate_route_request_packet(relay,
                                         packet,
                                         payload,
                                         payload_len,
                                         previous_hop_id,
                                         now_ms,
                                         &fields);
}

int mesh_relay_validate_route_reply(const struct mesh_relay *relay,
                                    const struct proto_packet *packet,
                                    const uint8_t *payload,
                                    size_t payload_len,
                                    uint64_t previous_hop_id,
                                    uint32_t now_ms)
{
    struct route_discovery_fields fields;

    return validate_route_reply_packet(relay,
                                       packet,
                                       payload,
                                       payload_len,
                                       previous_hop_id,
                                       now_ms,
                                       &fields);
}

static void add_route_reply_ack_action(struct mesh_relay *relay,
                                       const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint64_t previous_hop_id,
                                       struct mesh_relay_result *result)
{
    struct route_discovery_fields fields = {0};

    if (validate_route_reply_packet(relay,
                                    packet,
                                    payload,
                                    payload_len,
                                    previous_hop_id,
                                    0u,
                                    &fields) != PROTO_OK) {
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

static int route_reply_ack_expectation_from_outbound(
    const struct mesh_outbound *route_reply,
    uint64_t local_id,
    struct mesh_route_reply_ack_expectation *expectation)
{
    struct route_discovery_fields fields = {0};
    int ret;

    if (route_reply == NULL || expectation == NULL ||
        !id_is_unicast(local_id) ||
        route_reply->packet.msg_type != MSG_ROUTE_REPLY ||
        route_reply->packet.payload_len != route_reply->payload_len ||
        route_reply->packet.flags != 0u ||
        route_reply->packet.session_id == 0u ||
        route_reply->packet.seq == 0u ||
        !mesh_route_control_payload_len_valid(
            route_reply->packet.msg_type,
            route_reply->payload_len) ||
        !id_is_unicast(route_reply->next_hop_id) ||
        route_reply->next_hop_id == local_id) {
        return PROTO_ERR_ARG;
    }
    if (!route_discovery_required_tlvs_present(route_reply->payload,
                                               route_reply->payload_len)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = parse_route_discovery_tlvs(route_reply->payload,
                                     route_reply->payload_len,
                                     route_reply->packet.session_id,
                                     0u,
                                     &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (route_reply->packet.src_id != fields.target_id ||
        route_reply->packet.dst_id != fields.origin_id ||
        fields.flood_epoch_id == 0u ||
        fields.flood_epoch_id != route_reply->packet.session_id ||
        fields.flood_profile_version !=
            MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION ||
        fields.quality > 100u ||
        (fields.relay_capacity_state == RELAY_CAP_UNKNOWN ?
             (fields.queue_free_hint != 0u ||
              fields.channel9_busy_hint != 0u ||
              fields.capacity_validity_interval_ms != 0u) :
             fields.capacity_validity_interval_ms == 0u) ||
        fields.path.count == 0u ||
        fields.path.count != (uint8_t)(fields.hop_count + 1u) ||
        mesh_route_control_ttl_validate(route_reply->packet.msg_type,
                                        route_reply->packet.ttl,
                                        fields.hop_count) != PROTO_OK ||
        mesh_route_path_validate(&fields.path,
                                 fields.target_id,
                                 local_id) != PROTO_OK ||
        mesh_route_path_contains(&fields.path,
                                 route_reply->next_hop_id)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = validate_route_reply_identity(&route_reply->packet,
                                        route_reply->payload,
                                        route_reply->payload_len,
                                        &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(expectation, 0, sizeof(*expectation));
    expectation->peer_id = route_reply->next_hop_id;
    memcpy(expectation->reply_commitment,
           fields.reply_commitment,
           sizeof(expectation->reply_commitment));
    expectation->session_id = route_reply->packet.session_id;
    expectation->active = true;
    return PROTO_OK;
}

static void arm_route_reply_ack_expectation(
    struct mesh_relay *relay,
    const struct mesh_outbound *route_reply)
{
    struct mesh_route_reply_ack_expectation expectation;

    if (relay == NULL ||
        route_reply == NULL ||
        route_reply_ack_expectation_from_outbound(route_reply,
                                                  relay->local_id,
                                                  &expectation) !=
            PROTO_OK) {
        return;
    }
    relay->route_reply_ack_expectation = expectation;
}

void mesh_relay_abandon_route_reply_ack(
    struct mesh_relay *relay,
    const struct mesh_outbound *route_reply)
{
    struct mesh_route_reply_ack_expectation expectation;

    if (relay == NULL || route_reply == NULL ||
        !relay->route_reply_ack_expectation.active ||
        route_reply_ack_expectation_from_outbound(route_reply,
                                                  relay->local_id,
                                                  &expectation) !=
            PROTO_OK ||
        expectation.peer_id !=
            relay->route_reply_ack_expectation.peer_id ||
        expectation.session_id !=
            relay->route_reply_ack_expectation.session_id ||
        !semantic_digest_equal(
            expectation.reply_commitment,
            relay->route_reply_ack_expectation.reply_commitment,
            SEMANTIC_DIGEST_SHA256_LEN)) {
        return;
    }
    memset(&relay->route_reply_ack_expectation,
           0,
           sizeof(relay->route_reply_ack_expectation));
}

int mesh_relay_accept_route_reply_ack(struct mesh_relay *relay,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id)
{
    struct route_reply_ack_fields fields = {0};
    int ret;

    if (relay == NULL || packet == NULL || payload == NULL ||
        packet->msg_type != MSG_ROUTE_REPLY_ACK ||
        packet->payload_len != payload_len ||
        packet->flags != 0u ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        packet->dst_id != relay->local_id ||
        packet->src_id != previous_hop_id ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        payload_len != MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN) {
        return PROTO_ERR_MALFORMED;
    }
    ret = mesh_route_control_ttl_validate(packet->msg_type,
                                          packet->ttl,
                                          0u);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = parse_route_reply_ack_tlvs(payload, payload_len, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!relay->route_reply_ack_expectation.active ||
        relay->route_reply_ack_expectation.peer_id != previous_hop_id ||
        relay->route_reply_ack_expectation.session_id !=
            packet->session_id ||
        !semantic_digest_equal(
            relay->route_reply_ack_expectation.reply_commitment,
            fields.reply_commitment,
            SEMANTIC_DIGEST_SHA256_LEN)) {
        return PROTO_ERR_STALE;
    }

    memset(&relay->route_reply_ack_expectation,
           0,
           sizeof(relay->route_reply_ack_expectation));
    return PROTO_OK;
}

static int handle_route_reply_ack(struct mesh_relay *relay,
                                  const struct proto_packet *packet,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  uint64_t previous_hop_id,
                                  struct mesh_relay_result *result)
{
    int ret;

    ret = mesh_relay_accept_route_reply_ack(relay,
                                            packet,
                                            payload,
                                            payload_len,
                                            previous_hop_id);
    if (ret != PROTO_OK) {
        return ret;
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
        fields->flood_profile_version !=
            MESH_ROUTE_DISCOVERY_FLOOD_PROFILE_VERSION ||
        fields->random_backoff_max_ms !=
            FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS ||
        fields->random_backoff_slot_ms !=
            FLOOD_RANDOM_BACKOFF_DEFAULT_SLOT_MS ||
        fields->flood_retry_count != FLOOD_DEFAULT_RETRY_COUNT ||
        fields->path_quality_min > 100u ||
        fields->gateway_capacity_state > RELAY_CAP_BLACK ||
        fields->hop_count >= MESH_ROUTE_PATH_MAX_NODES ||
        fields->path.count == 0u ||
        fields->path.count != (uint8_t)(fields->hop_count + 1u) ||
        mesh_route_path_validate(&fields->path,
                                 fields->gateway_id,
                                 fields->path.node_ids[fields->path.count - 1u]) !=
            PROTO_OK ||
        (fields->gateway_capacity_state == RELAY_CAP_UNKNOWN ?
             fields->capacity_validity_interval_ms != 0u :
             fields->capacity_validity_interval_ms == 0u) ||
        fields->route_cost != gateway_route_cost(fields->hop_count, fields->path_quality_min) ||
        (fields->operation_policy_present &&
         (fields->operation_policy_tlvs_len !=
              OPERATION_POLICY_ALL_TLVS_LEN ||
          mesh_relay_operation_policy_set_validate_complete(
              &fields->operation_policy) != PROTO_OK)) ||
        (!fields->operation_policy_present &&
         fields->operation_policy_tlvs_len != 0u)) {
        return false;
    }
    return true;
}

static bool gateway_route_adv_sequence_strictly_newer(uint32_t candidate,
                                                      uint32_t current)
{
    uint32_t delta;

    if (candidate == 0u || current == 0u) {
        return false;
    }
    delta = candidate - current;
    return delta != 0u && delta < UINT32_C(0x80000000);
}

static int gateway_route_epoch_expand(uint16_t candidate,
                                      uint32_t current,
                                      uint32_t *expanded)
{
    uint16_t delta;

    if (candidate == 0u || expanded == NULL) {
        return PROTO_ERR_MALFORMED;
    }
    delta = (uint16_t)(candidate - (uint16_t)current);
    if (delta >= UINT16_C(0x8000)) {
        return PROTO_ERR_STALE;
    }
    *expanded = current + delta;
    return PROTO_OK;
}

static int validate_gateway_route_adv_packet(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    struct gateway_route_adv_fields *fields,
    uint32_t *route_epoch)
{
    int ret;

    if (relay == NULL || packet == NULL || payload == NULL || fields == NULL ||
        route_epoch == NULL ||
        relay->role != MESH_RELAY_ROLE_ANCHOR ||
        !id_is_unicast(relay->local_id) ||
        !id_is_unicast(relay->gateway_id) ||
        relay->local_id == relay->gateway_id ||
        packet->msg_type != MSG_GATEWAY_ROUTE_ADV ||
        packet->payload_len != payload_len ||
        packet->flags != 0u ||
        packet->session_id == 0u ||
        packet->seq == 0u ||
        packet->dst_id != MESH_BROADCAST_ID ||
        !id_is_unicast(previous_hop_id) ||
        previous_hop_id == relay->local_id ||
        !mesh_route_control_payload_len_valid(packet->msg_type, payload_len)) {
        return PROTO_ERR_MALFORMED;
    }

    memset(fields, 0, sizeof(*fields));
    if (!gateway_route_adv_required_tlvs_present(payload, payload_len)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = parse_gateway_route_adv_tlvs(payload, payload_len, fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_route_control_ttl_validate(packet->msg_type,
                                          packet->ttl,
                                          fields->hop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!gateway_route_adv_fields_valid(fields) ||
        fields->gateway_epoch == 0u ||
        packet->src_id != fields->gateway_id ||
        fields->gateway_id != relay->gateway_id ||
        packet->session_id != fields->gateway_route_seq ||
        fields->flood_epoch_id != fields->gateway_route_seq ||
        fields->slot_seed !=
            gateway_route_adv_slot_seed(fields->gateway_id,
                                        fields->gateway_route_seq,
                                        fields->gateway_epoch) ||
        fields->flood_packet_age_ms != packet->message_age_ms ||
        fields->path.node_ids[fields->path.count - 1u] != previous_hop_id ||
        mesh_route_path_contains(&fields->path, relay->local_id)) {
        return PROTO_ERR_MALFORMED;
    }
    ret = gateway_route_epoch_expand(fields->gateway_epoch,
                                     relay->upstream.current_epoch,
                                     route_epoch);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (*route_epoch == relay->upstream.current_epoch &&
        relay->gateway_route_adv_seq != 0u &&
        !gateway_route_adv_sequence_strictly_newer(
            fields->gateway_route_seq,
            relay->gateway_route_adv_seq)) {
        return PROTO_ERR_STALE;
    }
    return PROTO_OK;
}

int mesh_relay_validate_gateway_route_adv(
    const struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id)
{
#if IMEC_MESH_RELAY_GATEWAY_ONLY
    (void)relay;
    (void)packet;
    (void)payload;
    (void)payload_len;
    (void)previous_hop_id;
    return PROTO_ERR_MALFORMED;
#else
    struct gateway_route_adv_fields fields;
    uint32_t route_epoch;

    return validate_gateway_route_adv_packet(relay,
                                             packet,
                                             payload,
                                             payload_len,
                                             previous_hop_id,
                                             &fields,
                                             &route_epoch);
#endif
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
    ret = mesh_route_path_append(&fields.path, local_id);
    if (ret != PROTO_OK) {
        return ret;
    }
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
    out->packet.ttl = (uint8_t)(packet->ttl - 1u);
    out->packet.payload_len = (uint16_t)out_payload_len;
    out->payload_len = (uint16_t)out_payload_len;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = MESH_BROADCAST_ID;
    out->queued_at_ms = now_ms;
    out->queued_at_valid = true;
    out->flood_retry_count = fields.flood_retry_count;
    flood_control.random_backoff_max_ms = fields.random_backoff_max_ms;
    flood_control.random_backoff_slot_ms = fields.random_backoff_slot_ms;
    flood_control.retry_count = fields.flood_retry_count;
    out->earliest_tx_ms = now_ms + flood_forward_total_delay_ms(local_id,
                                                                fields.slot_seed,
                                                                fields.hop_count,
                                                                &flood_control,
                                                                random_value);
    out->earliest_tx_valid = true;
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
    struct mesh_route_path candidate_path;
    struct reactive_route_rollback route_rollback = {0};
    const struct route_candidate *candidate;
    uint32_t previous_gateway_epoch;
    uint32_t route_epoch;
    uint8_t path_quality;
    bool forward_valid = false;
    int ret;
    int route_ret;

    previous_gateway_epoch = relay->upstream.current_epoch;
    ret = validate_gateway_route_adv_packet(relay,
                                            packet,
                                            payload,
                                            payload_len,
                                            previous_hop_id,
                                            &fields,
                                            &route_epoch);
    if (ret != PROTO_OK) {
        return ret;
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
        forward_valid = true;
    } else if (ret != PROTO_ERR_STALE) {
        return ret;
    }

    path_quality = combined_quality(fields.path_quality_min, link_quality);
    candidate_path = fields.path;
    ret = mesh_route_path_append(&candidate_path, relay->local_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    route_ret = upsert_reactive_route(relay,
                                      fields.gateway_id,
                                      previous_hop_id,
                                      route_epoch,
                                      fields.hop_count,
                                      path_quality,
                                      fields.gateway_capacity_state,
                                      UINT16_MAX,
                                      0u,
                                      fields.capacity_validity_interval_ms,
                                      now_ms,
                                      NULL,
                                      0u,
                                      &route_rollback);
    if (route_ret != PROTO_OK &&
        route_ret != PROTO_ERR_NO_SPACE) {
        return route_ret;
    }
    if (route_ret == PROTO_OK) {
        candidate = find_upstream_candidate(relay,
                                            previous_hop_id,
                                            route_epoch);
        ret = store_upstream_candidate_ancestry(relay,
                                                candidate,
                                                &candidate_path,
                                                NULL);
        if (ret != PROTO_OK) {
            rollback_reactive_route(relay, &route_rollback);
            return ret;
        }
        commit_route_epoch_transition(relay,
                                      previous_gateway_epoch,
                                      &route_rollback);
    }
    if (route_ret == PROTO_OK && relay->route_discovery.active &&
        relay->route_discovery.target_id == fields.gateway_id) {
        mesh_relay_note_route_discovery_ready(relay, fields.gateway_id);
        result->route_discovery_target_id = fields.gateway_id;
        result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY;
    }

    if (forward_valid) {
        result->actions |= MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV;
    }
    if (fields.operation_policy_present) {
        result->operation_policy = fields.operation_policy;
        result->actions |= MESH_RELAY_ACTION_INSTALL_OPERATION_POLICY;
    }

    /*
     * Commit freshness last. A parse, forwarding-build, route, or ancestry
     * failure must leave this exact sequence retryable.
     */
    relay->gateway_route_adv_seq = fields.gateway_route_seq;
    note_route_state_change(result);
    if (pending_recover_original_after_new_gateway_epoch(relay, now_ms)) {
        /* Reuse the existing adapter scheduling hook. The retained
         * owner is still the original; this bit denotes the confirmation
         * workflow transition, not a replacement packet. */
        result->actions |= MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING;
    }
    return PROTO_OK;
}

static int build_route_reply(struct mesh_relay *relay,
                             const struct route_discovery_fields *request,
                             const struct route_candidate *gateway_route,
                             const struct mesh_route_path *gateway_route_path,
                             uint64_t next_hop_id,
                             uint32_t session_id,
                             uint32_t route_epoch,
                             struct mesh_outbound *out)
{
    struct route_discovery_fields fields;
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
    fields.slot_seed = route_discovery_slot_seed(fields.origin_id,
                                                 fields.target_id,
                                                 session_id,
                                                 route_epoch);
    if (gateway_route != NULL && gateway_route->hop_count < UINT8_MAX) {
        if (gateway_route_path == NULL ||
            mesh_route_path_validate(gateway_route_path,
                                     request->target_id,
                                     relay->local_id) != PROTO_OK) {
            return PROTO_ERR_NOT_FOUND;
        }
        fields.path = *gateway_route_path;
        fields.hop_count = gateway_route->hop_count + 1u;
        fields.quality = gateway_route->link_quality;
    } else {
        memset(&fields.path, 0, sizeof(fields.path));
        fields.path.node_ids[0] = relay->local_id;
        fields.path.count = 1u;
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
    out->packet.ttl = MESH_NETWORK_MAX_HOPS;
    out->packet.payload_len = (uint16_t)payload_len;
    ret = append_route_reply_commitment(&out->packet,
                                        out->payload,
                                        sizeof(out->payload),
                                        &payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
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
    ret = validate_route_reply_identity(packet,
                                        payload,
                                        payload_len,
                                        &fields);
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
    if (mesh_route_path_contains(&fields.path, next_hop_id)) {
        return PROTO_ERR_STALE;
    }
    ret = mesh_route_path_append(&fields.path, relay->local_id);
    if (ret != PROTO_OK) {
        return ret;
    }
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
    fields.reply_commitment_present = false;
    memset(fields.reply_commitment, 0, sizeof(fields.reply_commitment));
    ret = append_route_discovery_tlvs(out->payload,
                                      sizeof(out->payload),
                                      &out_payload_len,
                                      &fields);
    if (ret != PROTO_OK) {
        return ret;
    }

    out->packet = *packet;
    out->packet.ttl = (uint8_t)(packet->ttl - 1u);
    out->packet.payload_len = (uint16_t)out_payload_len;
    ret = append_route_reply_commitment(&out->packet,
                                        out->payload,
                                        sizeof(out->payload),
                                        &out_payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
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
#if !IMEC_MESH_RELAY_GATEWAY_ONLY
    struct flood_seen_entry *flood_entry = NULL;
    const struct route_candidate *selected = NULL;
    const struct mesh_route_path *selected_path = NULL;
    struct mesh_route_path direct_selected_path = {0};
    struct route_candidate reply_route = {0};
    struct mesh_event_timing downstream_timing = {0};
    struct reactive_route_rollback route_rollback = {0};
    bool first_seen = false;
    bool best_updated = false;
    bool reply_available;
    bool gateway_reply_available = false;
    bool install_reply_timing = false;
    bool epoch_transition_committed = false;
    uint32_t previous_epoch;
#endif
    int ret;

    ret = validate_route_request_packet(relay,
                                        packet,
                                        payload,
                                        payload_len,
                                        previous_hop_id,
                                        now_ms,
                                        &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
#if IMEC_MESH_RELAY_GATEWAY_ONLY
    (void)link_quality;
    (void)random_value;
    (void)result;
    (void)duplicate_packet;
    return PROTO_ERR_STALE;
#else
    if (relay->role == MESH_RELAY_ROLE_GATEWAY) {
        return PROTO_ERR_STALE;
    }
    if (relay->role == MESH_RELAY_ROLE_ANCHOR &&
        relay_active_channel9_timing_count(relay) > 0u) {
        return PROTO_ERR_BUSY;
    }

    previous_epoch = relay->upstream.current_epoch;
    if (fields.route_epoch != previous_epoch &&
        route_epoch_strictly_newer(fields.route_epoch, previous_epoch)) {
        /*
         * A request from a node that has already entered a newer gateway
         * generation invalidates the receiver's old upstream path too. Clear
         * old reverse routes/timings before installing this request's reverse
         * path; an old selected gateway route must not be relabeled and
         * returned under the newer epoch.
         */
        route_table_init(&relay->upstream, fields.route_epoch);
        commit_route_epoch_transition(relay, previous_epoch, NULL);
        epoch_transition_committed = true;
        note_route_state_change(result);
    }
    fields.quality = combined_quality(fields.quality, link_quality);
    selected = route_selected(&relay->upstream);
    if (selected != NULL) {
        selected_path = upstream_candidate_ancestry(relay, selected);
        if (selected_path == NULL &&
            selected->next_hop_id == relay->gateway_id &&
            selected->gateway_id == relay->gateway_id &&
            selected->hop_count == 0u) {
            direct_selected_path.node_ids[0] = relay->gateway_id;
            direct_selected_path.node_ids[1] = relay->local_id;
            direct_selected_path.count = 2u;
            selected_path = &direct_selected_path;
        }
    }
    if (fields.target_id == relay->gateway_id &&
        selected != NULL &&
        selected->next_hop_id != previous_hop_id &&
        selected_path != NULL &&
        !mesh_route_paths_intersect(selected_path, &fields.path)) {
        reply_route = *selected;
        gateway_reply_available = true;
    }
    reply_available = fields.target_id == relay->local_id ||
        gateway_reply_available;
    install_reply_timing = reply_available &&
        fields.proposed_channel9_timing_valid;
    if (install_reply_timing) {
        downstream_timing = fields.proposed_channel9_timing;
        mesh_event_timing_set_local_first_slot_tx(&downstream_timing, false);
        ret = mesh_relay_check_channel9_timing_guarded_direction(
            relay,
            previous_hop_id,
            &downstream_timing,
            MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
            MESH_RELAY_EVENT_TIMINGS,
            NULL);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
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
                                packet->session_id,
                                install_reply_timing ? &route_rollback : NULL);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!epoch_transition_committed) {
        commit_route_epoch_transition(relay,
                                      previous_epoch,
                                      install_reply_timing ?
                                          &route_rollback : NULL);
    }
    flood_entry = route_solicit_flood_note(relay,
                                           &fields,
                                           packet->session_id,
                                           previous_hop_id,
                                           now_ms,
                                           &first_seen,
                                           &best_updated);

    if (fields.target_id == relay->local_id ||
        gateway_reply_available) {
        uint32_t reply_epoch = fields.route_epoch;
        const struct route_candidate *reply_route_ptr =
            gateway_reply_available ? &reply_route : NULL;

        ret = route_epoch_select_newer(reply_epoch,
                                       relay->upstream.current_epoch,
                                       &reply_epoch);
        if (ret == PROTO_OK && reply_route_ptr != NULL) {
            ret = route_epoch_select_newer(reply_epoch,
                                           reply_route_ptr->route_epoch,
                                           &reply_epoch);
        }
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = build_route_reply(relay,
                                &fields,
                                reply_route_ptr,
                                gateway_reply_available ? selected_path : NULL,
                                previous_hop_id,
                                packet->session_id,
                                reply_epoch,
                                &result->route_reply);
        if (ret != PROTO_OK && install_reply_timing) {
            rollback_reactive_route(relay, &route_rollback);
        }
        if (ret == PROTO_OK) {
            result->route_reply.earliest_tx_ms = route_reply_earliest_tx_ms(
                now_ms,
                fields.route_reply_rx_delay_ms,
                random_value);
            result->route_reply.earliest_tx_valid = true;
            if (install_reply_timing) {
                ret = mesh_relay_set_channel9_timing_guarded_direction(
                    relay,
                    previous_hop_id,
                    &downstream_timing,
                    MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
                    MESH_RELAY_EVENT_TIMINGS,
                    NULL);
                if (ret != PROTO_OK) {
                    rollback_reactive_route(relay, &route_rollback);
                    return ret;
                }
            }
            add_route_reply_backup(relay,
                                   &fields,
                                   packet->session_id,
                                   previous_hop_id,
                                   result);
            result->actions |= MESH_RELAY_ACTION_SEND_ROUTE_REPLY;
        }
        if (ret != PROTO_OK) {
            return ret;
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
                result->route_request.earliest_tx_valid = true;
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
#endif
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
    struct mesh_route_path candidate_path;
    struct mesh_upstream_ancestry_entry ancestry_before = {0};
    struct mesh_event_timing upstream_timing = {0};
    struct reactive_route_rollback route_rollback = {0};
    uint8_t completed_reply_digest[SEMANTIC_DIGEST_SHA256_LEN] = {0};
    const struct route_candidate *candidate;
    enum mesh_relay_channel9_direction timing_direction;
    bool gateway_target;
    bool install_timing;
    uint32_t previous_epoch;
    int ret;

    ret = validate_route_reply_packet(relay,
                                      packet,
                                      payload,
                                      payload_len,
                                      previous_hop_id,
                                      now_ms,
                                      &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (relay->role == MESH_RELAY_ROLE_CLICKER &&
        packet->dst_id == relay->local_id &&
        !relay_semantic_packet_digest(packet,
                                      payload,
                                      payload_len,
                                      completed_reply_digest)) {
        return PROTO_ERR_MALFORMED;
    }
    gateway_target = fields.target_id == relay->gateway_id;
    timing_direction = gateway_target ?
        MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM :
        MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM;
    previous_epoch = relay->upstream.current_epoch;
    fields.quality = combined_quality(fields.quality, link_quality);
    install_timing = fields.proposed_channel9_timing_valid;
    if (install_timing) {
        upstream_timing = fields.proposed_channel9_timing;
        mesh_event_timing_set_local_first_slot_tx(&upstream_timing, true);
        ret = mesh_relay_check_channel9_timing_guarded_direction(
            relay,
            previous_hop_id,
            &upstream_timing,
            timing_direction,
            MESH_RELAY_EVENT_TIMINGS,
            NULL);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    candidate_path = fields.path;
    ret = mesh_route_path_append(&candidate_path, relay->local_id);
    if (ret != PROTO_OK) {
        return ret;
    }
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
                                0u,
                                &route_rollback);
    if (ret == PROTO_ERR_NO_SPACE) {
        /*
         * A full local candidate table cannot consume a valid reply before
         * its origin sees it.  The final destination can ACK immediately
         * because it owns the reply.  An intermediate instead returns both
         * forward and ACK actions only after it has secured a reverse-path
         * forward; the application must withhold the upstream ACK until that
         * downstream hop ACKs the forwarded reply.
         */
        mesh_relay_note_route_discovery_ready(relay, fields.target_id);
        if (packet->dst_id == relay->local_id) {
            add_route_reply_ack_action(relay,
                                       packet,
                                       payload,
                                       payload_len,
                                       previous_hop_id,
                                       result);
            result->route_discovery_target_id = fields.target_id;
            result->actions |= MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY;
            return PROTO_OK;
        }
        if (mesh_relay_tx_active(relay)) {
            add_busy_action(relay,
                            packet,
                            payload,
                            payload_len,
                            previous_hop_id,
                            result);
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
            add_route_reply_ack_action(relay,
                                       packet,
                                       payload,
                                       payload_len,
                                       previous_hop_id,
                                       result);
        }
        return ret;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    candidate = NULL;
    if (gateway_target) {
        candidate = find_upstream_candidate(relay,
                                            previous_hop_id,
                                            fields.route_epoch);
        ret = store_upstream_candidate_ancestry(relay,
                                                candidate,
                                                &candidate_path,
                                                &ancestry_before);
        if (ret != PROTO_OK) {
            rollback_reactive_route(relay, &route_rollback);
            return ret;
        }
        commit_route_epoch_transition(relay,
                                      previous_epoch,
                                      &route_rollback);
        if (relay->upstream.current_epoch != previous_epoch) {
            note_route_state_change(result);
        }
    }
    if (install_timing) {
        ret = mesh_relay_set_channel9_timing_guarded_direction(
            relay,
            previous_hop_id,
            &upstream_timing,
            timing_direction,
            MESH_RELAY_EVENT_TIMINGS,
            NULL);
        if (ret != PROTO_OK) {
            if (gateway_target &&
                route_rollback.kind !=
                    REACTIVE_ROUTE_ROLLBACK_UPSTREAM_EPOCH_COMMITTED) {
                rollback_reactive_route(relay, &route_rollback);
                restore_upstream_candidate_ancestry(relay,
                                                    candidate,
                                                    &ancestry_before);
            } else {
                rollback_reactive_route(relay, &route_rollback);
            }
            return ret;
        }
    }
    mesh_relay_note_route_discovery_ready(relay, fields.target_id);

    if (packet->dst_id == relay->local_id) {
        if (relay->role == MESH_RELAY_ROLE_CLICKER) {
            relay->route_discovery.completed_request_id =
                packet->session_id;
            relay->route_discovery.completed_request_until_ms =
                now_ms + ROUTE_DEDUP_WINDOW_MS;
            relay->route_discovery.completed_reply_previous_hop_id =
                previous_hop_id;
            memcpy(relay->route_discovery.completed_reply_digest,
                   completed_reply_digest,
                   sizeof(completed_reply_digest));
        }
        add_route_reply_ack_action(relay,
                                   packet,
                                   payload,
                                   payload_len,
                                   previous_hop_id,
                                   result);
        result->route_discovery_target_id = fields.target_id;
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
        add_route_reply_ack_action(relay,
                                   packet,
                                   payload,
                                   payload_len,
                                   previous_hop_id,
                                   result);
    }
    return ret;
}

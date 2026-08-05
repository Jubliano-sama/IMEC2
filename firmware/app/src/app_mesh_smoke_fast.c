#include "app_mesh_smoke_fast.h"

#include <string.h>

#define MESH_SMOKE_FAST_BUSY_RETRY_MS 100u

static int tlv_read_u8(const uint8_t *payload,
                       size_t payload_len,
                       uint8_t type,
                       uint8_t *out)
{
    const uint8_t *value;
    uint8_t len;
    int ret = tlv_find_unique(payload, payload_len, type, &value, &len);

    if (ret != PROTO_OK) {
        return ret;
    }
    if (len != 1u) {
        return PROTO_ERR_MALFORMED;
    }
    *out = value[0];
    return PROTO_OK;
}

static int tlv_read_u16(const uint8_t *payload,
                        size_t payload_len,
                        uint8_t type,
                        uint16_t *out)
{
    const uint8_t *value;
    uint8_t len;
    int ret = tlv_find_unique(payload, payload_len, type, &value, &len);

    if (ret != PROTO_OK) {
        return ret;
    }
    if (len != 2u) {
        return PROTO_ERR_MALFORMED;
    }
    *out = proto_get_u16_le(value);
    return PROTO_OK;
}

static int tlv_read_u32(const uint8_t *payload,
                        size_t payload_len,
                        uint8_t type,
                        uint32_t *out)
{
    const uint8_t *value;
    uint8_t len;
    int ret = tlv_find_unique(payload, payload_len, type, &value, &len);

    if (ret != PROTO_OK) {
        return ret;
    }
    if (len != 4u) {
        return PROTO_ERR_MALFORMED;
    }
    *out = proto_get_u32_le(value);
    return PROTO_OK;
}

static int tlv_read_u64(const uint8_t *payload,
                        size_t payload_len,
                        uint8_t type,
                        uint64_t *out)
{
    const uint8_t *value;
    uint8_t len;
    int ret = tlv_find_unique(payload, payload_len, type, &value, &len);

    if (ret != PROTO_OK) {
        return ret;
    }
    if (len != 8u) {
        return PROTO_ERR_MALFORMED;
    }
    *out = proto_get_u64_le(value);
    return PROTO_OK;
}

static int find_crc_tlv_offset(const uint8_t *payload,
                               size_t payload_len,
                               size_t *crc_offset)
{
    size_t offset = 0u;
    bool found = false;

    if (payload == NULL || crc_offset == NULL) {
        return PROTO_ERR_ARG;
    }
    while (offset < payload_len) {
        uint8_t type;
        uint8_t len;

        if (payload_len - offset < 2u) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        len = payload[offset + 1u];
        if (payload_len - offset - 2u < len) {
            return PROTO_ERR_MALFORMED;
        }
        if (type == TLV_MESH_TEST_PAYLOAD_CRC) {
            if (found || len != sizeof(uint16_t) ||
                offset + PROTO_TLV_HEADER_LEN + len != payload_len) {
                return PROTO_ERR_MALFORMED;
            }
            *crc_offset = offset;
            found = true;
        }
        offset += 2u + len;
    }
    return found ? PROTO_OK : PROTO_ERR_NOT_FOUND;
}

static void latency_insert(struct mesh_smoke_fast_state *state, uint32_t latency_ms)
{
    state->latency_samples[state->latency_next] = latency_ms;
    state->latency_next = (uint8_t)((state->latency_next + 1u) %
                                    MESH_SMOKE_FAST_LATENCY_SAMPLES);
    if (state->latency_count < MESH_SMOKE_FAST_LATENCY_SAMPLES) {
        state->latency_count++;
    }
}

static uint32_t percentile(const struct mesh_smoke_fast_state *state,
                           uint8_t percentile_index)
{
    uint32_t sorted[MESH_SMOKE_FAST_LATENCY_SAMPLES];
    uint8_t count;

    if (state == NULL || state->latency_count == 0u) {
        return 0u;
    }
    count = state->latency_count;
    memcpy(sorted, state->latency_samples, (size_t)count * sizeof(sorted[0]));
    for (uint8_t i = 1u; i < count; i++) {
        uint32_t value = sorted[i];
        uint8_t j = i;

        while (j > 0u && sorted[j - 1u] > value) {
            sorted[j] = sorted[j - 1u];
            j--;
        }
        sorted[j] = value;
    }
    if (percentile_index >= count) {
        percentile_index = (uint8_t)(count - 1u);
    }
    return sorted[percentile_index];
}

static int missing_find(const struct mesh_smoke_fast_state *state, uint32_t packet_id)
{
    for (uint8_t i = 0u; i < MESH_SMOKE_FAST_MISSING_TRACKED; i++) {
        if (state->missing[i] == packet_id) {
            return i;
        }
    }
    return -1;
}

static void missing_track(struct mesh_smoke_fast_state *state, uint32_t packet_id)
{
    if (missing_find(state, packet_id) >= 0) {
        return;
    }
    for (uint8_t i = 0u; i < MESH_SMOKE_FAST_MISSING_TRACKED; i++) {
        if (state->missing[i] == 0u) {
            state->missing[i] = packet_id;
            return;
        }
    }
    state->missing[0] = packet_id;
}

static bool missing_clear(struct mesh_smoke_fast_state *state, uint32_t packet_id)
{
    int index = missing_find(state, packet_id);

    if (index < 0) {
        return false;
    }
    state->missing[index] = 0u;
    return true;
}

int mesh_smoke_fast_payload_append(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *payload_len,
                                   const struct mesh_smoke_fast_payload_input *input,
                                   size_t target_payload_len)
{
    size_t offset = 0u;
    uint16_t crc;
    int ret;

    if (payload == NULL || payload_len == NULL || input == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_append_u32(payload, payload_cap, &offset,
                         TLV_MESH_TEST_PACKET_ID, input->packet_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, &offset,
                         TLV_MESH_TEST_ATTEMPT, input->attempt);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, &offset,
                         TLV_MESH_TEST_DROP_COUNT, input->drop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, &offset,
                         TLV_MESH_TEST_ORIGIN_ID, input->origin_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, &offset,
                         TLV_MESH_TEST_TARGET_ID, input->target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, &offset,
                         TLV_MESH_TEST_FLAGS, input->flags);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, &offset,
                         TLV_EVENT_SEQ, input->packet_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, &offset,
                        TLV_RETRY_COUNT,
                        input->attempt > UINT8_MAX ? UINT8_MAX : (uint8_t)input->attempt);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, &offset,
                         TLV_UPTIME_MS, input->build_uptime_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, &offset,
                         TLV_MESH_TEST_PACKET_AGE_MS, input->packet_age_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, &offset,
                         TLV_MESH_TEST_SELECTED_PARENT_ID,
                         input->selected_parent_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, &offset,
                        TLV_MESH_TEST_CH9_TIMING_STATE,
                        input->ch9_timing_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, &offset,
                        TLV_DEVICE_ROLE, input->device_role);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, &offset,
                        TLV_MESH_CHANNEL, input->mesh_channel);
    if (ret != PROTO_OK) {
        return ret;
    }
    while (target_payload_len > offset + 4u) {
        uint8_t pad[UINT8_MAX] = {0};
        size_t remaining = target_payload_len - offset - 4u;
        uint8_t chunk_len = (uint8_t)(remaining > sizeof(pad) ?
                                      sizeof(pad) :
                                      remaining);

        ret = tlv_append_bytes(payload,
                               payload_cap,
                               &offset,
                               TLV_MESH_TEST_PADDING,
                               pad,
                               chunk_len);
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    crc = proto_crc16_ccitt_false(payload, offset);
    ret = tlv_append_u16(payload, payload_cap, &offset,
                         TLV_MESH_TEST_PAYLOAD_CRC, crc);
    if (ret != PROTO_OK) {
        return ret;
    }
    *payload_len = offset;
    return PROTO_OK;
}

int mesh_smoke_fast_payload_decode(const uint8_t *payload,
                                   size_t payload_len,
                                   struct mesh_smoke_fast_payload *out)
{
    size_t crc_offset;
    uint16_t actual_crc;
    int ret;

    if (payload == NULL || out == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    ret = find_crc_tlv_offset(payload, payload_len, &crc_offset);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u16(payload, payload_len,
                       TLV_MESH_TEST_PAYLOAD_CRC, &out->payload_crc);
    if (ret != PROTO_OK) {
        return ret;
    }
    actual_crc = proto_crc16_ccitt_false(payload, crc_offset);
    if (actual_crc != out->payload_crc) {
        return PROTO_ERR_BAD_CRC;
    }
    ret = tlv_read_u32(payload, payload_len,
                       TLV_MESH_TEST_PACKET_ID, &out->packet_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u16(payload, payload_len,
                       TLV_MESH_TEST_ATTEMPT, &out->attempt);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u32(payload, payload_len,
                       TLV_MESH_TEST_DROP_COUNT, &out->drop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u64(payload, payload_len,
                       TLV_MESH_TEST_ORIGIN_ID, &out->origin_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u64(payload, payload_len,
                       TLV_MESH_TEST_TARGET_ID, &out->target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u32(payload, payload_len,
                       TLV_MESH_TEST_FLAGS, &out->flags);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u8(payload, payload_len,
                      TLV_RETRY_COUNT, &out->retry_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u32(payload, payload_len, TLV_UPTIME_MS, &out->build_uptime_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u32(payload, payload_len,
                       TLV_MESH_TEST_PACKET_AGE_MS, &out->packet_age_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u64(payload, payload_len,
                       TLV_MESH_TEST_SELECTED_PARENT_ID,
                       &out->selected_parent_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u8(payload, payload_len,
                      TLV_MESH_TEST_CH9_TIMING_STATE,
                      &out->ch9_timing_state);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u8(payload, payload_len, TLV_DEVICE_ROLE, &out->device_role);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_read_u8(payload, payload_len, TLV_MESH_CHANNEL, &out->mesh_channel);
    if (ret != PROTO_OK) {
        return ret;
    }
    return PROTO_OK;
}

void mesh_smoke_fast_tx_decide(const struct mesh_smoke_fast_tx_gate *gate,
                               struct mesh_smoke_fast_tx_decision *decision)
{
    if (decision == NULL) {
        return;
    }
    memset(decision, 0, sizeof(*decision));
    if (gate == NULL || gate->queue_depth == 0u ||
        gate->queue_used >= gate->queue_depth) {
        decision->reason = MESH_SMOKE_FAST_DEFER_QUEUE_FULL;
        decision->delay_ms = MESH_SMOKE_FAST_BUSY_RETRY_MS;
        return;
    }
    decision->queue_headroom = gate->queue_depth - gate->queue_used;
    if (gate->relay_tx_active) {
        decision->reason = MESH_SMOKE_FAST_DEFER_RELAY_TX;
    } else if (gate->route_waiting_active) {
        decision->reason = MESH_SMOKE_FAST_DEFER_ROUTE_WAIT;
    } else if (gate->ack_wait_active) {
        decision->reason = MESH_SMOKE_FAST_DEFER_ACK_WAIT;
    } else {
        decision->can_queue = true;
        decision->reason = MESH_SMOKE_FAST_DEFER_NONE;
        decision->delay_ms = gate->fast_mode ? 0u : gate->configured_interval_ms;
        return;
    }
    decision->delay_ms = MESH_SMOKE_FAST_BUSY_RETRY_MS;
}

void mesh_smoke_fast_init(struct mesh_smoke_fast_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

int mesh_smoke_fast_note_delivery(struct mesh_smoke_fast_state *state,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  uint32_t gateway_now_ms,
                                  uint32_t gateway_ack_latency_ms,
                                  uint32_t queue_depth)
{
    struct mesh_smoke_fast_payload decoded;
    uint32_t previous_id;
    bool was_missing;
    int ret;

    if (state == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = mesh_smoke_fast_payload_decode(payload, payload_len, &decoded);
    if (ret == PROTO_ERR_BAD_CRC) {
        state->summary.crc_fail_count++;
        return ret;
    }
    if (ret != PROTO_OK) {
        return ret;
    }

    if (decoded.build_uptime_ms + decoded.packet_age_ms > gateway_now_ms) {
        state->summary.age_invalid_count++;
    }
    was_missing = missing_clear(state, decoded.packet_id);
    if (was_missing) {
        state->summary.later_delivered_missing_count++;
    }
    previous_id = state->summary.last_packet_id;
    if (previous_id != 0u) {
        if (decoded.packet_id == previous_id) {
            state->summary.duplicate_count++;
        } else if (decoded.packet_id < previous_id && !was_missing) {
            state->summary.duplicate_count++;
        } else if (decoded.packet_id > previous_id + 1u) {
            state->summary.gap_count++;
            state->summary.last_gap_start = previous_id + 1u;
            state->summary.last_gap_end = decoded.packet_id - 1u;
            for (uint32_t id = state->summary.last_gap_start;
                 id <= state->summary.last_gap_end;
                 id++) {
                state->summary.missing_count++;
                missing_track(state, id);
                if (id == UINT32_MAX) {
                    break;
                }
            }
        }
    }
    if (decoded.packet_id > state->summary.last_packet_id) {
        state->summary.last_packet_id = decoded.packet_id;
    }
    state->summary.delivered_count++;
    if (state->summary.first_delivered_ms == 0u) {
        state->summary.first_delivered_ms = gateway_now_ms;
    }
    state->summary.last_delivered_ms = gateway_now_ms;
    state->summary.retry_total += decoded.retry_count;
    if (decoded.retry_count > state->summary.retry_max) {
        state->summary.retry_max = decoded.retry_count;
    }
    if (queue_depth > state->summary.queue_depth_max) {
        state->summary.queue_depth_max = queue_depth;
    }
    latency_insert(state, gateway_ack_latency_ms);
    state->summary.gateway_ack_latency_max_ms =
        gateway_ack_latency_ms > state->summary.gateway_ack_latency_max_ms ?
        gateway_ack_latency_ms :
        state->summary.gateway_ack_latency_max_ms;
    return PROTO_OK;
}

void mesh_smoke_fast_note_missing_reason(struct mesh_smoke_fast_state *state,
                                         uint32_t packet_id,
                                         uint32_t reason)
{
    if (state == NULL) {
        return;
    }
    if (packet_id != 0u && missing_clear(state, packet_id)) {
        state->summary.attributed_missing_count++;
    }
    state->summary.last_drop_or_defer_reason = reason;
}

void mesh_smoke_fast_note_ch9_missed(struct mesh_smoke_fast_state *state)
{
    if (state != NULL) {
        state->summary.missed_ch9_events++;
    }
}

void mesh_smoke_fast_note_c5_refresh(struct mesh_smoke_fast_state *state)
{
    if (state != NULL) {
        state->summary.c5_refreshes++;
    }
}

void mesh_smoke_fast_get_summary(const struct mesh_smoke_fast_state *state,
                                 struct mesh_smoke_fast_summary *out)
{
    if (state == NULL || out == NULL) {
        return;
    }
    *out = state->summary;
    out->gateway_ack_latency_p50_ms = percentile(state, state->latency_count / 2u);
    out->gateway_ack_latency_p95_ms =
        percentile(state, (uint8_t)(((uint16_t)state->latency_count * 95u) / 100u));
}

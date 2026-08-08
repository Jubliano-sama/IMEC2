#include "report.h"

static bool ids_are_valid(uint64_t src_id, uint64_t dst_id)
{
    return src_id != 0u && dst_id != 0u && src_id != dst_id;
}

static bool range_status_valid(enum range_status status)
{
    return status >= RANGE_OK &&
           status <= RANGE_TIMING_INVALID &&
           status != RANGE_STS_QUALITY_FAIL;
}

int report_range_transport_seq(uint8_t attempt_index,
                               uint16_t fragment_index,
                               uint16_t *packet_seq)
{
    uint32_t sequence;

    if (packet_seq == NULL) {
        return PROTO_ERR_ARG;
    }
    if (attempt_index == 0u ||
        fragment_index >= RANGE_REPORT_MAX_TRANSPORT_FRAGMENTS) {
        return PROTO_ERR_MALFORMED;
    }

    /*
     * Relay duplicate identity includes the packet header sequence but not the
     * burst TLVs. Reserve a complete fragment namespace for every ranging
     * attempt so a later retry cannot conflict with an earlier attempt for the
     * same click event.
     */
    sequence =
        (((uint32_t)attempt_index - 1u) *
         RANGE_REPORT_MAX_TRANSPORT_FRAGMENTS) +
        fragment_index + 1u;
    *packet_seq = (uint16_t)sequence;
    return PROTO_OK;
}

enum click_payload_field {
    CLICK_PAYLOAD_CLICKER_ID = UINT32_C(1) << 0,
    CLICK_PAYLOAD_ANCHOR_ID = UINT32_C(1) << 1,
    CLICK_PAYLOAD_EVENT_SEQ = UINT32_C(1) << 2,
    CLICK_PAYLOAD_TIMESTAMP = UINT32_C(1) << 3,
    CLICK_PAYLOAD_DISTANCE = UINT32_C(1) << 4,
    CLICK_PAYLOAD_QUALITY = UINT32_C(1) << 5,
    CLICK_PAYLOAD_RANGE_STATUS = UINT32_C(1) << 6,
    CLICK_PAYLOAD_SAMPLE_COUNT = UINT32_C(1) << 7,
    CLICK_PAYLOAD_SAMPLE_INDEX = UINT32_C(1) << 8,
    CLICK_PAYLOAD_DISTANCE_SAMPLES = UINT32_C(1) << 9,
    CLICK_PAYLOAD_ROUND_INDICES = UINT32_C(1) << 10,
    CLICK_PAYLOAD_SEQUENCE_TIMESTAMPS = UINT32_C(1) << 11,
    CLICK_PAYLOAD_ATTEMPT_INDEX = UINT32_C(1) << 12,
    CLICK_PAYLOAD_DETECTION_SOURCE = UINT32_C(1) << 13,
    CLICK_PAYLOAD_BURST_ID = UINT32_C(1) << 14,
    CLICK_PAYLOAD_FRAGMENT_INDEX = UINT32_C(1) << 15,
    CLICK_PAYLOAD_FRAGMENT_COUNT = UINT32_C(1) << 16,
    CLICK_PAYLOAD_CIR_BYTE_OFFSET = UINT32_C(1) << 17,
    CLICK_PAYLOAD_CIR_TOTAL_BYTES = UINT32_C(1) << 18,
    CLICK_PAYLOAD_CIR_FIRST_PATH = UINT32_C(1) << 19,
    CLICK_PAYLOAD_CIR_START_INDEX = UINT32_C(1) << 20,
};

#define CLICK_PAYLOAD_COMMON_FIELDS \
    (CLICK_PAYLOAD_CLICKER_ID | CLICK_PAYLOAD_ANCHOR_ID | \
     CLICK_PAYLOAD_EVENT_SEQ | CLICK_PAYLOAD_TIMESTAMP)
#define CLICK_PAYLOAD_RANGE_FIELDS \
    (CLICK_PAYLOAD_DISTANCE | CLICK_PAYLOAD_QUALITY | CLICK_PAYLOAD_RANGE_STATUS)
#define CLICK_PAYLOAD_SAMPLE_FIELDS \
    (CLICK_PAYLOAD_SAMPLE_COUNT | CLICK_PAYLOAD_DISTANCE_SAMPLES | \
     CLICK_PAYLOAD_ROUND_INDICES | CLICK_PAYLOAD_SEQUENCE_TIMESTAMPS)
#define CLICK_PAYLOAD_DETECTION_FIELDS \
    (CLICK_PAYLOAD_ATTEMPT_INDEX | CLICK_PAYLOAD_DETECTION_SOURCE)
#define CLICK_PAYLOAD_CIR_FRAGMENT_FIELDS \
    (CLICK_PAYLOAD_FRAGMENT_INDEX | CLICK_PAYLOAD_FRAGMENT_COUNT | \
     CLICK_PAYLOAD_CIR_BYTE_OFFSET | CLICK_PAYLOAD_CIR_TOTAL_BYTES | \
     CLICK_PAYLOAD_CIR_FIRST_PATH | CLICK_PAYLOAD_CIR_START_INDEX)

int report_validate_click_payload(const struct proto_packet *packet,
                                  const uint8_t *payload,
                                  size_t payload_len)
{
    uint64_t clicker_id = 0u;
    uint64_t anchor_id = 0u;
    uint32_t event_seq = 0u;
    uint16_t sample_count = 0u;
    uint16_t sample_index = 0u;
    uint16_t fragment_index = 0u;
    uint16_t fragment_count = 0u;
    uint16_t cir_byte_offset = 0u;
    uint16_t cir_total_bytes = 0u;
    size_t distance_sample_count = 0u;
    size_t round_index_count = 0u;
    size_t sequence_timestamp_count = 0u;
    size_t cir_chunk_bytes = 0u;
    size_t offset = 0u;
    uint32_t seen = 0u;
    uint8_t mode_flags;

    if (packet == NULL || payload == NULL ||
        packet->msg_type != MSG_CLICK_REPORT ||
        packet->payload_len != payload_len ||
        payload_len == 0u) {
        return PROTO_ERR_ARG;
    }
    mode_flags = packet->flags & (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC);
    if ((packet->flags &
         ~(FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK |
           FLAG_DIAGNOSTIC)) != 0u ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        mode_flags == 0u ||
        mode_flags == (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC)) {
        return PROTO_ERR_MALFORMED;
    }

    while (offset < payload_len) {
        uint8_t type;
        uint8_t length;
        const uint8_t *value;
        uint32_t field = 0u;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        length = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if (length > payload_len - offset) {
            return PROTO_ERR_MALFORMED;
        }
        value = &payload[offset];

        switch (type) {
        case TLV_CLICKER_ID:
            field = CLICK_PAYLOAD_CLICKER_ID;
            if (length != sizeof(uint64_t)) {
                return PROTO_ERR_MALFORMED;
            }
            clicker_id = proto_get_u64_le(value);
            break;
        case TLV_ANCHOR_ID:
            field = CLICK_PAYLOAD_ANCHOR_ID;
            if (length != sizeof(uint64_t)) {
                return PROTO_ERR_MALFORMED;
            }
            anchor_id = proto_get_u64_le(value);
            break;
        case TLV_EVENT_SEQ:
            field = CLICK_PAYLOAD_EVENT_SEQ;
            if (length != sizeof(uint32_t)) {
                return PROTO_ERR_MALFORMED;
            }
            event_seq = proto_get_u32_le(value);
            break;
        case TLV_TIMESTAMP_MS:
            field = CLICK_PAYLOAD_TIMESTAMP;
            if (length != sizeof(uint64_t)) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        case TLV_DISTANCE_MM:
            field = CLICK_PAYLOAD_DISTANCE;
            if (length != sizeof(uint32_t)) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        case TLV_QUALITY:
            field = CLICK_PAYLOAD_QUALITY;
            if (length != sizeof(uint8_t) || value[0] > 100u) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        case TLV_RANGE_STATUS:
            field = CLICK_PAYLOAD_RANGE_STATUS;
            if (length != sizeof(uint8_t) ||
                !range_status_valid((enum range_status)value[0])) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        case TLV_SAMPLE_COUNT:
            field = CLICK_PAYLOAD_SAMPLE_COUNT;
            if (length != sizeof(uint16_t)) {
                return PROTO_ERR_MALFORMED;
            }
            sample_count = proto_get_u16_le(value);
            break;
        case TLV_SAMPLE_INDEX:
            field = CLICK_PAYLOAD_SAMPLE_INDEX;
            if (length != sizeof(uint16_t)) {
                return PROTO_ERR_MALFORMED;
            }
            sample_index = proto_get_u16_le(value);
            break;
        case TLV_DISTANCE_SAMPLES_MM:
            field = CLICK_PAYLOAD_DISTANCE_SAMPLES;
            if (length == 0u || length % sizeof(uint32_t) != 0u) {
                return PROTO_ERR_MALFORMED;
            }
            distance_sample_count = length / sizeof(uint32_t);
            break;
        case TLV_RANGE_ROUND_INDICES:
            field = CLICK_PAYLOAD_ROUND_INDICES;
            if (length == 0u) {
                return PROTO_ERR_MALFORMED;
            }
            round_index_count = length;
            break;
        case TLV_SEQUENCE_START_TIMESTAMPS_MS:
            field = CLICK_PAYLOAD_SEQUENCE_TIMESTAMPS;
            if (length == 0u || length % sizeof(uint64_t) != 0u) {
                return PROTO_ERR_MALFORMED;
            }
            sequence_timestamp_count = length / sizeof(uint64_t);
            break;
        case TLV_ATTEMPT_INDEX:
            field = CLICK_PAYLOAD_ATTEMPT_INDEX;
            if (length != sizeof(uint8_t) || value[0] == 0u) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        case TLV_DETECTION_SOURCE:
            field = CLICK_PAYLOAD_DETECTION_SOURCE;
            if (length != sizeof(uint8_t) ||
                value[0] != DETECTION_SOURCE_UWB_WAKE_CLAIM) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        case TLV_BURST_ID:
            field = CLICK_PAYLOAD_BURST_ID;
            if (length != sizeof(uint32_t) || proto_get_u32_le(value) == 0u) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        case TLV_DIAG_FRAGMENT_INDEX:
            field = CLICK_PAYLOAD_FRAGMENT_INDEX;
            if (length != sizeof(uint16_t)) {
                return PROTO_ERR_MALFORMED;
            }
            fragment_index = proto_get_u16_le(value);
            break;
        case TLV_DIAG_FRAGMENT_COUNT:
            field = CLICK_PAYLOAD_FRAGMENT_COUNT;
            if (length != sizeof(uint16_t)) {
                return PROTO_ERR_MALFORMED;
            }
            fragment_count = proto_get_u16_le(value);
            break;
        case TLV_UWB_CIR_BYTE_OFFSET:
            field = CLICK_PAYLOAD_CIR_BYTE_OFFSET;
            if (length != sizeof(uint16_t)) {
                return PROTO_ERR_MALFORMED;
            }
            cir_byte_offset = proto_get_u16_le(value);
            break;
        case TLV_UWB_CIR_TOTAL_BYTES:
            field = CLICK_PAYLOAD_CIR_TOTAL_BYTES;
            if (length != sizeof(uint16_t)) {
                return PROTO_ERR_MALFORMED;
            }
            cir_total_bytes = proto_get_u16_le(value);
            break;
        case TLV_UWB_CIR_FIRST_PATH_INDEX:
            field = CLICK_PAYLOAD_CIR_FIRST_PATH;
            if (length != sizeof(uint16_t)) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        case TLV_UWB_CIR_START_INDEX:
            field = CLICK_PAYLOAD_CIR_START_INDEX;
            if (length != sizeof(uint16_t)) {
                return PROTO_ERR_MALFORMED;
            }
            break;
        case TLV_UWB_CIR_FULL_CHUNK:
            if (length == 0u) {
                return PROTO_ERR_MALFORMED;
            }
            cir_chunk_bytes += length;
            break;
        default:
            break;
        }

        if (field != 0u) {
            if ((seen & field) != 0u) {
                return PROTO_ERR_MALFORMED;
            }
            seen |= field;
        }
        offset += length;
    }

    if ((seen & CLICK_PAYLOAD_COMMON_FIELDS) != CLICK_PAYLOAD_COMMON_FIELDS ||
        clicker_id == 0u || anchor_id == 0u ||
        clicker_id == anchor_id ||
        anchor_id != packet->src_id ||
        event_seq == 0u ||
        proto_click_report_session_id(clicker_id, event_seq) !=
            packet->session_id) {
        return PROTO_ERR_MALFORMED;
    }
    if ((seen & CLICK_PAYLOAD_DETECTION_FIELDS) != 0u &&
        (seen & CLICK_PAYLOAD_DETECTION_FIELDS) !=
            CLICK_PAYLOAD_DETECTION_FIELDS) {
        return PROTO_ERR_MALFORMED;
    }

    if (cir_chunk_bytes > 0u ||
        (seen & CLICK_PAYLOAD_CIR_FRAGMENT_FIELDS) != 0u) {
        if (mode_flags != FLAG_DIAGNOSTIC ||
            (seen & CLICK_PAYLOAD_CIR_FRAGMENT_FIELDS) !=
                CLICK_PAYLOAD_CIR_FRAGMENT_FIELDS ||
            fragment_count == 0u || fragment_index >= fragment_count ||
            cir_total_bytes == 0u || cir_byte_offset >= cir_total_bytes ||
            cir_chunk_bytes == 0u ||
            cir_chunk_bytes > (size_t)(cir_total_bytes - cir_byte_offset)) {
            return PROTO_ERR_MALFORMED;
        }
        return PROTO_OK;
    }

    if ((seen & CLICK_PAYLOAD_RANGE_FIELDS) != CLICK_PAYLOAD_RANGE_FIELDS) {
        return PROTO_ERR_MALFORMED;
    }
    if ((seen & (CLICK_PAYLOAD_SAMPLE_FIELDS |
                 CLICK_PAYLOAD_SAMPLE_INDEX)) != 0u) {
        if ((seen & CLICK_PAYLOAD_SAMPLE_FIELDS) !=
                CLICK_PAYLOAD_SAMPLE_FIELDS ||
            sample_count == 0u ||
            sample_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES ||
            distance_sample_count == 0u ||
            distance_sample_count != round_index_count ||
            distance_sample_count != sequence_timestamp_count ||
            sample_index >= sample_count ||
            distance_sample_count >
                (size_t)(sample_count - sample_index)) {
            return PROTO_ERR_MALFORMED;
        }
    }
    if (mode_flags == FLAG_COUNT_AS_CLICK &&
        ((seen & CLICK_PAYLOAD_SAMPLE_FIELDS) !=
             CLICK_PAYLOAD_SAMPLE_FIELDS ||
         (seen & CLICK_PAYLOAD_BURST_ID) == 0u)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static int append_distance_samples(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *offset,
                                   const struct range_report_fields *fields)
{
    uint8_t samples[RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET * sizeof(int32_t)];
    uint8_t sequence_start_timestamps[RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET *
                                      sizeof(uint64_t)];
    uint16_t chunk_count;
    bool fragmented;
    size_t sample_bytes;
    size_t sequence_timestamp_bytes;
    int ret;

    if (fields->sample_count == 0u) {
        return PROTO_OK;
    }
    chunk_count = fields->distance_sample_count == 0u ?
                  fields->sample_count :
                  fields->distance_sample_count;
    fragmented = fields->sample_index != 0u || chunk_count != fields->sample_count;
    if (fields->distance_samples_mm == NULL ||
        fields->range_round_indices == NULL ||
        fields->sequence_start_timestamps_ms == NULL ||
        fields->sample_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES ||
        chunk_count == 0u ||
        chunk_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET ||
        (fragmented && chunk_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT) ||
        fields->sample_index > fields->sample_count ||
        chunk_count > fields->sample_count - fields->sample_index) {
        return PROTO_ERR_MALFORMED;
    }

    sample_bytes = (size_t)chunk_count * sizeof(int32_t);
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_SAMPLE_COUNT,
                         fields->sample_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (fragmented) {
        ret = tlv_append_u16(payload, payload_cap, offset, TLV_SAMPLE_INDEX,
                             fields->sample_index);
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    for (uint16_t i = 0u; i < chunk_count; i++) {
        proto_put_u32_le(&samples[(size_t)i * sizeof(int32_t)],
                         (uint32_t)fields->distance_samples_mm[i]);
    }
    ret = tlv_append_bytes(payload, payload_cap, offset,
                           TLV_DISTANCE_SAMPLES_MM,
                           samples, (uint8_t)sample_bytes);
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = tlv_append_bytes(payload, payload_cap, offset,
                           TLV_RANGE_ROUND_INDICES,
                           fields->range_round_indices,
                           (uint8_t)chunk_count);
    if (ret != PROTO_OK) {
        return ret;
    }

    sequence_timestamp_bytes = (size_t)chunk_count * sizeof(uint64_t);
    for (uint16_t i = 0u; i < chunk_count; i++) {
        proto_put_u64_le(&sequence_start_timestamps[(size_t)i * sizeof(uint64_t)],
                         fields->sequence_start_timestamps_ms[i]);
    }
    return tlv_append_bytes(payload, payload_cap, offset,
                            TLV_SEQUENCE_START_TIMESTAMPS_MS,
                            sequence_start_timestamps,
                            (uint8_t)sequence_timestamp_bytes);
}

static int append_diagnostics(uint8_t *payload,
                              size_t payload_cap,
                              size_t *offset,
                              const struct range_report_diagnostics *diagnostics,
                              bool burst_id_present)
{
    int ret;

    if (diagnostics == NULL) {
        return PROTO_OK;
    }
    if ((diagnostics->clicker_diag_len > 0u && diagnostics->clicker_diag == NULL) ||
        (diagnostics->anchor_diag_len > 0u && diagnostics->anchor_diag == NULL)) {
        return PROTO_ERR_MALFORMED;
    }
    if (diagnostics->clicker_diag_len > RANGE_REPORT_MAX_DIAGNOSTIC_BYTES_SINGLE_PACKET ||
        diagnostics->anchor_diag_len > RANGE_REPORT_MAX_DIAGNOSTIC_BYTES_SINGLE_PACKET) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_DIAG_STATUS_FLAGS,
                         diagnostics->status_flags);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!burst_id_present) {
        ret = tlv_append_u32(payload, payload_cap, offset,
                             TLV_BURST_ID,
                             diagnostics->burst_id);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_EXCHANGE_STRIDE_US,
                         diagnostics->exchange_stride_us);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_BURST_DURATION_MS,
                         diagnostics->burst_duration_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (diagnostics->click_latency_present) {
        ret = tlv_append_u32(payload, payload_cap, offset,
                             TLV_CLICK_LATENCY_MS,
                             diagnostics->click_latency_ms);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_UWB_AWAKE_TIME_US,
                         diagnostics->uwb_awake_time_us);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_DIAG_BYTES_CAPTURED,
                         diagnostics->diag_bytes_captured);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_DIAG_BYTES_TRANSMITTED,
                         diagnostics->diag_bytes_transmitted);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_DIAG_BYTES_TRUNCATED,
                         diagnostics->diag_bytes_truncated);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_DIAG_FRAMES_DROPPED,
                         diagnostics->diag_frames_dropped);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_REPORT_FRAGMENT_COUNT,
                         diagnostics->report_fragment_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (diagnostics->channel9_report_latency_present) {
        ret = tlv_append_u32(payload, payload_cap, offset,
                             TLV_CHANNEL9_REPORT_LATENCY_MS,
                             diagnostics->channel9_report_latency_ms);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (diagnostics->gateway_ack_latency_present) {
        ret = tlv_append_u32(payload, payload_cap, offset,
                             TLV_GATEWAY_ACK_LATENCY_MS,
                             diagnostics->gateway_ack_latency_ms);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    ret = tlv_append_u8(payload, payload_cap, offset,
                        TLV_PHY_CONFIG_ID,
                        diagnostics->phy_config_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (diagnostics->clock_offset_present) {
        ret = tlv_append_u16(payload, payload_cap, offset,
                             TLV_UWB_CLOCK_OFFSET_RAW,
                             (uint16_t)diagnostics->clock_offset_raw);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (diagnostics->clicker_clock_offset_present) {
        ret = tlv_append_u16(payload, payload_cap, offset,
                             TLV_CLICKER_CLOCK_OFFSET_RAW,
                             (uint16_t)diagnostics->clicker_clock_offset_raw);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (diagnostics->carrier_integrator_present) {
        ret = tlv_append_i32(payload, payload_cap, offset,
                             TLV_UWB_CARRIER_INTEGRATOR,
                             diagnostics->carrier_integrator);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (diagnostics->clicker_diag_len > 0u) {
        ret = tlv_append_bytes(payload, payload_cap, offset,
                               TLV_CLICKER_DIAG_BYTES,
                               diagnostics->clicker_diag,
                               diagnostics->clicker_diag_len);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (diagnostics->anchor_diag_len > 0u) {
        ret = tlv_append_bytes(payload, payload_cap, offset,
                               TLV_ANCHOR_DIAG_BYTES,
                               diagnostics->anchor_diag,
                               diagnostics->anchor_diag_len);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    return PROTO_OK;
}

int report_append_range_tlvs(uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset,
                                  const struct range_report_fields *fields)
{
    int ret;

    if (fields == NULL) {
        return PROTO_ERR_ARG;
    }
    if (fields->clicker_id == 0u ||
        fields->anchor_id == 0u ||
        fields->clicker_id == fields->anchor_id ||
        fields->quality > 100u ||
        !range_status_valid(fields->range_status)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_CLICKER_ID, fields->clicker_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_ANCHOR_ID, fields->anchor_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_EVENT_SEQ, fields->event_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (fields->detection_attempt_present) {
        if (fields->attempt_index == 0u ||
            fields->detection_source != DETECTION_SOURCE_UWB_WAKE_CLAIM) {
            return PROTO_ERR_MALFORMED;
        }
        ret = tlv_append_u8(payload, payload_cap, offset,
                            TLV_ATTEMPT_INDEX, fields->attempt_index);
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = tlv_append_u8(payload, payload_cap, offset,
                            TLV_DETECTION_SOURCE, fields->detection_source);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_TIMESTAMP_MS, fields->timestamp_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_i32(payload, payload_cap, offset, TLV_DISTANCE_MM, fields->distance_mm);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = append_distance_samples(payload, payload_cap, offset, fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (fields->burst_id_present) {
        if (fields->burst_id == 0u) {
            return PROTO_ERR_MALFORMED;
        }
        ret = tlv_append_u32(payload, payload_cap, offset,
                             TLV_BURST_ID, fields->burst_id);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    ret = append_diagnostics(payload, payload_cap, offset, fields->diagnostics,
                             fields->burst_id_present);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, fields->quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!fields->omit_rsl) {
        ret = tlv_append_i8(payload, payload_cap, offset, TLV_UWB_RSL_DBM, fields->rsl_dbm);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (!fields->omit_cir && fields->cir_sample != NULL) {
        ret = tlv_append_bytes(payload, payload_cap, offset,
                               TLV_UWB_CIR_SAMPLE,
                               fields->cir_sample,
                               UWB_CIR_SAMPLE_LEN);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    return tlv_append_u8(payload, payload_cap, offset, TLV_RANGE_STATUS, (uint8_t)fields->range_status);
}

int report_append_cir_fragment_tlvs(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const struct range_report_cir_fragment *fragment)
{
    uint16_t chunk_offset = 0u;
    int ret;

    if (payload == NULL || offset == NULL || fragment == NULL ||
        fragment->clicker_id == 0u || fragment->anchor_id == 0u ||
        fragment->clicker_id == fragment->anchor_id ||
        fragment->event_seq == 0u || fragment->fragment_count == 0u ||
        fragment->fragment_index >= fragment->fragment_count ||
        fragment->chunk == NULL || fragment->chunk_len == 0u ||
        fragment->chunk_len > RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES ||
        fragment->total_bytes == 0u ||
        fragment->byte_offset >= fragment->total_bytes ||
        fragment->chunk_len > fragment->total_bytes - fragment->byte_offset) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_append_u64(payload, payload_cap, offset,
                         TLV_CLICKER_ID, fragment->clicker_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset,
                         TLV_ANCHOR_ID, fragment->anchor_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset,
                         TLV_EVENT_SEQ, fragment->event_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset,
                         TLV_TIMESTAMP_MS, fragment->timestamp_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_DIAG_FRAGMENT_INDEX, fragment->fragment_index);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_DIAG_FRAGMENT_COUNT, fragment->fragment_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_UWB_CIR_BYTE_OFFSET, fragment->byte_offset);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_UWB_CIR_TOTAL_BYTES, fragment->total_bytes);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_UWB_CIR_FIRST_PATH_INDEX, fragment->first_path_index);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset,
                         TLV_UWB_CIR_START_INDEX, fragment->start_index);
    if (ret != PROTO_OK) {
        return ret;
    }
    while (chunk_offset < fragment->chunk_len) {
        uint16_t remaining = fragment->chunk_len - chunk_offset;
        uint8_t tlv_len = (uint8_t)(remaining > UINT8_MAX ? UINT8_MAX : remaining);

        ret = tlv_append_bytes(payload, payload_cap, offset,
                               TLV_UWB_CIR_FULL_CHUNK,
                               &fragment->chunk[chunk_offset],
                               tlv_len);
        if (ret != PROTO_OK) {
            return ret;
        }
        chunk_offset += tlv_len;
    }
    return PROTO_OK;
}

int report_append_self_test_tlvs(uint8_t *payload,
                                      size_t payload_cap,
                                      size_t *offset,
                                      const struct self_test_report_fields *fields)
{
    int ret;

    if (fields == NULL) {
        return PROTO_ERR_ARG;
    }
    if (fields->clicker_id == 0u || fields->failure_code > 6u) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_append_u64(payload, payload_cap, offset, TLV_CLICKER_ID, fields->clicker_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_EVENT_SEQ, fields->event_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_ERROR_CODE, fields->failure_code);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u16(payload, payload_cap, offset, TLV_BATTERY_MV, fields->battery_mv);
}

int report_append_anchor_heartbeat_tlvs(uint8_t *payload,
                                        size_t payload_cap,
                                        size_t *offset,
                                        const struct anchor_heartbeat_fields *fields)
{
    int ret;

    if (fields == NULL) {
        return PROTO_ERR_ARG;
    }
    if (fields->device_role == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_append_u8(payload, payload_cap, offset, TLV_DEVICE_ROLE, fields->device_role);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_BATTERY_MV, fields->battery_mv);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_STATUS_BITS, fields->status_bits);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, offset, TLV_UPTIME_MS, fields->uptime_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u64(payload, payload_cap, offset,
                          TLV_TIMESTAMP_MS,
                          fields->timestamp_ms);
}

int report_init_range_packet(struct proto_packet *packet,
                                  uint64_t anchor_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t seq,
                                  uint8_t report_flags,
                                  uint16_t payload_len)
{
    uint8_t mode_flags = report_flags & (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC);

    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(anchor_id, gateway_id) ||
        session_id == 0u ||
        seq == 0u ||
        mode_flags == 0u ||
        mode_flags == (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC)) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_CLICK_REPORT;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED | mode_flags;
    packet->src_id = anchor_id;
    packet->dst_id = gateway_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = REPORT_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

int report_init_anchor_heartbeat_packet(struct proto_packet *packet,
                                        uint64_t anchor_id,
                                        uint64_t gateway_id,
                                        uint32_t session_id,
                                        uint16_t seq,
                                        uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(anchor_id, gateway_id) || session_id == 0u || seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_ANCHOR_HEARTBEAT;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED;
    packet->src_id = anchor_id;
    packet->dst_id = gateway_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = REPORT_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

int report_init_click_packet(struct proto_packet *packet,
                                  uint64_t anchor_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t seq,
                                  uint8_t payload_len)
{
    return report_init_range_packet(packet,
                                    anchor_id,
                                    gateway_id,
                                    session_id,
                                    seq,
                                    FLAG_COUNT_AS_CLICK,
                                    payload_len);
}

int report_init_self_test_packet(struct proto_packet *packet,
                                      uint64_t clicker_id,
                                      uint64_t gateway_id,
                                      uint32_t session_id,
                                      uint16_t seq,
                                      uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(clicker_id, gateway_id) || session_id == 0u || seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_SELF_TEST_REPORT;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    packet->src_id = clicker_id;
    packet->dst_id = gateway_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = REPORT_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

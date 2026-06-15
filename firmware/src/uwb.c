#include "uwb.h"

#include <string.h>

#define UWB_MESH_BROADCAST_ID 0u

static int validate_status(enum range_status status)
{
    if (status < RANGE_OK || status > RANGE_TIMING_INVALID) {
        return PROTO_ERR_MALFORMED;
    }
    if (status == RANGE_STS_QUALITY_FAIL) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static bool flags_valid(uint8_t flags)
{
    return flags == FLAG_DIAGNOSTIC ||
           flags == FLAG_COUNT_AS_CLICK;
}

static uint16_t short_addr_from_id(uint64_t device_id)
{
    uint16_t short_addr = (uint16_t)(device_id & 0xffffu);

    return short_addr == 0u ? 1u : short_addr;
}

static int validate_sync_prefix(const uint8_t *data,
                                size_t len,
                                size_t expected_len,
                                uint8_t expected_type)
{
    if (data == NULL) {
        return PROTO_ERR_ARG;
    }
    if (len != expected_len) {
        return PROTO_ERR_BAD_LENGTH;
    }
    if (data[0] != UWB_MARKER) {
        return PROTO_ERR_BAD_MAGIC;
    }
    if (data[1] != UWB_VERSION) {
        return PROTO_ERR_BAD_VERSION;
    }
    if (data[2] != expected_type) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static void put_sync_prefix(uint8_t *out, uint8_t type)
{
    out[0] = UWB_MARKER;
    out[1] = UWB_VERSION;
    out[2] = type;
}

static void append_crc(uint8_t *out, size_t crc_offset)
{
    proto_put_u16_le(&out[crc_offset], proto_crc16_ccitt_false(out, crc_offset));
}

static int verify_crc(const uint8_t *data, size_t len)
{
    uint16_t expected_crc;
    uint16_t actual_crc;

    if (len < UWB_FRAME_CRC_LEN) {
        return PROTO_ERR_BAD_LENGTH;
    }

    expected_crc = proto_get_u16_le(&data[len - UWB_FRAME_CRC_LEN]);
    actual_crc = proto_crc16_ccitt_false(data, len - UWB_FRAME_CRC_LEN);
    return expected_crc == actual_crc ? PROTO_OK : PROTO_ERR_BAD_CRC;
}

static int encode_header(const struct uwb_range_header *header,
                         uint8_t expected_type,
                         uint8_t *out)
{
    int ret;

    ret = uwb_header_validate(header, expected_type);
    if (ret != PROTO_OK) {
        return ret;
    }

    out[0] = UWB_MARKER;
    out[1] = UWB_VERSION;
    out[2] = header->type;
    out[3] = header->seq;
    proto_put_u32_le(&out[4], header->network_id);
    proto_put_u32_le(&out[8], header->session_id);
    proto_put_u64_le(&out[12], header->session_nonce);
    proto_put_u16_le(&out[20], header->initiator_short_addr);
    proto_put_u16_le(&out[22], header->responder_short_addr);
    out[24] = header->flags;
    proto_put_u64_le(&out[25], header->initiator_id);
    proto_put_u64_le(&out[33], header->responder_id);
    out[41] = header->round_index;
    return PROTO_OK;
}

static int decode_header(const uint8_t *data,
                         size_t len,
                         size_t expected_len,
                         uint8_t expected_type,
                         struct uwb_range_header *header)
{
    if (data == NULL || header == NULL) {
        return PROTO_ERR_ARG;
    }
    if (len != expected_len) {
        return PROTO_ERR_BAD_LENGTH;
    }
    if (data[0] != UWB_MARKER) {
        return PROTO_ERR_BAD_MAGIC;
    }
    if (data[1] != UWB_VERSION) {
        return PROTO_ERR_BAD_VERSION;
    }

    header->type = data[2];
    header->seq = data[3];
    header->network_id = proto_get_u32_le(&data[4]);
    header->session_id = proto_get_u32_le(&data[8]);
    header->session_nonce = proto_get_u64_le(&data[12]);
    header->initiator_short_addr = proto_get_u16_le(&data[20]);
    header->responder_short_addr = proto_get_u16_le(&data[22]);
    header->flags = data[24];
    header->initiator_id = proto_get_u64_le(&data[25]);
    header->responder_id = proto_get_u64_le(&data[33]);
    header->round_index = data[41];
    return uwb_header_validate(header, expected_type);
}

bool uwb_frame_type_valid(uint8_t type)
{
    return type == MSG_UWB_WAKE_CLAIM ||
           type == MSG_UWB_DISCOVER ||
           type == MSG_UWB_DISCOVERY_REPLY ||
           type == MSG_UWB_RANGE_SCHEDULE ||
           type == MSG_UWB_MESH ||
           type == MSG_UWB_RANGE_RELEASE ||
           type == MSG_UWB_POLL ||
           type == MSG_UWB_RESP ||
           type == MSG_UWB_FINAL ||
           type == MSG_UWB_REPORT ||
           type == MSG_UWB_CLICKER_DIAG ||
           type == MSG_UWB_SURVEY_DISCOVERY_PROBE;
}

int uwb_header_validate(const struct uwb_range_header *header, uint8_t expected_type)
{
    if (header == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!uwb_frame_type_valid(header->type) || header->type != expected_type) {
        return PROTO_ERR_MALFORMED;
    }
    if (header->network_id == 0u ||
        header->seq == 0u ||
        header->session_id == 0u ||
        header->session_nonce == 0u ||
        header->initiator_short_addr == 0u ||
        header->responder_short_addr == 0u ||
        header->initiator_short_addr == header->responder_short_addr ||
        header->initiator_id == 0u ||
        header->responder_id == 0u ||
        header->initiator_id == header->responder_id ||
        header->initiator_short_addr != short_addr_from_id(header->initiator_id) ||
        header->responder_short_addr != short_addr_from_id(header->responder_id)) {
        return PROTO_ERR_MALFORMED;
    }
    if (!flags_valid(header->flags)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int uwb_encode_poll(const struct uwb_range_header *header,
                         uint8_t *out,
                         size_t out_cap,
                         size_t *written)
{
    int ret;

    if (out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < UWB_POLL_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = encode_header(header, MSG_UWB_POLL, out);
    if (ret != PROTO_OK) {
        return ret;
    }

    *written = UWB_POLL_LEN;
    return PROTO_OK;
}

int uwb_decode_poll(const uint8_t *data,
                         size_t len,
                         struct uwb_range_header *header)
{
    return decode_header(data, len, UWB_POLL_LEN, MSG_UWB_POLL, header);
}

int uwb_encode_response(const struct uwb_response_frame *frame,
                             uint8_t *out,
                             size_t out_cap,
                             size_t *written)
{
    int ret;

    if (frame == NULL || out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < UWB_RESP_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = encode_header(&frame->header, MSG_UWB_RESP, out);
    if (ret != PROTO_OK) {
        return ret;
    }

    proto_put_u32_le(&out[UWB_HEADER_LEN], frame->poll_rx_ts_32);
    proto_put_u32_le(&out[UWB_HEADER_LEN + 4u], frame->resp_tx_ts_32);
    *written = UWB_RESP_LEN;
    return PROTO_OK;
}

int uwb_decode_response(const uint8_t *data,
                             size_t len,
                             struct uwb_response_frame *frame)
{
    int ret;

    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = decode_header(data, len, UWB_RESP_LEN, MSG_UWB_RESP, &frame->header);
    if (ret != PROTO_OK) {
        return ret;
    }

    frame->poll_rx_ts_32 = proto_get_u32_le(&data[UWB_HEADER_LEN]);
    frame->resp_tx_ts_32 = proto_get_u32_le(&data[UWB_HEADER_LEN + 4u]);
    return PROTO_OK;
}

int uwb_encode_final(const struct uwb_final_frame *frame,
                          uint8_t *out,
                          size_t out_cap,
                          size_t *written)
{
    int ret;

    if (frame == NULL || out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < UWB_FINAL_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = encode_header(&frame->header, MSG_UWB_FINAL, out);
    if (ret != PROTO_OK) {
        return ret;
    }

    proto_put_u32_le(&out[UWB_HEADER_LEN], frame->poll_tx_ts_32);
    proto_put_u32_le(&out[UWB_HEADER_LEN + 4u], frame->resp_rx_ts_32);
    proto_put_u32_le(&out[UWB_HEADER_LEN + 8u], frame->final_tx_ts_32);
    *written = UWB_FINAL_LEN;
    return PROTO_OK;
}

int uwb_decode_final(const uint8_t *data,
                          size_t len,
                          struct uwb_final_frame *frame)
{
    int ret;

    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = decode_header(data, len, UWB_FINAL_LEN, MSG_UWB_FINAL, &frame->header);
    if (ret != PROTO_OK) {
        return ret;
    }

    frame->poll_tx_ts_32 = proto_get_u32_le(&data[UWB_HEADER_LEN]);
    frame->resp_rx_ts_32 = proto_get_u32_le(&data[UWB_HEADER_LEN + 4u]);
    frame->final_tx_ts_32 = proto_get_u32_le(&data[UWB_HEADER_LEN + 8u]);
    return PROTO_OK;
}

int uwb_encode_report(const struct uwb_report_frame *frame,
                           uint8_t *out,
                           size_t out_cap,
                           size_t *written)
{
    int ret;

    if (frame == NULL || out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < UWB_REPORT_LEN) {
        return PROTO_ERR_NO_SPACE;
    }
    if (frame->quality > 100u) {
        return PROTO_ERR_MALFORMED;
    }

    ret = validate_status(frame->status);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = encode_header(&frame->header, MSG_UWB_REPORT, out);
    if (ret != PROTO_OK) {
        return ret;
    }

    proto_put_u32_le(&out[UWB_HEADER_LEN], (uint32_t)frame->distance_mm);
    out[UWB_HEADER_LEN + 4u] = frame->quality;
    proto_put_u16_le(&out[UWB_HEADER_LEN + 5u], (uint16_t)frame->status);
    out[UWB_HEADER_LEN + 7u] = (uint8_t)frame->rsl_dbm;
    *written = UWB_REPORT_LEN;
    return PROTO_OK;
}

int uwb_decode_report(const uint8_t *data,
                           size_t len,
                           struct uwb_report_frame *frame)
{
    int ret;

    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = decode_header(data, len, UWB_REPORT_LEN, MSG_UWB_REPORT, &frame->header);
    if (ret != PROTO_OK) {
        return ret;
    }

    frame->distance_mm = (int32_t)proto_get_u32_le(&data[UWB_HEADER_LEN]);
    frame->quality = data[UWB_HEADER_LEN + 4u];
    frame->status = (enum range_status)proto_get_u16_le(&data[UWB_HEADER_LEN + 5u]);
    frame->rsl_dbm = (int8_t)data[UWB_HEADER_LEN + 7u];

    if (frame->quality > 100u) {
        return PROTO_ERR_MALFORMED;
    }
    return validate_status(frame->status);
}

int uwb_encode_clicker_diag(const struct uwb_clicker_diag_frame *frame,
                            uint8_t *out,
                            size_t out_cap,
                            size_t *written)
{
    size_t frame_len;
    int ret;

    if (frame == NULL || out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (frame->diag_len > UWB_CLICKER_DIAG_MAX_BYTES ||
        frame->resp_quality > 100u ||
        frame->status_flags == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    frame_len = UWB_CLICKER_DIAG_FIXED_LEN + frame->diag_len;
    if (out_cap < frame_len) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = encode_header(&frame->header, MSG_UWB_CLICKER_DIAG, out);
    if (ret != PROTO_OK) {
        return ret;
    }

    proto_put_u32_le(&out[UWB_HEADER_LEN], frame->final_tx_ts_32);
    proto_put_u32_le(&out[UWB_HEADER_LEN + 4u], frame->status_flags);
    proto_put_u32_le(&out[UWB_HEADER_LEN + 8u], frame->status_detect_latency_us);
    out[UWB_HEADER_LEN + 12u] = frame->resp_quality;
    out[UWB_HEADER_LEN + 13u] = (uint8_t)frame->resp_rsl_dbm;
    out[UWB_HEADER_LEN + 14u] = frame->diag_len;
    if (frame->diag_len > 0u) {
        memcpy(&out[UWB_CLICKER_DIAG_FIXED_LEN], frame->diag_bytes, frame->diag_len);
    }
    *written = frame_len;
    return PROTO_OK;
}

int uwb_decode_clicker_diag(const uint8_t *data,
                            size_t len,
                            struct uwb_clicker_diag_frame *frame)
{
    uint8_t diag_len;
    int ret;

    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }
    if (data == NULL) {
        return PROTO_ERR_ARG;
    }
    if (len < UWB_CLICKER_DIAG_FIXED_LEN ||
        len > UWB_CLICKER_DIAG_MAX_LEN) {
        return PROTO_ERR_BAD_LENGTH;
    }

    ret = decode_header(data,
                        UWB_HEADER_LEN,
                        UWB_HEADER_LEN,
                        MSG_UWB_CLICKER_DIAG,
                        &frame->header);
    if (ret != PROTO_OK) {
        return ret;
    }
    frame->final_tx_ts_32 = proto_get_u32_le(&data[UWB_HEADER_LEN]);
    frame->status_flags = proto_get_u32_le(&data[UWB_HEADER_LEN + 4u]);
    frame->status_detect_latency_us = proto_get_u32_le(&data[UWB_HEADER_LEN + 8u]);
    frame->resp_quality = data[UWB_HEADER_LEN + 12u];
    frame->resp_rsl_dbm = (int8_t)data[UWB_HEADER_LEN + 13u];
    diag_len = data[UWB_HEADER_LEN + 14u];
    if (diag_len > UWB_CLICKER_DIAG_MAX_BYTES ||
        len != UWB_CLICKER_DIAG_FIXED_LEN + diag_len ||
        frame->resp_quality > 100u ||
        frame->status_flags == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    frame->diag_len = diag_len;
    if (diag_len > 0u) {
        memcpy(frame->diag_bytes, &data[UWB_CLICKER_DIAG_FIXED_LEN], diag_len);
    }
    return PROTO_OK;
}

static int validate_wake_claim(const struct uwb_wake_claim_frame *frame)
{
    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }
    if (frame->network_id == 0u ||
        frame->clicker_id == 0u ||
        frame->click_event_id == 0u ||
        frame->attempt_index == 0u ||
        frame->priority_id == 0u ||
        frame->wake_channel != UWB_CHANNEL_WAKE_CONTACT ||
        frame->ranging_channel != UWB_CHANNEL_WAKE_CONTACT ||
        frame->wake_train_ends_in_ms == 0u ||
        frame->discovery_starts_in_ms == 0u ||
        frame->wake_train_ends_in_ms > UWB_WAKE_CLAIM_MAX_WAKE_TRAIN_MS ||
        frame->discovery_starts_in_ms > UWB_WAKE_CLAIM_MAX_DISCOVERY_START_MS ||
        frame->claimed_duration_ms > UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS ||
        frame->discovery_starts_in_ms < frame->wake_train_ends_in_ms ||
        frame->claimed_duration_ms < frame->wake_train_ends_in_ms ||
        frame->claimed_duration_ms < frame->discovery_starts_in_ms ||
        frame->min_anchor_count == 0u ||
        frame->max_anchor_count < frame->min_anchor_count ||
        frame->max_anchor_count > UWB_RANGE_SCHEDULE_MAX_ANCHORS ||
        frame->nonce == 0u ||
        !flags_valid(frame->flags)) {
        return PROTO_ERR_MALFORMED;
    }
    if ((frame->flags & FLAG_COUNT_AS_CLICK) != 0u &&
        frame->min_anchor_count != UWB_NORMAL_CLICK_MIN_ANCHORS) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int uwb_validate_wake_claim(const struct uwb_wake_claim_frame *frame)
{
    return validate_wake_claim(frame);
}

static int validate_discover(const struct uwb_discover_frame *frame)
{
    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }
    if (frame->network_id == 0u ||
        frame->clicker_id == 0u ||
        frame->click_event_id == 0u ||
        frame->attempt_index == 0u ||
        frame->nonce == 0u ||
        frame->discovery_slot_count == 0u ||
        frame->discovery_slot_count > UWB_DISCOVERY_SLOT_COUNT ||
        !flags_valid(frame->flags)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static bool discovery_reply_status_valid(uint8_t status)
{
    return status == UWB_DISCOVERY_REPLY_PRESENT ||
           status == UWB_DISCOVERY_REPLY_BUSY ||
           status == UWB_DISCOVERY_REPLY_COLLISION;
}

static int validate_discovery_reply(const struct uwb_discovery_reply_frame *frame)
{
    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }
    if (frame->network_id == 0u ||
        frame->anchor_id == 0u ||
        frame->selected_clicker_id == 0u ||
        frame->click_event_id == 0u ||
        frame->attempt_index == 0u ||
        frame->nonce == 0u ||
        frame->anchor_slot >= UWB_DISCOVERY_SLOT_COUNT ||
        !discovery_reply_status_valid(frame->status) ||
        frame->rx_quality > 100u ||
        !flags_valid(frame->flags)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static int validate_survey_discovery_probe(const struct uwb_survey_discovery_probe_frame *frame)
{
    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }
    if (frame->network_id == 0u ||
        frame->survey_id == 0u ||
        frame->anchor_id == 0u ||
        frame->slot_count == 0u ||
        frame->slot_count > UWB_DISCOVERY_SLOT_COUNT ||
        frame->anchor_slot >= frame->slot_count ||
        !flags_valid(frame->flags)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static bool schedule_entry_duplicate(const struct uwb_range_schedule_frame *frame,
                                     uint8_t index)
{
    for (uint8_t i = 0u; i < index; i++) {
        if (frame->entries[i].anchor_id == frame->entries[index].anchor_id) {
            return true;
        }
    }
    return false;
}

static int validate_range_schedule(const struct uwb_range_schedule_frame *frame)
{
    size_t total_samples = 0u;

    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }
    if (frame->network_id == 0u ||
        frame->clicker_id == 0u ||
        frame->click_event_id == 0u ||
        frame->attempt_index == 0u ||
        frame->nonce == 0u ||
        frame->selected_count == 0u ||
        frame->selected_count > UWB_RANGE_SCHEDULE_MAX_ANCHORS ||
        frame->ranging_channel != UWB_CHANNEL_WAKE_CONTACT ||
        frame->reply_delay_us != UWB_DS_TWR_REPLY_DELAY_US ||
        frame->first_poll_delay_ms == 0u ||
        frame->poll_spacing_ms < UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS ||
        frame->burst_window_ms < UWB_RANGE_SCHEDULE_MIN_BURST_WINDOW_MS ||
        frame->exchange_stride_us < UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US ||
        frame->max_exchanges == 0u ||
        frame->min_successful_unique_anchors == 0u ||
        frame->min_successful_unique_anchors > frame->selected_count ||
        frame->sts_mode != UWB_RANGE_SCHEDULE_STS_DISABLED ||
        frame->diagnostics_required != UWB_RANGE_SCHEDULE_DIAGNOSTICS_REQUIRED ||
        frame->samples_per_anchor == 0u ||
        frame->samples_per_anchor > UWB_RANGING_REQUESTS_MAX_PER_ANCHOR ||
        !flags_valid(frame->flags)) {
        return PROTO_ERR_MALFORMED;
    }
    if ((uint32_t)frame->max_exchanges * frame->exchange_stride_us >
        (uint32_t)frame->burst_window_ms * 1000u) {
        return PROTO_ERR_MALFORMED;
    }
    if ((frame->flags & FLAG_COUNT_AS_CLICK) != 0u &&
        (frame->selected_count < UWB_NORMAL_CLICK_MIN_ANCHORS ||
         frame->min_successful_unique_anchors < UWB_NORMAL_CLICK_MIN_ANCHORS)) {
        return PROTO_ERR_MALFORMED;
    }

    for (uint8_t i = 0u; i < frame->selected_count; i++) {
        if (frame->entries[i].anchor_id == 0u ||
            frame->entries[i].seq == 0u ||
            frame->entries[i].sample_count != frame->samples_per_anchor ||
            frame->entries[i].seq > UINT8_MAX - frame->entries[i].sample_count + 1u ||
            schedule_entry_duplicate(frame, i)) {
            return PROTO_ERR_MALFORMED;
        }
        total_samples += frame->entries[i].sample_count;
    }
    if (total_samples == 0u || frame->max_exchanges < frame->selected_count) {
        return PROTO_ERR_MALFORMED;
    }

    return PROTO_OK;
}

static int validate_range_release(const struct uwb_range_release_frame *frame)
{
    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }
    if (frame->network_id == 0u ||
        frame->clicker_id == 0u ||
        frame->click_event_id == 0u ||
        frame->attempt_index == 0u ||
        frame->nonce == 0u ||
        frame->discovered_anchor_count == 0u ||
        frame->min_anchor_count == 0u ||
        frame->min_anchor_count > UWB_RANGE_SCHEDULE_MAX_ANCHORS ||
        frame->discovered_anchor_count >= frame->min_anchor_count ||
        frame->reason != UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS ||
        !flags_valid(frame->flags)) {
        return PROTO_ERR_MALFORMED;
    }
    if ((frame->flags & FLAG_COUNT_AS_CLICK) != 0u &&
        frame->min_anchor_count != UWB_NORMAL_CLICK_MIN_ANCHORS) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int uwb_validate_range_schedule(const struct uwb_range_schedule_frame *frame)
{
    return validate_range_schedule(frame);
}

int uwb_validate_range_release(const struct uwb_range_release_frame *frame)
{
    return validate_range_release(frame);
}

int uwb_encode_wake_claim(const struct uwb_wake_claim_frame *frame,
                          uint8_t *out,
                          size_t out_cap,
                          size_t *written)
{
    int ret;

    if (out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < UWB_WAKE_CLAIM_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = validate_wake_claim(frame);
    if (ret != PROTO_OK) {
        return ret;
    }

    put_sync_prefix(out, MSG_UWB_WAKE_CLAIM);
    proto_put_u32_le(&out[3], frame->network_id);
    proto_put_u64_le(&out[7], frame->clicker_id);
    proto_put_u32_le(&out[15], frame->click_event_id);
    out[19] = frame->attempt_index;
    proto_put_u64_le(&out[20], frame->priority_id);
    out[28] = frame->wake_channel;
    out[29] = frame->ranging_channel;
    proto_put_u16_le(&out[30], frame->wake_train_ends_in_ms);
    proto_put_u16_le(&out[32], frame->discovery_starts_in_ms);
    proto_put_u16_le(&out[34], frame->claimed_duration_ms);
    out[36] = frame->min_anchor_count;
    out[37] = frame->max_anchor_count;
    proto_put_u64_le(&out[38], frame->nonce);
    out[46] = frame->flags;
    append_crc(out, UWB_WAKE_CLAIM_LEN - UWB_FRAME_CRC_LEN);
    *written = UWB_WAKE_CLAIM_LEN;
    return PROTO_OK;
}

int uwb_decode_wake_claim(const uint8_t *data,
                          size_t len,
                          struct uwb_wake_claim_frame *frame)
{
    int ret;

    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_sync_prefix(data, len, UWB_WAKE_CLAIM_LEN, MSG_UWB_WAKE_CLAIM);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = verify_crc(data, len);
    if (ret != PROTO_OK) {
        return ret;
    }

    frame->network_id = proto_get_u32_le(&data[3]);
    frame->clicker_id = proto_get_u64_le(&data[7]);
    frame->click_event_id = proto_get_u32_le(&data[15]);
    frame->attempt_index = data[19];
    frame->priority_id = proto_get_u64_le(&data[20]);
    frame->wake_channel = data[28];
    frame->ranging_channel = data[29];
    frame->wake_train_ends_in_ms = proto_get_u16_le(&data[30]);
    frame->discovery_starts_in_ms = proto_get_u16_le(&data[32]);
    frame->claimed_duration_ms = proto_get_u16_le(&data[34]);
    frame->min_anchor_count = data[36];
    frame->max_anchor_count = data[37];
    frame->nonce = proto_get_u64_le(&data[38]);
    frame->flags = data[46];
    return validate_wake_claim(frame);
}

int uwb_encode_discover(const struct uwb_discover_frame *frame,
                        uint8_t *out,
                        size_t out_cap,
                        size_t *written)
{
    int ret;

    if (out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < UWB_DISCOVER_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = validate_discover(frame);
    if (ret != PROTO_OK) {
        return ret;
    }

    put_sync_prefix(out, MSG_UWB_DISCOVER);
    proto_put_u32_le(&out[3], frame->network_id);
    proto_put_u64_le(&out[7], frame->clicker_id);
    proto_put_u32_le(&out[15], frame->click_event_id);
    out[19] = frame->attempt_index;
    proto_put_u64_le(&out[20], frame->nonce);
    out[28] = frame->discovery_slot_count;
    out[29] = frame->flags;
    append_crc(out, UWB_DISCOVER_LEN - UWB_FRAME_CRC_LEN);
    *written = UWB_DISCOVER_LEN;
    return PROTO_OK;
}

int uwb_decode_discover(const uint8_t *data,
                        size_t len,
                        struct uwb_discover_frame *frame)
{
    int ret;

    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_sync_prefix(data, len, UWB_DISCOVER_LEN, MSG_UWB_DISCOVER);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = verify_crc(data, len);
    if (ret != PROTO_OK) {
        return ret;
    }

    frame->network_id = proto_get_u32_le(&data[3]);
    frame->clicker_id = proto_get_u64_le(&data[7]);
    frame->click_event_id = proto_get_u32_le(&data[15]);
    frame->attempt_index = data[19];
    frame->nonce = proto_get_u64_le(&data[20]);
    frame->discovery_slot_count = data[28];
    frame->flags = data[29];
    return validate_discover(frame);
}

int uwb_encode_discovery_reply(const struct uwb_discovery_reply_frame *frame,
                               uint8_t *out,
                               size_t out_cap,
                               size_t *written)
{
    int ret;

    if (out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < UWB_DISCOVERY_REPLY_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = validate_discovery_reply(frame);
    if (ret != PROTO_OK) {
        return ret;
    }

    put_sync_prefix(out, MSG_UWB_DISCOVERY_REPLY);
    proto_put_u32_le(&out[3], frame->network_id);
    proto_put_u64_le(&out[7], frame->anchor_id);
    proto_put_u64_le(&out[15], frame->selected_clicker_id);
    proto_put_u32_le(&out[23], frame->click_event_id);
    out[27] = frame->attempt_index;
    proto_put_u64_le(&out[28], frame->nonce);
    out[36] = frame->anchor_slot;
    out[37] = frame->status;
    out[38] = frame->rx_quality;
    proto_put_u16_le(&out[39], frame->battery_mv);
    out[41] = frame->flags;
    append_crc(out, UWB_DISCOVERY_REPLY_LEN - UWB_FRAME_CRC_LEN);
    *written = UWB_DISCOVERY_REPLY_LEN;
    return PROTO_OK;
}

int uwb_decode_discovery_reply(const uint8_t *data,
                               size_t len,
                               struct uwb_discovery_reply_frame *frame)
{
    int ret;

    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_sync_prefix(data, len, UWB_DISCOVERY_REPLY_LEN,
                               MSG_UWB_DISCOVERY_REPLY);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = verify_crc(data, len);
    if (ret != PROTO_OK) {
        return ret;
    }

    frame->network_id = proto_get_u32_le(&data[3]);
    frame->anchor_id = proto_get_u64_le(&data[7]);
    frame->selected_clicker_id = proto_get_u64_le(&data[15]);
    frame->click_event_id = proto_get_u32_le(&data[23]);
    frame->attempt_index = data[27];
    frame->nonce = proto_get_u64_le(&data[28]);
    frame->anchor_slot = data[36];
    frame->status = data[37];
    frame->rx_quality = data[38];
    frame->battery_mv = proto_get_u16_le(&data[39]);
    frame->flags = data[41];
    return validate_discovery_reply(frame);
}

int uwb_encode_survey_discovery_probe(const struct uwb_survey_discovery_probe_frame *frame,
                                      uint8_t *out,
                                      size_t out_cap,
                                      size_t *written)
{
    int ret;

    if (out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < UWB_SURVEY_DISCOVERY_PROBE_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = validate_survey_discovery_probe(frame);
    if (ret != PROTO_OK) {
        return ret;
    }

    put_sync_prefix(out, MSG_UWB_SURVEY_DISCOVERY_PROBE);
    proto_put_u32_le(&out[3], frame->network_id);
    proto_put_u32_le(&out[7], frame->survey_id);
    proto_put_u64_le(&out[11], frame->anchor_id);
    out[19] = frame->anchor_slot;
    out[20] = frame->slot_count;
    out[21] = frame->flags;
    append_crc(out, UWB_SURVEY_DISCOVERY_PROBE_LEN - UWB_FRAME_CRC_LEN);
    *written = UWB_SURVEY_DISCOVERY_PROBE_LEN;
    return PROTO_OK;
}

int uwb_decode_survey_discovery_probe(const uint8_t *data,
                                      size_t len,
                                      struct uwb_survey_discovery_probe_frame *frame)
{
    int ret;

    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_sync_prefix(data,
                               len,
                               UWB_SURVEY_DISCOVERY_PROBE_LEN,
                               MSG_UWB_SURVEY_DISCOVERY_PROBE);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = verify_crc(data, len);
    if (ret != PROTO_OK) {
        return ret;
    }

    frame->network_id = proto_get_u32_le(&data[3]);
    frame->survey_id = proto_get_u32_le(&data[7]);
    frame->anchor_id = proto_get_u64_le(&data[11]);
    frame->anchor_slot = data[19];
    frame->slot_count = data[20];
    frame->flags = data[21];
    return validate_survey_discovery_probe(frame);
}

size_t uwb_range_schedule_encoded_len(uint8_t selected_count)
{
    if (selected_count > UWB_RANGE_SCHEDULE_MAX_ANCHORS) {
        return 0u;
    }
    return UWB_RANGE_SCHEDULE_FIXED_LEN +
           ((size_t)selected_count * UWB_RANGE_SCHEDULE_ENTRY_LEN) +
           UWB_FRAME_CRC_LEN;
}

int uwb_encode_range_schedule(const struct uwb_range_schedule_frame *frame,
                              uint8_t *out,
                              size_t out_cap,
                              size_t *written)
{
    size_t len;
    size_t offset = UWB_RANGE_SCHEDULE_FIXED_LEN;
    int ret;

    if (out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_range_schedule(frame);
    if (ret != PROTO_OK) {
        return ret;
    }
    len = uwb_range_schedule_encoded_len(frame->selected_count);
    if (out_cap < len) {
        return PROTO_ERR_NO_SPACE;
    }

    put_sync_prefix(out, MSG_UWB_RANGE_SCHEDULE);
    proto_put_u32_le(&out[3], frame->network_id);
    proto_put_u64_le(&out[7], frame->clicker_id);
    proto_put_u32_le(&out[15], frame->click_event_id);
    out[19] = frame->attempt_index;
    proto_put_u64_le(&out[20], frame->nonce);
    out[28] = frame->selected_count;
    out[29] = frame->ranging_channel;
    proto_put_u16_le(&out[30], frame->reply_delay_us);
    proto_put_u16_le(&out[32], frame->first_poll_delay_ms);
    proto_put_u16_le(&out[34], frame->poll_spacing_ms);
    proto_put_u16_le(&out[36], frame->burst_window_ms);
    proto_put_u16_le(&out[38], frame->exchange_stride_us);
    out[40] = frame->max_exchanges;
    out[41] = frame->min_successful_unique_anchors;
    out[42] = frame->sts_mode;
    out[43] = frame->diagnostics_required;
    out[44] = frame->samples_per_anchor;
    out[45] = frame->flags;

    for (uint8_t i = 0u; i < frame->selected_count; i++) {
        proto_put_u64_le(&out[offset], frame->entries[i].anchor_id);
        out[offset + 8u] = frame->entries[i].seq;
        out[offset + 9u] = frame->entries[i].sample_count;
        offset += UWB_RANGE_SCHEDULE_ENTRY_LEN;
    }

    append_crc(out, len - UWB_FRAME_CRC_LEN);
    *written = len;
    return PROTO_OK;
}

int uwb_decode_range_schedule(const uint8_t *data,
                              size_t len,
                              struct uwb_range_schedule_frame *frame)
{
    size_t expected_len;
    size_t offset = UWB_RANGE_SCHEDULE_FIXED_LEN;
    int ret;

    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }
    if (data == NULL) {
        return PROTO_ERR_ARG;
    }
    if (len < UWB_RANGE_SCHEDULE_MIN_LEN) {
        return PROTO_ERR_BAD_LENGTH;
    }

    ret = validate_sync_prefix(data, len, len, MSG_UWB_RANGE_SCHEDULE);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = verify_crc(data, len);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(frame, 0, sizeof(*frame));
    frame->network_id = proto_get_u32_le(&data[3]);
    frame->clicker_id = proto_get_u64_le(&data[7]);
    frame->click_event_id = proto_get_u32_le(&data[15]);
    frame->attempt_index = data[19];
    frame->nonce = proto_get_u64_le(&data[20]);
    frame->selected_count = data[28];
    frame->ranging_channel = data[29];
    frame->reply_delay_us = proto_get_u16_le(&data[30]);
    frame->first_poll_delay_ms = proto_get_u16_le(&data[32]);
    frame->poll_spacing_ms = proto_get_u16_le(&data[34]);
    frame->burst_window_ms = proto_get_u16_le(&data[36]);
    frame->exchange_stride_us = proto_get_u16_le(&data[38]);
    frame->max_exchanges = data[40];
    frame->min_successful_unique_anchors = data[41];
    frame->sts_mode = data[42];
    frame->diagnostics_required = data[43];
    frame->samples_per_anchor = data[44];
    frame->flags = data[45];

    expected_len = uwb_range_schedule_encoded_len(frame->selected_count);
    if (expected_len == 0u || len != expected_len) {
        return PROTO_ERR_BAD_LENGTH;
    }

    for (uint8_t i = 0u; i < frame->selected_count; i++) {
        frame->entries[i].anchor_id = proto_get_u64_le(&data[offset]);
        frame->entries[i].seq = data[offset + 8u];
        frame->entries[i].sample_count = data[offset + 9u];
        offset += UWB_RANGE_SCHEDULE_ENTRY_LEN;
    }

    return validate_range_schedule(frame);
}

int uwb_encode_range_release(const struct uwb_range_release_frame *frame,
                             uint8_t *out,
                             size_t out_cap,
                             size_t *written)
{
    int ret;

    if (out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < UWB_RANGE_RELEASE_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = validate_range_release(frame);
    if (ret != PROTO_OK) {
        return ret;
    }

    put_sync_prefix(out, MSG_UWB_RANGE_RELEASE);
    proto_put_u32_le(&out[3], frame->network_id);
    proto_put_u64_le(&out[7], frame->clicker_id);
    proto_put_u32_le(&out[15], frame->click_event_id);
    out[19] = frame->attempt_index;
    proto_put_u64_le(&out[20], frame->nonce);
    out[28] = frame->discovered_anchor_count;
    out[29] = frame->min_anchor_count;
    out[30] = frame->reason;
    out[31] = frame->flags;
    append_crc(out, UWB_RANGE_RELEASE_LEN - UWB_FRAME_CRC_LEN);
    *written = UWB_RANGE_RELEASE_LEN;
    return PROTO_OK;
}

int uwb_decode_range_release(const uint8_t *data,
                             size_t len,
                             struct uwb_range_release_frame *frame)
{
    int ret;

    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_sync_prefix(data, len, UWB_RANGE_RELEASE_LEN,
                               MSG_UWB_RANGE_RELEASE);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = verify_crc(data, len);
    if (ret != PROTO_OK) {
        return ret;
    }

    frame->network_id = proto_get_u32_le(&data[3]);
    frame->clicker_id = proto_get_u64_le(&data[7]);
    frame->click_event_id = proto_get_u32_le(&data[15]);
    frame->attempt_index = data[19];
    frame->nonce = proto_get_u64_le(&data[20]);
    frame->discovered_anchor_count = data[28];
    frame->min_anchor_count = data[29];
    frame->reason = data[30];
    frame->flags = data[31];
    return validate_range_release(frame);
}

size_t uwb_range_schedule_total_samples(const struct uwb_range_schedule_frame *frame)
{
    size_t total;

    if (validate_range_schedule(frame) != PROTO_OK) {
        return 0u;
    }

    total = (size_t)frame->selected_count * frame->samples_per_anchor;
    return total < frame->max_exchanges ? total : frame->max_exchanges;
}

int uwb_range_schedule_sample_at(const struct uwb_range_schedule_frame *frame,
                                 size_t sample_index,
                                 uint64_t *anchor_id,
                                 uint8_t *seq)
{
    const struct uwb_range_schedule_entry *entry;
    size_t total_samples;
    size_t entry_index;
    size_t round;

    if (anchor_id == NULL || seq == NULL) {
        return PROTO_ERR_ARG;
    }
    if (validate_range_schedule(frame) != PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }
    total_samples = (size_t)frame->selected_count * frame->samples_per_anchor;
    if (total_samples > frame->max_exchanges) {
        total_samples = frame->max_exchanges;
    }
    if (sample_index >= total_samples) {
        return PROTO_ERR_NOT_FOUND;
    }

    entry_index = sample_index % frame->selected_count;
    round = sample_index / frame->selected_count;
    entry = &frame->entries[entry_index];
    if (round >= entry->sample_count) {
        return PROTO_ERR_NOT_FOUND;
    }

    *anchor_id = entry->anchor_id;
    *seq = (uint8_t)(entry->seq + round);
    return PROTO_OK;
}

int uwb_claim_precedence_compare(uint8_t left_attempt_index,
                                 uint64_t left_priority_id,
                                 uint64_t left_clicker_id,
                                 uint32_t left_click_event_id,
                                 uint8_t right_attempt_index,
                                 uint64_t right_priority_id,
                                 uint64_t right_clicker_id,
                                 uint32_t right_click_event_id)
{
    if (left_attempt_index != right_attempt_index) {
        return left_attempt_index > right_attempt_index ? 1 : -1;
    }
    if (left_priority_id != right_priority_id) {
        return left_priority_id < right_priority_id ? 1 : -1;
    }
    if (left_clicker_id != right_clicker_id) {
        return left_clicker_id < right_clicker_id ? 1 : -1;
    }
    if (left_click_event_id != right_click_event_id) {
        return left_click_event_id < right_click_event_id ? 1 : -1;
    }
    return 0;
}

int uwb_mesh_frame_encode(uint32_t network_id,
                          uint64_t previous_hop_id,
                          uint64_t next_hop_id,
                          const struct proto_packet *packet,
                          const uint8_t *payload,
                          uint8_t *out,
                          size_t out_cap,
                          size_t *written)
{
    size_t packet_len = 0u;
    size_t total_len;
    int ret;

    if (packet == NULL || out == NULL || written == NULL ||
        network_id == 0u ||
        previous_hop_id == 0u ||
        previous_hop_id == next_hop_id ||
        (packet->payload_len > 0u && payload == NULL)) {
        return PROTO_ERR_ARG;
    }
    if ((next_hop_id == UWB_MESH_BROADCAST_ID) !=
        (packet->dst_id == UWB_MESH_BROADCAST_ID)) {
        return PROTO_ERR_MALFORMED;
    }
    if (out_cap < UWB_MESH_FRAME_HEADER_LEN + UWB_FRAME_CRC_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    put_sync_prefix(out, MSG_UWB_MESH);
    proto_put_u32_le(&out[3], network_id);
    proto_put_u64_le(&out[7], previous_hop_id);
    proto_put_u64_le(&out[15], next_hop_id);

    ret = proto_packet_encode(packet,
                              payload,
                              &out[UWB_MESH_FRAME_HEADER_LEN],
                              out_cap - UWB_MESH_FRAME_HEADER_LEN - UWB_FRAME_CRC_LEN,
                              &packet_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (packet_len > PACKET_MAX_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    total_len = UWB_MESH_FRAME_HEADER_LEN + packet_len + UWB_FRAME_CRC_LEN;
    proto_put_u16_le(&out[23], (uint16_t)packet_len);
    append_crc(out, total_len - UWB_FRAME_CRC_LEN);
    *written = total_len;
    return PROTO_OK;
}

int uwb_mesh_frame_decode(const uint8_t *data,
                          size_t len,
                          uint32_t expected_network_id,
                          uint64_t local_id,
                          uint64_t *previous_hop_id,
                          struct proto_packet *packet,
                          uint8_t *payload,
                          size_t payload_cap,
                          size_t *payload_len)
{
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;
    uint32_t network_id;
    uint64_t hop_id;
    uint64_t next_hop_id;
    uint16_t packet_len;
    int ret;

    if (data == NULL || previous_hop_id == NULL || packet == NULL ||
        payload == NULL || payload_len == NULL ||
        expected_network_id == 0u ||
        local_id == 0u ||
        len < UWB_MESH_FRAME_HEADER_LEN + PACKET_HEADER_LEN + PACKET_CRC_LEN +
              UWB_FRAME_CRC_LEN ||
        len > UWB_MESH_MAX_FRAME_LEN) {
        return PROTO_ERR_ARG;
    }

    ret = validate_sync_prefix(data, len, len, MSG_UWB_MESH);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = verify_crc(data, len);
    if (ret != PROTO_OK) {
        return ret;
    }

    network_id = proto_get_u32_le(&data[3]);
    hop_id = proto_get_u64_le(&data[7]);
    next_hop_id = proto_get_u64_le(&data[15]);
    packet_len = proto_get_u16_le(&data[23]);
    if (network_id != expected_network_id ||
        hop_id == 0u ||
        hop_id == local_id ||
        (next_hop_id != 0u && next_hop_id != local_id) ||
        packet_len != len - UWB_MESH_FRAME_HEADER_LEN - UWB_FRAME_CRC_LEN) {
        return PROTO_ERR_MALFORMED;
    }

    ret = proto_packet_decode(&data[UWB_MESH_FRAME_HEADER_LEN],
                              packet_len,
                              packet,
                              &decoded_payload,
                              &decoded_payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if ((next_hop_id == UWB_MESH_BROADCAST_ID) !=
        (packet->dst_id == UWB_MESH_BROADCAST_ID)) {
        return PROTO_ERR_MALFORMED;
    }
    if (decoded_payload_len > payload_cap) {
        return PROTO_ERR_NO_SPACE;
    }

    if (decoded_payload_len > 0u) {
        memcpy(payload, decoded_payload, decoded_payload_len);
    }
    *payload_len = decoded_payload_len;
    *previous_hop_id = hop_id;
    return PROTO_OK;
}

void uwb_anchor_epoch_clear(struct uwb_anchor_epoch *epoch)
{
    if (epoch != NULL) {
        memset(epoch, 0, sizeof(*epoch));
    }
}

static bool epoch_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void epoch_set_from_claim(struct uwb_anchor_epoch *epoch,
                                 const struct uwb_wake_claim_frame *claim,
                                 uint32_t now_ms)
{
    epoch->active = true;
    epoch->network_id = claim->network_id;
    epoch->clicker_id = claim->clicker_id;
    epoch->click_event_id = claim->click_event_id;
    epoch->attempt_index = claim->attempt_index;
    epoch->priority_id = claim->priority_id;
    epoch->nonce = claim->nonce;
    epoch->epoch_ends_at_ms = now_ms + claim->claimed_duration_ms;
    epoch->flags = claim->flags;
}

static bool claim_is_same_epoch(const struct uwb_anchor_epoch *epoch,
                                const struct uwb_wake_claim_frame *claim)
{
    return epoch->network_id == claim->network_id &&
           epoch->clicker_id == claim->clicker_id &&
           epoch->click_event_id == claim->click_event_id &&
           epoch->attempt_index == claim->attempt_index &&
           epoch->priority_id == claim->priority_id &&
           epoch->nonce == claim->nonce &&
           epoch->flags == claim->flags;
}

static bool claim_is_same_click_event(const struct uwb_anchor_epoch *epoch,
                                      const struct uwb_wake_claim_frame *claim)
{
    return epoch->network_id == claim->network_id &&
           epoch->clicker_id == claim->clicker_id &&
           epoch->click_event_id == claim->click_event_id &&
           epoch->nonce == claim->nonce &&
           epoch->flags == claim->flags;
}

static bool claim_has_same_event_ids(const struct uwb_anchor_epoch *epoch,
                                     const struct uwb_wake_claim_frame *claim)
{
    return epoch->network_id == claim->network_id &&
           epoch->clicker_id == claim->clicker_id &&
           epoch->click_event_id == claim->click_event_id;
}

static bool claim_wins_arbitration(const struct uwb_anchor_epoch *epoch,
                                   const struct uwb_wake_claim_frame *claim)
{
    return uwb_claim_precedence_compare(claim->attempt_index,
                                        claim->priority_id,
                                        claim->clicker_id,
                                        claim->click_event_id,
                                        epoch->attempt_index,
                                        epoch->priority_id,
                                        epoch->clicker_id,
                                        epoch->click_event_id) > 0;
}

int uwb_anchor_epoch_consider_claim(struct uwb_anchor_epoch *epoch,
                                    const struct uwb_wake_claim_frame *claim,
                                    uint32_t now_ms,
                                    enum uwb_anchor_claim_decision *decision)
{
    int ret;

    if (epoch == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_wake_claim(claim);
    if (ret != PROTO_OK) {
        if (decision != NULL) {
            *decision = UWB_ANCHOR_CLAIM_REJECTED_MALFORMED;
        }
        return ret;
    }

    if (epoch->active && epoch_deadline_reached(now_ms, epoch->epoch_ends_at_ms)) {
        uwb_anchor_epoch_clear(epoch);
    }

    if (!epoch->active) {
        epoch_set_from_claim(epoch, claim, now_ms);
        if (decision != NULL) {
            *decision = UWB_ANCHOR_CLAIM_ACCEPTED;
        }
        return PROTO_OK;
    }

    if (claim_is_same_epoch(epoch, claim)) {
        epoch->epoch_ends_at_ms = now_ms + claim->claimed_duration_ms;
        if (decision != NULL) {
            *decision = UWB_ANCHOR_CLAIM_ACCEPTED;
        }
        return PROTO_OK;
    }

    if (claim_is_same_click_event(epoch, claim)) {
        if (claim->attempt_index < epoch->attempt_index) {
            if (decision != NULL) {
                *decision = UWB_ANCHOR_CLAIM_REJECTED_STALE;
            }
            return PROTO_ERR_STALE;
        }
        if (claim->attempt_index == epoch->attempt_index) {
            if (decision != NULL) {
                *decision = UWB_ANCHOR_CLAIM_REJECTED_MALFORMED;
            }
            return PROTO_ERR_MALFORMED;
        }

        epoch_set_from_claim(epoch, claim, now_ms);
        if (decision != NULL) {
            *decision = UWB_ANCHOR_CLAIM_ACCEPTED;
        }
        return PROTO_OK;
    }

    if (claim_has_same_event_ids(epoch, claim)) {
        if (decision != NULL) {
            *decision = UWB_ANCHOR_CLAIM_REJECTED_MALFORMED;
        }
        return PROTO_ERR_MALFORMED;
    }

    if (claim->network_id != epoch->network_id) {
        if (decision != NULL) {
            *decision = UWB_ANCHOR_CLAIM_REJECTED_BUSY;
        }
        return PROTO_ERR_BUSY;
    }

    if (claim->clicker_id == epoch->clicker_id &&
        claim->click_event_id == epoch->click_event_id &&
        claim->attempt_index < epoch->attempt_index) {
        if (decision != NULL) {
            *decision = UWB_ANCHOR_CLAIM_REJECTED_STALE;
        }
        return PROTO_ERR_STALE;
    }

    if (claim_wins_arbitration(epoch, claim)) {
        epoch_set_from_claim(epoch, claim, now_ms);
        if (decision != NULL) {
            *decision = UWB_ANCHOR_CLAIM_REPLACED_BY_PRIORITY;
        }
        return PROTO_OK;
    }

    if (decision != NULL) {
        *decision = UWB_ANCHOR_CLAIM_REJECTED_LOST_ARBITRATION;
    }
    return PROTO_ERR_BUSY;
}

bool uwb_anchor_epoch_matches(const struct uwb_anchor_epoch *epoch,
                              uint32_t network_id,
                              uint64_t clicker_id,
                              uint32_t click_event_id,
                              uint8_t attempt_index,
                              uint64_t nonce)
{
    if (epoch == NULL || !epoch->active) {
        return false;
    }

    return epoch->network_id == network_id &&
           epoch->clicker_id == clicker_id &&
           epoch->click_event_id == click_event_id &&
           epoch->attempt_index == attempt_index &&
           epoch->nonce == nonce;
}

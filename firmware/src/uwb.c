#include "uwb.h"

static int validate_status(enum range_status status)
{
    if (status < RANGE_OK || status > RANGE_INTERNAL_ERROR) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
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
    proto_put_u32_le(&out[4], header->session_id);
    proto_put_u16_le(&out[8], header->initiator_short_addr);
    proto_put_u16_le(&out[10], header->responder_short_addr);
    out[12] = header->flags;
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
    header->session_id = proto_get_u32_le(&data[4]);
    header->initiator_short_addr = proto_get_u16_le(&data[8]);
    header->responder_short_addr = proto_get_u16_le(&data[10]);
    header->flags = data[12];
    return uwb_header_validate(header, expected_type);
}

bool uwb_frame_type_valid(uint8_t type)
{
    return type == MSG_UWB_POLL ||
           type == MSG_UWB_RESP ||
           type == MSG_UWB_FINAL ||
           type == MSG_UWB_REPORT;
}

int uwb_header_validate(const struct uwb_range_header *header, uint8_t expected_type)
{
    if (header == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!uwb_frame_type_valid(header->type) || header->type != expected_type) {
        return PROTO_ERR_MALFORMED;
    }
    if (header->session_id == 0u ||
        header->initiator_short_addr == 0u ||
        header->responder_short_addr == 0u ||
        header->initiator_short_addr == header->responder_short_addr) {
        return PROTO_ERR_MALFORMED;
    }
    if ((header->flags & FLAG_DIAGNOSTIC) != 0u &&
        (header->flags & FLAG_COUNT_AS_CLICK) != 0u) {
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

    proto_put_u32_le(&out[13], frame->poll_rx_ts_32);
    proto_put_u32_le(&out[17], frame->resp_tx_ts_32);
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

    frame->poll_rx_ts_32 = proto_get_u32_le(&data[13]);
    frame->resp_tx_ts_32 = proto_get_u32_le(&data[17]);
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

    proto_put_u32_le(&out[13], frame->poll_tx_ts_32);
    proto_put_u32_le(&out[17], frame->resp_rx_ts_32);
    proto_put_u32_le(&out[21], frame->final_tx_ts_32);
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

    frame->poll_tx_ts_32 = proto_get_u32_le(&data[13]);
    frame->resp_rx_ts_32 = proto_get_u32_le(&data[17]);
    frame->final_tx_ts_32 = proto_get_u32_le(&data[21]);
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

    proto_put_u32_le(&out[13], (uint32_t)frame->distance_mm);
    out[17] = frame->quality;
    proto_put_u16_le(&out[18], (uint16_t)frame->status);
    out[20] = (uint8_t)frame->rsl_dbm;
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

    frame->distance_mm = (int32_t)proto_get_u32_le(&data[13]);
    frame->quality = data[17];
    frame->status = (enum range_status)proto_get_u16_le(&data[18]);
    frame->rsl_dbm = (int8_t)data[20];

    if (frame->quality > 100u) {
        return PROTO_ERR_MALFORMED;
    }
    return validate_status(frame->status);
}

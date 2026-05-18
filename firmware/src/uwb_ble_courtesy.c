#include "uwb_ble_courtesy.h"

static int validate_courtesy_frame(const struct uwb_ble_courtesy_frame *frame)
{
    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }
    if (frame->network_id == 0u ||
        frame->clicker_id == 0u ||
        frame->click_event_id == 0u ||
        frame->attempt_index == 0u ||
        frame->defer_duration_units == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

uint8_t uwb_ble_courtesy_duration_units_from_ms(uint32_t duration_ms)
{
    uint32_t units;

    if (duration_ms == 0u) {
        return 0u;
    }
    if (duration_ms >= UWB_BLE_COURTESY_MAX_DURATION_MS) {
        return UINT8_MAX;
    }

    units = (duration_ms + UWB_BLE_COURTESY_DURATION_UNIT_MS - 1u) /
            UWB_BLE_COURTESY_DURATION_UNIT_MS;
    return (uint8_t)units;
}

uint32_t uwb_ble_courtesy_duration_ms(uint8_t duration_units)
{
    return (uint32_t)duration_units * UWB_BLE_COURTESY_DURATION_UNIT_MS;
}

int uwb_ble_courtesy_encode(const struct uwb_ble_courtesy_frame *frame,
                            uint8_t *out,
                            size_t out_cap,
                            size_t *written)
{
    int ret;

    if (out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_courtesy_frame(frame);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (out_cap < UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    proto_put_u16_le(&out[0], UWB_BLE_COURTESY_COMPANY_ID);
    out[2] = UWB_BLE_COURTESY_MARKER_VERSION;
    proto_put_u32_le(&out[3], frame->network_id);
    proto_put_u64_le(&out[7], frame->clicker_id);
    proto_put_u32_le(&out[15], frame->click_event_id);
    out[19] = frame->attempt_index;
    proto_put_u64_le(&out[20], frame->priority_id);
    out[28] = frame->defer_duration_units;
    *written = UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN;
    return PROTO_OK;
}

int uwb_ble_courtesy_decode(const uint8_t *data,
                            size_t len,
                            struct uwb_ble_courtesy_frame *frame)
{
    if (data == NULL || frame == NULL) {
        return PROTO_ERR_ARG;
    }
    if (len != UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN) {
        return PROTO_ERR_BAD_LENGTH;
    }
    if (proto_get_u16_le(&data[0]) != UWB_BLE_COURTESY_COMPANY_ID ||
        data[2] != UWB_BLE_COURTESY_MARKER_VERSION) {
        return PROTO_ERR_BAD_MAGIC;
    }

    frame->network_id = proto_get_u32_le(&data[3]);
    frame->clicker_id = proto_get_u64_le(&data[7]);
    frame->click_event_id = proto_get_u32_le(&data[15]);
    frame->attempt_index = data[19];
    frame->priority_id = proto_get_u64_le(&data[20]);
    frame->defer_duration_units = data[28];
    return validate_courtesy_frame(frame);
}

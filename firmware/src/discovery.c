#include "discovery.h"

static int validate_common(uint64_t device_id, uint8_t flags)
{
    if (device_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    if (ble_flags_are_diagnostic(flags) && ble_flags_count_as_click(flags)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static int validate_prefix(const uint8_t *data, size_t len, size_t expected_len, uint8_t expected_type)
{
    if (data == NULL) {
        return PROTO_ERR_ARG;
    }
    if (len != expected_len) {
        return PROTO_ERR_BAD_LENGTH;
    }
    if (proto_get_u16_le(data) != BLE_COMPANY_ID) {
        return PROTO_ERR_BAD_MAGIC;
    }
    if (data[2] != PROTO_VERSION) {
        return PROTO_ERR_BAD_VERSION;
    }
    if (data[3] != expected_type) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

bool ble_flags_are_diagnostic(uint8_t flags)
{
    return (flags & FLAG_DIAGNOSTIC) != 0u;
}

bool ble_flags_count_as_click(uint8_t flags)
{
    return (flags & FLAG_COUNT_AS_CLICK) != 0u;
}

uint8_t ble_flags_for_click(void)
{
    return FLAG_COUNT_AS_CLICK;
}

uint8_t ble_flags_for_diagnostic(void)
{
    return FLAG_DIAGNOSTIC;
}

int ble_discovery_req_encode(const struct ble_discovery_req *request,
                                  uint8_t *out,
                                  size_t out_cap,
                                  size_t *written)
{
    int ret;

    if (request == NULL || out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < BLE_DISCOVERY_REQ_LEN) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = validate_common(request->clicker_id, request->flags);
    if (ret != PROTO_OK) {
        return ret;
    }

    proto_put_u16_le(&out[0], BLE_COMPANY_ID);
    out[2] = PROTO_VERSION;
    out[3] = MSG_BLE_DISCOVERY_REQ;
    proto_put_u64_le(&out[4], request->clicker_id);
    proto_put_u32_le(&out[12], request->event_seq);
    out[16] = request->flags;
    *written = BLE_DISCOVERY_REQ_LEN;
    return PROTO_OK;
}

int ble_discovery_req_decode(const uint8_t *data,
                                  size_t len,
                                  struct ble_discovery_req *request)
{
    int ret;

    if (request == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_prefix(data, len, BLE_DISCOVERY_REQ_LEN, MSG_BLE_DISCOVERY_REQ);
    if (ret != PROTO_OK) {
        return ret;
    }

    request->clicker_id = proto_get_u64_le(&data[4]);
    request->event_seq = proto_get_u32_le(&data[12]);
    request->flags = data[16];
    return validate_common(request->clicker_id, request->flags);
}

int ble_discovery_ready_encode(const struct ble_discovery_ready *ready,
                                    uint8_t *out,
                                    size_t out_cap,
                                    size_t *written)
{
    int ret;

    if (ready == NULL || out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < BLE_DISCOVERY_READY_LEN) {
        return PROTO_ERR_NO_SPACE;
    }
    if (ready->uwb_short_addr == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    ret = validate_common(ready->anchor_id, ready->flags);
    if (ret != PROTO_OK) {
        return ret;
    }

    proto_put_u16_le(&out[0], BLE_COMPANY_ID);
    out[2] = PROTO_VERSION;
    out[3] = MSG_BLE_DISCOVERY_READY;
    proto_put_u64_le(&out[4], ready->anchor_id);
    proto_put_u16_le(&out[12], ready->uwb_short_addr);
    out[14] = ready->flags;
    out[15] = (uint8_t)ready->rssi_hint;
    *written = BLE_DISCOVERY_READY_LEN;
    return PROTO_OK;
}

int ble_discovery_ready_decode(const uint8_t *data,
                                    size_t len,
                                    struct ble_discovery_ready *ready)
{
    int ret;

    if (ready == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_prefix(data, len, BLE_DISCOVERY_READY_LEN, MSG_BLE_DISCOVERY_READY);
    if (ret != PROTO_OK) {
        return ret;
    }

    ready->anchor_id = proto_get_u64_le(&data[4]);
    ready->uwb_short_addr = proto_get_u16_le(&data[12]);
    ready->flags = data[14];
    ready->rssi_hint = (int8_t)data[15];

    if (ready->uwb_short_addr == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    return validate_common(ready->anchor_id, ready->flags);
}

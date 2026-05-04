#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_MAGIC 0xC1u
#define PROTO_VERSION 0x01u
#define PACKET_HEADER_LEN 28u
#define PACKET_CRC_LEN 2u
#define PACKET_MAX_PAYLOAD_LEN 255u
#define PACKET_MAX_LEN (PACKET_HEADER_LEN + PACKET_MAX_PAYLOAD_LEN + PACKET_CRC_LEN)

enum result {
    PROTO_OK = 0,
    PROTO_ERR_ARG = -1,
    PROTO_ERR_NO_SPACE = -2,
    PROTO_ERR_BAD_MAGIC = -3,
    PROTO_ERR_BAD_VERSION = -4,
    PROTO_ERR_BAD_LENGTH = -5,
    PROTO_ERR_BAD_CRC = -6,
    PROTO_ERR_NOT_FOUND = -7,
    PROTO_ERR_MALFORMED = -8,
    PROTO_ERR_STALE = -9,
    PROTO_ERR_BUSY = -10,
};

enum msg_type {
    MSG_BLE_DISCOVERY_REQ = 0x01,
    MSG_BLE_DISCOVERY_READY = 0x02,

    MSG_UWB_POLL = 0x10,
    MSG_UWB_RESP = 0x11,
    MSG_UWB_FINAL = 0x12,
    MSG_UWB_REPORT = 0x13,

    MSG_CLICK_REPORT = 0x20,
    MSG_SELF_TEST_REPORT = 0x21,
    MSG_ANCHOR_HEARTBEAT = 0x22,

    MSG_MESH_DATA = 0x30,
    MSG_MESH_ACK = 0x31,
    MSG_GATEWAY_ACK = 0x32,
    MSG_ROUTE_ADV = 0x33,
    MSG_ROUTE_STATUS = 0x34,

    MSG_COMMAND = 0x40,
    MSG_COMMAND_RESULT = 0x41,

    MSG_SURVEY_REACH_REQ = 0x50,
    MSG_SURVEY_REACH_REPORT = 0x51,
    MSG_SURVEY_PAIR_PREPARE = 0x52,
    MSG_SURVEY_PAIR_RESULT = 0x53,

    MSG_ERROR = 0x7F,
};

enum pkt_flags {
    FLAG_ACK_REQUESTED = 0x01,
    FLAG_HOP_ACK = 0x02,
    FLAG_GATEWAY_ACK_REQUIRED = 0x04,
    FLAG_GATEWAY_ACK = 0x08,
    FLAG_DIAGNOSTIC = 0x10,
    FLAG_COUNT_AS_CLICK = 0x20,
    FLAG_ERROR = 0x40,
};

enum tlv_type {
    TLV_DEVICE_ROLE = 0x01,
    TLV_BATTERY_MV = 0x02,
    TLV_STATUS_BITS = 0x03,
    TLV_ERROR_CODE = 0x04,
    TLV_ERROR_DETAIL = 0x05,
    TLV_EVENT_SEQ = 0x06,
    TLV_TIMESTAMP_MS = 0x07,
    TLV_RSSI_DBM = 0x08,
    TLV_UWB_SHORT_ADDR = 0x09,
    TLV_ANCHOR_ID = 0x0A,
    TLV_CLICKER_ID = 0x0B,
    TLV_DISTANCE_MM = 0x0C,
    TLV_QUALITY = 0x0D,
    TLV_SAMPLE_INDEX = 0x0E,
    TLV_SAMPLE_COUNT = 0x0F,
    TLV_COMMAND_ID = 0x10,
    TLV_COMMAND_STATUS = 0x11,
    TLV_REQUESTED_MSG_SEQ = 0x12,
    TLV_NEXT_HOP_ID = 0x13,
    TLV_GATEWAY_ID = 0x14,
    TLV_SURVEY_ID = 0x15,
    TLV_PEER_ID_LIST = 0x16,
    TLV_REACHABILITY_ENTRY = 0x17,
    TLV_RANGE_FLAGS = 0x18,
    TLV_LED_PATTERN_ID = 0x19,
    TLV_DURATION_MS = 0x1A,
    TLV_RETRY_COUNT = 0x1B,
    TLV_FW_VERSION = 0x1C,
    TLV_UPTIME_MS = 0x1D,
    TLV_REASON = 0x1E,
    TLV_INITIATOR_ID = 0x1F,
    TLV_RESPONDER_ID = 0x20,
    TLV_RANGE_STATUS = 0x21,
    TLV_ROUTE_EPOCH = 0x22,
    TLV_HOP_COUNT = 0x23,
    TLV_UWB_RSL_DBM = 0x24,
};

enum device_role {
    ROLE_CLICKER = 1,
    ROLE_ANCHOR = 2,
    ROLE_GATEWAY = 3,
};

enum command_id {
    CMD_PING = 0x0001,
    CMD_GET_STATUS = 0x0002,
    CMD_SET_LED_PATTERN = 0x0003,
    CMD_REBOOT = 0x0004,
    CMD_SET_ROLE = 0x0005,
    CMD_SET_ROUTE = 0x0006,
    CMD_CLEAR_ROUTE = 0x0007,
    CMD_SET_SCAN_DUTY = 0x0008,
    CMD_START_HEARTBEAT = 0x0009,
    CMD_STOP_HEARTBEAT = 0x000A,
    CMD_SURVEY_REACHABILITY = 0x0100,
    CMD_SURVEY_PREPARE_PAIR = 0x0101,
    CMD_SURVEY_START_PAIR = 0x0102,
    CMD_SURVEY_ABORT = 0x0103,
    CMD_VENDOR_BASE = 0x8000,
};

enum command_status {
    COMMAND_OK = 0,
    COMMAND_UNSUPPORTED_COMMAND = 1,
    COMMAND_MALFORMED_PAYLOAD = 2,
    COMMAND_BUSY = 3,
    COMMAND_DENIED = 4,
    COMMAND_TIMEOUT = 5,
    COMMAND_RADIO_ERROR = 6,
    COMMAND_INVALID_STATE = 7,
    COMMAND_INTERNAL_ERROR = 8,
};

enum range_status {
    RANGE_OK = 0,
    RANGE_RX_TIMEOUT = 1,
    RANGE_RX_ERROR = 2,
    RANGE_BAD_FRAME = 3,
    RANGE_WRONG_TARGET = 4,
    RANGE_STS_QUALITY_FAIL = 5,
    RANGE_DELAYED_TX_MISSED = 6,
    RANGE_INTERNAL_ERROR = 7,
};

struct proto_packet {
    uint8_t msg_type;
    uint8_t flags;
    uint64_t src_id;
    uint64_t dst_id;
    uint32_t session_id;
    uint16_t seq;
    uint8_t ttl;
    uint8_t payload_len;
};

uint16_t proto_crc16_ccitt_false(const uint8_t *data, size_t len);

size_t proto_packet_encoded_len(uint8_t payload_len);
int proto_packet_encode(const struct proto_packet *packet,
                       const uint8_t *payload,
                       uint8_t *out,
                       size_t out_len,
                       size_t *written);
int proto_packet_decode(const uint8_t *data,
                       size_t len,
                       struct proto_packet *packet,
                       const uint8_t **payload,
                       size_t *payload_len);

int tlv_append_bytes(uint8_t *payload,
                          size_t payload_cap,
                          size_t *offset,
                          uint8_t type,
                          const uint8_t *value,
                          uint8_t len);
int tlv_append_u8(uint8_t *payload, size_t payload_cap, size_t *offset, uint8_t type, uint8_t value);
int tlv_append_i8(uint8_t *payload, size_t payload_cap, size_t *offset, uint8_t type, int8_t value);
int tlv_append_u16(uint8_t *payload, size_t payload_cap, size_t *offset, uint8_t type, uint16_t value);
int tlv_append_u32(uint8_t *payload, size_t payload_cap, size_t *offset, uint8_t type, uint32_t value);
int tlv_append_i32(uint8_t *payload, size_t payload_cap, size_t *offset, uint8_t type, int32_t value);
int tlv_append_u64(uint8_t *payload, size_t payload_cap, size_t *offset, uint8_t type, uint64_t value);
int tlv_find(const uint8_t *payload,
                  size_t payload_len,
                  uint8_t type,
                  const uint8_t **value,
                  uint8_t *len);

uint16_t proto_get_u16_le(const uint8_t *data);
uint32_t proto_get_u32_le(const uint8_t *data);
uint64_t proto_get_u64_le(const uint8_t *data);
void proto_put_u16_le(uint8_t *data, uint16_t value);
void proto_put_u32_le(uint8_t *data, uint32_t value);
void proto_put_u64_le(uint8_t *data, uint64_t value);

int proto_cobs_encode(const uint8_t *input, size_t input_len, uint8_t *out, size_t out_cap, size_t *written);
int proto_cobs_decode(const uint8_t *input, size_t input_len, uint8_t *out, size_t out_cap, size_t *written);

#ifdef __cplusplus
}
#endif

#endif

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
#define PACKET_HEADER_LEN 32u
#define PACKET_EXT_HEADER_LEN 34u
#define PACKET_CRC_LEN 2u
#define PACKET_EXT_LENGTH_SENTINEL 0xFFu
#define PACKET_MAX_PAYLOAD_LEN 255u
#define PACKET_MAX_LEN (PACKET_EXT_HEADER_LEN + PACKET_MAX_PAYLOAD_LEN + PACKET_CRC_LEN)
#define PACKET_EXT_MAX_PAYLOAD_LEN 958u
#define PACKET_EXT_MAX_LEN (PACKET_EXT_HEADER_LEN + PACKET_EXT_MAX_PAYLOAD_LEN + PACKET_CRC_LEN)
#define RESULT_BUNDLE_RECORD_HEADER_LEN 32u
#define RESULT_BUNDLE_RECORD_MAX_PAYLOAD_LEN (255u - RESULT_BUNDLE_RECORD_HEADER_LEN)
#define UWB_CIR_SAMPLE_LEN 6u

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
    /* 0x01 and 0x02 are retired legacy discovery advertising IDs. */

    MSG_UWB_WAKE_CLAIM = 0x08,
    MSG_UWB_DISCOVER = 0x09,
    MSG_UWB_DISCOVERY_REPLY = 0x0A,
    MSG_UWB_RANGE_SCHEDULE = 0x0B,
    MSG_UWB_MESH = 0x0C,
    MSG_UWB_RANGE_RELEASE = 0x0D,

    MSG_UWB_POLL = 0x10,
    MSG_UWB_RESP = 0x11,
    MSG_UWB_FINAL = 0x12,
    MSG_UWB_REPORT = 0x13,
    MSG_UWB_CLICKER_DIAG = 0x14,
    MSG_UWB_SURVEY_DISCOVERY_PROBE = 0x15,
    MSG_UWB_ANCHOR_DIAG = 0x16,
    MSG_UWB_ANCHOR_DIAG_FRAGMENT = 0x17,
    MSG_UWB_ANCHOR_PAIR_SCHEDULE = 0x18,
    MSG_UWB_ANCHOR_PAIR_RESULT = 0x19,

    MSG_CLICK_REPORT = 0x20,
    MSG_SELF_TEST_REPORT = 0x21,
    MSG_ANCHOR_HEARTBEAT = 0x22,

    MSG_MESH_DATA = 0x30,
    MSG_MESH_HOP_ACK = 0x31,
    MSG_GATEWAY_ACK = 0x32,
    /* 0x33 and 0x34 are reserved; legacy route beacons must not be emitted. */
    MSG_ROUTE_REQ = 0x35,
    MSG_ROUTE_REPLY = 0x36,
    MSG_MESH_EVENT_PROPOSE = 0x37,
    MSG_MESH_EVENT_ACCEPT = 0x38,
    MSG_MESH_EVENT_UPDATE = 0x39,
    MSG_MESH_EVENT_END = 0x3A,
    MSG_ROUTE_REPLY_ACK = 0x3B,
    MSG_GATEWAY_ROUTE_ADV = 0x3C,
    MSG_RELAY_BUSY = 0x3D,
    MSG_RESULT_BUSY = 0x3E,
    MSG_GATEWAY_ROUTE_REQ = 0x3F,

    MSG_COMMAND = 0x40,
    MSG_COMMAND_RESULT = 0x41,
    MSG_RESULT_OFFER = 0x42,
    MSG_RESULT_GRANT = 0x43,
    MSG_RESULT_BUNDLE = 0x44,
    MSG_GATEWAY_COLLECTION_EACK = 0x45,

    MSG_SURVEY_REACH_REQ = 0x50,
    MSG_SURVEY_REACH_REPORT = 0x51,
    MSG_SURVEY_PAIR_PREPARE = 0x52,
    MSG_SURVEY_PAIR_RESULT = 0x53,
    MSG_SURVEY_DISCOVERY_START = 0x54,
    MSG_SURVEY_DISCOVERY_REPORT = 0x55,

    MSG_ERROR = 0x7F,
};

enum pkt_flags {
    FLAG_ROUTE_SETUP = 0x02,
    FLAG_GATEWAY_ACK_REQUIRED = 0x04,
    FLAG_GATEWAY_ACK = 0x08,
    FLAG_DIAGNOSTIC = 0x10,
    FLAG_COUNT_AS_CLICK = 0x20,
    FLAG_ERROR = 0x40,
    FLAG_RANGE_ONLY = 0x80,
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
    TLV_DISTANCE_SAMPLES_MM = 0x25,
    TLV_UWB_CIR_SAMPLE = 0x26,
    /* 0x27 retired: gateway time-sync age. */
    TLV_RANGE_ROUND_INDICES = 0x28,
    TLV_SEQUENCE_START_TIMESTAMPS_MS = 0x29,
    TLV_MESH_CHANNEL = 0x2A,
    TLV_MESH_EVENT_INTERVAL_MS = 0x2B,
    TLV_MESH_EVENT_WINDOW_MS = 0x2C,
    TLV_MESH_NEXT_EVENT_TIME_MS = 0x2D,
    TLV_MESH_EVENT_COUNTER = 0x2E,
    TLV_MESH_EVENT_GUARD_MS = 0x2F,
    TLV_MESH_CLOCK_SKEW_PPM = 0x30,
    TLV_MESH_MAX_MISSED_EVENTS = 0x31,
    TLV_MESH_SUPERVISION_TIMEOUT_MS = 0x32,
    TLV_DIAG_STATUS_FLAGS = 0x33,
    TLV_BURST_ID = 0x34,
    TLV_EXCHANGE_STRIDE_US = 0x35,
    TLV_BURST_DURATION_MS = 0x36,
    TLV_CLICK_LATENCY_MS = 0x37,
    TLV_UWB_AWAKE_TIME_US = 0x38,
    TLV_DIAG_BYTES_CAPTURED = 0x39,
    TLV_DIAG_BYTES_TRANSMITTED = 0x3A,
    TLV_DIAG_BYTES_TRUNCATED = 0x3B,
    TLV_DIAG_FRAMES_DROPPED = 0x3C,
    TLV_REPORT_FRAGMENT_COUNT = 0x3D,
    TLV_CHANNEL9_REPORT_LATENCY_MS = 0x3E,
    TLV_GATEWAY_ACK_LATENCY_MS = 0x3F,
    TLV_CLICKER_DIAG_BYTES = 0x40,
    TLV_ANCHOR_DIAG_BYTES = 0x41,
    TLV_PHY_CONFIG_ID = 0x42,
    TLV_MESH_CHANNEL_SWITCHES = 0x43,
    TLV_MESH_PLL_READY_FAILURES = 0x44,
    TLV_MESH_LATE_CHANNEL5_RETURNS = 0x45,
    TLV_MESH_DEFERRALS = 0x46,
    TLV_MESH_CH9_EVENT_MISSES = 0x47,
    TLV_MESH_CHANNEL5_PREEMPTIONS = 0x48,
    TLV_MESH_CH9_REPORT_LATENCY_MS = 0x49,
    TLV_DISCOVERY_START_DELAY_MS = 0x4A,
    TLV_DISCOVERY_SLOT_MS = 0x4B,
    TLV_DISCOVERY_SLOT_COUNT = 0x4C,
    TLV_UWB_CLOCK_OFFSET_RAW = 0x4D,
    TLV_UWB_CARRIER_INTEGRATOR = 0x4E,
    TLV_UWB_CIR_FULL_CHUNK = 0x4F,
    TLV_UWB_CIR_BYTE_OFFSET = 0x50,
    TLV_UWB_CIR_TOTAL_BYTES = 0x51,
    TLV_UWB_CIR_FIRST_PATH_INDEX = 0x52,
    TLV_UWB_RAW_TIMESTAMPS = 0x53,
    TLV_UWB_RX_DIAG_BYTES = 0x54,
    TLV_DIAG_FRAGMENT_INDEX = 0x55,
    TLV_DIAG_FRAGMENT_COUNT = 0x56,
    TLV_DIAG_SOURCE = 0x57,
    TLV_UWB_CIR_START_INDEX = 0x58,
    TLV_MESH_TEST_PACKET_ID = 0x59,
    TLV_MESH_TEST_ATTEMPT = 0x5A,
    TLV_MESH_TEST_DROP_COUNT = 0x5B,
    TLV_MESH_TEST_ORIGIN_ID = 0x5C,
    TLV_MESH_TEST_TARGET_ID = 0x5D,
    TLV_MESH_TEST_FLAGS = 0x5E,
    TLV_MESH_ACK_SEQ_LIST = 0x5F,
    TLV_MESH_ACK_PACKET_ID_LIST = 0x60,
    TLV_MESH_TEST_PADDING = 0x61,
    TLV_MESH_ACK_SESSION_LIST = 0x62,
    TLV_FLOOD_EPOCH_ID = 0x63,
    TLV_FLOOD_PROFILE_VERSION = 0x64,
    TLV_SLOT_SEED = 0x65,
    TLV_ACCUMULATED_COST = 0x66,
    TLV_PATH_QUALITY_MIN = 0x67,
    TLV_RELAY_CAPACITY_STATE = 0x68,
    TLV_QUEUE_FREE_HINT = 0x69,
    TLV_CHANNEL9_BUSY_HINT = 0x6A,
    TLV_GATEWAY_EPOCH = 0x6B,
    TLV_COMMAND_SEQ = 0x6C,
    TLV_COLLECTION_EPOCH_ID = 0x6D,
    TLV_NODE_BOOT_COUNTER = 0x6E,
    TLV_REPLY_NONCE = 0x6F,
    TLV_METRIC_CRC = 0x70,
    TLV_GATEWAY_ROUTE_SEQ = 0x71,
    TLV_RETRY_AFTER_MS = 0x72,
    TLV_REQUESTED_MSG_SESSION_ID = 0x73,
    TLV_ALTERNATE_PARENT_ID = 0x74,
    TLV_COMMAND_SCOPE = 0x75,
    TLV_COMMAND_RESPONSE_MODE = 0x76,
    TLV_MEMBERSHIP_EPOCH = 0x77,
    TLV_EXPECTED_NODE_COUNT = 0x78,
    TLV_EXECUTE_DELAY_MS = 0x79,
    TLV_COMMAND_EXPIRY_S = 0x7A,
    TLV_COLLECTION_SLOT_SEED = 0x7B,
    TLV_RESULT_SEQ = 0x7C,
    TLV_PAYLOAD_LEN = 0x7D,
    TLV_PAYLOAD_CRC = 0x7E,
    TLV_CAPACITY_VALIDITY_INTERVAL_MS = 0x7F,
    TLV_NODE_ID = 0x80,
    TLV_PRIORITY = 0x81,
    TLV_GRANTED_CHANNEL = 0x82,
    TLV_MAX_BYTES = 0x83,
    TLV_EVENT_OFFSET_HINT = 0x84,
    TLV_BUNDLE_ID = 0x85,
    TLV_RECORD_COUNT = 0x86,
    TLV_BUNDLE_CRC = 0x87,
    TLV_EACK_FORMAT = 0x88,
    TLV_RECEIVED_COUNT = 0x89,
    TLV_RETRY_ROUND = 0x8A,
    TLV_NEXT_RETRY_SPREAD_MS = 0x8B,
    TLV_COLLECTION_OPEN = 0x8C,
    TLV_RESULT_RECORD = 0x8D,
    TLV_MESH_DUPLICATE_COUNT = 0x8E,
    TLV_COLLECTION_PENDING_COUNT = 0x8F,
    TLV_PARENT_HOLDDOWN_COUNT = 0x90,
    TLV_ROUTE_DISCOVERY_ATTEMPTS = 0x91,
    TLV_OUTBOX_DELIVERY_STATE = 0x92,
    TLV_FLOOD_SUPPRESSION_COUNT = 0x93,
    TLV_ROUTE_REPLY_RETRY_COUNT = 0x94,
    TLV_BUSY_RESPONSE_COUNT = 0x95,
    TLV_EXPECTED_NODE_ID = 0x96,
    TLV_MESH_TEST_PACKET_AGE_MS = 0x97,
    TLV_MESH_TEST_SELECTED_PARENT_ID = 0x98,
    TLV_MESH_TEST_CH9_TIMING_STATE = 0x99,
    TLV_MESH_TEST_PAYLOAD_CRC = 0x9A,
    TLV_ROUTE_REQUEST_FLAGS = 0x9B,
    TLV_ROUTE_REPLY_RX_DELAY_MS = 0x9C,
    TLV_MESH_LOST_PACKET_COUNT = 0x9D,
    TLV_FLOOD_RANDOM_BACKOFF_MAX_MS = 0x9E,
    TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS = 0x9F,
    TLV_FLOOD_RETRY_COUNT = 0xA0,
    TLV_FLOOD_PACKET_AGE_MS = 0xA1,
    TLV_MESH_CH9_BATCH_ID = 0xA2,
    TLV_MESH_CH9_BATCH_FLAGS = 0xA3,
    TLV_DISCOVERY_ASSIGNMENT_PHASE = 0xA4,
    TLV_DISCOVERY_ASSIGNMENT_EPOCH = 0xA5,
    TLV_DISCOVERY_ASSIGNMENT_HASH = 0xA6,
    TLV_DISCOVERY_ASSIGNMENT_TABLE = 0xA7,
    TLV_CLICKER_CLOCK_OFFSET_RAW = 0xA8,
};

#define MESH_CH9_BATCH_METADATA_TLV_BYTES \
    ((2u + sizeof(uint32_t)) + (2u + sizeof(uint8_t)))
#define MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED (1u << 0)

enum status_bit {
    STATUS_BIT_UWB_SCAN_ACTIVE = 1u << 0,
    STATUS_BIT_UWB_WAKE_DECODE_FAILURE = 1u << 1,
    STATUS_BIT_UWB_CLAIM_COLLISION = 1u << 2,
    STATUS_BIT_UWB_DS_TWR_FAILURE = 1u << 3,
    STATUS_BIT_UWB_TIMING_REJECTION = 1u << 4,
    STATUS_BIT_UWB_MESH_RX = 1u << 5,
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
    /* 0x000B retired: gateway time sync. */
    CMD_FORCE_REDISCOVERY = 0x000C,
    CMD_SURVEY_REACHABILITY = 0x0100,
    CMD_SURVEY_PREPARE_PAIR = 0x0101,
    CMD_SURVEY_START_PAIR = 0x0102,
    CMD_SURVEY_ABORT = 0x0103,
    CMD_ASSIGN_DISCOVERY_SLOTS = 0x0104,
    CMD_ML_START_COLLECTION = 0x8000,
    CMD_ML_START_FAST_RANGING = 0x8001,
    CMD_ML_START_ANCHOR_PAIR_SURVEY = 0x8002,
    CMD_ML_START_LIVE_TRACKING = 0x8003,
    CMD_ML_LIVE_TRACKING_HEARTBEAT = 0x8004,
    CMD_ML_STOP_LIVE_TRACKING = 0x8005,
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

enum command_scope {
    CMD_SCOPE_SINGLE_NODE = 0,
    CMD_SCOPE_GROUP = 1,
    CMD_SCOPE_ALL_REGISTERED = 2,
    CMD_SCOPE_ALL_HEARD = 3,
};

enum command_response_mode {
    CMD_RESPONSE_NONE = 0,
    CMD_RESPONSE_ACK_ONLY = 1,
    CMD_RESPONSE_SMALL_RESULT = 2,
    CMD_RESPONSE_LARGE_RESULT = 3,
};

enum collection_eack_format {
    EACK_FORMAT_ROSTER_BITMAP = 0,
    EACK_FORMAT_EXPLICIT_RECEIVED_LIST = 1,
    EACK_FORMAT_EXPLICIT_MISSING_LIST = 2,
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
    RANGE_TIMING_INVALID = 8,
};

struct proto_packet {
    uint8_t msg_type;
    uint8_t flags;
    uint64_t src_id;
    uint64_t dst_id;
    uint32_t session_id;
    uint16_t seq;
    uint8_t ttl;
    uint16_t payload_len;
    uint32_t message_age_ms;
};

struct command_result_id {
    uint64_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t command_seq;
    uint64_t node_id;
    uint32_t node_boot_counter;
    uint16_t result_seq;
};

struct result_offer {
    struct command_result_id result_id;
    uint16_t result_len;
    uint16_t result_crc;
    uint8_t priority;
};

struct result_grant {
    struct command_result_id result_id;
    uint8_t granted_channel;
    uint16_t max_bytes;
    uint32_t event_offset_hint;
};

struct result_busy {
    struct command_result_id result_id;
    uint16_t retry_after_ms;
    uint8_t capacity_state;
    uint16_t capacity_validity_interval_ms;
    uint64_t optional_alternate_parent;
    bool has_optional_alternate_parent;
};

struct result_bundle_header {
    uint64_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t command_seq;
    uint32_t collection_epoch_id;
    uint16_t bundle_id;
    uint8_t record_count;
    uint16_t bundle_crc;
};

struct result_bundle_record {
    struct command_result_id result_id;
    uint16_t payload_len;
    uint16_t payload_crc;
    const uint8_t *payload;
};

struct gateway_collection_eack {
    uint64_t gateway_id;
    uint16_t gateway_epoch;
    uint32_t command_seq;
    uint32_t collection_epoch_id;
    uint16_t membership_epoch;
    uint16_t expected_count;
    uint16_t received_count;
    uint8_t eack_format;
    uint8_t retry_round;
    uint32_t next_retry_spread_ms;
    bool collection_open;
};

uint16_t proto_crc16_ccitt_false(const uint8_t *data, size_t len);

size_t proto_packet_header_len(uint16_t payload_len);
size_t proto_packet_encoded_len(uint16_t payload_len);
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

int command_result_id_append_tlvs(uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *offset,
                                  const struct command_result_id *id);
int command_result_id_from_tlvs(const uint8_t *payload,
                                size_t payload_len,
                                struct command_result_id *id);
int result_offer_append_tlvs(uint8_t *payload,
                             size_t payload_cap,
                             size_t *offset,
                             const struct result_offer *offer);
int result_offer_from_tlvs(const uint8_t *payload,
                           size_t payload_len,
                           struct result_offer *offer);
int result_grant_append_tlvs(uint8_t *payload,
                             size_t payload_cap,
                             size_t *offset,
                             const struct result_grant *grant);
int result_grant_from_tlvs(const uint8_t *payload,
                           size_t payload_len,
                           struct result_grant *grant);
int result_busy_append_tlvs(uint8_t *payload,
                            size_t payload_cap,
                            size_t *offset,
                            const struct result_busy *busy);
int result_busy_from_tlvs(const uint8_t *payload,
                          size_t payload_len,
                          struct result_busy *busy);
int result_bundle_header_append_tlvs(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *offset,
                                     const struct result_bundle_header *bundle);
int result_bundle_header_from_tlvs(const uint8_t *payload,
                                   size_t payload_len,
                                   struct result_bundle_header *bundle);
int result_bundle_record_append_tlv(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *offset,
                                    const struct result_bundle_record *record);
int result_bundle_record_next_from_tlvs(const uint8_t *payload,
                                        size_t payload_len,
                                        size_t *offset,
                                        struct result_bundle_record *record);
int gateway_collection_eack_append_tlvs(uint8_t *payload,
                                        size_t payload_cap,
                                        size_t *offset,
                                        const struct gateway_collection_eack *eack);
int gateway_collection_eack_from_tlvs(const uint8_t *payload,
                                      size_t payload_len,
                                      struct gateway_collection_eack *eack);
int gateway_collection_eack_contains_node_id(const uint8_t *payload,
                                             size_t payload_len,
                                             uint64_t node_id,
                                             bool *contains);

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

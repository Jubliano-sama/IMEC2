#ifndef UWB_H
#define UWB_H

#include "protocol.h"
#include "uwb_rf_scope.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UWB_MARKER 0xCAu
#define UWB_VERSION 0x01u
#define UWB_CHANNEL_WAKE_CONTACT 5u
#define UWB_CHANNEL_MESH_PAYLOAD 9u
#define UWB_SYNC_HEADER_LEN 3u
#define UWB_FRAME_CRC_LEN 2u
#define UWB_HEADER_LEN 42u
#define UWB_POLL_LEN UWB_HEADER_LEN
#define UWB_CLICK_POLL_LEN (UWB_HEADER_LEN + 4u)
#define UWB_POLL_METADATA_VERSION_NONE 0u
#define UWB_POLL_METADATA_VERSION_CLICK_AGE 1u
#define UWB_POLL_METADATA_CLICK_AGE_PRESENT (1u << 0)
#define UWB_POLL_METADATA_CLICK_AGE_SATURATED (1u << 1)
#define UWB_CLICK_AGE_MAX_MS UINT16_MAX
#define UWB_RESP_LEN (UWB_HEADER_LEN + 8u)
#define UWB_FINAL_LEN (UWB_HEADER_LEN + 15u)
#define UWB_FINAL_DIAG_CLICKER_CLOCK_OFFSET_PRESENT (1u << 0)
#define UWB_REPORT_LEN (UWB_HEADER_LEN + 8u)
#define UWB_CLICKER_DIAG_FIXED_LEN (UWB_HEADER_LEN + 15u)
#define UWB_CLICKER_DIAG_MAX_BYTES 48u
#define UWB_CLICKER_DIAG_MAX_LEN (UWB_CLICKER_DIAG_FIXED_LEN + UWB_CLICKER_DIAG_MAX_BYTES)
#define UWB_CLICKER_DIAG_STATUS_RESP_RX_PRESENT (1u << 0)
#define UWB_CLICKER_DIAG_STATUS_RESP_RSL_PRESENT (1u << 1)
#define UWB_CLICKER_DIAG_STATUS_COMPACT_BYTES_PRESENT (1u << 2)
#define UWB_ANCHOR_DIAG_FIXED_LEN (UWB_HEADER_LEN + 43u)
#define UWB_ANCHOR_DIAG_MAX_BYTES 40u
#define UWB_ANCHOR_DIAG_MAX_LEN (UWB_ANCHOR_DIAG_FIXED_LEN + UWB_ANCHOR_DIAG_MAX_BYTES)
#define UWB_ANCHOR_DIAG_STATUS_RSL_PRESENT (1u << 0)
#define UWB_ANCHOR_DIAG_STATUS_CIR_SAMPLE_PRESENT (1u << 1)
#define UWB_ANCHOR_DIAG_STATUS_CLOCK_OFFSET_PRESENT (1u << 2)
#define UWB_ANCHOR_DIAG_STATUS_CARRIER_INTEGRATOR_PRESENT (1u << 3)
#define UWB_ANCHOR_DIAG_STATUS_RAW_TIMESTAMPS_PRESENT (1u << 4)
#define UWB_ANCHOR_DIAG_STATUS_RX_DIAG_PRESENT (1u << 5)
#define UWB_ANCHOR_DIAG_STATUS_FULL_CIR_PRESENT (1u << 6)
#define UWB_ANCHOR_DIAG_FRAGMENT_FIXED_LEN (UWB_HEADER_LEN + 11u)
#define UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES 64u
#define UWB_ANCHOR_DIAG_FRAGMENT_MAX_LEN \
    (UWB_ANCHOR_DIAG_FRAGMENT_FIXED_LEN + UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES)
#define UWB_ANCHOR_DIAG_FRAGMENT_BLOCK_CIR 1u
#define UWB_ANCHOR_DIAG_FRAGMENT_BLOCK_RX_DIAG 2u
#define UWB_ANCHOR_DIAG_FRAGMENT_FLAG_LAST (1u << 0)
#define UWB_WAKE_CLAIM_LEN 49u
#define UWB_DISCOVER_LEN 32u
#define UWB_DISCOVERY_REPLY_LEN 44u
#define UWB_DISCOVERY_SLOT_COUNT 50u
#define UWB_RANGE_RELEASE_LEN 34u
#define UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS 1u
#define UWB_ENUM_RECORD_LEN 9u
#define UWB_ENUM_RECORDS_PER_BUNDLE 10u
#define UWB_ENUM_MAX_HOPS 8u
#define UWB_ENUM_BUNDLE_BASE_LEN 31u
#define UWB_ENUM_BUNDLE_MAX_LEN \
    (UWB_ENUM_BUNDLE_BASE_LEN + \
     (UWB_ENUM_RECORDS_PER_BUNDLE * UWB_ENUM_RECORD_LEN))
#define UWB_ENUM_HOP_ACK_LEN 30u
#define UWB_WAKE_CLAIM_MAX_WAKE_TRAIN_MS 1000u
#define UWB_WAKE_CLAIM_MAX_DISCOVERY_START_MS 1000u
#define UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS 2000u
/*
 * A retransmitted claim carries countdowns sampled before RF transmission,
 * while the anchor timestamps it after reception.  Allow bounded transport
 * skew, but compare every copy with the first accepted absolute schedule so
 * repeated frames cannot move the discovery or ownership horizon.
 */
#define UWB_WAKE_CLAIM_RETRANSMIT_SKEW_MS 25u
#define UWB_RANGE_SCHEDULE_FIXED_LEN 47u
#define UWB_RANGE_SCHEDULE_ENTRY_LEN 9u
#define UWB_RANGE_SCHEDULE_MAX_ANCHORS 8u
#ifndef UWB_NORMAL_CLICK_MIN_ANCHORS
#define UWB_NORMAL_CLICK_MIN_ANCHORS 3u
#endif
#define UWB_NORMAL_CLICK_MAX_ANCHORS 4u
#define UWB_RANGE_SCHEDULE_MIN_LEN \
    (UWB_RANGE_SCHEDULE_FIXED_LEN + UWB_FRAME_CRC_LEN)
#define UWB_RANGE_SCHEDULE_MAX_LEN \
    (UWB_RANGE_SCHEDULE_FIXED_LEN + \
     (UWB_RANGE_SCHEDULE_ENTRY_LEN * UWB_RANGE_SCHEDULE_MAX_ANCHORS) + \
     UWB_FRAME_CRC_LEN)
#define UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS 50u
#define UWB_RANGE_SCHEDULE_MIN_BURST_WINDOW_MS 400u
#define UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS 400u
#define UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US 50000u
#define UWB_RANGE_SCHEDULE_SINGLE_ANCHOR_MIN_EXCHANGE_STRIDE_US 50000u
#define UWB_RANGE_SCHEDULE_STS_DISABLED 0u
#define UWB_RANGE_SCHEDULE_DIAGNOSTICS_OMITTED 0u
#define UWB_RANGE_SCHEDULE_DIAGNOSTICS_REQUIRED 1u
#define UWB_RANGING_REQUESTS_MAX_PER_ANCHOR 100u
/*
 * Provisional DS-TWR delayed-TX presets from bring-up. The main firmware runs
 * the long-range PHY, so the protocol currently advertises the long-range
 * value. Both presets must be recalibrated on the final firmware path before
 * treating either value as a timing guarantee.
 */
#define UWB_RANGE_REPLY_DELAY_SHORT_RANGE_UUS 2750u
#define UWB_RANGE_REPLY_DELAY_LONG_RANGE_UUS 8000u
#define UWB_RANGE_REPLY_DELAY_UUS UWB_RANGE_REPLY_DELAY_LONG_RANGE_UUS
#define UWB_DS_TWR_REPLY_DELAY_US UWB_RANGE_REPLY_DELAY_UUS
#define UWB_DS_TWR_REPLY_DELAY_MIN_US UWB_DS_TWR_REPLY_DELAY_US
#define UWB_DS_TWR_REPLY_DELAY_MAX_US UWB_DS_TWR_REPLY_DELAY_US
#define UWB_MESH_FRAME_HEADER_LEN 25u
#define UWB_PHY_EXTENDED_FRAME_MAX_LEN 1023u
#define UWB_PHY_FCS_LEN 2u
#define UWB_PHY_EXTENDED_PAYLOAD_MAX_LEN \
    (UWB_PHY_EXTENDED_FRAME_MAX_LEN - UWB_PHY_FCS_LEN - \
     UWB_RF_SCOPE_WIRE_LEN)
#define UWB_MESH_MAX_PACKET_LEN \
    (UWB_PHY_EXTENDED_PAYLOAD_MAX_LEN - UWB_MESH_FRAME_HEADER_LEN - \
     UWB_FRAME_CRC_LEN)
#define UWB_MESH_MAX_PAYLOAD_LEN PACKET_EXT_MAX_PAYLOAD_LEN
#define UWB_MESH_MAX_FRAME_LEN \
    (UWB_MESH_FRAME_HEADER_LEN + UWB_MESH_MAX_PACKET_LEN + UWB_FRAME_CRC_LEN)

#if PACKET_EXT_MAX_LEN > UWB_MESH_MAX_PACKET_LEN
#error "Extended protocol packets must fit in one channel-9 UWB mesh frame"
#endif

#define UWB_DISCOVERY_REPLY_PRESENT 0x01u
#define UWB_DISCOVERY_REPLY_BUSY 0x02u
#define UWB_DISCOVERY_REPLY_COLLISION 0x03u

struct uwb_range_header {
    uint8_t type;
    uint8_t seq;
    uint8_t round_index;
    uint32_t network_id;
    uint32_t session_id;
    uint64_t session_nonce;
    uint16_t initiator_short_addr;
    uint16_t responder_short_addr;
    uint8_t flags;
    uint64_t initiator_id;
    uint64_t responder_id;
};

/*
 * A normal-click POLL is the first frame of each DS-TWR exchange.  It carries
 * the clicker's current event age so every responding anchor can project the
 * physical button instant into its own uptime domain. Diagnostic POLLs retain
 * the compact header-only representation.
 */
struct uwb_poll_frame {
    struct uwb_range_header header;
    uint8_t metadata_version;
    uint8_t metadata_flags;
    uint16_t click_age_ms;
};

struct uwb_response_frame {
    struct uwb_range_header header;
    uint32_t poll_rx_ts_32;
    uint32_t resp_tx_ts_32;
};

struct uwb_final_frame {
    struct uwb_range_header header;
    uint32_t poll_tx_ts_32;
    uint32_t resp_rx_ts_32;
    uint32_t final_tx_ts_32;
    uint8_t diagnostic_flags;
    int16_t clicker_clock_offset_raw;
};

struct uwb_report_frame {
    struct uwb_range_header header;
    int32_t distance_mm;
    uint8_t quality;
    enum range_status status;
    int8_t rsl_dbm;
};

struct uwb_clicker_diag_frame {
    struct uwb_range_header header;
    uint32_t final_tx_ts_32;
    uint32_t status_flags;
    uint32_t status_detect_latency_us;
    uint8_t resp_quality;
    int8_t resp_rsl_dbm;
    uint8_t diag_len;
    uint8_t diag_bytes[UWB_CLICKER_DIAG_MAX_BYTES];
};

struct uwb_anchor_diag_frame {
    struct uwb_range_header header;
    int32_t distance_mm;
    uint8_t quality;
    enum range_status status;
    int8_t rsl_dbm;
    uint32_t status_flags;
    int16_t clock_offset_raw;
    int32_t carrier_integrator;
    uint32_t poll_rx_ts_32;
    uint32_t resp_tx_ts_32;
    uint32_t final_rx_ts_32;
    uint32_t poll_tx_ts_32;
    uint32_t resp_rx_ts_32;
    uint32_t final_tx_ts_32;
    uint8_t diag_len;
    uint8_t diag_bytes[UWB_ANCHOR_DIAG_MAX_BYTES];
};

struct uwb_anchor_diag_fragment_frame {
    struct uwb_range_header header;
    uint8_t block_type;
    uint16_t offset;
    uint16_t total_len;
    uint16_t first_path_index;
    uint8_t fragment_index;
    uint8_t fragment_count;
    uint8_t flags;
    uint8_t chunk_len;
    uint8_t chunk[UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES];
};

struct uwb_wake_claim_frame {
    uint32_t network_id;
    uint64_t clicker_id;
    uint32_t click_event_id;
    uint8_t attempt_index;
    uint64_t priority_id;
    uint8_t wake_channel;
    uint8_t ranging_channel;
    uint16_t wake_train_ends_in_ms;
    uint16_t discovery_starts_in_ms;
    uint16_t claimed_duration_ms;
    uint8_t min_anchor_count;
    uint8_t max_anchor_count;
    uint64_t nonce;
    uint8_t flags;
};

struct uwb_discover_frame {
    uint32_t network_id;
    uint64_t clicker_id;
    uint32_t click_event_id;
    uint8_t attempt_index;
    uint64_t nonce;
    uint8_t discovery_slot_count;
    uint8_t flags;
};

struct uwb_discovery_reply_frame {
    uint32_t network_id;
    uint64_t anchor_id;
    uint64_t selected_clicker_id;
    uint32_t click_event_id;
    uint8_t attempt_index;
    uint64_t nonce;
    uint8_t anchor_slot;
    uint8_t status;
    uint8_t rx_quality;
    uint16_t battery_mv;
    uint8_t flags;
};

struct uwb_range_schedule_entry {
    uint64_t anchor_id;
    uint8_t seq;
    uint8_t sample_count;
};

struct uwb_range_schedule_frame {
    uint32_t network_id;
    uint64_t clicker_id;
    uint32_t click_event_id;
    uint8_t attempt_index;
    uint64_t nonce;
    uint8_t selected_count;
    uint8_t ranging_channel;
    uint16_t reply_delay_us;
    uint16_t first_poll_delay_ms;
    uint16_t poll_spacing_ms;
    uint16_t burst_window_ms;
    uint16_t exchange_stride_us;
    uint16_t max_exchanges;
    uint8_t min_successful_unique_anchors;
    uint8_t sts_mode;
    uint8_t diagnostics_required;
    uint8_t samples_per_anchor;
    uint8_t flags;
    struct uwb_range_schedule_entry entries[UWB_RANGE_SCHEDULE_MAX_ANCHORS];
};

struct uwb_range_release_frame {
    uint32_t network_id;
    uint64_t clicker_id;
    uint32_t click_event_id;
    uint8_t attempt_index;
    uint64_t nonce;
    uint8_t discovered_anchor_count;
    uint8_t min_anchor_count;
    uint8_t reason;
    uint8_t flags;
};

struct uwb_enumeration_record {
    uint64_t anchor_id;
    uint8_t hop_count;
};

struct uwb_enumeration_bundle_frame {
    uint32_t network_id;
    uint32_t epoch;
    uint64_t sender_id;
    uint64_t parent_id;
    uint8_t sequence;
    uint8_t record_count;
    struct uwb_enumeration_record records[UWB_ENUM_RECORDS_PER_BUNDLE];
};

struct uwb_enumeration_hop_ack_frame {
    uint32_t network_id;
    uint32_t epoch;
    uint64_t parent_id;
    uint64_t child_id;
    uint8_t sequence;
};

enum uwb_anchor_claim_decision {
    UWB_ANCHOR_CLAIM_ACCEPTED = 0,
    UWB_ANCHOR_CLAIM_REJECTED_STALE = 1,
    UWB_ANCHOR_CLAIM_REJECTED_BUSY = 2,
    UWB_ANCHOR_CLAIM_REJECTED_LOST_ARBITRATION = 3,
    UWB_ANCHOR_CLAIM_REPLACED_BY_PRIORITY = 4,
    UWB_ANCHOR_CLAIM_REJECTED_MALFORMED = 5,
};

struct uwb_anchor_epoch {
    bool active;
    uint32_t network_id;
    uint64_t clicker_id;
    uint32_t click_event_id;
    uint8_t attempt_index;
    uint64_t priority_id;
    uint64_t nonce;
    uint32_t epoch_ends_at_ms;
    uint32_t wake_train_ends_at_ms;
    uint16_t post_wake_claimed_duration_ms;
    uint16_t discovery_after_wake_ms;
    uint8_t wake_channel;
    uint8_t ranging_channel;
    uint8_t min_anchor_count;
    uint8_t max_anchor_count;
    uint8_t flags;
};

bool uwb_frame_type_valid(uint8_t type);
int uwb_header_validate(const struct uwb_range_header *header, uint8_t expected_type);
int uwb_encode_poll(const struct uwb_range_header *header,
                         uint8_t *out,
                         size_t out_cap,
                         size_t *written);
int uwb_encode_click_poll(const struct uwb_range_header *header,
                          uint32_t click_age_ms,
                          uint8_t *out,
                          size_t out_cap,
                          size_t *written);
int uwb_decode_poll(const uint8_t *data,
                         size_t len,
                         struct uwb_range_header *header);
int uwb_decode_poll_frame(const uint8_t *data,
                          size_t len,
                          struct uwb_poll_frame *frame);
int uwb_encode_response(const struct uwb_response_frame *frame,
                             uint8_t *out,
                             size_t out_cap,
                             size_t *written);
int uwb_decode_response(const uint8_t *data,
                             size_t len,
                             struct uwb_response_frame *frame);
int uwb_encode_final(const struct uwb_final_frame *frame,
                          uint8_t *out,
                          size_t out_cap,
                          size_t *written);
int uwb_decode_final(const uint8_t *data,
                          size_t len,
                          struct uwb_final_frame *frame);
int uwb_encode_report(const struct uwb_report_frame *frame,
                           uint8_t *out,
                           size_t out_cap,
                           size_t *written);
int uwb_decode_report(const uint8_t *data,
                           size_t len,
                           struct uwb_report_frame *frame);
int uwb_encode_clicker_diag(const struct uwb_clicker_diag_frame *frame,
                            uint8_t *out,
                            size_t out_cap,
                            size_t *written);
int uwb_decode_clicker_diag(const uint8_t *data,
                            size_t len,
                            struct uwb_clicker_diag_frame *frame);
int uwb_encode_anchor_diag(const struct uwb_anchor_diag_frame *frame,
                           uint8_t *out,
                           size_t out_cap,
                           size_t *written);
int uwb_decode_anchor_diag(const uint8_t *data,
                           size_t len,
                           struct uwb_anchor_diag_frame *frame);
int uwb_encode_anchor_diag_fragment(const struct uwb_anchor_diag_fragment_frame *frame,
                                    uint8_t *out,
                                    size_t out_cap,
                                    size_t *written);
int uwb_decode_anchor_diag_fragment(const uint8_t *data,
                                    size_t len,
                                    struct uwb_anchor_diag_fragment_frame *frame);

int uwb_encode_wake_claim(const struct uwb_wake_claim_frame *frame,
                          uint8_t *out,
                          size_t out_cap,
                          size_t *written);
int uwb_decode_wake_claim(const uint8_t *data,
                          size_t len,
                          struct uwb_wake_claim_frame *frame);
int uwb_validate_wake_claim(const struct uwb_wake_claim_frame *frame);
int uwb_encode_discover(const struct uwb_discover_frame *frame,
                        uint8_t *out,
                        size_t out_cap,
                        size_t *written);
int uwb_decode_discover(const uint8_t *data,
                        size_t len,
                        struct uwb_discover_frame *frame);
int uwb_encode_discovery_reply(const struct uwb_discovery_reply_frame *frame,
                               uint8_t *out,
                               size_t out_cap,
                               size_t *written);
int uwb_decode_discovery_reply(const uint8_t *data,
                               size_t len,
                               struct uwb_discovery_reply_frame *frame);
int uwb_encode_range_schedule(const struct uwb_range_schedule_frame *frame,
                              uint8_t *out,
                              size_t out_cap,
                              size_t *written);
int uwb_decode_range_schedule(const uint8_t *data,
                              size_t len,
                              struct uwb_range_schedule_frame *frame);
int uwb_validate_range_schedule(const struct uwb_range_schedule_frame *frame);
int uwb_encode_range_release(const struct uwb_range_release_frame *frame,
                             uint8_t *out,
                             size_t out_cap,
                             size_t *written);
int uwb_decode_range_release(const uint8_t *data,
                             size_t len,
                             struct uwb_range_release_frame *frame);
size_t uwb_enumeration_bundle_encoded_len(uint8_t record_count);
int uwb_encode_enumeration_bundle(
    const struct uwb_enumeration_bundle_frame *frame,
    uint8_t *out,
    size_t out_cap,
    size_t *written);
int uwb_decode_enumeration_bundle(
    const uint8_t *data,
    size_t len,
    struct uwb_enumeration_bundle_frame *frame);
int uwb_encode_enumeration_hop_ack(
    const struct uwb_enumeration_hop_ack_frame *frame,
    uint8_t *out,
    size_t out_cap,
    size_t *written);
int uwb_decode_enumeration_hop_ack(
    const uint8_t *data,
    size_t len,
    struct uwb_enumeration_hop_ack_frame *frame);
int uwb_validate_range_release(const struct uwb_range_release_frame *frame);
int uwb_discovery_slot_for_anchor(uint64_t anchor_id,
                                  uint8_t slot_count,
                                  uint8_t *anchor_slot);
size_t uwb_range_schedule_encoded_len(uint8_t selected_count);
size_t uwb_range_schedule_total_samples(const struct uwb_range_schedule_frame *frame);
int uwb_range_schedule_sample_at(const struct uwb_range_schedule_frame *frame,
                                 size_t sample_index,
                                 uint64_t *anchor_id,
                                 uint8_t *seq);
int uwb_claim_precedence_compare(uint8_t left_attempt_index,
                                 uint64_t left_priority_id,
                                 uint64_t left_clicker_id,
                                 uint32_t left_click_event_id,
                                 uint8_t right_attempt_index,
                                 uint64_t right_priority_id,
                                 uint64_t right_clicker_id,
                                 uint32_t right_click_event_id);

int uwb_mesh_frame_encode(uint32_t network_id,
                          uint64_t previous_hop_id,
                          uint64_t next_hop_id,
                          const struct proto_packet *packet,
                          const uint8_t *payload,
                          uint8_t *out,
                          size_t out_cap,
                          size_t *written);
int uwb_mesh_frame_decode(const uint8_t *data,
                          size_t len,
                          uint32_t expected_network_id,
                          uint64_t local_id,
                          uint64_t *previous_hop_id,
                          struct proto_packet *packet,
                          uint8_t *payload,
                          size_t payload_cap,
                          size_t *payload_len);

void uwb_anchor_epoch_clear(struct uwb_anchor_epoch *epoch);
int uwb_anchor_epoch_consider_claim(struct uwb_anchor_epoch *epoch,
                                    const struct uwb_wake_claim_frame *claim,
                                    uint32_t now_ms,
                                    enum uwb_anchor_claim_decision *decision);
bool uwb_anchor_epoch_matches(const struct uwb_anchor_epoch *epoch,
                              uint32_t network_id,
                              uint64_t clicker_id,
                              uint32_t click_event_id,
                              uint8_t attempt_index,
                              uint64_t nonce);

#ifdef __cplusplus
}
#endif

#endif

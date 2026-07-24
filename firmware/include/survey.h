#ifndef SURVEY_H
#define SURVEY_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SURVEY_MIN_SAMPLE_COUNT 1u
#define SURVEY_MAX_SAMPLE_COUNT 1000u
#define SURVEY_MIN_USABLE_DISTANCE_MM 50
#define SURVEY_GATEWAY_PAIR_MAX_RERUNS 2u
/*
 * The wire format permits larger surveys, but the connected mesh runtime can
 * have one bounded reliable uplink per sample in flight. This value is a
 * cross-role contract: gateways must not admit work that mesh anchors cannot
 * execute without an explicit delivery outcome.
 */
#define SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT 4u
#define SURVEY_DEFAULT_TTL 4u
#define SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS 600000u
/*
 * A known reverse path gets a route-depth-aware natural control deadline.
 * The base covers one complete gateway-ACK custody/retry horizon; each
 * additional RF hop reserves another bounded relay/control interval. Unknown
 * or invalid route depth deliberately retains the established 90 s ceiling.
 */
#define SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS 30000u
#define SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS 15000u
#define SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS 90000u
#define SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS 5000u
#define SURVEY_PAIR_INITIATOR_TIMEOUT_MS 150u
#define SURVEY_PAIR_START_SKEW_MARGIN_MS 1000u
/*
 * START transport may take many seconds across the mesh, but matching round
 * GO gives both endpoints one shared execution time. The channel-5 RX window
 * therefore covers only local scheduling skew plus one DS-TWR attempt; it
 * must never inherit a multi-hop command-result timeout.
 */
#define SURVEY_PAIR_RESPONDER_WINDOW_MS                                      \
    (SURVEY_PAIR_START_SKEW_MARGIN_MS + SURVEY_PAIR_INITIATOR_TIMEOUT_MS)
#define SURVEY_PAIR_CONTROL_CLEANUP_MARGIN_MS 30000u
#define SURVEY_PAIR_PREPARED_LEASE_MS 660000u
#if SURVEY_PAIR_INITIATOR_TIMEOUT_MS >                                       \
    (UINT32_MAX - SURVEY_PAIR_START_SKEW_MARGIN_MS)
#error "Survey pair responder window overflows uint32_t"
#endif
#if SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS > 2147483647u
#error "Survey pair control timeout must fit wrap-safe signed time arithmetic"
#endif
#if SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS == 0u ||                          \
    SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS == 0u
#error "Survey pair control route-depth budgets must be positive"
#endif
#if SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS >                                \
    (UINT32_MAX - ((SURVEY_DEFAULT_TTL - 1u) *                           \
                   SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS))
#error "Survey pair route-depth timeout overflows uint32_t"
#endif
#if (SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS +                               \
     ((SURVEY_DEFAULT_TTL - 1u) *                                       \
      SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS)) >                         \
    SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS
#error "Known survey route-depth timeout exceeds the fallback ceiling"
#endif
#if SURVEY_PAIR_RESPONDER_WINDOW_MS <= SURVEY_PAIR_START_SKEW_MARGIN_MS
#error "Survey pair responder window must include one DS-TWR attempt"
#endif
#if SURVEY_PAIR_PREPARED_LEASE_MS > 2147483647u
#error "Survey pair prepared lease must fit wrap-safe signed time arithmetic"
#endif
#if SURVEY_PAIR_PREPARED_LEASE_MS <                                      \
    (SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS +                       \
     SURVEY_PAIR_CONTROL_CLEANUP_MARGIN_MS)
#error "Survey pair prepared lease cannot cover one gateway operation and cleanup"
#endif
#define SURVEY_REACHABILITY_ENTRY_LEN 10u
#define SURVEY_GATEWAY_MAX_REPORTS 50u
#define SURVEY_GATEWAY_MAX_PEERS_PER_REPORT 12u
#define SURVEY_REACH_REPORT_MAX_PAYLOAD_LEN                              \
    (PROTO_TLV_U32_ENCODED_LEN + PROTO_TLV_U64_ENCODED_LEN +           \
     PROTO_TLV_U16_ENCODED_LEN +                                        \
     SURVEY_GATEWAY_MAX_PEERS_PER_REPORT *                              \
         (PROTO_TLV_HEADER_LEN + SURVEY_REACHABILITY_ENTRY_LEN))
#if SURVEY_REACH_REPORT_MAX_PAYLOAD_LEN > PACKET_MAX_PAYLOAD_LEN
#error "Maximum survey reachability report exceeds the standard packet payload"
#endif
#define SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR 6u
#define SURVEY_GATEWAY_MAX_PAIRS \
    ((SURVEY_GATEWAY_MAX_REPORTS * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2u)
#define SURVEY_DISCOVERY_MAX_SLOT_COUNT 50u
#define SURVEY_ML_ANCHOR_PAIR_MIN_DISCOVERY_SLOT_COUNT 2u
#define SURVEY_ML_ANCHOR_PAIR_MAX_DISCOVERY_SLOT_COUNT 8u
#define SURVEY_DISCOVERY_RX_GUARD_MS 8u
#define SURVEY_DISCOVERY_TX_TIMEOUT_MS 20u
#define SURVEY_DISCOVERY_TX_TRANSITION_GUARD_MS 2u
#define SURVEY_DISCOVERY_MIN_SLOT_MS 30u
#define SURVEY_DISCOVERY_MAX_SLOT_MS 1000u
#define SURVEY_DISCOVERY_MAX_START_DELAY_MS 60000u
#define SURVEY_DISCOVERY_MAX_ROUND_COUNT 4u
#define SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS 5000u
#define SURVEY_DISCOVERY_REPORT_CUSTODY_PER_ADDITIONAL_HOP_MS 4000u
#define SURVEY_DISCOVERY_REPORT_CUSTODY_MAX_MS \
    (SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS + \
     ((SURVEY_DEFAULT_TTL - 1u) * \
      SURVEY_DISCOVERY_REPORT_CUSTODY_PER_ADDITIONAL_HOP_MS))
#define SURVEY_DISCOVERY_REPORT_RETRY_INITIAL_MS 50u
#define SURVEY_DISCOVERY_REPORT_RETRY_MAX_MS 500u

struct survey_reachability_entry {
    uint64_t peer_id;
    int8_t rssi_dbm;
    uint8_t quality;
} __attribute__((packed));

_Static_assert(sizeof(struct survey_reachability_entry) ==
                   SURVEY_REACHABILITY_ENTRY_LEN,
               "survey reachability storage must not carry alignment padding");

struct survey_pair {
    uint64_t initiator_id;
    uint64_t responder_id;
    uint32_t survey_id;
    uint16_t sample_count;
};

struct survey_sample {
    struct survey_pair pair;
    /* Nonzero synchronized-round generation; zero is legacy serialization. */
    uint16_t round_id;
    uint16_t sample_index;
    int32_t distance_mm;
    uint8_t quality;
    enum range_status range_status;
};

#define SURVEY_SAMPLE_TLV_MAX_LEN                                         \
    (2u * PROTO_TLV_U64_ENCODED_LEN + 2u * PROTO_TLV_U32_ENCODED_LEN +   \
     3u * PROTO_TLV_U16_ENCODED_LEN + 2u * PROTO_TLV_U8_ENCODED_LEN)

struct survey_reachability_report {
    uint64_t anchor_id;
    const struct survey_reachability_entry *entries;
    size_t entry_count;
};

struct survey_discovery_config {
    uint32_t survey_id;
    uint32_t start_delay_ms;
    uint16_t slot_ms;
    uint8_t slot_count;
    uint8_t round_count;
};

struct survey_ml_anchor_pair_request {
    uint8_t discovery_slot_count;
};

struct survey_discovery_timing {
    uint32_t wait_ms;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    bool pending;
    bool active;
    bool expired;
};

struct survey_discovery_attempt_schedule {
    uint32_t window_start_ms;
    uint32_t tx_ms;
    uint32_t latest_tx_start_ms;
    uint32_t slot_end_ms;
    uint32_t window_end_ms;
};

enum survey_pending_report_action {
    SURVEY_PENDING_REPORT_IDLE = 0,
    SURVEY_PENDING_REPORT_WAIT,
    SURVEY_PENDING_REPORT_ATTEMPT,
    SURVEY_PENDING_REPORT_EXPIRED,
};

struct survey_pending_report_state {
    uint32_t survey_id;
    uint32_t deadline_ms;
    uint32_t next_attempt_ms;
    uint16_t retry_count;
    bool active;
};

struct survey_gateway_reverse_hint {
    uint64_t target_id;
    uint64_t next_hop_id;
    uint8_t quality;
    uint8_t hop_count;
    bool valid;
};

struct survey_gateway_report_slot {
    uint64_t anchor_id;
    struct survey_reachability_entry entries[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    uint64_t reverse_next_hop_id;
    size_t entry_count;
    uint8_t reverse_quality;
    uint8_t reverse_hop_count;
    bool reverse_hint_valid;
    bool valid;
};

struct survey_gateway_pair_entry {
    uint64_t initiator_id;
    uint64_t responder_id;
};

_Static_assert(sizeof(struct survey_gateway_pair_entry) == 16u,
               "gateway survey pair storage must contain endpoints only");

/*
 * Caller-owned launch metadata for one entry in the existing pair plan. The
 * planner never reorders the pair array: pairs with the same round index may
 * run together, in pair_index_in_round order.
 */
struct survey_pair_round_metadata {
    uint8_t round_index;
    uint8_t pair_index_in_round;
    uint8_t pair_count_in_round;
};

_Static_assert(SURVEY_GATEWAY_MAX_PAIRS <= UINT8_MAX,
               "survey round metadata indices must cover every pair");
_Static_assert(sizeof(struct survey_pair_round_metadata) == 3u,
               "survey round metadata must remain compact");

struct survey_gateway_context {
    uint32_t survey_id;
    uint16_t sample_count;
    struct survey_gateway_report_slot reports[SURVEY_GATEWAY_MAX_REPORTS];
    struct survey_gateway_pair_entry pairs[SURVEY_GATEWAY_MAX_PAIRS];
    size_t report_count;
    size_t pair_count;
    size_t next_pair_index;
    bool pairs_planned;
};

enum survey_gateway_auto_stage {
    SURVEY_GATEWAY_AUTO_IDLE = 0,
    SURVEY_GATEWAY_AUTO_LOAD_PAIR,
    SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR,
    SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER,
    SURVEY_GATEWAY_AUTO_START_RESPONDER,
    SURVEY_GATEWAY_AUTO_START_INITIATOR,
};

struct survey_gateway_auto_context {
    struct survey_pair pair;
    enum survey_gateway_auto_stage stage;
    uint8_t pair_reruns_started;
    bool running;
    bool waiting;
};

struct survey_gateway_auto_action {
    struct survey_pair pair;
    enum survey_gateway_auto_stage stage;
    enum command_id command_id;
    uint64_t target_id;
    bool complete;
};

bool survey_sample_count_valid(uint16_t sample_count);
int survey_pair_validate(const struct survey_pair *pair);
int survey_sample_validate(const struct survey_sample *sample);
bool survey_sample_distance_usable(const struct survey_sample *sample);
bool survey_pair_missing_samples_all_unusable(
    uint16_t sample_count,
    uint16_t usable_mask,
    uint16_t initiator_unusable_mask,
    uint16_t responder_unusable_mask);
uint64_t survey_sample_nonce(const struct survey_pair *pair, uint16_t sample_index);
int survey_reachability_entry_validate(const struct survey_reachability_entry *entry);
int survey_discovery_config_validate(const struct survey_discovery_config *config);
uint32_t survey_discovery_duration_ms(const struct survey_discovery_config *config);
uint8_t survey_discovery_opportunity_slot(uint64_t anchor_id,
                                          uint32_t survey_id,
                                          uint8_t opportunity,
                                          uint8_t slot_count);
int survey_discovery_opportunity_window_ms(
    const struct survey_discovery_config *config,
    uint8_t opportunity,
    uint32_t *start_ms,
    uint32_t *end_ms);
int survey_discovery_opportunity_tx_ms(
    const struct survey_discovery_config *config,
    uint64_t anchor_id,
    uint8_t opportunity,
    uint32_t *tx_ms);
uint32_t survey_discovery_probe_tx_budget_ms(void);
int survey_discovery_schedule_attempt(
    const struct survey_discovery_config *config,
    uint64_t anchor_id,
    uint8_t opportunity,
    uint32_t earliest_relative_ms,
    struct survey_discovery_attempt_schedule *schedule);
int survey_pending_report_begin(struct survey_pending_report_state *state,
                                uint32_t survey_id,
                                uint32_t now_ms,
                                uint32_t earliest_attempt_ms);
enum survey_pending_report_action survey_pending_report_action(
    const struct survey_pending_report_state *state,
    uint32_t now_ms);
uint32_t survey_pending_report_delay_ms(
    const struct survey_pending_report_state *state,
    uint32_t now_ms);
int survey_pending_report_note_temporary_failure(
    struct survey_pending_report_state *state,
    uint32_t now_ms);
void survey_pending_report_clear(struct survey_pending_report_state *state);
int survey_discovery_timing_from_age(const struct survey_discovery_config *config,
                                     uint32_t message_age_ms,
                                     struct survey_discovery_timing *timing);
int survey_discovery_start_at_ms(const struct survey_discovery_timing *timing,
                                 uint32_t received_at_ms,
                                 uint32_t *start_at_ms);
int survey_discovery_report_delay_ms(const struct survey_discovery_config *config,
                                     uint8_t anchor_slot,
                                     uint32_t report_slot_ms,
                                     uint32_t *delay_ms);
int survey_gateway_begin(struct survey_gateway_context *context,
                         uint32_t survey_id,
                         uint16_t sample_count);
int survey_gateway_note_reach_report(struct survey_gateway_context *context,
                                     uint32_t survey_id,
                                     uint64_t anchor_id,
                                     const struct survey_reachability_entry *entries,
                                     size_t entry_count);
int survey_gateway_note_reach_report_with_reverse_hint(
    struct survey_gateway_context *context,
    uint32_t survey_id,
    uint64_t anchor_id,
    const struct survey_reachability_entry *entries,
    size_t entry_count,
    const struct survey_gateway_reverse_hint *reverse_hint);
int survey_gateway_reverse_hint_for_target(
    const struct survey_gateway_context *context,
    uint64_t target_id,
    struct survey_gateway_reverse_hint *reverse_hint);
uint8_t survey_gateway_hop_count_from_report_ttl(uint8_t remaining_ttl);
uint32_t survey_pair_control_timeout_ms(uint8_t gateway_hop_count);
uint32_t survey_discovery_report_custody_ms(uint8_t gateway_hop_count);
int survey_extract_expected_node_count_tlv(const uint8_t *payload,
                                           size_t payload_len,
                                           uint16_t *expected_count,
                                           bool *present);

enum survey_gateway_collection_decision {
    SURVEY_GATEWAY_COLLECTION_WAIT = 0,
    SURVEY_GATEWAY_COLLECTION_CLOSE,
    SURVEY_GATEWAY_COLLECTION_COUNT_MISMATCH,
};

enum survey_gateway_collection_decision survey_gateway_collection_decide(
    bool emission_horizon_elapsed,
    bool safety_deadline_elapsed,
    size_t report_count,
    uint16_t expected_count,
    bool expected_present);
int survey_gateway_plan_pairs(struct survey_gateway_context *context);
/*
 * Packs the existing pair plan into deterministic concurrent rounds without
 * copying its reachability graph. Cross-pair conflicts are derived directly
 * from the first accepted report slots. Saturated peer lists use the retained
 * gateway hop depth only as a conservative fallback: a depth difference of at
 * least two proves separation, while unknown or adjacent depths serialize.
 */
int survey_gateway_plan_pair_rounds(
    const struct survey_gateway_context *context,
    struct survey_pair_round_metadata *metadata,
    size_t metadata_cap,
    size_t *round_count);
int survey_gateway_pair_at(const struct survey_gateway_context *context,
                           size_t pair_index,
                           struct survey_pair *pair);
int survey_gateway_next_pair(struct survey_gateway_context *context,
                             struct survey_pair *pair);
int survey_gateway_auto_begin(struct survey_gateway_auto_context *context);
int survey_gateway_auto_next_action(struct survey_gateway_auto_context *auto_context,
                                    struct survey_gateway_context *gateway_context,
                                    struct survey_gateway_auto_action *action);
bool survey_gateway_auto_no_unstarted_pairs(
    const struct survey_gateway_auto_context *auto_context,
    const struct survey_gateway_context *gateway_context);
int survey_gateway_auto_mark_waiting(struct survey_gateway_auto_context *context);
bool survey_gateway_auto_command_matches(const struct survey_gateway_auto_context *context,
                                         enum command_id command_id,
                                         uint64_t target_id,
                                         uint32_t survey_id);
int survey_gateway_auto_retry_pending(struct survey_gateway_auto_context *context,
                                      enum command_id command_id,
                                      uint64_t target_id,
                                      uint32_t survey_id);
int survey_gateway_auto_rerun_pair(
    struct survey_gateway_auto_context *context);
int survey_gateway_auto_note_result(struct survey_gateway_auto_context *context,
                                    enum command_id command_id,
                                    uint64_t target_id,
                                    uint32_t survey_id,
                                    enum command_status status,
                                    bool *pair_launched,
                                    bool *pair_skipped);
int survey_extract_reach_request_tlvs(const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t *survey_id,
                                      uint32_t *duration_ms);
int survey_extract_reach_report_tlvs(const uint8_t *payload,
                                     size_t payload_len,
                                     uint32_t *survey_id,
                                     uint64_t *anchor_id,
                                     struct survey_reachability_entry *entries,
                                     size_t entry_cap,
                                     size_t *entry_count);
int survey_extract_discovery_start_tlvs(const uint8_t *payload,
                                        size_t payload_len,
                                        struct survey_discovery_config *config);
int survey_extract_discovery_slot_count_tlv(const uint8_t *payload,
                                            size_t payload_len,
                                            uint8_t default_slot_count,
                                            uint8_t *slot_count);
int survey_extract_ml_anchor_pair_request_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t default_slot_count,
    struct survey_ml_anchor_pair_request *request);
int survey_extract_pair_tlvs(const uint8_t *payload,
                             size_t payload_len,
                             struct survey_pair *pair);
/*
 * Builds one connected, degree-bounded graph before adding preferred extra
 * pairs. Returns PROTO_ERR_NOT_FOUND when the reported reachability graph
 * cannot be connected without exceeding the per-anchor degree ceiling.
 */
int survey_plan_pairs_from_reachability(uint32_t survey_id,
                                        const struct survey_reachability_report *reports,
                                        size_t report_count,
                                        uint16_t sample_count,
                                        struct survey_pair *pairs,
                                        size_t pair_cap,
                                        size_t *pair_count);
int survey_append_reach_request_tlvs(uint8_t *payload,
                                          size_t payload_cap,
                                          size_t *offset,
                                          uint32_t survey_id,
                                          uint32_t duration_ms);
int survey_append_discovery_start_tlvs(uint8_t *payload,
                                       size_t payload_cap,
                                       size_t *offset,
                                       const struct survey_discovery_config *config);
int survey_append_reachability_entry_tlv(uint8_t *payload,
                                              size_t payload_cap,
                                              size_t *offset,
                                              const struct survey_reachability_entry *entry);
int survey_append_reach_report_tlvs(uint8_t *payload,
                                         size_t payload_cap,
                                         size_t *offset,
                                         uint32_t survey_id,
                                         uint64_t anchor_id,
                                         const struct survey_reachability_entry *entries,
                                         size_t entry_count);
int survey_append_pair_tlvs(uint8_t *payload,
                                 size_t payload_cap,
                                 size_t *offset,
                                 const struct survey_pair *pair);
int survey_append_sample_tlvs(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *offset,
                                   const struct survey_sample *sample);
int survey_extract_sample_tlvs(const uint8_t *payload,
                               size_t payload_len,
                               struct survey_sample *sample);
int survey_init_result_packet(struct proto_packet *packet,
                                   const struct survey_sample *sample,
                                   uint64_t gateway_id,
                                   uint16_t seq,
                                   uint8_t payload_len);
int survey_init_result_packet_from_reporter(struct proto_packet *packet,
                                            const struct survey_sample *sample,
                                            uint64_t reporter_id,
                                            uint64_t gateway_id,
                                            uint16_t seq,
                                            uint8_t payload_len);
int survey_init_reach_request_packet(struct proto_packet *packet,
                                     uint64_t gateway_id,
                                     uint32_t survey_id,
                                     uint16_t seq,
                                     uint8_t payload_len);
int survey_init_reach_report_packet(struct proto_packet *packet,
                                         uint64_t anchor_id,
                                         uint64_t gateway_id,
                                         uint32_t survey_id,
                                         uint16_t seq,
                                         uint8_t payload_len);
int survey_init_discovery_start_packet(struct proto_packet *packet,
                                       uint64_t gateway_id,
                                       const struct survey_discovery_config *config,
                                       uint16_t seq,
                                       uint8_t payload_len);
int survey_init_discovery_report_packet(struct proto_packet *packet,
                                        uint64_t anchor_id,
                                        uint64_t gateway_id,
                                        uint32_t survey_id,
                                        uint16_t seq,
                                        uint8_t payload_len);
int survey_init_pair_prepare_packet(struct proto_packet *packet,
                                    const struct survey_pair *pair,
                                    uint64_t gateway_id,
                                    uint16_t seq,
                                    uint8_t payload_len);

#ifdef __cplusplus
}
#endif

#endif

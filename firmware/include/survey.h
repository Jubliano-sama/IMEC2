#ifndef SURVEY_H
#define SURVEY_H

#include "protocol.h"
#include "node_comm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SURVEY_MIN_SAMPLE_COUNT 1u
#define SURVEY_MAX_SAMPLE_COUNT 1000u
#define SURVEY_MIN_USABLE_DISTANCE_MM 0
#define SURVEY_GATEWAY_PAIR_MAX_RERUNS 2u
/*
 * The wire format permits larger surveys, but the connected mesh runtime can
 * have one bounded reliable uplink per sample in flight. This value is a
 * cross-role contract: gateways must not admit work that mesh anchors cannot
 * execute without an explicit delivery outcome.
 */
#define SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT 5u
#define SURVEY_GATEWAY_MAX_REPORTS 50u
#define SURVEY_DEFAULT_TTL 4u
/*
 * A normal three-anchor survey must terminate as one bounded user action,
 * even when a report or pair control never settles. Larger experimental
 * fleets may still request a longer command budget explicitly.
 */
#define SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS 360000u
/*
 * A known reverse path gets a route-depth-aware natural control deadline.
 * The base covers one complete gateway-ACK custody/retry horizon; each
 * additional RF hop reserves another bounded relay/control interval. Unknown
 * or invalid route depth deliberately retains the established 90 s ceiling.
 */
#define SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS 30000u
#define SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS 15000u
#define SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS 90000u
/*
 * Redrive a terminal-but-unanswered pair control independently from its
 * route-depth failure deadline.  A one-second cadence leaves several full
 * 640 ms two-link radio turns between copies while keeping the four ordered
 * controls of a healthy pair inside the six-minute three-anchor operation.
 */
#define SURVEY_PAIR_CONTROL_REDRIVE_INTERVAL_MS 1000u
#define SURVEY_PAIR_ABORT_RESULT_TIMEOUT_MS 12000u
#define SURVEY_GATEWAY_RESPONSE_ACK_SETTLE_MS 3000u
#define SURVEY_PAIR_CONTROL_MAX_REQUEST_TIMEOUT_MS                     \
    (SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS +                            \
     ((SURVEY_DEFAULT_TTL - 1u) *                                    \
      SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS))
/*
 * Gateway host output deliberately has one bounded in-RAM owner, not an NVS
 * journal. A different survey record is therefore flow-controlled at the
 * gateway and remains in exact producer custody until the current item has
 * received its GUI receipt and the mesh ACK handoff owns the remaining work.
 * Size the gateway observation and one mesh-attempt planning horizon for the
 * conservative fleet envelope: any of 50 anchors may be a responder and one
 * responder emits five sample records. A synchronized batch is smaller
 * because an endpoint participates in at most one pair. The 500 ms accounting
 * interval is conservative for one bounded
 * pair-result record on the 30 ms BLE link; the remaining 20 s covers
 * notification/receipt retry, scheduling jitter, and the last ACK handoff.
 * This horizon never authorizes deleting an unconfirmed source
 * record: anchors retain bounded RAM slots until exact gateway proof, and
 * backpressure prevents a later survey from overwriting them. A gateway reset
 * does not restore the host item, but a still-running anchor retains its RAM
 * source for retry. An anchor reset drops that source deliberately and its new
 * boot incarnation terminates any affected old survey.
 */
#define SURVEY_GATEWAY_HOST_RECORD_SERVICE_BUDGET_MS 500u
#define SURVEY_PAIR_RESULT_MAX_BURST_RECORDS \
    (SURVEY_GATEWAY_MAX_REPORTS * SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT)
#define SURVEY_PAIR_RESULT_FLOW_CONTROL_GUARD_MS 20000u
#define SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS \
    ((SURVEY_PAIR_RESULT_MAX_BURST_RECORDS * \
      SURVEY_GATEWAY_HOST_RECORD_SERVICE_BUDGET_MS) + \
     SURVEY_PAIR_RESULT_FLOW_CONTROL_GUARD_MS)
#define SURVEY_PAIR_RESULT_DELIVERY_TIMEOUT_MS \
    SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS
#define SURVEY_MESH_RESULT_OUTBOX_EXPIRY_S \
    (((SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS + 999u) / 1000u) + 5u)
#define SURVEY_PAIR_CONTROL_RESULT_OUTBOX_EXPIRY_S \
    ((SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS + 999u) / 1000u)
#define SURVEY_PAIR_INITIATOR_TIMEOUT_MS 150u
#define SURVEY_PAIR_START_SKEW_MARGIN_MS 1000u
/*
 * START transport may take many seconds across the mesh, so the two START
 * controls carry one shared future execution time. The channel-5 RX window
 * covers only local scheduling skew plus one DS-TWR attempt; it must never
 * inherit a multi-hop command-result timeout.
 */
#define SURVEY_PAIR_RESPONDER_WINDOW_MS                                      \
    (SURVEY_PAIR_START_SKEW_MARGIN_MS + SURVEY_PAIR_INITIATOR_TIMEOUT_MS)
#define SURVEY_PAIR_CONTROL_CLEANUP_MARGIN_MS                         \
    (2u * (SURVEY_PAIR_CONTROL_MAX_REQUEST_TIMEOUT_MS +              \
           SURVEY_PAIR_ABORT_RESULT_TIMEOUT_MS))
#define SURVEY_DISCOVERY_DELIVERY_TERMINAL_POLL_MS 5u
#define SURVEY_DISCOVERY_OPERATION_TERMINAL_GUARD_MS 1u
#define SURVEY_PAIR_PREPARED_LEASE_MS                                \
    (SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS +                    \
     SURVEY_PAIR_CONTROL_CLEANUP_MARGIN_MS)
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
#if SURVEY_PAIR_CONTROL_REDRIVE_INTERVAL_MS == 0u ||                    \
    SURVEY_PAIR_CONTROL_REDRIVE_INTERVAL_MS >=                         \
        SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS
#error "Survey pair redrive must be positive and precede the shortest request deadline"
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
#define SURVEY_GATEWAY_MAX_PEERS_PER_REPORT 12u
#define SURVEY_REACH_REPORT_MAX_PAYLOAD_LEN                              \
    (2u * PROTO_TLV_U32_ENCODED_LEN + 2u * PROTO_TLV_U64_ENCODED_LEN + \
     PROTO_TLV_U16_ENCODED_LEN +                                        \
     SURVEY_GATEWAY_MAX_PEERS_PER_REPORT *                              \
         (PROTO_TLV_HEADER_LEN + SURVEY_REACHABILITY_ENTRY_LEN))
#if SURVEY_REACH_REPORT_MAX_PAYLOAD_LEN > PACKET_MAX_PAYLOAD_LEN
#error "Maximum survey reachability report exceeds the standard packet payload"
#endif
#define SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR 6u
#define SURVEY_GATEWAY_MAX_PAIRS \
    ((SURVEY_GATEWAY_MAX_REPORTS * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2u)
/*
 * One synchronized batch consumes one nonzero round generation. In the
 * automatic path every planned pair can run once and rerun at most twice.
 * Each responder participates in at most one pair per batch and emits at most
 * five sample identities, so source/session/sequence remains injective.
 */
#define SURVEY_PAIR_RESULT_MAX_BATCH_COUNT \
    (SURVEY_GATEWAY_MAX_PAIRS * (SURVEY_GATEWAY_PAIR_MAX_RERUNS + 1u))
#define SURVEY_PAIR_RESULT_TRANSPORT_SEQUENCE_MAX \
    (SURVEY_PAIR_RESULT_MAX_BATCH_COUNT * \
     SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT)
_Static_assert(SURVEY_PAIR_RESULT_MAX_BATCH_COUNT == 450u,
               "automatic survey must remain bounded to 450 pair batches");
_Static_assert(SURVEY_PAIR_RESULT_TRANSPORT_SEQUENCE_MAX == 2250u,
               "automatic survey pair-result identity space must remain 2250");
_Static_assert(SURVEY_PAIR_RESULT_TRANSPORT_SEQUENCE_MAX <= UINT16_MAX,
               "survey pair-result transport sequence must fit uint16_t");
#define SURVEY_DISCOVERY_MAX_SLOT_COUNT 50u
#define SURVEY_ML_ANCHOR_PAIR_MIN_DISCOVERY_SLOT_COUNT 2u
#define SURVEY_ML_ANCHOR_PAIR_MAX_DISCOVERY_SLOT_COUNT 8u
#define SURVEY_DISCOVERY_RX_GUARD_MS 8u
#define SURVEY_DISCOVERY_TX_TIMEOUT_MS 20u
#define SURVEY_DISCOVERY_TX_TRANSITION_GUARD_MS 2u
#define SURVEY_DISCOVERY_MIN_SLOT_MS 30u
#define SURVEY_DISCOVERY_MAX_SLOT_MS 1000u
#define SURVEY_DISCOVERY_MAX_START_DELAY_MS 90000u
#define SURVEY_DISCOVERY_CONTROL_HOP_BUDGET_MS 2000u
#define SURVEY_DISCOVERY_ORIGIN_REDRIVE_COUNT 4u
#define SURVEY_DISCOVERY_MAX_ROUND_COUNT 4u
#define SURVEY_DISCOVERY_REPORT_MAX_BURST_RECORDS \
    SURVEY_GATEWAY_MAX_REPORTS
/*
 * These bounds size gateway observation and one mesh-attempt planning window
 * for the legal 50-report burst. They never authorize deletion at the anchor:
 * an unconfirmed discovery report remains in its exact anchor-local RAM slot,
 * and later discovery generations are backpressured until gateway proof
 * arrives. Anchor reset drops the slot and the new boot incarnation
 * invalidates the affected gateway operation.
 */
#define SURVEY_DISCOVERY_REPORT_FLOW_CONTROL_GUARD_MS 5000u
#define SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS \
    ((SURVEY_DISCOVERY_REPORT_MAX_BURST_RECORDS * \
      SURVEY_GATEWAY_HOST_RECORD_SERVICE_BUDGET_MS) + \
     SURVEY_DISCOVERY_REPORT_FLOW_CONTROL_GUARD_MS)
#define SURVEY_DISCOVERY_REPORT_CUSTODY_PER_ADDITIONAL_HOP_MS 4000u
#define SURVEY_DISCOVERY_REPORT_CUSTODY_MAX_MS \
    (SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS + \
     ((SURVEY_DEFAULT_TTL - 1u) * \
      SURVEY_DISCOVERY_REPORT_CUSTODY_PER_ADDITIONAL_HOP_MS))
#define SURVEY_DISCOVERY_REPORT_RETRY_INITIAL_MS 50u
#define SURVEY_DISCOVERY_REPORT_RETRY_MAX_MS 500u
#define SURVEY_MAX_SUPPORTED_FORCED_DEPTH 2u
#define SURVEY_MAX_SERIAL_SLOT_HOLDERS 2u
#define SURVEY_MAX_CH9_HOLD_MS SURVEY_DISCOVERY_REPORT_CUSTODY_MAX_MS
#define SURVEY_ROUTE_BACKOFF_MAX_MS 1000u
#define SURVEY_ROUTE_TRANSFER_BUDGET_MS 5000u
#define SURVEY_ROUTE_ACQUIRE_BUDGET_MS \
    (SURVEY_MAX_SERIAL_SLOT_HOLDERS * \
     (SURVEY_MAX_CH9_HOLD_MS + SURVEY_ROUTE_BACKOFF_MAX_MS + SURVEY_ROUTE_TRANSFER_BUDGET_MS))

_Static_assert(SURVEY_PAIR_RESULT_MAX_BURST_RECORDS == 250u,
               "maximum synchronized survey burst must remain 250 records");
_Static_assert(SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS == 145000u,
               "survey pair-result flow-control horizon must remain 145 s");
_Static_assert(SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS == 30000u,
               "survey discovery flow-control horizon must remain 30 s");
_Static_assert(SURVEY_MESH_RESULT_OUTBOX_EXPIRY_S * 1000u >=
                   SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS + 5000u,
               "survey relay outbox must outlive pair-result custody");

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
    /*
     * Gateway-reserved operation identity. Zero is retained only for
     * bounded legacy/native helpers; production mesh handlers require it.
     */
    uint64_t operation_generation;
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

/*
 * Exact identity for the observation fields that may vary inside one already
 * validated reporter/pair/round/sample-index slot. The last byte is also the
 * validity sentinel, so arrays can be reset with UINT8_MAX without padding or
 * a collision-prone hash.
 */
#define SURVEY_SAMPLE_OBSERVATION_IDENTITY_LEN 6u
#define SURVEY_SAMPLE_OBSERVATION_IDENTITY_INVALID UINT8_MAX
struct survey_sample_observation_identity {
    uint8_t encoded[SURVEY_SAMPLE_OBSERVATION_IDENTITY_LEN];
};
_Static_assert(sizeof(struct survey_sample_observation_identity) ==
                   SURVEY_SAMPLE_OBSERVATION_IDENTITY_LEN,
               "survey sample observation identity must remain compact");

#define SURVEY_SAMPLE_TLV_MAX_LEN                                         \
    (3u * PROTO_TLV_U64_ENCODED_LEN + 2u * PROTO_TLV_U32_ENCODED_LEN +   \
     3u * PROTO_TLV_U16_ENCODED_LEN + 2u * PROTO_TLV_U8_ENCODED_LEN)

struct survey_reachability_report {
    uint64_t anchor_id;
    const struct survey_reachability_entry *entries;
    size_t entry_count;
};

struct survey_discovery_config {
    uint64_t operation_generation;
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

#define SURVEY_GATEWAY_COMPACT_NODE_INDEX_MASK UINT8_C(0x3f)
#define SURVEY_GATEWAY_REPORT_ORDER_PART_COUNT 3u

/*
 * Gateway-only reachability storage interns every exact node ID once.  The
 * compact entries retain all 600 directed report edges while keeping the
 * 50-node/150-pair worst case within the gateway RAM budget.
 */
struct survey_gateway_compact_reachability_entry {
    /*
     * Low six bits select one of 50 interned IDs. In the first three entry
     * cells, the two high bits preserve the report's six-bit acceptance
     * ordinal, including for reports with fewer than three peers.
     */
    uint8_t peer_index;
    int8_t rssi_dbm;
    uint8_t quality;
};

struct survey_gateway_report_slot {
    struct survey_gateway_compact_reachability_entry
        entries[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    uint8_t reverse_next_hop_index;
    uint8_t reverse_quality;
    uint8_t reverse_hop_count;
    /*
     * High nibble: command status. Low nibble: entry count. UINT8_MAX marks
     * an unused report slot; valid values cannot collide with that sentinel.
     */
    uint8_t metadata;
};

struct survey_gateway_pair_entry {
    uint8_t initiator_index;
    uint8_t responder_index;
};

_Static_assert(SURVEY_GATEWAY_MAX_REPORTS < UINT8_MAX,
               "gateway node indices must leave one invalid sentinel");
_Static_assert(SURVEY_GATEWAY_MAX_REPORTS <=
                   SURVEY_GATEWAY_COMPACT_NODE_INDEX_MASK + 1u,
               "gateway node indices must fit the compact six-bit field");
_Static_assert(SURVEY_GATEWAY_MAX_REPORTS <=
                   (1u << (2u * SURVEY_GATEWAY_REPORT_ORDER_PART_COUNT)),
               "gateway report ordinals must fit the compact order field");
_Static_assert(SURVEY_GATEWAY_MAX_PEERS_PER_REPORT >=
                   SURVEY_GATEWAY_REPORT_ORDER_PART_COUNT,
               "gateway report slots must retain compact order cells");
_Static_assert(SURVEY_GATEWAY_MAX_PEERS_PER_REPORT < 0x0fu,
               "gateway report entry count must fit below invalid metadata");
_Static_assert(COMMAND_INTERNAL_ERROR < 0x0fu,
               "gateway report status must fit below invalid metadata");
_Static_assert(sizeof(struct survey_gateway_compact_reachability_entry) == 3u,
               "gateway reachability entries must remain index compact");
_Static_assert(sizeof(struct survey_gateway_report_slot) == 40u,
               "gateway report slots must retain the 40-byte RAM contract");
_Static_assert(sizeof(struct survey_gateway_pair_entry) == 2u,
               "gateway survey pairs must store two validated node indices");

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
    uint64_t operation_generation;
    uint64_t node_ids[SURVEY_GATEWAY_MAX_REPORTS];
    uint32_t survey_id;
    struct survey_gateway_report_slot reports[SURVEY_GATEWAY_MAX_REPORTS];
    struct survey_gateway_pair_entry pairs[SURVEY_GATEWAY_MAX_PAIRS];
    uint16_t sample_count;
    uint8_t node_count;
    uint8_t report_count;
    uint8_t pair_count;
    bool pairs_planned;
    bool topology_complete;
};

_Static_assert(sizeof(struct survey_gateway_context) == 2720u,
               "gateway survey context must preserve the 7 KiB RAM recovery");

bool survey_sample_count_valid(uint16_t sample_count);
int survey_pair_validate(const struct survey_pair *pair);
int survey_sample_validate(const struct survey_sample *sample);
int survey_pair_result_next_round_id(uint16_t current_round_id,
                                     uint16_t *next_round_id);
/*
 * Maps one synchronized round/sample identity to nonzero wire sequence.
 * Legacy round zero uses sample_index + 1 within its immutable survey session.
 */
int survey_pair_result_transport_sequence(uint16_t round_id,
                                          uint16_t sample_index,
                                          uint16_t *sequence);
bool survey_sample_distance_usable(const struct survey_sample *sample);
bool survey_sample_matches_pair_run(const struct survey_sample *sample,
                                    const struct survey_pair *pair,
                                    uint16_t round_id);
/* Exact observation identity is used only after reporter, pair, round, and
 * sample-index ownership have been validated by the caller. */
int survey_sample_observation_identity_capture(
    const struct survey_sample *sample,
    struct survey_sample_observation_identity *identity);
bool survey_sample_observation_identity_valid(
    const struct survey_sample_observation_identity *identity);
bool survey_sample_observation_identity_equal(
    const struct survey_sample_observation_identity *left,
    const struct survey_sample_observation_identity *right);
int survey_pair_note_sample_masks(const struct survey_sample *sample,
                                  uint64_t reporter_id,
                                  uint16_t *usable_mask,
                                  uint16_t *responder_usable_mask,
                                  uint16_t *initiator_unusable_mask,
                                  uint16_t *responder_unusable_mask,
                                  bool *changed);
bool survey_pair_missing_samples_all_unusable(
    uint16_t sample_count,
    uint16_t usable_mask,
    uint16_t initiator_unusable_mask,
    uint16_t responder_unusable_mask);
uint64_t survey_sample_nonce(const struct survey_pair *pair, uint16_t sample_index);
int survey_reachability_entry_validate(const struct survey_reachability_entry *entry);
int survey_reachability_report_endpoints_validate(
    uint64_t anchor_id,
    uint64_t gateway_id,
    const struct survey_reachability_entry *entries,
    size_t entry_count);
int survey_reachability_entry_retain(
    struct survey_reachability_entry *entries,
    size_t entry_cap,
    size_t *entry_count,
    const struct survey_reachability_entry *candidate);
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
int survey_discovery_opportunity_slot_tx_ms(
    const struct survey_discovery_config *config,
    uint8_t slot,
    uint8_t opportunity,
    uint32_t *tx_ms);
int survey_discovery_opportunity_tx_ms(
    const struct survey_discovery_config *config,
    uint64_t anchor_id,
    uint8_t opportunity,
    uint32_t *tx_ms);
uint32_t survey_discovery_probe_tx_budget_ms(void);
int survey_discovery_schedule_slot_attempt(
    const struct survey_discovery_config *config,
    uint8_t slot,
    uint8_t opportunity,
    uint32_t earliest_relative_ms,
    struct survey_discovery_attempt_schedule *schedule);
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
int survey_gateway_begin_operation(struct survey_gateway_context *context,
                                   uint32_t survey_id,
                                   uint64_t operation_generation,
                                   uint16_t sample_count);
int survey_gateway_context_validate(
    const struct survey_gateway_context *context);
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
int survey_gateway_note_reach_report_with_reverse_hint_status(
    struct survey_gateway_context *context,
    uint32_t survey_id,
    uint64_t anchor_id,
    const struct survey_reachability_entry *entries,
    size_t entry_count,
    enum command_status report_status,
    const struct survey_gateway_reverse_hint *reverse_hint);
int survey_gateway_reverse_hint_for_target(
    const struct survey_gateway_context *context,
    uint64_t target_id,
    struct survey_gateway_reverse_hint *reverse_hint);
/*
 * Report indices are stable logical ordinals over accepted reports, not
 * intern-table indices. These accessors validate every compact index before
 * reconstructing exact IDs.
 */
int survey_gateway_report_info_at(
    const struct survey_gateway_context *context,
    size_t report_index,
    uint64_t *anchor_id,
    size_t *entry_count,
    enum command_status *report_status);
int survey_gateway_report_entry_at(
    const struct survey_gateway_context *context,
    size_t report_index,
    size_t entry_index,
    struct survey_reachability_entry *entry);
/*
 * Returns PROTO_OK for an exact accepted duplicate, PROTO_ERR_NOT_FOUND when
 * the anchor has not reported, and PROTO_ERR_MALFORMED for a conflicting
 * duplicate or corrupt compact state.
 */
int survey_gateway_reach_report_compare(
    const struct survey_gateway_context *context,
    uint64_t anchor_id,
    const struct survey_reachability_entry *entries,
    size_t entry_count,
    enum command_status report_status);
uint8_t survey_gateway_hop_count_from_report_ttl(uint8_t remaining_ttl);
uint32_t survey_pair_control_timeout_ms(uint8_t gateway_hop_count);
uint32_t survey_pair_control_round_trip_timeout_ms(
    uint8_t gateway_hop_count);
uint32_t survey_discovery_report_custody_ms(uint8_t gateway_hop_count);
uint64_t survey_discovery_report_deadline_ms(uint64_t now_ms,
                                             uint32_t eligible_tx_ms,
                                             uint8_t gateway_hop_count);
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
bool survey_gateway_discovery_collection_survives_terminal(
    bool delivered,
    uint8_t attempts_started);
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
 * Operation generations are strictly nonzero. Packet session IDs use the
 * low 32 bits, and the durable allocator skips values whose low word is zero.
 */
uint32_t survey_operation_session_id(uint64_t operation_generation);
int survey_operation_generation_append_tlv(uint8_t *payload,
                                           size_t payload_cap,
                                           size_t *offset,
                                           uint64_t operation_generation);
int survey_operation_generation_extract_tlv(const uint8_t *payload,
                                            size_t payload_len,
                                            uint64_t *operation_generation);
/*
 * Builds a deterministic degree-bounded forest, connecting every candidate
 * component before adding preferred extra pairs. Disconnected input keeps all
 * useful component work; PROTO_ERR_NOT_FOUND is reserved for input with no
 * usable reachability edge.
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
int survey_pair_result_payload_validate(const uint8_t *payload,
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
                                        uint64_t operation_generation,
                                        uint32_t boot_incarnation,
                                        uint16_t seq,
                                        uint8_t payload_len);

/*
 * Discovery report sequence numbers are boot-local transport identity. They
 * never wrap: zero reports exhaustion and requires a reboot to obtain a new
 * durable boot incarnation before another discovery report can be emitted.
 */
uint16_t survey_discovery_sequence_next(uint16_t *sequence_state);
int survey_init_pair_prepare_packet(struct proto_packet *packet,
                                    const struct survey_pair *pair,
                                    uint64_t gateway_id,
                                    uint64_t target_id,
                                    uint16_t seq,
                                    uint8_t payload_len);

#ifdef __cplusplus
}
#endif

#endif

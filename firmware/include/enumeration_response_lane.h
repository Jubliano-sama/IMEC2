#ifndef ENUMERATION_RESPONSE_LANE_H
#define ENUMERATION_RESPONSE_LANE_H

#include "discovery_assignment.h"
#include "mesh_capacity.h"
#include "mesh_relay.h"
#include "uwb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One bounded schedule can expand through every production mesh hop band. */
#define ENUMERATION_RESPONSE_ROUND_MS 125u
#define ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH 12u
#define ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP 2u
#define ENUMERATION_RESPONSE_MAX_ROUNDS_PER_DEPTH \
    (ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH + \
     ((UWB_ENUM_MAX_HOPS - 1u) * \
      ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP))
/* H1 has no upstream relay tail. Deeper bands append two ordered retry
 * rounds for every parent that must carry that depth's aggregate gatewayward. */
#define ENUMERATION_RESPONSE_DEPTH_MS \
    (ENUMERATION_RESPONSE_ROUND_MS * \
     ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH)
#define ENUMERATION_RESPONSE_LANE_MS \
    (ENUMERATION_RESPONSE_ROUND_MS * \
     ((UWB_ENUM_MAX_HOPS * ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH) + \
      ((ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP * UWB_ENUM_MAX_HOPS * \
        (UWB_ENUM_MAX_HOPS - 1u)) / 2u)))
#define ENUMERATION_RESPONSE_MAX_BUNDLES \
    ((MESH_CONNECTED_MAX_ANCHORS + UWB_ENUM_RECORDS_PER_BUNDLE - 1u) / \
     UWB_ENUM_RECORDS_PER_BUNDLE)
#define ENUMERATION_RESPONSE_HOP_STORAGE_BYTES \
    ((MESH_CONNECTED_MAX_ANCHORS * 3u + 7u) / 8u)
#define ENUMERATION_RESPONSE_TX_ACK_RESERVE_MS 50u
#define ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS 40u
#define ENUMERATION_RESPONSE_ANCHOR_PREPARE_MS 40u
#define ENUMERATION_RESPONSE_TX_WINDOW_MS \
    (ENUMERATION_RESPONSE_ROUND_MS - ENUMERATION_RESPONSE_TX_ACK_RESERVE_MS)
#define ENUMERATION_RESPONSE_NO_OFFSET 0xffu
#define ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS 10u
#define ENUMERATION_RESPONSE_TX_LATE_GUARD_MS 4u
#define ENUMERATION_RESPONSE_HIA_CLOCK_GUARD_MS 100u
#define ENUMERATION_RESPONSE_HIA_LEAD_DEPTHS 2u
/* The gateway wake is depth zero.  A depth-one anchor may start its compact
 * identity block when the HIA front is logically at depth three, and every
 * deeper source follows the same gateway-relative 4.5 s depth clock. */
#define ENUMERATION_RESPONSE_HIA_FIRST_DEPTH_START_DELAY_MS \
    (((ENUMERATION_RESPONSE_HIA_LEAD_DEPTHS + 1u) * \
      MESH_GATEWAY_ROUTE_DEPTH_BLOCK_MS) + \
     ENUMERATION_RESPONSE_HIA_CLOCK_GUARD_MS)
#define ENUMERATION_RESPONSE_HIA_LOCAL_START_DELAY_MS \
    ((ENUMERATION_RESPONSE_HIA_LEAD_DEPTHS * \
      MESH_GATEWAY_ROUTE_DEPTH_BLOCK_MS) + \
     ENUMERATION_RESPONSE_HIA_CLOCK_GUARD_MS)
/* CLAIM creation is the shared clock edge. The control flood and every
 * pipelined forward must be clear before the first response band starts. */
/* CLAIM is wake-free, but it follows the Here-I-Am activation wave with one
 * complete activation-hop lead at every relay. Start responses only after
 * every possible CLAIM copy burst plus the gateway preparation edge. */
#define ENUMERATION_RESPONSE_START_DELAY_MS \
    (DISCOVERY_ASSIGNMENT_CONTROL_PROPAGATION_MARGIN_MS + \
     (UWB_ENUM_MAX_HOPS * \
      DISCOVERY_ASSIGNMENT_RELAY_BEFORE_RESPONSE_MAX_MS) + \
     ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS)

struct enumeration_response_timing {
    uint8_t depth;
    uint8_t round;
    uint8_t round_offset_ms;
};

struct enumeration_response_lane {
    uint64_t record_ids[MESH_CONNECTED_MAX_ANCHORS];
    uint64_t parent_id;
    uint64_t start_ms;
    uint32_t network_id;
    uint32_t epoch;
    /* Hop depths are stored as (hop - 1), so three bits cover depths 1..8.
     * Each bundle gets a millisecond offset drawn across the round instead of
     * sharing a globally aligned cell. */
    uint8_t record_hops_packed[ENUMERATION_RESPONSE_HOP_STORAGE_BYTES];
    uint8_t round_offsets_ms[ENUMERATION_RESPONSE_MAX_BUNDLES];
    uint8_t hop_count;
    uint8_t record_count;
    uint8_t acked_mask;
    uint8_t attempted_mask;
    uint8_t prepared_round;
    bool active;
};

bool enumeration_response_timing_at(
    uint64_t start_ms,
    uint64_t now_ms,
    struct enumeration_response_timing *timing);
/*
 * Translate the gateway-originated CLAIM schedule into the receiver's local
 * uptime domain. packet_age_ms includes every gateway/relay hold but is never
 * compared with local uptime: separately rebooted devices have unrelated
 * clocks. starts_in_ms is deliberately signed so a late deeper-hop CLAIM can
 * join a representable lane already in progress.
 */
bool enumeration_response_claim_start(
    uint64_t now_ms,
    uint32_t packet_age_ms,
    uint32_t advertised_start_delay_ms,
    uint64_t *start_ms,
    int64_t *starts_in_ms);
bool enumeration_response_timing_at_depth(
    uint64_t start_ms,
    uint64_t now_ms,
    uint8_t max_hop_count,
    struct enumeration_response_timing *timing);
uint32_t enumeration_response_duration_ms(uint8_t max_hop_count);
uint32_t enumeration_response_depth_duration_ms(uint8_t depth);
bool enumeration_response_lane_tx_round_allowed(
    uint8_t response_depth,
    uint8_t local_hop_count,
    uint8_t round);
uint32_t enumeration_response_ms_until_lane(
    uint64_t start_ms,
    uint64_t now_ms);
bool enumeration_response_lane_complete(uint64_t start_ms, uint64_t now_ms);
bool enumeration_response_lane_complete_depth(
    uint64_t start_ms,
    uint64_t now_ms,
    uint8_t max_hop_count);
/* HIA-triggered enumeration uses the same compact BUNDLE/ACK lane, but each
 * source depth is pinned to the gateway's 4.5 s route-wave clock. start_ms is
 * the local-time start of first_hop_count's compact source block. Once an
 * anchor has sent its own identity, the intervening quiet time remains RX. */
bool enumeration_response_hia_timing_at_depth(
    uint64_t start_ms,
    uint64_t now_ms,
    uint8_t first_hop_count,
    uint8_t max_hop_count,
    struct enumeration_response_timing *timing);
uint32_t enumeration_response_hia_duration_ms(uint8_t first_hop_count,
                                              uint8_t max_hop_count);
bool enumeration_response_hia_complete_depth(uint64_t start_ms,
                                             uint64_t now_ms,
                                             uint8_t first_hop_count,
                                             uint8_t max_hop_count);

int enumeration_response_lane_begin(
    struct enumeration_response_lane *lane,
    uint32_t network_id,
    uint32_t epoch,
    uint64_t local_id,
    uint64_t parent_id,
    uint8_t hop_count,
    uint64_t start_ms);
void enumeration_response_lane_stop(struct enumeration_response_lane *lane);
int enumeration_response_lane_merge_bundle(
    struct enumeration_response_lane *lane,
    const struct uwb_enumeration_bundle_frame *bundle,
    bool *added_records);
int enumeration_response_lane_prepare_round(
    struct enumeration_response_lane *lane,
    uint8_t round,
    uint32_t random_value);
int enumeration_response_lane_bundle_for_offset(
    struct enumeration_response_lane *lane,
    const struct enumeration_response_timing *timing,
    struct uwb_enumeration_bundle_frame *bundle);
bool enumeration_response_lane_note_ack(
    struct enumeration_response_lane *lane,
    const struct uwb_enumeration_hop_ack_frame *ack);
bool enumeration_response_lane_all_acked(
    const struct enumeration_response_lane *lane);
uint64_t enumeration_response_lane_local_id(
    const struct enumeration_response_lane *lane);
uint8_t enumeration_response_lane_bundle_count(
    const struct enumeration_response_lane *lane);
uint8_t enumeration_response_lane_round_offset_ms(
    const struct enumeration_response_lane *lane,
    uint8_t sequence);
uint8_t enumeration_response_lane_next_offset_ms(
    const struct enumeration_response_lane *lane,
    const struct enumeration_response_timing *timing);

#ifdef __cplusplus
}
#endif

#endif

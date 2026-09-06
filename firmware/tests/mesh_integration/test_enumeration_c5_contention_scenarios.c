#include "dwm3000_timing.h"
#include "enumeration_response_lane.h"
#include "mesh.h"
#include "mesh_radio_timing.h"
#include "protocol.h"
#include "uwb.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This is a bounded compact-lane RF model, not a replacement for the full
 * application simulator. Routes are learned from a decoded production HIA
 * advert. Source scheduling and custody/ACK behavior use the production lane.
 * The receiver's RAM admission is a bounded identity set. Host processing,
 * TX/RX turnaround and collision recovery cost explicit nonzero time.
 *
 * All radios share a collision domain: overlapping complete air intervals
 * corrupt both frames; no capture effect or direct-delivery fallback exists.
 * Timing sweeps are hypotheses to qualify on hardware, not measured limits. */
#define MAX_ANCHORS 50u
#define SEED_COUNT 1024u
#define NETWORK_ID UINT32_C(0x494d4543)
#define GATEWAY_ID UINT64_C(0x9000000000000001)
#define ID_BASE UINT64_C(0xa000000000000100)
#define EPOCH UINT32_C(17)
#define START_MS UINT64_C(14600)
#define RX_REARM_US UINT64_C(500)
#define TX_RX_TURNAROUND_US UINT64_C(500)

struct air_frame {
    uint64_t start_us, end_us;
    uint8_t sender;
    size_t length;
    uint8_t bytes[UWB_ENUM_BUNDLE_MAX_LEN];
};
struct source {
    struct enumeration_response_lane lane;
    uint32_t random_state;
    uint32_t phase_us;
};
struct outcome {
    unsigned collected, acked, attempts, collisions, missed_rx, lost_acks;
    uint64_t finished_us;
};
static struct mesh_relay gateway;
static struct mesh_relay anchor;

static uint32_t random_next(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value == 0u ? UINT32_C(0x9e3779b9) : value;
    return *state;
}
static bool overlaps(uint64_t start_a, uint64_t end_a,
                     uint64_t start_b, uint64_t end_b)
{
    return start_a < end_b && start_b < end_a;
}
static bool contained(uint64_t start, uint64_t end,
                      uint64_t rx_start, uint64_t rx_end)
{
    return rx_start <= start && end <= rx_end;
}
static int compare_frames(const void *lhs, const void *rhs)
{
    const struct air_frame *a = lhs, *b = rhs;
    return a->start_us < b->start_us ? -1 : a->start_us > b->start_us;
}

static void initialize_sources(struct source *sources, uint8_t count,
                               uint32_t seed, bool same_slot)
{
    struct mesh_outbound advert;
    struct mesh_relay_result received;
    uint8_t wire[UWB_MESH_MAX_PACKET_LEN];
    size_t wire_len = 0u;
    struct proto_packet decoded;
    const uint8_t *payload;
    size_t payload_len;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY_ID, GATEWAY_ID, EPOCH);
    assert(mesh_relay_build_gateway_route_adv(
               &gateway, seed + 1u, 1000u, &advert) == PROTO_OK);
    assert(proto_packet_encode(&advert.packet, advert.payload, wire,
                               sizeof(wire), &wire_len) == PROTO_OK);
    assert(proto_packet_decode(wire, wire_len, &decoded,
                                &payload, &payload_len) == PROTO_OK);
    /* The gateway is the only source in its depth block. Its decoded HIA
     * activation is a scenario precondition; no low-duty activation is
     * inferred from reception of the later short control frame. */
    uint64_t advert_start_us = (uint64_t)advert.earliest_tx_ms * 1000u;
    uint64_t advert_end_us = advert_start_us +
        dwm3000_timing_airtime_us_ceil(
            DWM3000_TIMING_PHY_CH5_MESH_CONTROL, wire_len);
    assert(contained(advert_start_us, advert_end_us,
                     UINT64_C(3000000), START_MS * 1000u));

    for (uint8_t i = 0u; i < count; i++) {
        uint64_t id = ID_BASE + i;
        if (same_slot) {
            uint64_t first = ID_BASE + (uint64_t)seed * 512u;
            id = first;
            for (uint8_t member = 0u; member < i; member++) {
                do {
                    id++;
                } while (discovery_assignment_hash(id) % UWB_DISCOVERY_SLOT_COUNT !=
                         discovery_assignment_hash(first) % UWB_DISCOVERY_SLOT_COUNT);
            }
        }
        const struct route_candidate *selected;

        mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR,
                        id, GATEWAY_ID, EPOCH);
        assert(mesh_relay_handle_rx(
                   &anchor, &decoded, payload, payload_len,
                   GATEWAY_ID, 100u, (uint32_t)(advert_end_us / 1000u),
                   &received) == PROTO_OK);
        selected = route_selected(&anchor.upstream);
        assert(selected != NULL && selected->next_hop_id == GATEWAY_ID);
        assert(enumeration_response_lane_begin(
                   &sources[i].lane, NETWORK_ID, EPOCH, id,
                   selected->next_hop_id, (uint8_t)(selected->hop_count + 1u),
                   START_MS) == PROTO_OK);
        sources[i].random_state = (seed + 1u) * UINT32_C(0x9e3779b9) ^
                                  (uint32_t)id ^ ((uint32_t)i << 19u);
        sources[i].phase_us = random_next(&sources[i].random_state) % 1000u;
    }
}

static struct outcome run_star(uint8_t count, uint32_t seed,
                               uint32_t ack_turnaround_us,
                               uint32_t collision_recovery_us, bool same_slot)
{
    struct source sources[MAX_ANCHORS];
    struct outcome result = {0};
    bool collected[MAX_ANCHORS] = {false};
    uint64_t gateway_rx_start_us = START_MS * 1000u;

    assert(count > 0u && count <= MAX_ANCHORS);
    assert(ack_turnaround_us >= TX_RX_TURNAROUND_US);
    assert(collision_recovery_us > 0u);
    initialize_sources(sources, count, seed, same_slot);
    for (uint8_t round = 0u;
         round < ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH; round++) {
        struct air_frame frames[MAX_ANCHORS];
        size_t frame_count = 0u;
        uint64_t round_start = START_MS * 1000u +
            (uint64_t)round * ENUMERATION_RESPONSE_ROUND_MS * 1000u;
        uint64_t round_end = round_start +
            ENUMERATION_RESPONSE_ROUND_MS * 1000u;

        for (uint8_t i = 0u; i < count; i++) {
            struct enumeration_response_timing timing = {1u, round, 0u};
            struct uwb_enumeration_bundle_frame bundle;
            uint8_t offset;
            if (enumeration_response_lane_all_acked(&sources[i].lane)) {
                continue;
            }
            assert(enumeration_response_lane_prepare_round(
                       &sources[i].lane, round,
                       same_slot ? 7u : random_next(&sources[i].random_state)) == PROTO_OK);
            offset = enumeration_response_lane_round_offset_ms(
                &sources[i].lane, 0u);
            if (offset == ENUMERATION_RESPONSE_NO_OFFSET) continue;
            assert(offset < ENUMERATION_RESPONSE_TX_WINDOW_MS);
            timing.round_offset_ms = offset;
            assert(enumeration_response_lane_bundle_for_offset(
                       &sources[i].lane, &timing, &bundle) == PROTO_OK);
            struct air_frame *frame = &frames[frame_count++];
            assert(uwb_encode_enumeration_bundle(
                       &bundle, frame->bytes, sizeof(frame->bytes),
                       &frame->length) == PROTO_OK);
            frame->sender = i;
            frame->start_us = round_start + (uint64_t)offset * 1000u +
                (same_slot ? 0u : sources[i].phase_us +
                 random_next(&sources[i].random_state) % 501u);
            frame->end_us = frame->start_us + dwm3000_timing_airtime_us_ceil(
                DWM3000_TIMING_PHY_CH5_MESH_CONTROL, frame->length);
            assert(contained(frame->start_us, frame->end_us, round_start,
                             round_end));
            result.attempts++;
        }
        qsort(frames, frame_count, sizeof(frames[0]), compare_frames);
        for (size_t i = 0u; i < frame_count; i++) {
            const struct air_frame *frame = &frames[i];
            struct uwb_enumeration_bundle_frame bundle;
            struct uwb_enumeration_hop_ack_frame ack, received_ack;
            uint8_t ack_bytes[UWB_ENUM_HOP_ACK_LEN];
            size_t ack_length = 0u;
            bool collision = false;

            if (!contained(frame->start_us, frame->end_us,
                           gateway_rx_start_us, round_end)) {
                result.missed_rx++;
                continue;
            }
            for (size_t j = 0u; j < frame_count; j++) {
                if (i != j && overlaps(frame->start_us, frame->end_us,
                                       frames[j].start_us, frames[j].end_us)) {
                    collision = true;
                }
            }
            if (collision) {
                result.collisions++;
                gateway_rx_start_us = frame->end_us + collision_recovery_us;
                continue;
            }
            assert(uwb_decode_enumeration_bundle(
                       frame->bytes, frame->length, &bundle) == PROTO_OK);
            assert(bundle.network_id == NETWORK_ID && bundle.epoch == EPOCH);
            assert(bundle.parent_id == GATEWAY_ID && bundle.record_count == 1u);
            assert(bundle.records[0].anchor_id ==
                   enumeration_response_lane_local_id(&sources[frame->sender].lane));
            assert(bundle.records[0].hop_count == 1u);
            collected[frame->sender] = true;
            ack = (struct uwb_enumeration_hop_ack_frame) {
                .network_id = bundle.network_id, .epoch = bundle.epoch,
                .parent_id = GATEWAY_ID, .child_id = bundle.sender_id,
                .sequence = bundle.sequence,
            };
            assert(uwb_encode_enumeration_hop_ack(
                       &ack, ack_bytes, sizeof(ack_bytes), &ack_length) == PROTO_OK);
            uint64_t ack_start = frame->end_us + ack_turnaround_us;
            uint64_t ack_end = ack_start + dwm3000_timing_airtime_us_ceil(
                DWM3000_TIMING_PHY_CH5_MESH_CONTROL, ack_length);
            gateway_rx_start_us = ack_end + RX_REARM_US;
            collision = !contained(ack_start, ack_end,
                                    frame->end_us + TX_RX_TURNAROUND_US,
                                    round_end);
            for (size_t j = 0u; j < frame_count; j++) {
                if (overlaps(ack_start, ack_end,
                             frames[j].start_us, frames[j].end_us)) {
                    collision = true;
                }
            }
            if (collision) {
                result.lost_acks++;
                continue;
            }
            assert(uwb_decode_enumeration_hop_ack(
                       ack_bytes, ack_length, &received_ack) == PROTO_OK);
            assert(enumeration_response_lane_note_ack(
                       &sources[frame->sender].lane, &received_ack));
            result.finished_us = ack_end;
        }
    }
    for (uint8_t i = 0u; i < count; i++) {
        result.collected += collected[i] ? 1u : 0u;
        result.acked += enumeration_response_lane_all_acked(&sources[i].lane) ? 1u : 0u;
    }
    assert(result.acked <= result.collected && result.collected <= count);
    assert(result.attempts <= count * ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH);
    assert(result.finished_us <= START_MS * 1000u +
           ENUMERATION_RESPONSE_DEPTH_MS * 1000u);
    return result;
}

static unsigned sweep(uint8_t count, uint32_t ack_us, uint32_t recovery_us,
                      bool require_complete)
{
    unsigned failures = 0u, min_acked = count, min_collected = count;
    unsigned collisions = 0u, lost_acks = 0u;
    for (uint32_t seed = 0u; seed < SEED_COUNT; seed++) {
        struct outcome result = run_star(count, seed, ack_us, recovery_us, false);
        if (result.acked != count) {
            printf("  incomplete seed=%u collected=%u acked=%u attempts=%u\n",
                   seed, result.collected, result.acked, result.attempts);
        }
        failures += result.acked != count ? 1u : 0u;
        if (result.acked < min_acked) min_acked = result.acked;
        if (result.collected < min_collected) min_collected = result.collected;
        collisions += result.collisions;
        lost_acks += result.lost_acks;
    }
    printf("compact_star nodes=%u seeds=%u ack_turnaround_us=%u "
           "collision_recovery_us=%u incomplete=%u min_collected=%u "
           "min_acked=%u collisions=%u lost_acks=%u\n",
           count, SEED_COUNT, ack_us, recovery_us, failures,
           min_collected, min_acked, collisions, lost_acks);
    assert(!require_complete || failures == 0u);
    return failures;
}

int main(void)
{
    /* One microsecond of missing frame airtime is never successful RX. */
    assert(!contained(10u, 20u, 11u, 20u));
    assert(!contained(10u, 20u, 10u, 19u));
    assert(overlaps(10u, 20u, 19u, 30u));
    assert(!overlaps(10u, 20u, 20u, 30u));
    assert(ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH == 20u);
    assert(ENUMERATION_RESPONSE_ROUND_MS == 125u);
    assert(ENUMERATION_RESPONSE_TX_WINDOW_MS == 75u);
    /* Distinct FICR identities with the same temporary hash slot and a
     * deliberately identical, stuck random draw first collide. Every pair
     * must then finish within this same operation, using real bundle/ACK
     * frames; neither the temporary slot nor a fresh RNG value is needed. */
    for (uint32_t seed = 0u; seed < 256u; seed++) {
        struct outcome pair = run_star(2u, seed, 4000u, 500u, true);
        assert(pair.collisions > 0u);
        assert(pair.attempts > 2u);
        assert(pair.collected == 2u && pair.acked == 2u);
    }
    /* The whole demo population can share one temporary slot as well.
     * Use no sub-millisecond phase or scheduling jitter to rescue it. */
    for (uint32_t seed = 0u; seed < SEED_COUNT; seed++) {
        struct outcome group = run_star(30u, seed, 4000u, 500u, true);
        assert(group.collisions > 0u && group.attempts > 30u);
        assert(group.collected == 30u && group.acked == 30u);
    }
    puts("same_hash_slot nodes=30 seeds=1024 repeated_random=7 all_acked=yes");
    sweep(12u, 1000u, 500u, true);
    sweep(30u, 1000u, 500u, true);
    sweep(50u, 4000u, 500u, false);
    sweep(30u, 4000u, 500u, true);
    sweep(30u, 4000u, 40000u, false);
    assert(sweep(30u, 40000u, 500u, false) > 0u);
    puts("compact contention model: PASS for 30 nodes; 50-node capacity is not a delivery guarantee; timing envelope requires hardware measurement");
    return 0;
}

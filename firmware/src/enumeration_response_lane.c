#include "enumeration_response_lane.h"

#include "protocol.h"

#include <string.h>

_Static_assert(MESH_CONNECTED_MAX_ANCHORS == 50u,
               "the qualified lane carries exactly fifty anchors");
_Static_assert(UWB_ENUM_MAX_HOPS == 8u,
               "the qualified lane covers every production mesh hop depth");
_Static_assert(ENUMERATION_RESPONSE_DEPTH_MS == 1500u,
               "one-hop source responses must occupy 1500 ms");
_Static_assert(MESH_ENUMERATION_CLAIM_RELAY_HOP_MAX_MS == 5470u,
               "pipelined CLAIM relay bound changed");
_Static_assert(ENUMERATION_RESPONSE_START_DELAY_MS == 43950u,
               "response edge must follow every pipelined CLAIM relay");
_Static_assert(ENUMERATION_RESPONSE_LANE_MS == 19000u,
               "eight response depths plus ordered relay tails occupy 19 s");
_Static_assert(sizeof(struct enumeration_response_lane) == 456u,
               "the persistent response lane must remain compact");

static uint8_t packed_hop_get(const struct enumeration_response_lane *lane,
                              uint8_t index)
{
    uint16_t bits;
    uint16_t bit_offset = (uint16_t)index * 3u;
    uint8_t byte_offset = (uint8_t)(bit_offset / 8u);
    uint8_t shift = (uint8_t)(bit_offset % 8u);

    bits = lane->record_hops_packed[byte_offset];
    if (shift > 5u) {
        bits |= (uint16_t)lane->record_hops_packed[byte_offset + 1u] << 8u;
    }
    return (uint8_t)(((bits >> shift) & 0x07u) + 1u);
}

static void packed_hop_set(struct enumeration_response_lane *lane,
                           uint8_t index,
                           uint8_t hop_count)
{
    uint16_t bits;
    uint16_t mask;
    uint16_t bit_offset = (uint16_t)index * 3u;
    uint8_t byte_offset = (uint8_t)(bit_offset / 8u);
    uint8_t shift = (uint8_t)(bit_offset % 8u);

    bits = lane->record_hops_packed[byte_offset];
    if (shift > 5u) {
        bits |= (uint16_t)lane->record_hops_packed[byte_offset + 1u] << 8u;
    }
    mask = (uint16_t)0x07u << shift;
    bits = (uint16_t)((bits & ~mask) |
                      (((uint16_t)(hop_count - 1u) << shift) & mask));
    lane->record_hops_packed[byte_offset] = (uint8_t)bits;
    if (shift > 5u) {
        lane->record_hops_packed[byte_offset + 1u] = (uint8_t)(bits >> 8u);
    }
}

static uint8_t bundle_count_for_records(uint8_t record_count)
{
    return (uint8_t)((record_count + UWB_ENUM_RECORDS_PER_BUNDLE - 1u) /
                     UWB_ENUM_RECORDS_PER_BUNDLE);
}

bool enumeration_response_claim_start(
    uint64_t now_ms,
    uint32_t packet_age_ms,
    uint32_t advertised_start_delay_ms,
    uint64_t *start_ms,
    int64_t *starts_in_ms)
{
    uint64_t origin_ms;

    if (start_ms == NULL || starts_in_ms == NULL || packet_age_ms > now_ms) {
        return false;
    }
    origin_ms = now_ms - packet_age_ms;
    *start_ms = origin_ms + advertised_start_delay_ms;
    *starts_in_ms = (int64_t)advertised_start_delay_ms -
                    (int64_t)packet_age_ms;
    return true;
}

bool enumeration_response_timing_at(
    uint64_t start_ms,
    uint64_t now_ms,
    struct enumeration_response_timing *timing)
{
    return enumeration_response_timing_at_depth(
        start_ms, now_ms, UWB_ENUM_MAX_HOPS, timing);
}

uint32_t enumeration_response_duration_ms(uint8_t max_hop_count)
{
    if (max_hop_count == 0u || max_hop_count > UWB_ENUM_MAX_HOPS) {
        return 0u;
    }
    return ENUMERATION_RESPONSE_ROUND_MS *
        ((uint32_t)max_hop_count *
             ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH +
         ((uint32_t)ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP *
          max_hop_count * (max_hop_count - 1u)) / 2u);
}

uint32_t enumeration_response_depth_duration_ms(uint8_t depth)
{
    if (depth == 0u || depth > UWB_ENUM_MAX_HOPS) {
        return 0u;
    }
    return ENUMERATION_RESPONSE_ROUND_MS *
        (ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH +
         (uint32_t)(depth - 1u) *
             ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP);
}

bool enumeration_response_lane_tx_round_allowed(
    uint8_t response_depth,
    uint8_t local_hop_count,
    uint8_t round)
{
    uint8_t first_forward_round;

    if (response_depth == 0u || response_depth > UWB_ENUM_MAX_HOPS ||
        local_hop_count == 0u || local_hop_count > response_depth) {
        return false;
    }
    if (local_hop_count == response_depth) {
        return round < ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH;
    }
    first_forward_round = (uint8_t)(
        ENUMERATION_RESPONSE_SOURCE_ROUNDS_PER_DEPTH +
        (response_depth - local_hop_count - 1u) *
            ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP);
    return round >= first_forward_round &&
           round < first_forward_round +
               ENUMERATION_RESPONSE_FORWARD_ROUNDS_PER_HOP;
}

bool enumeration_response_timing_at_depth(
    uint64_t start_ms,
    uint64_t now_ms,
    uint8_t max_hop_count,
    struct enumeration_response_timing *timing)
{
    uint64_t elapsed_ms;
    uint32_t depth_start_offset_ms = 0u;
    uint32_t within_depth_ms;
    uint8_t depth;
    uint32_t duration_ms =
        enumeration_response_duration_ms(max_hop_count);

    if (timing == NULL || duration_ms == 0u || now_ms < start_ms) {
        return false;
    }
    elapsed_ms = now_ms - start_ms;
    if (elapsed_ms >= duration_ms) {
        return false;
    }
    for (depth = 1u; depth <= max_hop_count; depth++) {
        uint32_t depth_duration_ms =
            enumeration_response_depth_duration_ms(depth);

        if (elapsed_ms <
            (uint64_t)depth_start_offset_ms + depth_duration_ms) {
            break;
        }
        depth_start_offset_ms += depth_duration_ms;
    }
    if (depth > max_hop_count) {
        return false;
    }
    within_depth_ms = (uint32_t)elapsed_ms - depth_start_offset_ms;
    /* Start at the gateway and expand one hop at a time. This lets the
     * gateway close the lane after one complete, empty next-depth band
     * instead of always paying for the eight-hop ceiling. Lower-hop parents
     * remain active in later bands so newly received child records can keep
     * moving toward the gateway. */
    timing->depth = depth;
    timing->round = (uint8_t)(within_depth_ms /
                              ENUMERATION_RESPONSE_ROUND_MS);
    timing->round_offset_ms =
        (uint8_t)(within_depth_ms % ENUMERATION_RESPONSE_ROUND_MS);
    return true;
}

uint32_t enumeration_response_ms_until_lane(
    uint64_t start_ms,
    uint64_t now_ms)
{
    uint64_t remaining_ms;

    if (now_ms >= start_ms) {
        return 0u;
    }
    remaining_ms = start_ms - now_ms;
    return remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_ms;
}

bool enumeration_response_lane_complete(uint64_t start_ms, uint64_t now_ms)
{
    return enumeration_response_lane_complete_depth(
        start_ms, now_ms, UWB_ENUM_MAX_HOPS);
}

bool enumeration_response_lane_complete_depth(
    uint64_t start_ms,
    uint64_t now_ms,
    uint8_t max_hop_count)
{
    uint32_t duration_ms =
        enumeration_response_duration_ms(max_hop_count);

    if (duration_ms == 0u) {
        return false;
    }
    return now_ms >= start_ms &&
           now_ms - start_ms >= duration_ms;
}

int enumeration_response_lane_begin(
    struct enumeration_response_lane *lane,
    uint32_t network_id,
    uint32_t epoch,
    uint64_t local_id,
    uint64_t parent_id,
    uint8_t hop_count,
    uint64_t start_ms)
{
    if (lane == NULL) {
        return PROTO_ERR_ARG;
    }
    if (network_id == 0u || epoch == 0u || local_id == 0u ||
        parent_id == 0u || local_id == parent_id || hop_count == 0u ||
        hop_count > UWB_ENUM_MAX_HOPS || start_ms == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    memset(lane, 0, sizeof(*lane));
    lane->network_id = network_id;
    lane->epoch = epoch;
    lane->parent_id = parent_id;
    lane->hop_count = hop_count;
    lane->start_ms = start_ms;
    lane->record_count = 1u;
    lane->record_ids[0] = local_id;
    packed_hop_set(lane, 0u, hop_count);
    lane->prepared_round = UINT8_MAX;
    memset(lane->round_offsets_ms,
           ENUMERATION_RESPONSE_NO_OFFSET,
           sizeof(lane->round_offsets_ms));
    lane->active = true;
    return PROTO_OK;
}

void enumeration_response_lane_stop(struct enumeration_response_lane *lane)
{
    if (lane != NULL) {
        lane->active = false;
    }
}

static int record_index(const struct enumeration_response_lane *lane,
                        uint64_t anchor_id)
{
    for (uint8_t i = 0u; i < lane->record_count; i++) {
        if (lane->record_ids[i] == anchor_id) {
            return i;
        }
    }
    return -1;
}

int enumeration_response_lane_merge_bundle(
    struct enumeration_response_lane *lane,
    const struct uwb_enumeration_bundle_frame *bundle,
    bool *added_records)
{
    bool added = false;
    uint8_t previous_record_count;

    if (lane == NULL || bundle == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!lane->active ||
        bundle->network_id != lane->network_id ||
        bundle->epoch != lane->epoch ||
        bundle->parent_id != lane->record_ids[0] ||
        bundle->sender_id == 0u || bundle->sender_id == lane->record_ids[0] ||
        bundle->record_count == 0u ||
        bundle->record_count > UWB_ENUM_RECORDS_PER_BUNDLE) {
        return PROTO_ERR_MALFORMED;
    }

    previous_record_count = lane->record_count;
    for (uint8_t i = 0u; i < bundle->record_count; i++) {
        int index = record_index(lane, bundle->records[i].anchor_id);

        if (bundle->records[i].anchor_id == 0u ||
            bundle->records[i].hop_count == 0u ||
            bundle->records[i].hop_count > UWB_ENUM_MAX_HOPS) {
            return PROTO_ERR_MALFORMED;
        }
        if (index >= 0) {
            if (packed_hop_get(lane, (uint8_t)index) !=
                bundle->records[i].hop_count) {
                return PROTO_ERR_MALFORMED;
            }
            continue;
        }
        if (lane->record_count >= MESH_CONNECTED_MAX_ANCHORS) {
            return PROTO_ERR_NO_SPACE;
        }
        lane->record_ids[lane->record_count] =
            bundle->records[i].anchor_id;
        packed_hop_set(lane,
                       lane->record_count,
                       bundle->records[i].hop_count);
        lane->record_count++;
        added = true;
    }
    if (added) {
        uint8_t bundle_count = bundle_count_for_records(lane->record_count);
        uint8_t first_changed_bundle =
            (uint8_t)(previous_record_count / UWB_ENUM_RECORDS_PER_BUNDLE);

        /* A parent may already have delivered its own shallower bundle when
         * a deeper child arrives. Mark every affected aggregate dirty so it
         * is sent again in the later depth band with the new records. ACKs
         * are content-local, so no earlier ACK can legitimately acknowledge the
         * expanded contents. */
        for (uint8_t sequence = first_changed_bundle;
             sequence < bundle_count;
             sequence++) {
            lane->acked_mask &= (uint8_t)~(1u << sequence);
            lane->attempted_mask &= (uint8_t)~(1u << sequence);
        }
        lane->prepared_round = UINT8_MAX;
    }
    if (added_records != NULL) {
        *added_records = added;
    }
    return PROTO_OK;
}

int enumeration_response_lane_prepare_round(
    struct enumeration_response_lane *lane,
    uint8_t round,
    uint32_t random_value)
{
    uint8_t pending[ENUMERATION_RESPONSE_MAX_BUNDLES];
    uint8_t pending_count = 0u;
    uint8_t bundle_count;
    uint32_t random_state;

    if (lane == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!lane->active || round >= ENUMERATION_RESPONSE_MAX_ROUNDS_PER_DEPTH) {
        return PROTO_ERR_MALFORMED;
    }
    bundle_count = bundle_count_for_records(lane->record_count);
    if (lane->prepared_round == round) {
        return PROTO_OK;
    }
    lane->attempted_mask = 0u;
    for (uint8_t sequence = 0u; sequence < bundle_count; sequence++) {
        if ((lane->acked_mask & (1u << sequence)) == 0u) {
            pending[pending_count++] = sequence;
        }
        lane->round_offsets_ms[sequence] = ENUMERATION_RESPONSE_NO_OFFSET;
    }
    if (pending_count == 0u) {
        lane->prepared_round = round;
        return PROTO_OK;
    }

    random_state = random_value;
    for (uint8_t i = 0u; i < pending_count; i++) {
        bool assigned = false;

        for (uint16_t draw = 0u;
             draw < (uint16_t)ENUMERATION_RESPONSE_TX_WINDOW_MS * 2u;
             draw++) {
            uint8_t offset_ms;
            bool separated = true;

            if (draw != 0u || i != 0u) {
                random_state ^= random_state << 13u;
                random_state ^= random_state >> 17u;
                random_state ^= random_state << 5u;
                if (random_state == 0u) {
                    random_state = 0x9e3779b9u ^ random_value ^ draw;
                }
            }
            offset_ms = (uint8_t)(random_state %
                ENUMERATION_RESPONSE_TX_WINDOW_MS);
            for (uint8_t previous = 0u; previous < i; previous++) {
                uint8_t previous_offset =
                    lane->round_offsets_ms[pending[previous]];
                uint8_t distance = offset_ms > previous_offset ?
                    (uint8_t)(offset_ms - previous_offset) :
                    (uint8_t)(previous_offset - offset_ms);

                if (distance <
                    ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS) {
                    separated = false;
                    break;
                }
            }
            if (!separated) {
                continue;
            }
            lane->round_offsets_ms[pending[i]] = offset_ms;
            assigned = true;
            break;
        }
        /* Random draws should find space quickly. The bounded linear fallback
         * makes full-roster capacity deterministic even for an unlucky PRNG
         * sequence; it does not create shared network-wide slots. */
        for (uint8_t offset_ms = 0u;
             !assigned && offset_ms < ENUMERATION_RESPONSE_TX_WINDOW_MS;
             offset_ms++) {
            bool separated = true;

            for (uint8_t previous = 0u; previous < i; previous++) {
                uint8_t previous_offset =
                    lane->round_offsets_ms[pending[previous]];
                uint8_t distance = offset_ms > previous_offset ?
                    (uint8_t)(offset_ms - previous_offset) :
                    (uint8_t)(previous_offset - offset_ms);

                if (distance <
                    ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS) {
                    separated = false;
                    break;
                }
            }
            if (separated) {
                lane->round_offsets_ms[pending[i]] = offset_ms;
                assigned = true;
            }
        }
        if (!assigned) {
            return PROTO_ERR_BUSY;
        }
    }
    lane->prepared_round = round;
    return PROTO_OK;
}

int enumeration_response_lane_bundle_for_offset(
    struct enumeration_response_lane *lane,
    const struct enumeration_response_timing *timing,
    struct uwb_enumeration_bundle_frame *bundle)
{
    uint8_t first_record;
    uint8_t remaining;
    uint8_t sequence = UINT8_MAX;
    uint8_t bundle_count;

    if (lane == NULL || timing == NULL || bundle == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!lane->active ||
        !enumeration_response_lane_tx_round_allowed(
            timing->depth, lane->hop_count, timing->round) ||
        timing->round >= ENUMERATION_RESPONSE_MAX_ROUNDS_PER_DEPTH ||
        lane->prepared_round != timing->round) {
        return PROTO_ERR_NOT_FOUND;
    }
    bundle_count = bundle_count_for_records(lane->record_count);
    for (uint8_t i = 0u; i < bundle_count; i++) {
        uint8_t offset_ms = lane->round_offsets_ms[i];

        if (offset_ms != ENUMERATION_RESPONSE_NO_OFFSET &&
            timing->round_offset_ms >= offset_ms &&
            (uint8_t)(timing->round_offset_ms - offset_ms) <=
                ENUMERATION_RESPONSE_TX_LATE_GUARD_MS &&
            (lane->acked_mask & (1u << i)) == 0u) {
            sequence = i;
            break;
        }
    }
    if (sequence == UINT8_MAX ||
        (lane->attempted_mask & (1u << sequence)) != 0u) {
        return PROTO_ERR_NOT_FOUND;
    }

    memset(bundle, 0, sizeof(*bundle));
    bundle->network_id = lane->network_id;
    bundle->epoch = lane->epoch;
    bundle->sender_id = lane->record_ids[0];
    bundle->parent_id = lane->parent_id;
    bundle->sequence = sequence;
    first_record = (uint8_t)(sequence * UWB_ENUM_RECORDS_PER_BUNDLE);
    remaining = (uint8_t)(lane->record_count - first_record);
    bundle->record_count = remaining > UWB_ENUM_RECORDS_PER_BUNDLE ?
        UWB_ENUM_RECORDS_PER_BUNDLE : remaining;
    for (uint8_t i = 0u; i < bundle->record_count; i++) {
        bundle->records[i].anchor_id = lane->record_ids[first_record + i];
        bundle->records[i].hop_count =
            packed_hop_get(lane, (uint8_t)(first_record + i));
    }
    lane->attempted_mask |= (uint8_t)(1u << sequence);
    return PROTO_OK;
}

bool enumeration_response_lane_note_ack(
    struct enumeration_response_lane *lane,
    const struct uwb_enumeration_hop_ack_frame *ack)
{
    if (lane == NULL || ack == NULL || !lane->active ||
        ack->network_id != lane->network_id || ack->epoch != lane->epoch ||
        ack->parent_id != lane->parent_id ||
        ack->child_id != lane->record_ids[0] ||
        ack->sequence >= bundle_count_for_records(lane->record_count)) {
        return false;
    }
    lane->acked_mask |= (uint8_t)(1u << ack->sequence);
    return true;
}

bool enumeration_response_lane_all_acked(
    const struct enumeration_response_lane *lane)
{
    uint8_t expected_mask;

    if (lane == NULL || !lane->active || lane->record_count == 0u) {
        return false;
    }
    expected_mask = (uint8_t)((1u <<
        bundle_count_for_records(lane->record_count)) - 1u);
    return (lane->acked_mask & expected_mask) == expected_mask;
}

uint64_t enumeration_response_lane_local_id(
    const struct enumeration_response_lane *lane)
{
    return lane == NULL || lane->record_count == 0u ? 0u :
        lane->record_ids[0];
}

uint8_t enumeration_response_lane_bundle_count(
    const struct enumeration_response_lane *lane)
{
    return lane == NULL ? 0u : bundle_count_for_records(lane->record_count);
}

uint8_t enumeration_response_lane_round_offset_ms(
    const struct enumeration_response_lane *lane,
    uint8_t sequence)
{
    if (lane == NULL || sequence >= ENUMERATION_RESPONSE_MAX_BUNDLES) {
        return ENUMERATION_RESPONSE_NO_OFFSET;
    }
    return lane->round_offsets_ms[sequence];
}

uint8_t enumeration_response_lane_next_offset_ms(
    const struct enumeration_response_lane *lane,
    const struct enumeration_response_timing *timing)
{
    uint8_t next_offset_ms = ENUMERATION_RESPONSE_NO_OFFSET;
    uint8_t bundle_count;

    if (lane == NULL || timing == NULL || !lane->active ||
        lane->prepared_round != timing->round) {
        return ENUMERATION_RESPONSE_NO_OFFSET;
    }
    bundle_count = bundle_count_for_records(lane->record_count);
    for (uint8_t sequence = 0u; sequence < bundle_count; sequence++) {
        uint8_t offset_ms = lane->round_offsets_ms[sequence];

        if (offset_ms == ENUMERATION_RESPONSE_NO_OFFSET ||
            offset_ms <= timing->round_offset_ms ||
            (lane->acked_mask & (1u << sequence)) != 0u ||
            (lane->attempted_mask & (1u << sequence)) != 0u) {
            continue;
        }
        if (next_offset_ms == ENUMERATION_RESPONSE_NO_OFFSET ||
            offset_ms < next_offset_ms) {
            next_offset_ms = offset_ms;
        }
    }
    return next_offset_ms;
}

#include "discovery_assignment.h"
#include "device_identity.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "mesh_sim.h"
#include "protocol.h"
#include "survey.h"
#include "uwb.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    ANCHOR_COUNT = UWB_DISCOVERY_SLOT_COUNT,
    CLAIM_MAX_ROUNDS = 4u,
    TABLE_MAX_ROUNDS = 1u,
    RADIO_GUARD_US = 500u,
    PHASE_GUARD_US = 1000u,
    DISCOVERY_COMMAND_EXPIRY_S = 20u,
};

#define SCENARIO_SEED UINT32_C(0x50D15C07)
#define DISCOVERY_EPOCH UINT32_C(0xD15C5001)
#define CLAIM_SESSION_BASE UINT32_C(0xC1A10000)
#define TABLE_SESSION UINT32_C(0x7AB15001)
#define SURVEY_ID UINT32_C(0x50665006)
#define GATEWAY_ID UINT64_C(0xA001000000000001)
#define ANCHOR_ID_BASE UINT64_C(0xA002000000010000)

_Static_assert(MESH_NETWORK_MAX_HOPS == DISCOVERY_ASSIGNMENT_MAX_HOPS,
               "assignment scheduling and ACK return must share one hop ceiling");
_Static_assert(UWB_DISCOVERY_SLOT_COUNT == 50u,
               "the deployment-scale discovery scenario must cover 50 anchors");

_Static_assert(ANCHOR_COUNT == 50u, "scenario must exercise all 50 discovery slots");
_Static_assert(ANCHOR_COUNT + 1u <= MESH_SIM_MAX_ROLES,
               "simulator must hold the gateway and 50 anchors");
_Static_assert(SURVEY_GATEWAY_MAX_PAIRS == 150u,
               "50 anchors at degree six must produce 150 pairs");

struct anchor_state {
    uint64_t id;
    uint64_t hash;
    uint8_t node_index;
    uint8_t hop_count;
    uint8_t claim_slot;
    uint8_t assignment_slot;
    uint8_t table_first_round;
    bool claim_collected;
    bool table_received;
    bool acked;
};

struct scenario_summary {
    size_t claim_collisions;
    size_t ack_collisions;
    size_t initial_table_misses;
    uint8_t claim_rounds;
    uint8_t table_rounds;
};

static int fail_at(int line, const char *format, ...)
{
    va_list args;

    fprintf(stderr, "FAIL mesh_discovery seed=0x%08" PRIx32 " line=%d: ",
            SCENARIO_SEED, line);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    return 1;
}

#define REQUIRE(condition, ...)                 \
    do {                                        \
        if (!(condition)) {                     \
            return fail_at(__LINE__, __VA_ARGS__); \
        }                                       \
    } while (0)

static int test_identical_anchor_image_identity_and_ordering(void)
{
    struct discovery_assignment_claim claims[2];
    struct discovery_assignment_entry entries[2];
    uint64_t first_reboot_id = 0u;
    int ret;

    REQUIRE(device_identity_anchor_from_ficr(UINT32_C(0x01020304),
                                             UINT32_C(0x11121314),
                                             &claims[0].anchor_id),
            "first hardware identity rejected");
    REQUIRE(device_identity_anchor_from_ficr(UINT32_C(0x21222324),
                                             UINT32_C(0x31323334),
                                             &claims[1].anchor_id),
            "second hardware identity rejected");
    REQUIRE(claims[0].anchor_id != claims[1].anchor_id,
            "identical image produced duplicate anchor IDs");
    REQUIRE(device_identity_anchor_from_ficr(UINT32_C(0x01020304),
                                             UINT32_C(0x11121314),
                                             &first_reboot_id) &&
                first_reboot_id == claims[0].anchor_id,
            "anchor identity changed across reboot");
    for (size_t i = 0u; i < 2u; i++) {
        claims[i].hash = discovery_assignment_hash(claims[i].anchor_id);
    }
    ret = discovery_assignment_sort_claims(claims, 2u);
    REQUIRE(ret == PROTO_OK, "gateway claim ordering ret=%d", ret);
    ret = discovery_assignment_entries_from_claims(claims, 2u, entries, 2u);
    REQUIRE(ret == PROTO_OK, "gateway assignment ret=%d", ret);
    REQUIRE(entries[0].slot == 0u && entries[1].slot == 1u &&
                entries[0].anchor_id != entries[1].anchor_id,
            "gateway did not assign independent logical slots");
    return 0;
}

static struct anchor_state *anchor_by_id(struct anchor_state *anchors,
                                         uint64_t anchor_id)
{
    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        if (anchors[i].id == anchor_id) {
            return &anchors[i];
        }
    }
    return NULL;
}

static size_t decoded_state_count(const struct anchor_state *anchors,
                                  enum discovery_assignment_phase phase)
{
    size_t count = 0u;

    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        if ((phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
             anchors[i].claim_collected) ||
            (phase == DISCOVERY_ASSIGNMENT_PHASE_TABLE &&
             anchors[i].table_received) ||
            (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK && anchors[i].acked)) {
            count++;
        }
    }
    return count;
}

static const struct mesh_sim_reception *find_reception(
    const struct mesh_sim_world *world,
    size_t first_reception,
    uint64_t source_id,
    uint64_t receiver_id,
    size_t *match_count)
{
    const struct mesh_sim_reception *match = NULL;

    *match_count = 0u;
    for (size_t i = first_reception; i < world->reception_count; i++) {
        const struct mesh_sim_reception *reception = &world->receptions[i];

        if (reception->source_id == source_id &&
            reception->receiver_id == receiver_id) {
            match = reception;
            (*match_count)++;
        }
    }
    return match;
}

static int decode_transmission(const struct mesh_sim_world *world,
                               uint16_t transmission_index,
                               struct proto_packet *packet,
                               const uint8_t **payload,
                               size_t *payload_len)
{
    const struct mesh_sim_transmission *transmission;

    if (transmission_index >= world->transmission_count) {
        return PROTO_ERR_ARG;
    }
    transmission = &world->transmissions[transmission_index];
    return proto_packet_decode(transmission->frame,
                               transmission->frame_len,
                               packet,
                               payload,
                               payload_len);
}

/* Discovery assignment is app-owned, while mesh_sim dispatches protocol frames
 * through the relay core. Keep the exact wire image, model its PHY reception,
 * then protocol-decode only frames that mesh_sim proved were received. */
static int schedule_application_packet(struct mesh_sim_world *world,
                                       uint8_t node_index,
                                       uint64_t start_us,
                                       uint8_t channel,
                                       enum mesh_sim_phy phy,
                                       const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint16_t *transmission_index,
                                       size_t *frame_len)
{
    uint8_t frame[PACKET_EXT_MAX_LEN];
    size_t encoded_len = 0u;
    int ret;

    if (packet == NULL || packet->payload_len != payload_len) {
        return PROTO_ERR_ARG;
    }
    ret = proto_packet_encode(packet,
                              payload,
                              frame,
                              sizeof(frame),
                              &encoded_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_sim_schedule_raw_tx(world,
                                   node_index,
                                   start_us,
                                   channel,
                                   phy,
                                   frame,
                                   encoded_len,
                                   false,
                                   transmission_index);
    if (ret == MESH_SIM_OK && frame_len != NULL) {
        *frame_len = encoded_len;
    }
    return ret;
}

static int append_discovery_command_envelope(
    struct mesh_outbound *outbound,
    enum discovery_assignment_phase phase,
    uint32_t command_session,
    uint16_t packet_seq)
{
    size_t payload_len = 0u;
    int ret;

    if (outbound == NULL || command_session == 0u || packet_seq == 0u) {
        return PROTO_ERR_ARG;
    }
    memset(outbound, 0, sizeof(*outbound));
    ret = tlv_append_u16(outbound->payload,
                         sizeof(outbound->payload),
                         &payload_len,
                         TLV_COMMAND_ID,
                         CMD_ASSIGN_DISCOVERY_SLOTS);
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound->payload,
                            sizeof(outbound->payload),
                            &payload_len,
                            TLV_COMMAND_SCOPE,
                            CMD_SCOPE_ALL_HEARD);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound->payload,
                            sizeof(outbound->payload),
                            &payload_len,
                            TLV_COMMAND_RESPONSE_MODE,
                            CMD_RESPONSE_NONE);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_COMMAND_SEQ,
                             command_session);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_FLOOD_EPOCH_ID,
                             command_session);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_COMMAND_EXPIRY_S,
                             DISCOVERY_COMMAND_EXPIRY_S);
    }
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(outbound->payload,
                                                       sizeof(outbound->payload),
                                                       &payload_len,
                                                       phase,
                                                       DISCOVERY_EPOCH);
    }
    if (ret != PROTO_OK) {
        return ret;
    }

    outbound->packet = (struct proto_packet) {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = GATEWAY_ID,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = command_session,
        .seq = packet_seq,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
        .payload_len = (uint16_t)payload_len,
    };
    outbound->payload_len = (uint16_t)payload_len;
    outbound->next_hop_id = MESH_BROADCAST_ID;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return gateway_command_append_default_flood_controls(outbound);
}

static int build_discovery_response(const struct anchor_state *anchor,
                                    enum discovery_assignment_phase phase,
                                    uint32_t command_session,
                                    uint16_t packet_seq,
                                    struct mesh_outbound *outbound)
{
    size_t payload_len = 0u;
    int ret;

    memset(outbound, 0, sizeof(*outbound));
    ret = mesh_append_command_result(outbound->payload,
                                     sizeof(outbound->payload),
                                     &payload_len,
                                     CMD_ASSIGN_DISCOVERY_SLOTS,
                                     COMMAND_OK,
                                     0u);
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(outbound->payload,
                                                       sizeof(outbound->payload),
                                                       &payload_len,
                                                       phase,
                                                       DISCOVERY_EPOCH);
    }
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_claim_hash(outbound->payload,
                                                     sizeof(outbound->payload),
                                                     &payload_len,
                                                     anchor->hash);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound->payload,
                            sizeof(outbound->payload),
                            &payload_len,
                            TLV_HOP_COUNT,
                            anchor->hop_count);
    }
    if (ret != PROTO_OK || payload_len > UINT8_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    ret = mesh_init_command_result(&outbound->packet,
                                   anchor->id,
                                   GATEWAY_ID,
                                   command_session,
                                   packet_seq,
                                   (uint8_t)payload_len,
                                   true);
    if (ret != PROTO_OK) {
        return ret;
    }
    outbound->payload_len = (uint16_t)payload_len;
    return PROTO_OK;
}

static int validate_discovery_response(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct anchor_state *anchor,
    enum discovery_assignment_phase expected_phase,
    uint32_t expected_session)
{
    enum discovery_assignment_phase phase = 0;
    const uint8_t *raw = NULL;
    uint64_t hash = 0u;
    uint32_t epoch = 0u;
    uint8_t raw_len = 0u;
    int ret;

    REQUIRE(packet->msg_type == MSG_COMMAND_RESULT &&
            packet->src_id == anchor->id && packet->dst_id == GATEWAY_ID,
            "response identity phase=%u src=0x%016" PRIx64,
            expected_phase, packet->src_id);
    REQUIRE(packet->session_id == expected_session &&
            packet->payload_len == payload_len &&
            (packet->flags & FLAG_DIAGNOSTIC) != 0u,
            "response envelope phase=%u src=0x%016" PRIx64
            " session=%" PRIu32 " expected=%" PRIu32,
            expected_phase, anchor->id, packet->session_id, expected_session);
    ret = tlv_find(payload, payload_len, TLV_COMMAND_ID, &raw, &raw_len);
    REQUIRE(ret == PROTO_OK && raw_len == sizeof(uint16_t) &&
            proto_get_u16_le(raw) == CMD_ASSIGN_DISCOVERY_SLOTS,
            "response command TLV phase=%u src=0x%016" PRIx64 " ret=%d",
            expected_phase, anchor->id, ret);
    ret = tlv_find(payload, payload_len, TLV_COMMAND_STATUS, &raw, &raw_len);
    REQUIRE(ret == PROTO_OK && raw_len == sizeof(uint16_t) &&
            proto_get_u16_le(raw) == COMMAND_OK,
            "response status phase=%u src=0x%016" PRIx64 " ret=%d",
            expected_phase, anchor->id, ret);
    ret = discovery_assignment_extract_control_tlvs(payload,
                                                    payload_len,
                                                    &phase,
                                                    &epoch);
    REQUIRE(ret == PROTO_OK && phase == expected_phase &&
            epoch == DISCOVERY_EPOCH,
            "response control phase=%u src=0x%016" PRIx64
            " decoded_phase=%u epoch=%" PRIu32 " ret=%d",
            expected_phase, anchor->id, phase, epoch, ret);
    ret = discovery_assignment_extract_claim_hash(payload, payload_len, &hash);
    REQUIRE(ret == PROTO_OK && hash == anchor->hash,
            "response hash phase=%u src=0x%016" PRIx64
            " hash=0x%016" PRIx64 " ret=%d",
            expected_phase, anchor->id, hash, ret);
    ret = tlv_find(payload, payload_len, TLV_HOP_COUNT, &raw, &raw_len);
    REQUIRE(ret == PROTO_OK && raw_len == sizeof(uint8_t) &&
            raw[0] == anchor->hop_count,
            "response hop phase=%u src=0x%016" PRIx64
            " hop=%u expected=%u ret=%d",
            expected_phase, anchor->id,
            raw_len == sizeof(uint8_t) ? raw[0] : 0u,
            anchor->hop_count, ret);
    return 0;
}

static int verify_response_order_is_hop_then_slot(void)
{
    static const uint16_t spreads[] = {
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MIN_MS,
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_MAX_MS,
    };
    static const uint8_t bench_slots[] = {0u, 10u, 29u};

    for (size_t spread_index = 0u;
         spread_index < sizeof(spreads) / sizeof(spreads[0]);
         spread_index++) {
        uint16_t spread_ms = spreads[spread_index];
        uint32_t jitter_cap_ms =
            DISCOVERY_ASSIGNMENT_RESPONSE_JITTER_CAP_MS(spread_ms);

        for (uint8_t hop = 1u; hop < DISCOVERY_ASSIGNMENT_MAX_HOPS; hop++) {
            uint32_t latest_ms = 0u;
            uint32_t next_earliest_ms = 0u;
            int ret = discovery_assignment_response_delay_ms(
                ANCHOR_COUNT - 1u,
                ANCHOR_COUNT,
                hop,
                spread_ms,
                0u,
                jitter_cap_ms - 1u,
                &latest_ms);

            REQUIRE(ret == PROTO_OK,
                    "latest hop response hop=%u spread=%u ret=%d",
                    hop, spread_ms, ret);
            ret = discovery_assignment_response_delay_ms(
                0u,
                ANCHOR_COUNT,
                hop + 1u,
                spread_ms,
                0u,
                0u,
                &next_earliest_ms);
            REQUIRE(ret == PROTO_OK,
                    "next hop response hop=%u spread=%u ret=%d",
                    hop + 1u, spread_ms, ret);
            REQUIRE(latest_ms < next_earliest_ms,
                    "first-contact cells overlap hop=%u spread=%u latest=%" PRIu32
                    " next=%" PRIu32,
                    hop, spread_ms, latest_ms, next_earliest_ms);
        }
    }

    for (size_t index = 0u; index + 1u < 3u; index++) {
        uint8_t hop = (uint8_t)index + 1u;
        uint32_t jitter_cap_ms =
            DISCOVERY_ASSIGNMENT_RESPONSE_JITTER_CAP_MS(
                DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS);
        uint32_t latest_ms = 0u;
        uint32_t next_earliest_ms = 0u;
        int ret = discovery_assignment_response_delay_ms(
            bench_slots[index],
            30u,
            hop,
            DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
            0u,
            jitter_cap_ms - 1u,
            &latest_ms);

        REQUIRE(ret == PROTO_OK,
                "bench hop=%u slot=%u latest rejected ret=%d",
                hop, bench_slots[index], ret);
        ret = discovery_assignment_response_delay_ms(
            bench_slots[index + 1u],
            30u,
            hop + 1u,
            DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
            0u,
            0u,
            &next_earliest_ms);
        REQUIRE(ret == PROTO_OK,
                "bench hop=%u slot=%u earliest rejected ret=%d",
                hop + 1u, bench_slots[index + 1u], ret);
        REQUIRE(latest_ms < next_earliest_ms,
                "bench hop=%u slot=%u overlaps hop=%u slot=%u"
                " latest=%" PRIu32 " next=%" PRIu32,
                hop, bench_slots[index], hop + 1u,
                bench_slots[index + 1u], latest_ms,
                next_earliest_ms);
    }
    return 0;
}

static int initialize_world(struct mesh_sim_world *world,
                            struct anchor_state *anchors,
                            uint8_t *gateway_index,
                            size_t claim_collision_pair[2])
{
    uint64_t candidate = ANCHOR_ID_BASE;
    uint8_t hop_mask = 0u;
    int ret;

    mesh_sim_init(world, SCENARIO_SEED);
    ret = mesh_sim_add_role(world,
                            MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID,
                            GATEWAY_ID,
                            DISCOVERY_EPOCH,
                            gateway_index);
    REQUIRE(ret == MESH_SIM_OK,
            "add gateway ret=%d", ret);

    anchors[0].id = candidate++;
    anchors[0].hash = discovery_assignment_hash(anchors[0].id);
    anchors[0].claim_slot = (uint8_t)(anchors[0].hash % ANCHOR_COUNT);
    while (discovery_assignment_hash(candidate) % ANCHOR_COUNT !=
           anchors[0].claim_slot) {
        candidate++;
    }
    anchors[1].id = candidate++;

    for (size_t i = 2u; i < ANCHOR_COUNT; i++) {
        anchors[i].id = candidate++;
    }
    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        if (i != 0u) {
            anchors[i].hash = discovery_assignment_hash(anchors[i].id);
            anchors[i].claim_slot = (uint8_t)(anchors[i].hash % ANCHOR_COUNT);
        }
        anchors[i].hop_count = (uint8_t)(1u + (i % DISCOVERY_ASSIGNMENT_MAX_HOPS));
        anchors[i].assignment_slot = UINT8_MAX;
        anchors[i].table_first_round = UINT8_MAX;
        ret = mesh_sim_add_role(world,
                                MESH_SIM_ROLE_ANCHOR,
                                anchors[i].id,
                                GATEWAY_ID,
                                DISCOVERY_EPOCH,
                                &anchors[i].node_index);
        REQUIRE(ret == MESH_SIM_OK,
                "add anchor index=%zu ret=%d", i, ret);
        ret = mesh_sim_set_link(world,
                                *gateway_index,
                                anchors[i].node_index,
                                (uint8_t)(90u + (i % 6u)),
                                (uint16_t)(1u + (i % DISCOVERY_ASSIGNMENT_MAX_HOPS)));
        REQUIRE(ret == MESH_SIM_OK,
                "link anchor index=%zu ret=%d", i, ret);
    }

    claim_collision_pair[0] = 0u;
    claim_collision_pair[1] = 1u;
    anchors[0].hop_count = 6u;
    anchors[1].hop_count = 6u;
    REQUIRE(anchors[0].claim_slot == anchors[1].claim_slot,
            "constructed claim collision slots=%u/%u",
            anchors[0].claim_slot, anchors[1].claim_slot);

    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        REQUIRE(anchors[i].hop_count >= 1u &&
                anchors[i].hop_count <= DISCOVERY_ASSIGNMENT_MAX_HOPS,
                "anchor index=%zu invalid hop=%u", i, anchors[i].hop_count);
        hop_mask |= (uint8_t)(1u << (anchors[i].hop_count - 1u));
    }
    REQUIRE(hop_mask == UINT8_MAX,
            "hop coverage mask=0x%02x", hop_mask);
    return 0;
}

static int collect_claims(struct mesh_sim_world *world,
                          struct anchor_state *anchors,
                          uint8_t gateway_index,
                          const size_t collision_pair[2],
                          struct discovery_assignment_claim *claims,
                          size_t *claim_count,
                          struct scenario_summary *summary)
{
    const uint32_t window_ms = discovery_assignment_collection_window_ms(
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
        DISCOVERY_ASSIGNMENT_MAX_HOPS);
    size_t first_round_count = 0u;

    REQUIRE(window_ms != 0u,
            "claim collection window is zero");
    *claim_count = 0u;

    for (uint8_t round = 0u; round < CLAIM_MAX_ROUNDS; round++) {
        uint16_t transmission_indices[ANCHOR_COUNT];
        uint64_t round_start_us = world->now_us + PHASE_GUARD_US;
        uint64_t round_end_us = round_start_us + (uint64_t)window_ms * 1000u;
        size_t first_reception = world->reception_count;
        uint16_t window_index = UINT16_MAX;
        int ret;

        for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
            transmission_indices[i] = UINT16_MAX;
        }
        ret = mesh_sim_schedule_rx(world,
                                   gateway_index,
                                   round_start_us,
                                   round_end_us,
                                   UWB_CHANNEL_MESH_PAYLOAD,
                                   MESH_SIM_PHY_CHANNEL9_MESH,
                                   &window_index);
        REQUIRE(ret == MESH_SIM_OK,
                "claim round=%u gateway RX ret=%d", round, ret);

        for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
            struct mesh_outbound response;
            uint32_t random_value;
            uint32_t delay_ms = 0u;
            uint16_t seq;

            if (anchors[i].claim_collected) {
                continue;
            }
            random_value = mesh_sim_random(world);
            if (round == 0u &&
                (i == collision_pair[0] || i == collision_pair[1])) {
                random_value = 0u;
            }
            ret = discovery_assignment_response_delay_ms(
                anchors[i].claim_slot,
                ANCHOR_COUNT,
                anchors[i].hop_count,
                DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
                round,
                random_value,
                &delay_ms);
            REQUIRE(ret == PROTO_OK,
                    "claim delay round=%u index=%zu ret=%d", round, i, ret);
            seq = (uint16_t)(1u + (uint16_t)round * ANCHOR_COUNT + i);
            ret = build_discovery_response(&anchors[i],
                                           DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                           CLAIM_SESSION_BASE + round + 1u,
                                           seq,
                                           &response);
            REQUIRE(ret == PROTO_OK,
                    "build claim round=%u index=%zu ret=%d", round, i, ret);
            ret = schedule_application_packet(
                world,
                anchors[i].node_index,
                round_start_us + (uint64_t)delay_ms * 1000u,
                UWB_CHANNEL_MESH_PAYLOAD,
                MESH_SIM_PHY_CHANNEL9_MESH,
                &response.packet,
                response.payload,
                response.payload_len,
                &transmission_indices[i],
                NULL);
            REQUIRE(ret == MESH_SIM_OK,
                    "schedule claim round=%u index=%zu delay=%" PRIu32
                    " ret=%d",
                    round, i, delay_ms, ret);
            REQUIRE(world->transmissions[transmission_indices[i]].end_rctu +
                        world->propagation_rctu[anchors[i].node_index][gateway_index] <=
                    world->rx_windows[window_index].end_rctu,
                    "claim outside window round=%u index=%zu delay=%" PRIu32,
                    round, i, delay_ms);
        }

        ret = mesh_sim_run_until(world, round_end_us);
        REQUIRE(ret == MESH_SIM_OK,
                "run claim round=%u ret=%d last=%d",
                round, ret, world->last_error);

        for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
            const struct mesh_sim_reception *reception;
            struct proto_packet packet;
            const uint8_t *payload = NULL;
            size_t payload_len = 0u;
            size_t matches = 0u;

            if (transmission_indices[i] == UINT16_MAX) {
                continue;
            }
            reception = find_reception(world,
                                       first_reception,
                                       anchors[i].id,
                                       GATEWAY_ID,
                                       &matches);
            REQUIRE(reception != NULL && matches == 1u,
                    "claim reception round=%u index=%zu matches=%zu",
                    round, i, matches);
            if (reception->outcome != MESH_SIM_RX_DECODED) {
                REQUIRE(reception->outcome == MESH_SIM_RX_COLLISION,
                        "full-window claim round=%u index=%zu outcome=%u",
                        round, i, reception->outcome);
                summary->claim_collisions++;
                continue;
            }
            REQUIRE(world->rx_windows[window_index].start_rctu <=
                        reception->start_rctu &&
                    world->rx_windows[window_index].end_rctu >=
                        reception->end_rctu,
                    "decoded claim not contained round=%u index=%zu", round, i);
            ret = decode_transmission(world,
                                      transmission_indices[i],
                                      &packet,
                                      &payload,
                                      &payload_len);
            REQUIRE(ret == PROTO_OK,
                    "decode claim round=%u index=%zu ret=%d", round, i, ret);
            ret = validate_discovery_response(
                &packet,
                payload,
                payload_len,
                &anchors[i],
                DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                CLAIM_SESSION_BASE + round + 1u);
            if (ret != 0) {
                return ret;
            }
            anchors[i].claim_collected = true;
            claims[*claim_count] = (struct discovery_assignment_claim) {
                .anchor_id = anchors[i].id,
                .hash = anchors[i].hash,
            };
            (*claim_count)++;
        }

        summary->claim_rounds = (uint8_t)(round + 1u);
        if (round == 0u) {
            first_round_count = *claim_count;
            REQUIRE(!anchors[collision_pair[0]].claim_collected &&
                    !anchors[collision_pair[1]].claim_collected,
                    "forced claim collision decoded unexpectedly");
        }
        if (*claim_count == ANCHOR_COUNT) {
            break;
        }
    }

    REQUIRE(first_round_count > 0u && first_round_count < ANCHOR_COUNT,
            "claim retry not exercised first_round=%zu", first_round_count);
    REQUIRE(*claim_count == ANCHOR_COUNT,
            "claim collection incomplete decoded=%zu missing=%zu rounds=%u collisions=%zu",
            *claim_count, ANCHOR_COUNT - *claim_count,
            summary->claim_rounds, summary->claim_collisions);
    REQUIRE(summary->claim_rounds >= 2u &&
            summary->claim_rounds <= CLAIM_MAX_ROUNDS &&
            summary->claim_collisions >= 2u,
            "claim retry evidence rounds=%u collisions=%zu",
            summary->claim_rounds, summary->claim_collisions);
    REQUIRE(decoded_state_count(anchors, DISCOVERY_ASSIGNMENT_PHASE_CLAIM) ==
                ANCHOR_COUNT,
            "claim state diverged from decoded roster");
    return 0;
}

static int build_assignment_table(
    struct anchor_state *anchors,
    struct discovery_assignment_claim *claims,
    size_t claim_count,
    struct discovery_assignment_entry *entries,
    struct mesh_outbound *table_command,
    size_t *encoded_frame_len)
{
    struct discovery_assignment_entry parsed[ANCHOR_COUNT];
    enum discovery_assignment_phase phase = 0;
    uint8_t encoded[PACKET_EXT_MAX_LEN];
    uint8_t slot_count = 0u;
    uint32_t epoch = 0u;
    size_t parsed_count = 0u;
    size_t payload_len;
    int ret;

    ret = discovery_assignment_sort_claims(claims, claim_count);
    REQUIRE(ret == PROTO_OK,
            "sort decoded claims ret=%d count=%zu", ret, claim_count);
    ret = discovery_assignment_entries_from_claims(claims,
                                                   claim_count,
                                                   entries,
                                                   ANCHOR_COUNT);
    REQUIRE(ret == PROTO_OK,
            "assign decoded claims ret=%d count=%zu", ret, claim_count);
    for (size_t i = 0u; i < claim_count; i++) {
        struct anchor_state *anchor = anchor_by_id(anchors, entries[i].anchor_id);

        REQUIRE(anchor != NULL && entries[i].slot == i &&
                entries[i].hash == anchor->hash,
                "assignment index=%zu id=0x%016" PRIx64 " slot=%u",
                i, entries[i].anchor_id, entries[i].slot);
        if (i > 0u) {
            REQUIRE(entries[i - 1u].hash < entries[i].hash ||
                    (entries[i - 1u].hash == entries[i].hash &&
                     entries[i - 1u].anchor_id < entries[i].anchor_id),
                    "assignment sort order index=%zu", i);
        }
        anchor->assignment_slot = entries[i].slot;
    }

    ret = append_discovery_command_envelope(table_command,
                                            DISCOVERY_ASSIGNMENT_PHASE_TABLE,
                                            TABLE_SESSION,
                                            1000u);
    REQUIRE(ret == PROTO_OK,
            "build table command envelope ret=%d", ret);
    payload_len = table_command->payload_len;
    ret = discovery_assignment_append_table_tlvs(table_command->payload,
                                                 sizeof(table_command->payload),
                                                 &payload_len,
                                                 entries,
                                                 claim_count);
    REQUIRE(ret == PROTO_OK,
            "append 50-entry table ret=%d base=%u",
            ret, table_command->payload_len);
    table_command->payload_len = (uint16_t)payload_len;
    table_command->packet.payload_len = (uint16_t)payload_len;

    REQUIRE(payload_len > PACKET_MAX_PAYLOAD_LEN &&
            payload_len <= PACKET_EXT_MAX_PAYLOAD_LEN &&
            proto_packet_header_len((uint16_t)payload_len) == PACKET_EXT_HEADER_LEN,
            "table payload is not one extended packet bytes=%zu max=%u",
            payload_len, PACKET_EXT_MAX_PAYLOAD_LEN);
    ret = proto_packet_encode(&table_command->packet,
                              table_command->payload,
                              encoded,
                              sizeof(encoded),
                              encoded_frame_len);
    REQUIRE(ret == PROTO_OK && *encoded_frame_len > PACKET_MAX_LEN &&
            *encoded_frame_len <= PACKET_EXT_MAX_LEN &&
            *encoded_frame_len == proto_packet_encoded_len((uint16_t)payload_len),
            "table frame encode ret=%d payload=%zu frame=%zu max=%u",
            ret, payload_len, *encoded_frame_len, PACKET_EXT_MAX_LEN);
    ret = discovery_assignment_extract_control_tlvs(table_command->payload,
                                                    table_command->payload_len,
                                                    &phase,
                                                    &epoch);
    REQUIRE(ret == PROTO_OK && phase == DISCOVERY_ASSIGNMENT_PHASE_TABLE &&
            epoch == DISCOVERY_EPOCH,
            "table control round-trip phase=%u epoch=%" PRIu32 " ret=%d",
            phase, epoch, ret);
    ret = discovery_assignment_parse_table_tlvs(table_command->payload,
                                                table_command->payload_len,
                                                parsed,
                                                ANCHOR_COUNT,
                                                &parsed_count,
                                                &slot_count);
    REQUIRE(ret == PROTO_OK && parsed_count == ANCHOR_COUNT &&
            slot_count == ANCHOR_COUNT,
            "table parse ret=%d entries=%zu slots=%u",
            ret, parsed_count, slot_count);
    for (size_t i = 0u; i < parsed_count; i++) {
        REQUIRE(parsed[i].anchor_id == entries[i].anchor_id &&
                parsed[i].hash == entries[i].hash &&
                parsed[i].slot == entries[i].slot,
                "table round-trip mismatch index=%zu", i);
    }

    /* Model reboot restore from the persisted gateway table by stable ID. */
    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        anchors[i].assignment_slot = UINT8_MAX;
    }
    for (size_t i = 0u; i < parsed_count; i++) {
        struct anchor_state *anchor = anchor_by_id(anchors,
                                                   parsed[i].anchor_id);

        REQUIRE(anchor != NULL && parsed[i].slot < slot_count,
                "persisted assignment restore index=%zu", i);
        anchor->assignment_slot = parsed[i].slot;
    }
    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        REQUIRE(anchors[i].assignment_slot < slot_count &&
                    parsed[anchors[i].assignment_slot].anchor_id ==
                        anchors[i].id,
                "persisted gateway order changed across reboot anchor=%zu",
                i);
    }
    return 0;
}

static int validate_table_reception(
    const struct mesh_sim_world *world,
    uint16_t transmission_index,
    const struct anchor_state *anchor)
{
    struct discovery_assignment_entry entries[ANCHOR_COUNT];
    struct proto_packet packet = {0};
    enum discovery_assignment_phase phase = 0;
    const uint8_t *payload = NULL;
    uint8_t slot_count = 0u;
    uint32_t epoch = 0u;
    size_t entry_count = 0u;
    size_t payload_len = 0u;
    int ret;

    ret = decode_transmission(world,
                              transmission_index,
                              &packet,
                              &payload,
                              &payload_len);
    REQUIRE(ret == PROTO_OK && packet.msg_type == MSG_COMMAND &&
            packet.src_id == GATEWAY_ID && packet.dst_id == MESH_BROADCAST_ID &&
            packet.session_id == TABLE_SESSION && packet.payload_len == payload_len,
            "decode table for slot=%u ret=%d msg=0x%02x session=%" PRIu32,
            anchor->assignment_slot, ret, packet.msg_type, packet.session_id);
    ret = discovery_assignment_extract_control_tlvs(payload,
                                                    payload_len,
                                                    &phase,
                                                    &epoch);
    REQUIRE(ret == PROTO_OK && phase == DISCOVERY_ASSIGNMENT_PHASE_TABLE &&
            epoch == DISCOVERY_EPOCH,
            "table control slot=%u phase=%u epoch=%" PRIu32 " ret=%d",
            anchor->assignment_slot, phase, epoch, ret);
    ret = discovery_assignment_parse_table_tlvs(payload,
                                                payload_len,
                                                entries,
                                                ANCHOR_COUNT,
                                                &entry_count,
                                                &slot_count);
    REQUIRE(ret == PROTO_OK && entry_count == ANCHOR_COUNT &&
            slot_count == ANCHOR_COUNT,
            "table parse slot=%u ret=%d count=%zu slots=%u",
            anchor->assignment_slot, ret, entry_count, slot_count);
    REQUIRE(entries[anchor->assignment_slot].anchor_id == anchor->id &&
            entries[anchor->assignment_slot].hash == anchor->hash &&
            entries[anchor->assignment_slot].slot == anchor->assignment_slot,
            "table ownership slot=%u id=0x%016" PRIx64,
            anchor->assignment_slot, anchor->id);
    return 0;
}

static int publish_table_and_collect_acks(
    struct mesh_sim_world *world,
    struct anchor_state *anchors,
    uint8_t gateway_index,
    struct mesh_outbound *table_command,
    size_t expected_table_frame_len,
    struct scenario_summary *summary)
{
    const uint32_t collection_window_ms =
        discovery_assignment_collection_window_ms(
            DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
            DISCOVERY_ASSIGNMENT_MAX_HOPS);
    size_t first_round_acked = 0u;

    for (uint8_t round = 0u; round < TABLE_MAX_ROUNDS; round++) {
        bool table_decoded_this_round[ANCHOR_COUNT] = { false };
        uint16_t table_rx_windows[ANCHOR_COUNT];
        uint16_t ack_transmissions[ANCHOR_COUNT];
        uint64_t table_start_us = world->now_us + PHASE_GUARD_US;
        uint64_t table_phase_end_us = 0u;
        size_t first_table_reception = world->reception_count;
        uint16_t table_transmission = UINT16_MAX;
        size_t table_frame_len = 0u;
        int ret;

        table_command->packet.seq = (uint16_t)(1000u + round);
        ret = schedule_application_packet(
            world,
            gateway_index,
            table_start_us,
            UWB_CHANNEL_WAKE_CONTACT,
            MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
            &table_command->packet,
            table_command->payload,
            table_command->payload_len,
            &table_transmission,
            &table_frame_len);
        REQUIRE(ret == MESH_SIM_OK && table_frame_len == expected_table_frame_len,
                "schedule table round=%u ret=%d frame=%zu expected=%zu",
                round, ret, table_frame_len, expected_table_frame_len);

        for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
            const struct mesh_sim_transmission *transmission =
                &world->transmissions[table_transmission];
            uint64_t propagation_us =
                world->propagation_us[gateway_index][anchors[i].node_index];
            uint64_t rx_start_us = table_start_us - RADIO_GUARD_US;
            uint64_t rx_end_us;

            rx_end_us = transmission->end_us + propagation_us + RADIO_GUARD_US;
            ret = mesh_sim_schedule_rx(world,
                                       anchors[i].node_index,
                                       rx_start_us,
                                       rx_end_us,
                                       UWB_CHANNEL_WAKE_CONTACT,
                                       MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                       &table_rx_windows[i]);
            REQUIRE(ret == MESH_SIM_OK,
                    "table RX round=%u index=%zu start=%" PRIu64
                    " end=%" PRIu64 " ret=%d",
                    round, i, rx_start_us, rx_end_us, ret);
            if (rx_end_us > table_phase_end_us) {
                table_phase_end_us = rx_end_us;
            }
        }

        ret = mesh_sim_run_until(world, table_phase_end_us + RADIO_GUARD_US);
        REQUIRE(ret == MESH_SIM_OK,
                "run table round=%u ret=%d last=%d",
                round, ret, world->last_error);

        for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
            const struct mesh_sim_reception *reception;
            size_t matches = 0u;

            reception = find_reception(world,
                                       first_table_reception,
                                       GATEWAY_ID,
                                       anchors[i].id,
                                       &matches);
            REQUIRE(reception != NULL && matches == 1u,
                    "table reception round=%u index=%zu matches=%zu",
                    round, i, matches);
            REQUIRE(reception->outcome == MESH_SIM_RX_DECODED,
                    "single-origin table missed round=%u slot=%u outcome=%u",
                    round, anchors[i].assignment_slot, reception->outcome);
            REQUIRE(world->rx_windows[table_rx_windows[i]].start_rctu <=
                        reception->start_rctu &&
                    world->rx_windows[table_rx_windows[i]].end_rctu >=
                        reception->end_rctu,
                    "decoded table not contained round=%u slot=%u",
                    round, anchors[i].assignment_slot);
            ret = validate_table_reception(world,
                                           table_transmission,
                                           &anchors[i]);
            if (ret != 0) {
                return ret;
            }
            table_decoded_this_round[i] = true;
            anchors[i].table_received = true;
            if (anchors[i].table_first_round == UINT8_MAX) {
                anchors[i].table_first_round = round;
            }
        }

        {
            uint64_t ack_start_us = world->now_us + PHASE_GUARD_US;
            uint64_t ack_end_us = ack_start_us +
                                  (uint64_t)collection_window_ms * 1000u;
            size_t first_ack_reception = world->reception_count;
            uint16_t gateway_rx_window = UINT16_MAX;

            for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
                ack_transmissions[i] = UINT16_MAX;
            }
            ret = mesh_sim_schedule_rx(world,
                                       gateway_index,
                                       ack_start_us,
                                       ack_end_us,
                                       UWB_CHANNEL_MESH_PAYLOAD,
                                       MESH_SIM_PHY_CHANNEL9_MESH,
                                       &gateway_rx_window);
            REQUIRE(ret == MESH_SIM_OK,
                    "ACK gateway RX round=%u ret=%d", round, ret);

            for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
                struct mesh_outbound response;
                uint32_t random_value;
                uint32_t delay_ms = 0u;
                uint16_t seq;

                if (!table_decoded_this_round[i]) {
                    continue;
                }
                random_value = mesh_sim_random(world);
                ret = discovery_assignment_response_delay_ms(
                    anchors[i].assignment_slot,
                    ANCHOR_COUNT,
                    anchors[i].hop_count,
                    DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
                    0u,
                    random_value,
                    &delay_ms);
                REQUIRE(ret == PROTO_OK,
                        "ACK delay round=%u slot=%u ret=%d",
                        round, anchors[i].assignment_slot, ret);
                seq = (uint16_t)(2000u + (uint16_t)round * ANCHOR_COUNT + i);
                ret = build_discovery_response(&anchors[i],
                                               DISCOVERY_ASSIGNMENT_PHASE_ACK,
                                               TABLE_SESSION,
                                               seq,
                                               &response);
                REQUIRE(ret == PROTO_OK,
                        "build ACK round=%u slot=%u ret=%d",
                        round, anchors[i].assignment_slot, ret);
                ret = schedule_application_packet(
                    world,
                    anchors[i].node_index,
                    ack_start_us + (uint64_t)delay_ms * 1000u,
                    UWB_CHANNEL_MESH_PAYLOAD,
                    MESH_SIM_PHY_CHANNEL9_MESH,
                    &response.packet,
                    response.payload,
                    response.payload_len,
                    &ack_transmissions[i],
                    NULL);
                REQUIRE(ret == MESH_SIM_OK,
                        "schedule ACK round=%u slot=%u delay=%" PRIu32
                        " ret=%d",
                        round, anchors[i].assignment_slot, delay_ms, ret);
                REQUIRE(world->transmissions[ack_transmissions[i]].end_rctu +
                            world->propagation_rctu[anchors[i].node_index][gateway_index] <=
                        world->rx_windows[gateway_rx_window].end_rctu,
                        "ACK outside window round=%u slot=%u",
                        round, anchors[i].assignment_slot);
            }

            ret = mesh_sim_run_until(world, ack_end_us);
            REQUIRE(ret == MESH_SIM_OK,
                    "run ACK round=%u ret=%d last=%d",
                    round, ret, world->last_error);

            for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
                const struct mesh_sim_reception *reception;
                struct proto_packet packet;
                const uint8_t *payload = NULL;
                size_t payload_len = 0u;
                size_t matches = 0u;

                if (ack_transmissions[i] == UINT16_MAX) {
                    continue;
                }
                reception = find_reception(world,
                                           first_ack_reception,
                                           anchors[i].id,
                                           GATEWAY_ID,
                                           &matches);
                REQUIRE(reception != NULL && matches == 1u,
                        "ACK reception round=%u slot=%u matches=%zu",
                        round, anchors[i].assignment_slot, matches);
                if (reception->outcome != MESH_SIM_RX_DECODED) {
                    REQUIRE(reception->outcome == MESH_SIM_RX_COLLISION,
                            "full-window ACK round=%u slot=%u outcome=%u",
                            round, anchors[i].assignment_slot,
                            reception->outcome);
                    summary->ack_collisions++;
                    continue;
                }
                REQUIRE(world->rx_windows[gateway_rx_window].start_rctu <=
                            reception->start_rctu &&
                        world->rx_windows[gateway_rx_window].end_rctu >=
                            reception->end_rctu,
                        "decoded ACK not contained round=%u slot=%u",
                        round, anchors[i].assignment_slot);
                ret = decode_transmission(world,
                                          ack_transmissions[i],
                                          &packet,
                                          &payload,
                                          &payload_len);
                REQUIRE(ret == PROTO_OK,
                        "decode ACK round=%u slot=%u ret=%d",
                        round, anchors[i].assignment_slot, ret);
                ret = validate_discovery_response(
                    &packet,
                    payload,
                    payload_len,
                    &anchors[i],
                    DISCOVERY_ASSIGNMENT_PHASE_ACK,
                    TABLE_SESSION);
                if (ret != 0) {
                    return ret;
                }
                REQUIRE(anchors[i].table_received,
                        "ACK decoded before table slot=%u",
                        anchors[i].assignment_slot);
                anchors[i].acked = true;
            }
        }

        summary->table_rounds = (uint8_t)(round + 1u);
        if (round == 0u) {
            first_round_acked = decoded_state_count(
                anchors,
                DISCOVERY_ASSIGNMENT_PHASE_ACK);
        }
        if (decoded_state_count(anchors, DISCOVERY_ASSIGNMENT_PHASE_ACK) ==
            ANCHOR_COUNT) {
            break;
        }
    }

    REQUIRE(summary->initial_table_misses == 0u,
            "continuous-RX anchors missed single table origin count=%zu",
            summary->initial_table_misses);
    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        REQUIRE(anchors[i].table_received && anchors[i].acked,
                "table/ACK incomplete slot=%u table=%u ack=%u first_round=%u",
                anchors[i].assignment_slot,
                anchors[i].table_received ? 1u : 0u,
                anchors[i].acked ? 1u : 0u,
                anchors[i].table_first_round);
        REQUIRE(anchors[i].table_first_round == 0u,
                "table slot=%u first_round=%u expected=0",
                anchors[i].assignment_slot,
                anchors[i].table_first_round);
    }
    REQUIRE(first_round_acked == ANCHOR_COUNT,
            "single TABLE origin did not collect every ACK first_round=%zu",
            first_round_acked);
    REQUIRE(summary->table_rounds == TABLE_MAX_ROUNDS &&
            summary->ack_collisions == 0u,
            "single-origin table evidence rounds=%u ACK collisions=%zu",
            summary->table_rounds, summary->ack_collisions);
    REQUIRE(decoded_state_count(anchors, DISCOVERY_ASSIGNMENT_PHASE_TABLE) ==
                ANCHOR_COUNT &&
            decoded_state_count(anchors, DISCOVERY_ASSIGNMENT_PHASE_ACK) ==
                ANCHOR_COUNT,
            "decoded table/ACK state incomplete");
    return 0;
}

static int test_discovery_assignment_radio_scenario(struct scenario_summary *summary)
{
    static struct mesh_sim_world world;
    struct discovery_assignment_claim claims[ANCHOR_COUNT];
    struct discovery_assignment_entry entries[ANCHOR_COUNT];
    struct mesh_outbound table_command;
    struct anchor_state anchors[ANCHOR_COUNT] = {0};
    size_t claim_collision_pair[2] = { SIZE_MAX, SIZE_MAX };
    size_t claim_count = 0u;
    size_t table_frame_len = 0u;
    uint8_t gateway_index = UINT8_MAX;
    int ret;

    memset(summary, 0, sizeof(*summary));
    ret = test_identical_anchor_image_identity_and_ordering();
    if (ret != 0) {
        return ret;
    }
    ret = verify_response_order_is_hop_then_slot();
    if (ret != 0) {
        return ret;
    }
    ret = initialize_world(&world,
                           anchors,
                           &gateway_index,
                           claim_collision_pair);
    if (ret != 0) {
        return ret;
    }
    ret = collect_claims(&world,
                         anchors,
                         gateway_index,
                         claim_collision_pair,
                         claims,
                         &claim_count,
                         summary);
    if (ret != 0) {
        return ret;
    }
    ret = build_assignment_table(anchors,
                                 claims,
                                 claim_count,
                                 entries,
                                 &table_command,
                                 &table_frame_len);
    if (ret != 0) {
        return ret;
    }
    return publish_table_and_collect_acks(&world,
                                          anchors,
                                          gateway_index,
                                          &table_command,
                                          table_frame_len,
                                          summary);
}

static int test_survey_pairing_ceiling(void)
{
    static struct survey_gateway_context context;
    struct survey_reachability_entry
        entries[ANCHOR_COUNT][SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR];
    uint8_t degree[ANCHOR_COUNT] = {0};
    int ret;

    ret = survey_gateway_begin(&context, SURVEY_ID, 1u);
    REQUIRE(ret == PROTO_OK,
            "survey begin ret=%d", ret);
    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        size_t entry_index = 0u;

        for (size_t distance = 1u;
             distance <= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR / 2u;
             distance++) {
            size_t forward = (i + distance) % ANCHOR_COUNT;
            size_t reverse = (i + ANCHOR_COUNT - distance) % ANCHOR_COUNT;

            entries[i][entry_index++] = (struct survey_reachability_entry) {
                .peer_id = ANCHOR_ID_BASE + forward,
                .rssi_dbm = (int8_t)(-40 - (int)distance),
                .quality = (uint8_t)(100u - distance),
            };
            entries[i][entry_index++] = (struct survey_reachability_entry) {
                .peer_id = ANCHOR_ID_BASE + reverse,
                .rssi_dbm = (int8_t)(-40 - (int)distance),
                .quality = (uint8_t)(100u - distance),
            };
        }
        REQUIRE(entry_index == SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR,
                "survey report index=%zu peers=%zu", i, entry_index);
        ret = survey_gateway_note_reach_report(&context,
                                               SURVEY_ID,
                                               ANCHOR_ID_BASE + i,
                                               entries[i],
                                               entry_index);
        REQUIRE(ret == PROTO_OK,
                "survey report index=%zu ret=%d", i, ret);
    }

    ret = survey_gateway_plan_pairs(&context);
    REQUIRE(ret == PROTO_OK && context.report_count == ANCHOR_COUNT &&
            context.pair_count == SURVEY_GATEWAY_MAX_PAIRS,
            "survey plan ret=%d reports=%zu pairs=%zu expected=%u",
            ret, context.report_count, context.pair_count,
            SURVEY_GATEWAY_MAX_PAIRS);
    for (size_t i = 0u; i < context.pair_count; i++) {
        struct survey_pair pair;
        uint64_t initiator_offset;
        uint64_t responder_offset;
        size_t initiator;
        size_t responder;

        ret = survey_gateway_pair_at(&context, i, &pair);
        REQUIRE(ret == PROTO_OK,
                "survey pair reconstruction index=%zu ret=%d", i, ret);
        REQUIRE(pair.initiator_id >= ANCHOR_ID_BASE &&
                pair.responder_id >= ANCHOR_ID_BASE,
                "survey pair index=%zu ids=0x%016" PRIx64 "/0x%016" PRIx64,
                i, pair.initiator_id, pair.responder_id);
        initiator_offset = pair.initiator_id - ANCHOR_ID_BASE;
        responder_offset = pair.responder_id - ANCHOR_ID_BASE;
        REQUIRE(initiator_offset < ANCHOR_COUNT,
                "survey initiator index out of range pair=%zu offset=%" PRIu64,
                i, initiator_offset);
        REQUIRE(responder_offset < ANCHOR_COUNT,
                "survey responder index out of range pair=%zu offset=%" PRIu64,
                i, responder_offset);
        REQUIRE(pair.survey_id == SURVEY_ID,
                "survey pair ID mismatch index=%zu survey=%" PRIu32,
                i, pair.survey_id);
        REQUIRE(pair.sample_count == 1u,
                "survey pair sample count index=%zu count=%u",
                i, pair.sample_count);
        initiator = (size_t)initiator_offset;
        responder = (size_t)responder_offset;
        degree[initiator]++;
        degree[responder]++;
        for (size_t j = 0u; j < i; j++) {
            struct survey_pair previous_pair;

            REQUIRE(survey_gateway_pair_at(
                        &context, j, &previous_pair) == PROTO_OK,
                    "previous survey pair reconstruction index=%zu", j);
            REQUIRE(!(previous_pair.initiator_id == pair.initiator_id &&
                      previous_pair.responder_id == pair.responder_id),
                    "duplicate survey pair index=%zu previous=%zu", i, j);
        }
    }
    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        REQUIRE(degree[i] == SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR,
                "survey degree anchor=%zu degree=%u expected=%u",
                i, degree[i], SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR);
    }
    return 0;
}

int main(void)
{
    struct scenario_summary summary;
    int ret;

    ret = test_discovery_assignment_radio_scenario(&summary);
    if (ret != 0) {
        return ret;
    }
    ret = test_survey_pairing_ceiling();
    if (ret != 0) {
        return ret;
    }
    printf("mesh discovery scenarios: PASS seed=0x%08" PRIx32
           " claim_rounds=%u claim_collisions=%zu table_rounds=%u"
           " table_misses=%zu ack_collisions=%zu survey_pairs=%u\n",
           SCENARIO_SEED,
           summary.claim_rounds,
           summary.claim_collisions,
           summary.table_rounds,
           summary.initial_table_misses,
           summary.ack_collisions,
           SURVEY_GATEWAY_MAX_PAIRS);
    return 0;
}

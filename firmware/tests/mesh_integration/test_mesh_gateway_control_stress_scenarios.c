#include "app_gateway_command_ingress.h"
#include "app_gateway_command_lifecycle.h"
#include "app_gateway_command_observability.h"
#include "app_mesh_flood.h"
#include "app_mesh_rx_policy.h"
#include "gateway_command.h"
#include "mesh_relay.h"
#include "mesh_sim.h"
#include "protocol.h"
#include "serial_frame.h"
#include "survey.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GATEWAY_ID UINT64_C(0xa001000000000001)
#define ANCHOR_BASE UINT64_C(0xa002000000010000)
#define ROUTE_EPOCH 17u
#define ROUTE_ADV_SEQ UINT32_C(0x48494101)
#define PAIR_SURVEY_ID UINT32_C(0x07130071)
#define RADIO_GUARD_US UINT64_C(500)

static int failures;

#define CHECK(expression) do {                                                \
    if (!(expression)) {                                                      \
        fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #expression);       \
        failures++;                                                           \
        return;                                                               \
    }                                                                         \
} while (0)

static bool has_action(const struct mesh_relay_result *result,
                       enum mesh_relay_action action)
{
    return result != NULL && (result->actions & action) != 0u;
}

static size_t upstream_candidate_count(const struct mesh_relay *relay)
{
    size_t count = 0u;

    if (relay == NULL) {
        return 0u;
    }
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        if (relay->upstream.candidates[i].valid) {
            count++;
        }
    }
    return count;
}

static int schedule_phy_only_outbound(struct mesh_sim_world *world,
                                      uint8_t node_index,
                                      uint64_t start_us,
                                      const struct mesh_outbound *outbound,
                                      uint16_t *transmission_index)
{
    uint8_t frame[PACKET_EXT_MAX_LEN];
    size_t frame_len = 0u;
    int ret;

    if (world == NULL || outbound == NULL) {
        return MESH_SIM_ERR_ARG;
    }
    ret = proto_packet_encode(&outbound->packet, outbound->payload, frame,
                              sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_sim_schedule_raw_tx(world, node_index, start_us,
                                    UWB_CHANNEL_WAKE_CONTACT,
                                    MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                    frame, frame_len, false,
                                    transmission_index);
}

static void test_here_i_am_direct_scale_matrix(void)
{
    static const size_t counts[] = {2u, 6u, 16u, 20u, 32u, 50u};

    for (size_t c = 0u; c < sizeof(counts) / sizeof(counts[0]); c++) {
        struct mesh_relay gateway;
        struct mesh_outbound adv;

        mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                        GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH);
        CHECK(mesh_relay_build_gateway_route_adv(&gateway, ROUTE_ADV_SEQ,
                                                  1000u, &adv) == PROTO_OK);
        CHECK(adv.packet.msg_type == MSG_GATEWAY_ROUTE_ADV);
        CHECK(adv.packet.ttl == FLOOD_EPOCH_GLOBAL_TTL);
        for (size_t i = 0u; i < counts[c]; i++) {
            struct mesh_relay anchor;
            struct mesh_relay_result result;
            const struct route_candidate *selected;

            mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR,
                            ANCHOR_BASE + i, GATEWAY_ID, ROUTE_EPOCH);
            CHECK(mesh_relay_handle_rx_with_random(&anchor, &adv.packet,
                      adv.payload, adv.payload_len, GATEWAY_ID, 90u,
                      1010u, (uint32_t)i, &result) == PROTO_OK);
            CHECK(result.status == PROTO_OK);
            CHECK(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
            selected = route_selected(&anchor.upstream);
            CHECK(selected != NULL);
            CHECK(selected->next_hop_id == GATEWAY_ID);
            CHECK(selected->route_epoch == ROUTE_EPOCH);

            CHECK(mesh_relay_handle_rx_with_random(&anchor, &adv.packet,
                      adv.payload, adv.payload_len, GATEWAY_ID, 90u,
                      1020u, (uint32_t)i, &result) == PROTO_OK);
            CHECK(result.status == PROTO_ERR_STALE);
            CHECK(has_action(&result, MESH_RELAY_ACTION_DROP));
            CHECK(!has_action(&result,
                              MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
        }
    }
}

static void test_here_i_am_20_direct_50_mixed_hop_radio_scale(void)
{
    enum {
        DIRECT_ANCHORS = 20,
        SECOND_HOP_ANCHORS = 20,
        THIRD_HOP_ANCHORS = 10,
        TOTAL_ANCHORS = DIRECT_ANCHORS + SECOND_HOP_ANCHORS +
                        THIRD_HOP_ANCHORS,
    };
    static struct mesh_sim_world world;
    static struct mesh_outbound forwards[TOTAL_ANCHORS];
    bool decoded[TOTAL_ANCHORS] = {0};
    uint8_t node_indices[TOTAL_ANCHORS];
    uint8_t gateway_index;
    struct mesh_outbound gateway_adv;
    uint64_t gateway_tx_start_us = UINT64_C(10000);
    uint64_t next_tx_start_us;
    uint32_t airtime_us;
    uint32_t tx_stride_us;
    uint16_t tx_index;

    _Static_assert(TOTAL_ANCHORS + 1u <= MESH_SIM_MAX_ROLES,
                   "Here-I-Am scale exceeds simulator role capacity");

    mesh_sim_init(&world, UINT32_C(0x48494152));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
              GATEWAY_ID, ROUTE_EPOCH, &gateway_index) == MESH_SIM_OK);
    for (size_t i = 0u; i < TOTAL_ANCHORS; i++) {
        CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                  ANCHOR_BASE + i, GATEWAY_ID, ROUTE_EPOCH,
                  &node_indices[i]) == MESH_SIM_OK);
    }
    CHECK(mesh_relay_build_gateway_route_adv(
              &world.roles[gateway_index].relay, ROUTE_ADV_SEQ,
              1000u, &gateway_adv) == PROTO_OK);
    airtime_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
        PACKET_HEADER_LEN + gateway_adv.payload_len + PACKET_CRC_LEN);
    CHECK(airtime_us > 0u);
    tx_stride_us = airtime_us + (uint32_t)(2u * RADIO_GUARD_US) + 1000u;

    for (size_t i = 0u; i < TOTAL_ANCHORS; i++) {
        size_t parent_anchor;
        uint8_t parent_index;
        uint64_t parent_id;
        const struct mesh_outbound *incoming;
        struct mesh_relay_result result;
        const struct route_candidate *selected;
        uint8_t expected_hops;

        if (i < DIRECT_ANCHORS) {
            parent_index = gateway_index;
            parent_id = GATEWAY_ID;
            incoming = &gateway_adv;
            expected_hops = 1u;
        } else if (i < DIRECT_ANCHORS + SECOND_HOP_ANCHORS) {
            parent_anchor = i - DIRECT_ANCHORS;
            parent_index = node_indices[parent_anchor];
            parent_id = ANCHOR_BASE + parent_anchor;
            incoming = &forwards[parent_anchor];
            expected_hops = 2u;
        } else {
            parent_anchor = DIRECT_ANCHORS +
                            (i - DIRECT_ANCHORS - SECOND_HOP_ANCHORS);
            parent_index = node_indices[parent_anchor];
            parent_id = ANCHOR_BASE + parent_anchor;
            incoming = &forwards[parent_anchor];
            expected_hops = 3u;
        }

        CHECK(mesh_sim_set_link(&world, parent_index, node_indices[i],
                  90u, 0u) == MESH_SIM_OK);
        CHECK(mesh_relay_handle_rx_with_random(
                  &world.roles[node_indices[i]].relay,
                  &incoming->packet, incoming->payload, incoming->payload_len,
                  parent_id, 90u, 1010u + (uint32_t)i,
                  UINT32_C(0x9e3779b9) * (uint32_t)(i + 1u),
                  &result) == PROTO_OK);
        CHECK(result.status == PROTO_OK);
        CHECK(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
        selected = route_selected(&world.roles[node_indices[i]].relay.upstream);
        CHECK(selected != NULL);
        CHECK(selected->next_hop_id == parent_id);
        CHECK(selected->route_epoch == ROUTE_EPOCH);
        CHECK(selected->hop_count + 1u == expected_hops);
        CHECK(result.gateway_route_adv.packet.ttl ==
              FLOOD_EPOCH_GLOBAL_TTL - expected_hops);
        for (uint8_t timing = 0u; timing < MESH_RELAY_EVENT_TIMINGS; timing++) {
            CHECK(!world.roles[node_indices[i]].relay.event_timings[timing].valid);
        }
        forwards[i] = result.gateway_route_adv;
    }

    for (size_t i = 0u; i < DIRECT_ANCHORS; i++) {
        uint16_t rx_index;

        CHECK(mesh_sim_schedule_rx(&world, node_indices[i],
                  gateway_tx_start_us - RADIO_GUARD_US,
                  gateway_tx_start_us + airtime_us + RADIO_GUARD_US,
                  UWB_CHANNEL_WAKE_CONTACT,
                  MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                  &rx_index) == MESH_SIM_OK);
    }
    CHECK(schedule_phy_only_outbound(&world, gateway_index,
              gateway_tx_start_us, &gateway_adv, &tx_index) == MESH_SIM_OK);

    next_tx_start_us = gateway_tx_start_us + tx_stride_us;
    for (size_t i = DIRECT_ANCHORS;
         i < DIRECT_ANCHORS + SECOND_HOP_ANCHORS; i++) {
        size_t parent_anchor = i - DIRECT_ANCHORS;
        uint16_t rx_index;

        CHECK(mesh_sim_schedule_rx(&world, node_indices[i],
                  next_tx_start_us - RADIO_GUARD_US,
                  next_tx_start_us + airtime_us + RADIO_GUARD_US,
                  UWB_CHANNEL_WAKE_CONTACT,
                  MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                  &rx_index) == MESH_SIM_OK);
        CHECK(schedule_phy_only_outbound(&world, node_indices[parent_anchor],
                  next_tx_start_us, &forwards[parent_anchor],
                  &tx_index) == MESH_SIM_OK);
        next_tx_start_us += tx_stride_us;
    }
    for (size_t i = DIRECT_ANCHORS + SECOND_HOP_ANCHORS;
         i < TOTAL_ANCHORS; i++) {
        size_t parent_anchor = DIRECT_ANCHORS +
                               (i - DIRECT_ANCHORS - SECOND_HOP_ANCHORS);
        uint16_t rx_index;

        CHECK(mesh_sim_schedule_rx(&world, node_indices[i],
                  next_tx_start_us - RADIO_GUARD_US,
                  next_tx_start_us + airtime_us + RADIO_GUARD_US,
                  UWB_CHANNEL_WAKE_CONTACT,
                  MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                  &rx_index) == MESH_SIM_OK);
        CHECK(schedule_phy_only_outbound(&world, node_indices[parent_anchor],
                  next_tx_start_us, &forwards[parent_anchor],
                  &tx_index) == MESH_SIM_OK);
        next_tx_start_us += tx_stride_us;
    }

    CHECK(mesh_sim_run(&world) == MESH_SIM_OK);
    CHECK(world.transmission_count == 1u + SECOND_HOP_ANCHORS +
                                      THIRD_HOP_ANCHORS);
    CHECK(world.reception_count == TOTAL_ANCHORS);
    for (size_t i = 0u; i < world.reception_count; i++) {
        const struct mesh_sim_reception *rx = &world.receptions[i];
        size_t receiver = (size_t)(rx->receiver_id - ANCHOR_BASE);
        uint64_t expected_source;

        CHECK(world.receptions[i].outcome == MESH_SIM_RX_DECODED);
        CHECK(receiver < TOTAL_ANCHORS);
        CHECK(!decoded[receiver]);
        decoded[receiver] = true;
        if (receiver < DIRECT_ANCHORS) {
            expected_source = GATEWAY_ID;
        } else if (receiver < DIRECT_ANCHORS + SECOND_HOP_ANCHORS) {
            expected_source = ANCHOR_BASE + receiver - DIRECT_ANCHORS;
        } else {
            expected_source = ANCHOR_BASE + DIRECT_ANCHORS +
                              receiver - DIRECT_ANCHORS - SECOND_HOP_ANCHORS;
        }
        CHECK(rx->source_id == expected_source);
    }
}

static void test_here_i_am_ttl_and_epoch_fail_closed(void)
{
    struct mesh_relay gateway;
    struct mesh_relay chain[MESH_NETWORK_MAX_HOPS];
    struct mesh_outbound current;
    uint64_t previous_id = GATEWAY_ID;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH);
    CHECK(mesh_relay_build_gateway_route_adv(&gateway, ROUTE_ADV_SEQ,
                                              1000u, &current) == PROTO_OK);
    for (size_t hop = 0u; hop < MESH_NETWORK_MAX_HOPS; hop++) {
        struct mesh_relay_result result;
        const struct route_candidate *selected;
        uint64_t id = ANCHOR_BASE + hop;

        mesh_relay_init(&chain[hop], MESH_RELAY_ROLE_ANCHOR,
                        id, GATEWAY_ID, ROUTE_EPOCH);
        CHECK(mesh_relay_handle_rx_with_random(&chain[hop], &current.packet,
                  current.payload, current.payload_len, previous_id, 85u,
                  1100u + (uint32_t)hop, (uint32_t)hop,
                  &result) == PROTO_OK);
        CHECK(result.status == PROTO_OK);
        selected = route_selected(&chain[hop].upstream);
        CHECK(selected != NULL && selected->next_hop_id == previous_id);
        if (hop + 1u < MESH_NETWORK_MAX_HOPS) {
            CHECK(has_action(&result,
                              MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
            current = result.gateway_route_adv;
        } else {
            CHECK(!has_action(&result,
                               MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
        }
        previous_id = id;
    }

    {
        struct mesh_relay newer;
        struct mesh_relay_result result;

        mesh_relay_init(&newer, MESH_RELAY_ROLE_ANCHOR,
                        ANCHOR_BASE + 100u, GATEWAY_ID, ROUTE_EPOCH + 1u);
        CHECK(mesh_relay_build_gateway_route_adv(&gateway, ROUTE_ADV_SEQ + 1u,
                                                  2000u, &current) == PROTO_OK);
        CHECK(mesh_relay_handle_rx_with_random(&newer, &current.packet,
                  current.payload, current.payload_len, GATEWAY_ID, 90u,
                  2010u, 0u, &result) == PROTO_OK);
        CHECK(result.status == PROTO_ERR_STALE);
        CHECK(has_action(&result, MESH_RELAY_ACTION_DROP));
        CHECK(route_selected(&newer.upstream) == NULL);

        current.packet.src_id = ANCHOR_BASE + 999u;
        CHECK(mesh_relay_handle_rx_with_random(&newer, &current.packet,
                  current.payload, current.payload_len, GATEWAY_ID, 90u,
                  2020u, 1u, &result) == PROTO_OK);
        CHECK(result.status == PROTO_ERR_MALFORMED);
        CHECK(has_action(&result, MESH_RELAY_ACTION_DROP));
    }
}

struct flood_ctx {
    uint32_t now_ms;
    uint8_t sends;
    bool click_active;
    bool busy;
    struct mesh_relay *receiver;
    struct gateway_command_pending *pending;
    struct mesh_outbound forward;
    enum command_id command_id;
    uint64_t previous_hop_id;
    uint8_t awake_from_send;
    uint8_t deliveries;
    uint8_t forwards;
    uint8_t duplicate_drops;
    uint8_t command_results;
    bool forward_valid;
};

static uint32_t flood_now(void *ctx)
{
    return ((struct flood_ctx *)ctx)->now_ms;
}

static void flood_sleep(uint32_t due_ms, void *ctx)
{
    ((struct flood_ctx *)ctx)->now_ms = due_ms;
}

static bool flood_defer(void *ctx)
{
    return ((struct flood_ctx *)ctx)->click_active;
}

static bool flood_quiet(uint32_t sniff_ms, void *ctx)
{
    if (sniff_ms != C5_POLITE_SNIFF_MS) {
        return false;
    }
    return !((struct flood_ctx *)ctx)->busy;
}

static uint32_t flood_random(void *ctx)
{
    return ((struct flood_ctx *)ctx)->sends + 7u;
}

static int flood_send(const struct mesh_outbound *out, void *ctx)
{
    struct flood_ctx *fixture = ctx;
    struct mesh_relay_result result;
    struct proto_packet command_result;
    uint8_t result_payload[16];
    size_t result_payload_len = 0u;
    int ret;

    if (out->packet.msg_type != MSG_GATEWAY_ROUTE_ADV &&
        out->packet.msg_type != MSG_COMMAND &&
        out->packet.msg_type != MSG_SURVEY_PAIR_PREPARE) {
        return -EINVAL;
    }
    fixture->sends++;
    if (fixture->receiver == NULL ||
        fixture->sends < fixture->awake_from_send) {
        return 0;
    }

    ret = mesh_relay_handle_rx_with_random(fixture->receiver,
                                            &out->packet,
                                            out->payload,
                                            out->payload_len,
                                            fixture->previous_hop_id,
                                            90u,
                                            fixture->now_ms,
                                            fixture->sends,
                                            &result);
    if (ret != PROTO_OK) {
        return -EIO;
    }
    if (has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL)) {
        fixture->deliveries++;
        ret = mesh_append_command_result(result_payload,
                                         sizeof(result_payload),
                                         &result_payload_len,
                                         fixture->command_id,
                                         COMMAND_OK,
                                         0u);
        if (ret != PROTO_OK ||
            mesh_init_command_result(&command_result,
                                     fixture->receiver->local_id,
                                     GATEWAY_ID,
                                     out->packet.session_id,
                                     out->packet.seq,
                                     (uint8_t)result_payload_len,
                                     false) != PROTO_OK ||
            !gateway_command_pending_complete_result(fixture->pending,
                                                     &command_result)) {
            return -EIO;
        }
        fixture->command_results++;
    }
    if (has_action(&result, MESH_RELAY_ACTION_FORWARD)) {
        fixture->forwards++;
        if (!fixture->forward_valid) {
            fixture->forward = result.forward;
            fixture->forward_valid = true;
        }
    }
    if (has_action(&result, MESH_RELAY_ACTION_DROP) &&
        result.status == PROTO_ERR_STALE) {
        fixture->duplicate_drops++;
    }
    return 0;
}

static int build_survey_pair_control(struct mesh_outbound *out,
                                     enum command_id command_id,
                                     uint64_t target_id,
                                     uint64_t next_hop_id,
                                     uint16_t seq)
{
    const struct survey_pair pair = {
        .survey_id = PAIR_SURVEY_ID,
        .initiator_id = target_id,
        .responder_id = target_id + UINT64_C(0x1000),
        .sample_count = 3u,
    };
    size_t payload_len = 0u;
    int ret;

    memset(out, 0, sizeof(*out));
    if (command_id == CMD_SURVEY_START_PAIR) {
        ret = mesh_append_command_id(out->payload,
                                     sizeof(out->payload),
                                     &payload_len,
                                     command_id);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    ret = survey_append_pair_tlvs(out->payload,
                                  sizeof(out->payload),
                                  &payload_len,
                                  &pair);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (command_id == CMD_SURVEY_PREPARE_PAIR) {
        ret = survey_init_pair_prepare_packet(&out->packet,
                                              &pair,
                                              GATEWAY_ID,
                                              seq,
                                              (uint8_t)payload_len);
    } else if (command_id == CMD_SURVEY_START_PAIR) {
        out->packet.msg_type = MSG_COMMAND;
        out->packet.src_id = GATEWAY_ID;
        out->packet.dst_id = target_id;
        out->packet.session_id = pair.survey_id;
        out->packet.seq = seq;
        out->packet.ttl = MESH_DEFAULT_TTL;
        out->packet.payload_len = (uint8_t)payload_len;
        ret = PROTO_OK;
    } else {
        return PROTO_ERR_ARG;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    out->packet.dst_id = target_id;
    out->payload_len = (uint16_t)payload_len;
    out->next_hop_id = next_hop_id;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return PROTO_OK;
}

static void test_gateway_control_reverse_route_contract(void)
{
    const uint64_t target_id = ANCHOR_BASE + UINT64_C(0x500);
    const uint64_t relay_id = ANCHOR_BASE + UINT64_C(0x501);
    const uint64_t alternate_id = ANCHOR_BASE + UINT64_C(0x502);
    const struct survey_discovery_config discovery = {
        .survey_id = PAIR_SURVEY_ID,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 6u,
    };
    struct mesh_relay anchor;
    struct mesh_relay gateway;
    struct mesh_outbound prepare;
    struct mesh_outbound command;
    struct proto_packet discovery_start;
    struct route_candidate better;
    struct route_table unchanged;
    const struct route_candidate *selected;

    CHECK(build_survey_pair_control(&prepare, CMD_SURVEY_PREPARE_PAIR,
                                     target_id, target_id, 81u) == PROTO_OK);
    CHECK(build_survey_pair_control(&command, CMD_SURVEY_START_PAIR,
                                     target_id, target_id, 82u) == PROTO_OK);
    CHECK(survey_init_discovery_start_packet(&discovery_start, GATEWAY_ID,
                                              &discovery, 83u, 0u) == PROTO_OK);

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR,
                    target_id, GATEWAY_ID, ROUTE_EPOCH);
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, GATEWAY_ID, 87u,
              SURVEY_DEFAULT_TTL, 1000u) == PROTO_OK);
    selected = route_selected(&anchor.upstream);
    CHECK(selected != NULL);
    CHECK(selected->next_hop_id == GATEWAY_ID);
    CHECK(selected->gateway_id == GATEWAY_ID);
    CHECK(selected->hop_count == 0u);
    CHECK(selected->link_quality == 87u);
    CHECK(selected->route_epoch == ROUTE_EPOCH);
    CHECK(selected->last_seen_ms == 1000u);
    CHECK(upstream_candidate_count(&anchor) == 1u);

    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, GATEWAY_ID, 96u,
              SURVEY_DEFAULT_TTL, 1010u) == PROTO_OK);
    selected = route_selected(&anchor.upstream);
    CHECK(selected != NULL && selected->next_hop_id == GATEWAY_ID);
    CHECK(selected->link_quality == 96u);
    CHECK(selected->last_seen_ms == 1010u);
    CHECK(upstream_candidate_count(&anchor) == 1u);

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR,
                    target_id, GATEWAY_ID, ROUTE_EPOCH);
    command.packet.ttl = MESH_DEFAULT_TTL - 1u;
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &command.packet, relay_id, 78u,
              MESH_DEFAULT_TTL, 1020u) == PROTO_OK);
    selected = route_selected(&anchor.upstream);
    CHECK(selected != NULL && selected->next_hop_id == relay_id);
    CHECK(selected->hop_count == 1u);
    CHECK(selected->link_quality == 78u);

    command.packet.dst_id = MESH_BROADCAST_ID;
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &command.packet, relay_id, 79u,
              MESH_DEFAULT_TTL, 1030u) == PROTO_OK);
    selected = route_selected(&anchor.upstream);
    CHECK(selected != NULL && selected->next_hop_id == relay_id);
    CHECK(selected->hop_count == 1u && selected->link_quality == 79u);

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR,
                    target_id, GATEWAY_ID, ROUTE_EPOCH);
    discovery_start.ttl = SURVEY_DEFAULT_TTL - 2u;
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &discovery_start, relay_id, 73u,
              SURVEY_DEFAULT_TTL, 1040u) == PROTO_OK);
    selected = route_selected(&anchor.upstream);
    CHECK(selected != NULL && selected->next_hop_id == relay_id);
    CHECK(selected->hop_count == 2u);

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR,
                    target_id, GATEWAY_ID, ROUTE_EPOCH);
    better = (struct route_candidate) {
        .next_hop_id = alternate_id,
        .gateway_id = GATEWAY_ID,
        .route_epoch = ROUTE_EPOCH,
        .last_seen_ms = 900u,
        .last_success_ms = 850u,
        .hop_count = 0u,
        .link_quality = 60u,
        .channel9_timing_valid = true,
        .valid = true,
    };
    CHECK(route_upsert_candidate(&anchor.upstream, &better) == PROTO_OK);
    prepare.packet.ttl = SURVEY_DEFAULT_TTL - 2u;
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, relay_id, 100u,
              SURVEY_DEFAULT_TTL, 1050u) == PROTO_OK);
    selected = route_selected(&anchor.upstream);
    CHECK(selected != NULL && selected->next_hop_id == alternate_id);
    CHECK(upstream_candidate_count(&anchor) == 2u);
    CHECK(anchor.upstream.candidates[0].channel9_timing_valid);

    unchanged = anchor.upstream;
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH);
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &gateway, &prepare.packet, relay_id, 90u,
              SURVEY_DEFAULT_TTL, 1060u) != PROTO_OK);
    CHECK(route_selected(&gateway.upstream) == NULL);

    prepare.packet.src_id = alternate_id;
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, relay_id, 90u,
              SURVEY_DEFAULT_TTL, 1061u) != PROTO_OK);
    CHECK(memcmp(&anchor.upstream, &unchanged, sizeof(unchanged)) == 0);
    prepare.packet.src_id = GATEWAY_ID;

    prepare.packet.dst_id = alternate_id;
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, relay_id, 90u,
              SURVEY_DEFAULT_TTL, 1062u) != PROTO_OK);
    CHECK(memcmp(&anchor.upstream, &unchanged, sizeof(unchanged)) == 0);
    prepare.packet.dst_id = target_id;

    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, MESH_BROADCAST_ID, 90u,
              SURVEY_DEFAULT_TTL, 1063u) != PROTO_OK);
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, target_id, 90u,
              SURVEY_DEFAULT_TTL, 1064u) != PROTO_OK);
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, relay_id, 101u,
              SURVEY_DEFAULT_TTL, 1065u) != PROTO_OK);
    CHECK(memcmp(&anchor.upstream, &unchanged, sizeof(unchanged)) == 0);

    prepare.packet.ttl = 0u;
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, relay_id, 90u,
              SURVEY_DEFAULT_TTL, 1066u) != PROTO_OK);
    prepare.packet.ttl = SURVEY_DEFAULT_TTL + 1u;
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, relay_id, 90u,
              SURVEY_DEFAULT_TTL, 1067u) != PROTO_OK);
    prepare.packet.ttl = SURVEY_DEFAULT_TTL;
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, relay_id, 90u,
              SURVEY_DEFAULT_TTL, 1068u) != PROTO_OK);
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, GATEWAY_ID, 90u,
              0u, 1069u) != PROTO_OK);
    CHECK(memcmp(&anchor.upstream, &unchanged, sizeof(unchanged)) == 0);

    prepare.packet.msg_type = MSG_COMMAND_RESULT;
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, GATEWAY_ID, 90u,
              SURVEY_DEFAULT_TTL, 1070u) != PROTO_OK);
    prepare.packet.msg_type = MSG_SURVEY_PAIR_PREPARE;
    prepare.packet.dst_id = MESH_BROADCAST_ID;
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, GATEWAY_ID, 90u,
              SURVEY_DEFAULT_TTL, 1071u) != PROTO_OK);
    CHECK(memcmp(&anchor.upstream, &unchanged, sizeof(unchanged)) == 0);

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR,
                    target_id, GATEWAY_ID, 0u);
    prepare.packet.dst_id = target_id;
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &anchor, &prepare.packet, GATEWAY_ID, 90u,
              SURVEY_DEFAULT_TTL, 1072u) != PROTO_OK);
    CHECK(route_selected(&anchor.upstream) == NULL);
}

static void test_gateway_control_reverse_route_fifty_anchor_reply_sweep(void)
{
    enum {
        DIRECT_ANCHORS = 20,
        TOTAL_ANCHORS = 50,
    };

    for (size_t i = 0u; i < TOTAL_ANCHORS; i++) {
        const uint64_t target_id = ANCHOR_BASE + UINT64_C(0x600) + i;
        const uint8_t gateway_hops = i < DIRECT_ANCHORS ? 0u :
            (uint8_t)(1u + ((i - DIRECT_ANCHORS) % 3u));
        const uint64_t previous_hop_id = gateway_hops == 0u ? GATEWAY_ID :
            ANCHOR_BASE + UINT64_C(0x1000) + i;
        struct mesh_relay anchor;
        struct mesh_outbound prepare;
        struct mesh_outbound result;
        struct proto_packet result_packet;
        const struct route_candidate *selected;
        uint8_t result_payload[16] = {0};
        size_t result_payload_len = 0u;

        mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR,
                        target_id, GATEWAY_ID, ROUTE_EPOCH);
        CHECK(build_survey_pair_control(&prepare, CMD_SURVEY_PREPARE_PAIR,
                                         target_id, previous_hop_id,
                                         (uint16_t)(200u + i)) == PROTO_OK);
        prepare.packet.ttl = (uint8_t)(SURVEY_DEFAULT_TTL - gateway_hops);
        CHECK(route_selected(&anchor.upstream) == NULL);
        CHECK(!anchor.route_discovery.active);
        CHECK(mesh_relay_note_gateway_control_reverse_route(
                  &anchor, &prepare.packet, previous_hop_id,
                  (uint8_t)(70u + (i % 31u)), SURVEY_DEFAULT_TTL,
                  2000u + (uint32_t)i) == PROTO_OK);
        selected = route_selected(&anchor.upstream);
        CHECK(selected != NULL);
        CHECK(selected->next_hop_id == previous_hop_id);
        CHECK(selected->hop_count == gateway_hops);
        CHECK(!anchor.route_discovery.active);

        CHECK(mesh_append_command_result(result_payload,
                                          sizeof(result_payload),
                                          &result_payload_len,
                                          CMD_SURVEY_PREPARE_PAIR,
                                          COMMAND_OK, 0u) == PROTO_OK);
        CHECK(mesh_init_command_result(&result_packet, target_id, GATEWAY_ID,
                                        PAIR_SURVEY_ID,
                                        (uint16_t)(200u + i),
                                        (uint8_t)result_payload_len,
                                        false) == PROTO_OK);
        CHECK(mesh_relay_start_tx(&anchor, &result_packet, result_payload,
                                   result_payload_len,
                                   2100u + (uint32_t)i,
                                   &result) == PROTO_OK);
        CHECK(result.packet.msg_type == MSG_COMMAND_RESULT);
        CHECK(result.next_hop_id == previous_hop_id);
        CHECK(!anchor.route_discovery.active);
    }
}

struct host_fixture {
    struct app_gateway_command_ingress_item item;
    struct app_gateway_command_lifecycle lifecycle;
    enum command_status terminal_status;
    uint8_t admission_count;
    uint8_t priority_count;
    uint8_t result_count;
};

static int host_admit(void *ctx, struct app_gateway_command_ingress_item *item)
{
    struct host_fixture *fixture = ctx;

    item->admission_id = 1u;
    fixture->item = *item;
    fixture->admission_count++;
    return app_gateway_command_lifecycle_admit(&fixture->lifecycle, item);
}

static int host_submit_priority(void *ctx)
{
    ((struct host_fixture *)ctx)->priority_count++;
    return 0;
}

static int host_cancel(void *ctx,
                       const struct app_gateway_command_identity *identity)
{
    (void)ctx;
    (void)identity;
    return 0;
}

static void host_result(void *ctx,
                        const struct proto_packet *command,
                        enum command_id command_id,
                        enum command_status status,
                        uint8_t reason)
{
    struct host_fixture *fixture = ctx;

    (void)command;
    (void)reason;
    if (command_id == CMD_FORCE_REDISCOVERY) {
        fixture->result_count++;
        fixture->terminal_status = status;
    }
}

static void test_here_i_am_production_helper_composition(void)
{
    struct host_fixture fixture = {0};
    struct app_gateway_command_ingress_item decoded;
    struct app_gateway_command_ingress_ops ops = {
        .gateway_role = true,
        .admit = host_admit,
        .submit_priority = host_submit_priority,
        .cancel_admitted = host_cancel,
        .emit_result = host_result,
        .ctx = &fixture,
    };
    struct gateway_command_observability_state observability;
    struct gateway_command_event terminal = {
        .kind = GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH,
        .stage = GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
        .status = COMMAND_OK,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        .command_id = CMD_FORCE_REDISCOVERY,
        .gateway_epoch = ROUTE_EPOCH,
        .correlation_id = 77u,
        .host_session_id = 77u,
        .host_seq = 12u,
    };
    struct gateway_command_event replay;
    struct mesh_relay gateway;
    struct mesh_outbound adv;
    enum app_gateway_command_lifecycle_dispatch dispatch;
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = UINT64_C(0x1111),
        .dst_id = GATEWAY_ID,
        .session_id = 77u,
        .seq = 12u,
    };
    uint8_t payload[8];
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t payload_len = 0u;
    size_t frame_len = 0u;
    bool handled = false;

    CHECK(app_gateway_command_lifecycle_init(&fixture.lifecycle, 1u) == 0);
    CHECK(mesh_append_command_id(payload, sizeof(payload), &payload_len,
                                 CMD_FORCE_REDISCOVERY) == PROTO_OK);
    command.payload_len = (uint16_t)payload_len;
    CHECK(serial_frame_encode_packet(&command, payload, frame, sizeof(frame),
                                     &frame_len) == PROTO_OK);
    CHECK(app_gateway_command_ingress_handle_frame(&ops, frame, frame_len,
                                                    &decoded, &handled) == 0);
    CHECK(handled && fixture.admission_count == 1u &&
          fixture.priority_count == 1u && fixture.result_count == 0u);
    CHECK(fixture.item.command_id == CMD_FORCE_REDISCOVERY);
    CHECK(app_gateway_command_lifecycle_begin_dispatch(
              &fixture.lifecycle, &fixture.item, &dispatch) == 0);
    CHECK(dispatch == APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_EXECUTE);

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH);
    CHECK(mesh_relay_build_gateway_route_adv(&gateway, ROUTE_ADV_SEQ,
                                              1000u, &adv) == PROTO_OK);
    CHECK(adv.packet.msg_type == MSG_GATEWAY_ROUTE_ADV &&
          adv.packet.src_id == GATEWAY_ID &&
          adv.packet.dst_id == MESH_BROADCAST_ID);
    CHECK(app_gateway_command_lifecycle_finish(&fixture.lifecycle,
                                                &fixture.item) == 0);
    host_result(&fixture, &command, CMD_FORCE_REDISCOVERY, COMMAND_OK, 0u);
    CHECK(fixture.result_count == 1u && fixture.terminal_status == COMMAND_OK);

    gateway_command_observability_init(&observability);
    CHECK(gateway_command_observability_prepare(&observability, &terminal,
                                                 true) == 0);
    gateway_command_observability_note_enqueue(&observability,
                                                terminal.event_seq,
                                                -ENOTCONN);
    CHECK(gateway_command_observability_pending_terminal(&observability,
                                                          &replay));
    CHECK(replay.kind == GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH);
    CHECK(replay.command_id == CMD_FORCE_REDISCOVERY);
    CHECK((replay.flags & GATEWAY_COMMAND_EVENT_FLAG_REPLAY) != 0u);
}

static void test_gateway_control_click_and_busy_deferral(void)
{
    struct mesh_outbound controls[2] = {
        {
            .packet = { .msg_type = MSG_GATEWAY_ROUTE_ADV,
                        .src_id = GATEWAY_ID,
                        .dst_id = MESH_BROADCAST_ID,
                        .ttl = FLOOD_EPOCH_GLOBAL_TTL },
            .next_hop_id = MESH_BROADCAST_ID,
            .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        },
        {
            .packet = { .msg_type = MSG_COMMAND,
                        .src_id = GATEWAY_ID,
                        .dst_id = MESH_BROADCAST_ID,
                        .ttl = FLOOD_EPOCH_GLOBAL_TTL },
            .next_hop_id = MESH_BROADCAST_ID,
            .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        },
    };

    for (size_t i = 0u; i < 2u; i++) {
        struct flood_ctx ctx = { .now_ms = 100u, .click_active = true };
        const struct app_mesh_flood_ops ops = {
            .now_ms = flood_now, .sleep_until_ms = flood_sleep,
            .defer_active = flood_defer, .c5_quiet = flood_quiet,
            .random_u32 = flood_random, .send = flood_send, .ctx = &ctx,
        };
        struct app_mesh_flood_result result;

        CHECK(app_mesh_flood_send_bounded(&controls[i], &ops, &result) ==
              -EAGAIN);
        CHECK(result.sent_count == 0u && result.deferred_count == 1u);
        ctx.click_active = false;
        ctx.busy = true;
        CHECK(app_mesh_flood_send_bounded(&controls[i], &ops, &result) ==
              -EAGAIN);
        CHECK(result.sent_count == 0u &&
              result.busy_skip_count == 1u);
        ctx.busy = false;
        CHECK(app_mesh_flood_send_bounded(&controls[i], &ops, &result) == 0);
        CHECK(result.sent_count == app_mesh_flood_repeat_limit());
    }
}

static void test_gateway_control_rx_handoff_stress_sweep(void)
{
    struct app_mesh_rx_handoff_state handoff;
    uint32_t real_attempts = 0u;

    app_mesh_rx_handoff_reset(&handoff);
    CHECK(app_mesh_rx_handoff_try_begin_scan(&handoff));

    for (uint32_t cycle = 0u; cycle < 512u; cycle++) {
        const uint32_t abort_delay_ms = cycle % 25u;
        bool abort_scan = false;
        bool attempted = false;

        if ((cycle % 11u) == 0u) {
            app_mesh_rx_handoff_end_scan(&handoff);
        }
        CHECK(app_mesh_rx_handoff_begin_control(&handoff, &abort_scan));
        CHECK(abort_scan == ((cycle % 11u) != 0u));
        CHECK(!app_mesh_rx_handoff_scan_rearm_allowed(&handoff));
        CHECK(!app_mesh_rx_handoff_try_begin_scan(&handoff));

        for (uint32_t elapsed_ms = 0u; elapsed_ms <= 25u; elapsed_ms++) {
            if (abort_scan && elapsed_ms == abort_delay_ms) {
                app_mesh_rx_handoff_end_scan(&handoff);
            }
            if (app_mesh_rx_handoff_control_ready(&handoff)) {
                real_attempts++;
                attempted = true;
                break;
            }
            CHECK(!app_mesh_rx_handoff_try_begin_scan(&handoff));
        }

        CHECK(attempted);
        app_mesh_rx_handoff_end_control(&handoff);
        CHECK(app_mesh_rx_handoff_scan_rearm_allowed(&handoff));
        CHECK(app_mesh_rx_handoff_try_begin_scan(&handoff));
    }

    CHECK(real_attempts == 512u);
    app_mesh_rx_handoff_end_scan(&handoff);
}

static void run_survey_pair_control_case(enum command_id command_id,
                                         bool relayed)
{
    static struct mesh_sim_world world;
    const uint64_t relay_id = ANCHOR_BASE + 300u;
    const uint64_t target_id = ANCHOR_BASE + 301u;
    struct mesh_outbound control;
    struct survey_gateway_context survey_context;
    struct survey_gateway_reverse_hint reverse_hint = {0};
    struct gateway_command_pending pending = {0};
    struct flood_ctx first = {
        .now_ms = 1000u,
        .command_id = command_id,
        .awake_from_send = 2u,
    };
    struct flood_ctx second = {
        .now_ms = 2000u,
        .command_id = command_id,
        .awake_from_send = 2u,
    };
    struct app_mesh_flood_ops first_ops = {
        .now_ms = flood_now,
        .sleep_until_ms = flood_sleep,
        .defer_active = flood_defer,
        .c5_quiet = flood_quiet,
        .random_u32 = flood_random,
        .send = flood_send,
        .ctx = &first,
    };
    struct app_mesh_flood_ops second_ops = first_ops;
    struct app_mesh_flood_result flood_result;
    struct mesh_relay *gateway;
    struct mesh_relay *relay;
    struct mesh_relay *target;
    uint64_t selected_next_hop = 0u;
    uint8_t gateway_index;
    uint8_t relay_index;
    uint8_t target_index;
    uint16_t seq = command_id == CMD_SURVEY_PREPARE_PAIR ? 71u : 72u;

    CHECK(app_mesh_flood_repeat_limit() >= 3u);
    mesh_sim_init(&world, relayed ? UINT32_C(0x52c50002) :
                                    UINT32_C(0x52c50001));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
              GATEWAY_ID, ROUTE_EPOCH, &gateway_index) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, relay_id,
              GATEWAY_ID, ROUTE_EPOCH, &relay_index) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, target_id,
              GATEWAY_ID, ROUTE_EPOCH, &target_index) == MESH_SIM_OK);
    gateway = &world.roles[gateway_index].relay;
    relay = &world.roles[relay_index].relay;
    target = &world.roles[target_index].relay;

    CHECK(mesh_relay_find_downlink(gateway, target_id) == NULL);
    CHECK(survey_gateway_begin(&survey_context,
                               PAIR_SURVEY_ID,
                               3u) == PROTO_OK);
    reverse_hint.target_id = target_id;
    reverse_hint.next_hop_id = relayed ? relay_id : target_id;
    reverse_hint.quality = 90u;
    reverse_hint.valid = true;
    CHECK(survey_gateway_note_reach_report_with_reverse_hint(
              &survey_context,
              PAIR_SURVEY_ID,
              target_id,
              NULL,
              0u,
              &reverse_hint) == PROTO_OK);

    /* Simulate a reset or route loss: only the accepted survey hint survives. */
    memset(gateway->downlinks, 0, sizeof(gateway->downlinks));
    CHECK(mesh_relay_select_next_hop(gateway,
                                     target_id,
                                     &selected_next_hop) ==
          PROTO_ERR_NOT_FOUND);
    memset(&reverse_hint, 0, sizeof(reverse_hint));
    CHECK(survey_gateway_reverse_hint_for_target(&survey_context,
                                                  target_id,
                                                  &reverse_hint) == PROTO_OK);
    CHECK(mesh_relay_note_gateway_survey_reverse_route(
              gateway,
              reverse_hint.target_id,
              reverse_hint.next_hop_id,
              reverse_hint.quality,
              900u) == PROTO_OK);
    CHECK(mesh_relay_select_next_hop(gateway,
                                     target_id,
                                     &selected_next_hop) == PROTO_OK);
    CHECK(selected_next_hop == (relayed ? relay_id : target_id));
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        CHECK(!gateway->event_timings[i].valid);
    }
    if (relayed) {
        CHECK(mesh_sim_set_link(&world,
                                relay_index,
                                target_index,
                                90u,
                                0u) == MESH_SIM_OK);
        CHECK(mesh_sim_install_downlink(&world,
                                         relay_index,
                                         target_id,
                                         target_index,
                                         1u,
                                         ROUTE_EPOCH) == MESH_SIM_OK);
    }

    CHECK(build_survey_pair_control(&control,
                                    command_id,
                                    target_id,
                                    selected_next_hop,
                                    seq) == PROTO_OK);
    CHECK(!mesh_relay_packet_requires_channel9_payload_event(&control.packet));
    CHECK(gateway_command_pending_start(&pending,
                                         &control.packet,
                                         command_id,
                                         1000u,
                                         GATEWAY_COMMAND_RESULT_TIMEOUT_MS) ==
          PROTO_OK);

    first.receiver = relayed ? relay : target;
    first.pending = &pending;
    first.previous_hop_id = GATEWAY_ID;
    first.click_active = true;
    CHECK(app_mesh_flood_send_bounded(&control,
                                       &first_ops,
                                       &flood_result) == -EAGAIN);
    CHECK(flood_result.sent_count == 0u &&
          flood_result.deferred_count == 1u);
    first.click_active = false;
    first.busy = true;
    CHECK(app_mesh_flood_send_bounded(&control,
                                       &first_ops,
                                       &flood_result) == -EAGAIN);
    CHECK(flood_result.sent_count == 0u &&
          flood_result.busy_skip_count == 1u);
    first.busy = false;
    CHECK(app_mesh_flood_send_bounded(&control,
                                       &first_ops,
                                       &flood_result) == 0);
    CHECK(flood_result.sent_count == app_mesh_flood_repeat_limit());
    CHECK(first.sends == app_mesh_flood_repeat_limit());

    if (!relayed) {
        CHECK(first.deliveries == 1u);
        CHECK(first.command_results == 1u);
        CHECK(first.duplicate_drops ==
              app_mesh_flood_repeat_limit() - first.awake_from_send);
        CHECK(!pending.active);
        return;
    }

    CHECK(first.deliveries == 0u);
    CHECK(first.forwards ==
          app_mesh_flood_repeat_limit() - first.awake_from_send + 1u);
    CHECK(first.forward_valid);
    CHECK(first.forward.packet.ttl == control.packet.ttl - 1u);
    CHECK(first.forward.next_hop_id == target_id);
    CHECK(!mesh_relay_packet_requires_channel9_payload_event(
              &first.forward.packet));

    first.forward.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    second.pending = &pending;
    second.receiver = target;
    second.previous_hop_id = relay_id;
    second_ops.ctx = &second;
    CHECK(app_mesh_flood_send_bounded(&first.forward,
                                       &second_ops,
                                       &flood_result) == 0);
    CHECK(flood_result.sent_count == app_mesh_flood_repeat_limit());
    CHECK(second.deliveries == 1u);
    CHECK(second.command_results == 1u);
    CHECK(second.duplicate_drops ==
          app_mesh_flood_repeat_limit() - second.awake_from_send);
    CHECK(!pending.active);
}

static void test_survey_pair_control_bounded_flood_lane(void)
{
    static const enum command_id commands[] = {
        CMD_SURVEY_PREPARE_PAIR,
        CMD_SURVEY_START_PAIR,
    };

    for (size_t i = 0u; i < sizeof(commands) / sizeof(commands[0]); i++) {
        run_survey_pair_control_case(commands[i], false);
        run_survey_pair_control_case(commands[i], true);
    }
}

static void test_here_i_am_radio_collision_containment_and_retry(void)
{
    static struct mesh_sim_world world;
    const uint64_t forwarder_a_id = UINT64_C(0x0000000100000001);
    const uint64_t forwarder_b_id = UINT64_C(0x0000000200000002);
    const uint64_t child_id = ANCHOR_BASE + 200u;
    struct mesh_relay gateway;
    struct mesh_relay forwarder_a;
    struct mesh_relay forwarder_b;
    struct mesh_relay child;
    struct mesh_relay_result result_a;
    struct mesh_relay_result result_b;
    struct mesh_relay_result child_result;
    struct mesh_outbound adv;
    uint8_t gateway_index, a_index, b_index, child_index;
    uint16_t tx_a, tx_b, retry_tx;
    uint16_t rx_collision, rx_partial, rx_retry;
    uint64_t collision_start_us;
    uint64_t partial_start_us;
    uint64_t retry_start_us;
    uint32_t airtime_us;
    bool saw_collision = false;
    bool saw_partial = false;
    bool saw_retry_decode = false;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH);
    mesh_relay_init(&forwarder_a, MESH_RELAY_ROLE_ANCHOR,
                    forwarder_a_id, GATEWAY_ID, ROUTE_EPOCH);
    mesh_relay_init(&forwarder_b, MESH_RELAY_ROLE_ANCHOR,
                    forwarder_b_id, GATEWAY_ID, ROUTE_EPOCH);
    mesh_relay_init(&child, MESH_RELAY_ROLE_ANCHOR,
                    child_id, GATEWAY_ID, ROUTE_EPOCH);
    CHECK(mesh_relay_build_gateway_route_adv(&gateway, ROUTE_ADV_SEQ,
                                              1000u, &adv) == PROTO_OK);
    CHECK(mesh_relay_handle_rx_with_random(&forwarder_a, &adv.packet, adv.payload,
              adv.payload_len, GATEWAY_ID, 90u, 1010u, 0u,
              &result_a) == PROTO_OK);
    CHECK(mesh_relay_handle_rx_with_random(&forwarder_b, &adv.packet, adv.payload,
              adv.payload_len, GATEWAY_ID, 90u, 1010u, 0u,
              &result_b) == PROTO_OK);
    CHECK(result_a.gateway_route_adv.earliest_tx_ms ==
          result_b.gateway_route_adv.earliest_tx_ms);
    collision_start_us =
        (uint64_t)result_a.gateway_route_adv.earliest_tx_ms * 1000u;
    partial_start_us = collision_start_us +
        (uint64_t)FLOOD_RELAY_REPEAT_MS * 1000u;
    retry_start_us = partial_start_us +
        (uint64_t)app_mesh_flood_backoff_ms(1u, 7u) * 1000u;

    mesh_sim_init(&world, UINT32_C(0x48494150));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
              GATEWAY_ID, ROUTE_EPOCH, &gateway_index) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, forwarder_a_id,
              GATEWAY_ID, ROUTE_EPOCH, &a_index) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, forwarder_b_id,
              GATEWAY_ID, ROUTE_EPOCH, &b_index) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, child_id,
              GATEWAY_ID, ROUTE_EPOCH, &child_index) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, a_index, child_index, 100u, 0u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, b_index, child_index, 100u, 0u) == MESH_SIM_OK);

    airtime_us = mesh_sim_frame_duration_us(MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                             PACKET_HEADER_LEN +
                                             result_a.gateway_route_adv.payload_len +
                                             PACKET_CRC_LEN);
    CHECK(airtime_us > 0u);
    CHECK(mesh_sim_schedule_rx(&world, child_index,
              collision_start_us - RADIO_GUARD_US,
              collision_start_us + airtime_us + RADIO_GUARD_US,
              UWB_CHANNEL_WAKE_CONTACT, MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              &rx_collision) == MESH_SIM_OK);
    CHECK(schedule_phy_only_outbound(&world, a_index, collision_start_us,
              &result_a.gateway_route_adv, &tx_a) == MESH_SIM_OK);
    CHECK(schedule_phy_only_outbound(&world, b_index, collision_start_us,
              &result_b.gateway_route_adv, &tx_b) == MESH_SIM_OK);

    CHECK(mesh_sim_schedule_rx(&world, child_index,
              partial_start_us - RADIO_GUARD_US,
              partial_start_us + airtime_us / 2u,
              UWB_CHANNEL_WAKE_CONTACT, MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              &rx_partial) == MESH_SIM_OK);
    CHECK(schedule_phy_only_outbound(&world, a_index, partial_start_us,
              &result_a.gateway_route_adv, &retry_tx) == MESH_SIM_OK);
    CHECK(mesh_sim_schedule_rx(&world, child_index,
              retry_start_us - RADIO_GUARD_US,
              retry_start_us + airtime_us + RADIO_GUARD_US,
              UWB_CHANNEL_WAKE_CONTACT, MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              &rx_retry) == MESH_SIM_OK);
    CHECK(schedule_phy_only_outbound(&world, a_index, retry_start_us,
              &result_a.gateway_route_adv, &retry_tx) == MESH_SIM_OK);
    CHECK(mesh_sim_run(&world) == MESH_SIM_OK);

    for (size_t i = 0u; i < world.reception_count; i++) {
        const struct mesh_sim_reception *rx = &world.receptions[i];

        saw_collision |= rx->outcome == MESH_SIM_RX_COLLISION;
        saw_partial |= rx->outcome == MESH_SIM_RX_FRAME_TIMEOUT ||
                       rx->outcome == MESH_SIM_RX_PREAMBLE_ONLY ||
                       rx->outcome == MESH_SIM_RX_SFD_TIMEOUT;
        saw_retry_decode |= rx->outcome == MESH_SIM_RX_DECODED &&
                            rx->source_id == forwarder_a_id &&
                            rx->start_us == retry_start_us;
    }
    CHECK(saw_collision && saw_partial && saw_retry_decode);

    CHECK(mesh_relay_handle_rx_with_random(&child,
              &result_a.gateway_route_adv.packet,
              result_a.gateway_route_adv.payload,
              result_a.gateway_route_adv.payload_len,
              forwarder_a_id, 85u, 1200u, 9u, &child_result) == PROTO_OK);
    CHECK(child_result.status == PROTO_OK);
    CHECK(route_selected(&child.upstream) != NULL);
    CHECK(route_selected(&child.upstream)->next_hop_id == forwarder_a_id);
    CHECK(mesh_relay_handle_rx_with_random(&child,
              &result_a.gateway_route_adv.packet,
              result_a.gateway_route_adv.payload,
              result_a.gateway_route_adv.payload_len,
              forwarder_a_id, 85u, 1210u, 10u, &child_result) == PROTO_OK);
    CHECK(child_result.status == PROTO_ERR_STALE);
}

static void test_simulator_fails_closed_without_flood_state_machine(void)
{
    static struct mesh_sim_world world;
    struct mesh_relay gateway;
    struct mesh_outbound adv;
    uint8_t gateway_index;
    uint8_t anchor_index;
    uint16_t tx_index;
    uint16_t rx_index;
    uint32_t airtime_us;
    uint64_t start_us = UINT64_C(10000);

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH);
    CHECK(mesh_relay_build_gateway_route_adv(&gateway, ROUTE_ADV_SEQ,
                                              10u, &adv) == PROTO_OK);
    mesh_sim_init(&world, UINT32_C(0x48494151));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
              GATEWAY_ID, ROUTE_EPOCH, &gateway_index) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_BASE,
              GATEWAY_ID, ROUTE_EPOCH, &anchor_index) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, gateway_index, anchor_index, 90u, 0u) ==
          MESH_SIM_OK);
    airtime_us = mesh_sim_frame_duration_us(MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                             PACKET_HEADER_LEN +
                                             adv.payload_len + PACKET_CRC_LEN);
    CHECK(mesh_sim_schedule_rx(&world, anchor_index,
              start_us - RADIO_GUARD_US,
              start_us + airtime_us + RADIO_GUARD_US,
              UWB_CHANNEL_WAKE_CONTACT, MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              &rx_index) == MESH_SIM_OK);
    CHECK(mesh_sim_schedule_outbound_tx(&world, gateway_index, start_us,
                                         &adv, &tx_index) == MESH_SIM_OK);
    CHECK(mesh_sim_run(&world) == MESH_SIM_ERR_UNSUPPORTED_ACTION);
    CHECK(world.last_error == MESH_SIM_ERR_UNSUPPORTED_ACTION);
    CHECK(world.transmission_count == 1u);
    CHECK(route_selected(&world.roles[anchor_index].relay.upstream) != NULL);
}

int main(void)
{
    test_here_i_am_production_helper_composition();
    test_here_i_am_direct_scale_matrix();
    test_here_i_am_20_direct_50_mixed_hop_radio_scale();
    test_here_i_am_ttl_and_epoch_fail_closed();
    test_gateway_control_reverse_route_contract();
    test_gateway_control_reverse_route_fifty_anchor_reply_sweep();
    test_gateway_control_click_and_busy_deferral();
    test_gateway_control_rx_handoff_stress_sweep();
    test_survey_pair_control_bounded_flood_lane();
    test_here_i_am_radio_collision_containment_and_retry();
    test_simulator_fails_closed_without_flood_state_machine();
    if (failures == 0) {
        puts("mesh gateway control stress scenarios: PASS here-i-am=direct20/mixed50/ttl8/radio survey-control=direct/relay handoff=512");
    }
    return failures == 0 ? 0 : 1;
}

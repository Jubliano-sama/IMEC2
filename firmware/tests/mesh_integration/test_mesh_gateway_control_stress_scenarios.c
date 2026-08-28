#include "app_gateway_command_ingress.h"
#include "app_gateway_command_lifecycle.h"
#include "app_gateway_command_observability.h"
#include "app_mesh_flood.h"
#include "app_mesh_rx_policy.h"
#include "discovery_assignment.h"
#include "mesh_radio_timing.h"
#include "gateway_command.h"
#include "mesh_relay.h"
#include "mesh_sim.h"
#include "protocol.h"
#include "protocol_rx_lifecycle.h"
#include "serial_frame.h"
#include "uwb.h"
#include "uwb_session.h"

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
#define RADIO_GUARD_US UINT64_C(500)
#define RF_HIDDEN_NETWORK_ID UINT32_C(0x494d4543)
#define RF_HIDDEN_RELAY_WAKE_START_US UINT64_C(100000)
#define RF_HIDDEN_RELAY_WAKE_US \
    (UINT64_C(1000) * MESH_RADIO_WAKE_TRAIN_MS)
#define RF_HIDDEN_POST_WAKE_CLAIM_MS 500u
#define RF_HIDDEN_CONTROL_LISTEN_US UINT64_C(200000)
#define RF_HIDDEN_SURVEY_LISTEN_US UINT64_C(800000)
#define RF_HIDDEN_SURVEY_GENERATION UINT64_C(0x5355525645590001)

_Static_assert(MESH_RADIO_WAKE_TRAIN_MS == 500u,
               "RF-hidden relay regressions require the ordinary 500 ms wake");

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

static struct operation_policy_set complete_route_operation_policy(void)
{
    struct operation_policy_set policy;

    operation_policy_set_defaults(&policy);
    policy.assignment_present = true;
    policy.assignment.expected_anchor_count = 50u;
    policy.assignment.operation_budget_ms =
        OPERATION_POLICY_ASSIGNMENT_DEFAULT_BUDGET_MS;
    policy.assignment.response_spread_ms = 750u;
    return policy;
}

static int copy_operation_policy_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t policy_tlvs[OPERATION_POLICY_ALL_TLVS_LEN],
    size_t *policy_tlvs_len)
{
    size_t offset = 0u;
    size_t copied = 0u;

    if (payload == NULL || policy_tlvs == NULL || policy_tlvs_len == NULL) {
        return PROTO_ERR_ARG;
    }
    while (offset < payload_len) {
        size_t tlv_offset = offset;
        uint8_t type;
        uint8_t value_len;
        size_t tlv_len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        value_len = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if (payload_len - offset < value_len) {
            return PROTO_ERR_MALFORMED;
        }
        tlv_len = PROTO_TLV_HEADER_LEN + value_len;
        if (type == TLV_OPERATION_POLICY) {
            if (copied + tlv_len > OPERATION_POLICY_ALL_TLVS_LEN) {
                return PROTO_ERR_NO_SPACE;
            }
            memcpy(&policy_tlvs[copied], &payload[tlv_offset], tlv_len);
            copied += tlv_len;
        }
        offset += value_len;
    }
    *policy_tlvs_len = copied;
    return PROTO_OK;
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

static bool schedule_relay_activation_train(struct mesh_sim_world *world,
                                            uint8_t sender_index,
                                            uint64_t sender_id,
                                            uint64_t start_us,
                                            uint64_t *last_end_us)
{
    struct uwb_clicker_session session;
    const struct uwb_clicker_config config = {
        .network_id = RF_HIDDEN_NETWORK_ID,
        .clicker_id = sender_id,
        .click_event_id = ROUTE_ADV_SEQ,
        .nonce = UINT64_C(0x48494157414b4501),
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .max_attempts = 1u,
        .samples_per_anchor = 1u,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .flags = FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
                 FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY,
    };
    const uint64_t close_us = start_us + RF_HIDDEN_RELAY_WAKE_US;
    uint64_t at_us = start_us;

    if (world == NULL || last_end_us == NULL ||
        uwb_clicker_session_start(&session, &config) != PROTO_OK) {
        return false;
    }
    *last_end_us = 0u;
    while (at_us < close_us) {
        struct uwb_wake_claim_frame claim;
        uint8_t frame[UWB_WAKE_CLAIM_LEN];
        uint64_t remaining_us = close_us - at_us;
        uint16_t remaining_ms =
            (uint16_t)((remaining_us + UINT64_C(999)) / UINT64_C(1000));
        uint16_t claimed_ms =
            (uint16_t)(remaining_ms + RF_HIDDEN_POST_WAKE_CLAIM_MS);
        uint16_t tx_index;
        size_t frame_len = 0u;
        uint32_t jitter_us;

        if (uwb_clicker_build_wake_claim(&session,
                                         sender_id,
                                         remaining_ms,
                                         remaining_ms,
                                         claimed_ms,
                                         &claim) != PROTO_OK ||
            uwb_encode_wake_claim(&claim,
                                  frame,
                                  sizeof(frame),
                                  &frame_len) != PROTO_OK ||
            mesh_sim_schedule_raw_tx(world,
                                     sender_index,
                                     at_us,
                                     UWB_CHANNEL_WAKE_CONTACT,
                                     MESH_SIM_PHY_CHANNEL5_WAKE,
                                     frame,
                                     frame_len,
                                     false,
                                     &tx_index) != MESH_SIM_OK) {
            return false;
        }
        *last_end_us = world->transmissions[tx_index].end_us;
        uwb_clicker_note_wake_claim_tx(&session, 1u);
        jitter_us = mesh_sim_random(world) %
                    (UWB_CLICKER_WAKE_CLAIM_JITTER_MAX_US + 1u);
        at_us = *last_end_us + jitter_us;
    }
    return *last_end_us > start_us;
}

static size_t count_rf_receptions(const struct mesh_sim_world *world,
                                  uint64_t source_id,
                                  uint64_t receiver_id,
                                  enum mesh_sim_phy phy)
{
    size_t count = 0u;

    for (size_t i = 0u; i < world->reception_count; i++) {
        const struct mesh_sim_reception *rx = &world->receptions[i];

        if (rx->source_id == source_id && rx->receiver_id == receiver_id &&
            rx->phy == phy && rx->outcome == MESH_SIM_RX_DECODED) {
            count++;
        }
    }
    return count;
}

static bool relay_activation_delivers_to_low_duty_child(
    struct mesh_sim_world *world,
    uint8_t relay_index,
    uint8_t child_index,
    uint64_t relay_id,
    uint64_t child_id,
    const struct mesh_outbound *outbound,
    uint64_t control_listen_us,
    uint16_t *listener_index)
{
    uint16_t tx_index;
    uint64_t wake_last_end_us;
    uint64_t listener_start_us;
    uint64_t adv_start_us;
    uint64_t listener_end_us;
    size_t control_receptions;

    if (world == NULL || outbound == NULL || control_listen_us == 0u) {
        return false;
    }
    control_receptions = count_rf_receptions(
        world, relay_id, child_id, MESH_SIM_PHY_CHANNEL5_MESH_CONTROL);

    /* The bare relay copy falls in the child's production low-duty gap. */
    if (schedule_phy_only_outbound(world,
                                   relay_index,
                                   UINT64_C(50000),
                                   outbound,
                                   &tx_index) != MESH_SIM_OK ||
        mesh_sim_run_until(world,
                           world->transmissions[tx_index].end_us + 1u) !=
            MESH_SIM_OK ||
        count_rf_receptions(world,
                            relay_id,
                            child_id,
                            MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) !=
            control_receptions ||
        !schedule_relay_activation_train(world,
                                         relay_index,
                                         relay_id,
                                         RF_HIDDEN_RELAY_WAKE_START_US,
                                         &wake_last_end_us) ||
        mesh_sim_run_until(world, wake_last_end_us + 1u) != MESH_SIM_OK ||
        count_rf_receptions(world,
                            relay_id,
                            child_id,
                            MESH_SIM_PHY_CHANNEL5_WAKE) == 0u ||
        world->roles[child_index].anchor_session.diagnostics.claims == 0u) {
        return false;
    }

    /* Low-duty acquisition is physical. This broad follow-up slice models
     * production's decoded-claim handoff to the extended-PHR listener. */
    listener_start_us = world->now_us +
        (uint64_t)MESH_RADIO_EVENT_RETUNE_GUARD_MS * 1000u;
    adv_start_us = listener_start_us + RADIO_GUARD_US;
    listener_end_us = listener_start_us + control_listen_us;
    if (mesh_sim_schedule_rx(world,
                             child_index,
                             listener_start_us,
                             listener_end_us,
                             UWB_CHANNEL_WAKE_CONTACT,
                             MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                             listener_index) != MESH_SIM_OK ||
        schedule_phy_only_outbound(world,
                                   relay_index,
                                   adv_start_us,
                                   outbound,
                                   &tx_index) != MESH_SIM_OK ||
        mesh_sim_run_until(world,
                           world->transmissions[tx_index].end_us + 1u) !=
            MESH_SIM_OK) {
        return false;
    }
    return count_rf_receptions(world,
                               relay_id,
                               child_id,
                               MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) ==
           control_receptions + 1u;
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

static void test_here_i_am_relay_wakes_rf_hidden_low_duty_child(void)
{
    static struct mesh_sim_world world;
    const uint64_t relay_id = ANCHOR_BASE + 300u;
    const uint64_t child_id = ANCHOR_BASE + 301u;
    const struct uwb_anchor_config child_config = {
        .network_id = RF_HIDDEN_NETWORK_ID,
        .anchor_id = child_id,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
    };
    struct mesh_outbound gateway_adv;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result child_result;
    uint8_t gateway_index;
    uint8_t relay_index;
    uint8_t child_index;

    mesh_sim_init(&world, UINT32_C(0x48494131));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
              GATEWAY_ID, ROUTE_EPOCH, &gateway_index) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, relay_id,
              GATEWAY_ID, ROUTE_EPOCH, &relay_index) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, child_id,
              GATEWAY_ID, ROUTE_EPOCH, &child_index) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, gateway_index, relay_index, 100u, 0u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, relay_index, child_index, 100u, 0u) ==
          MESH_SIM_OK);
    CHECK(!world.reachable[gateway_index][child_index]);
    CHECK(mesh_sim_init_anchor_session(&world, child_index, &child_config) ==
          PROTO_OK);
    CHECK(mesh_sim_start_anchor_low_duty(&world, child_index, 0u) ==
          MESH_SIM_OK);

    CHECK(mesh_relay_build_gateway_route_adv(
              &world.roles[gateway_index].relay,
              ROUTE_ADV_SEQ,
              10u,
              &gateway_adv) == PROTO_OK);
    CHECK(mesh_relay_handle_rx_with_random(
              &world.roles[relay_index].relay,
              &gateway_adv.packet,
              gateway_adv.payload,
              gateway_adv.payload_len,
              GATEWAY_ID,
              100u,
              11u,
              UINT32_C(0x48494152),
              &relay_result) == PROTO_OK);
    CHECK(relay_result.status == PROTO_OK);
    CHECK(has_action(&relay_result,
                     MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    CHECK(route_selected(&world.roles[child_index].relay.upstream) == NULL);
    CHECK(relay_activation_delivers_to_low_duty_child(
              &world,
              relay_index,
              child_index,
              relay_id,
              child_id,
              &relay_result.gateway_route_adv,
              RF_HIDDEN_CONTROL_LISTEN_US,
              NULL));

    CHECK(mesh_relay_handle_rx_with_random(
              &world.roles[child_index].relay,
              &relay_result.gateway_route_adv.packet,
              relay_result.gateway_route_adv.payload,
              relay_result.gateway_route_adv.payload_len,
              relay_id,
              100u,
              (uint32_t)(world.now_us / 1000u),
              UINT32_C(0x48494143),
              &child_result) == PROTO_OK);
    CHECK(child_result.status == PROTO_OK);
    CHECK(route_selected(&world.roles[child_index].relay.upstream) != NULL);
    CHECK(route_selected(&world.roles[child_index].relay.upstream)->next_hop_id ==
          relay_id);
}

static void test_compact_survey_relay_wakes_rf_hidden_low_duty_child(void)
{
    static const enum command_id command_ids[] = {
        CMD_SURVEY_START,
        CMD_SURVEY_PLAN,
    };
    static struct mesh_sim_world world;
    const uint64_t relay_id = ANCHOR_BASE + 320u;
    const uint64_t child_id = ANCHOR_BASE + 330u;
    const struct uwb_anchor_config child_config = {
        .network_id = RF_HIDDEN_NETWORK_ID,
        .anchor_id = child_id,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
    };
    struct proto_packet commands[2];
    struct mesh_relay_result relay_results[2];
    struct mesh_relay_result child_result;
    struct protocol_rx_lifecycle lifecycle;
    uint8_t payloads[2][64];
    size_t payload_lengths[2] = {0u};
    size_t wake_receptions;
    size_t control_receptions;
    uint64_t control_tx_us;
    uint16_t control_window_index;
    uint16_t tx_index;
    uint8_t gateway_index;
    uint8_t relay_index;
    uint8_t child_index;

    for (size_t i = 0u; i < sizeof(command_ids) / sizeof(command_ids[0]); i++) {
        commands[i] = (struct proto_packet) {
            .msg_type = MSG_COMMAND,
            .src_id = GATEWAY_ID,
            .dst_id = MESH_BROADCAST_ID,
            .session_id = 7200u + (uint32_t)i,
            .seq = (uint16_t)(40u + i),
            .ttl = FLOOD_EPOCH_GLOBAL_TTL,
        };
        CHECK(mesh_append_command_id(payloads[i],
                                     sizeof(payloads[i]),
                                     &payload_lengths[i],
                                     command_ids[i]) == PROTO_OK);
        CHECK(tlv_append_u8(payloads[i],
                            sizeof(payloads[i]),
                            &payload_lengths[i],
                            TLV_COMMAND_SCOPE,
                            CMD_SCOPE_ALL_HEARD) == PROTO_OK);
        CHECK(tlv_append_u8(payloads[i],
                            sizeof(payloads[i]),
                            &payload_lengths[i],
                            TLV_COMMAND_RESPONSE_MODE,
                            CMD_RESPONSE_NONE) == PROTO_OK);
        CHECK(tlv_append_u32(payloads[i],
                             sizeof(payloads[i]),
                             &payload_lengths[i],
                             TLV_COMMAND_SEQ,
                             commands[i].session_id) == PROTO_OK);
        CHECK(tlv_append_u32(payloads[i],
                             sizeof(payloads[i]),
                             &payload_lengths[i],
                             TLV_FLOOD_EPOCH_ID,
                             8200u + (uint32_t)i) == PROTO_OK);
        commands[i].payload_len = (uint16_t)payload_lengths[i];
        CHECK(gateway_command_uses_compact_scheduled_flood(
                  payloads[i], payload_lengths[i]));
    }

    mesh_sim_init(&world, UINT32_C(0x53555230));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
              GATEWAY_ID, ROUTE_EPOCH, &gateway_index) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, relay_id,
              GATEWAY_ID, ROUTE_EPOCH, &relay_index) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, child_id,
              GATEWAY_ID, ROUTE_EPOCH, &child_index) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, gateway_index, relay_index,
                            100u, 0u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, relay_index, child_index,
                            100u, 0u) == MESH_SIM_OK);
    CHECK(!world.reachable[gateway_index][child_index]);
    CHECK(mesh_sim_init_anchor_session(&world,
                                       child_index,
                                       &child_config) == PROTO_OK);
    CHECK(mesh_sim_start_anchor_low_duty(&world, child_index, 0u) ==
          MESH_SIM_OK);
    protocol_rx_lifecycle_init(&lifecycle);

    CHECK(mesh_relay_handle_rx_with_random(
              &world.roles[relay_index].relay,
              &commands[0],
              payloads[0],
              payload_lengths[0],
              GATEWAY_ID,
              100u,
              11u,
              UINT32_C(0x53555252),
              &relay_results[0]) == PROTO_OK);
    CHECK(relay_results[0].status == PROTO_OK);
    CHECK(has_action(&relay_results[0], MESH_RELAY_ACTION_FORWARD));
    CHECK(relay_results[0].forward.flood_retry_count ==
          MESH_ENUMERATION_RELAY_COPY_COUNT - 1u);
    CHECK(relay_activation_delivers_to_low_duty_child(
              &world,
              relay_index,
              child_index,
              relay_id,
              child_id,
              &relay_results[0].forward,
              RF_HIDDEN_SURVEY_LISTEN_US,
              &control_window_index));
    CHECK(mesh_relay_handle_rx_with_random(
              &world.roles[child_index].relay,
              &relay_results[0].forward.packet,
              relay_results[0].forward.payload,
              relay_results[0].forward.payload_len,
              relay_id,
              100u,
              (uint32_t)(world.now_us / 1000u),
              UINT32_C(0x53555243),
              &child_result) == PROTO_OK);
    CHECK(child_result.status == PROTO_OK);
    CHECK(has_action(&child_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    CHECK(protocol_rx_lifecycle_begin(
              &lifecycle,
              PROTOCOL_RX_OPERATION_SURVEY,
              RF_HIDDEN_SURVEY_GENERATION,
              (uint32_t)(world.now_us / 1000u),
              (uint32_t)(world.now_us / 1000u) + 5000u) ==
          PROTOCOL_RX_BEGIN_ACCEPTED);
    CHECK(lifecycle.mode == PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5);

    wake_receptions = count_rf_receptions(
        &world, relay_id, child_id, MESH_SIM_PHY_CHANNEL5_WAKE);
    control_receptions = count_rf_receptions(
        &world, relay_id, child_id, MESH_SIM_PHY_CHANNEL5_MESH_CONTROL);
    CHECK(wake_receptions > 0u);
    CHECK(world.rx_windows[control_window_index].continuous_operation);

    /* The initial START activation owns all three copies. Once the first
     * copy starts the survey listener, its two redrives are bare. */
    for (uint8_t redrive = 0u;
         redrive < MESH_ENUMERATION_RELAY_COPY_COUNT - 1u;
         redrive++) {
        control_tx_us = world.now_us + UINT64_C(100000);
        CHECK(schedule_phy_only_outbound(&world,
                                         relay_index,
                                         control_tx_us,
                                         &relay_results[0].forward,
                                         &tx_index) == MESH_SIM_OK);
        CHECK(world.transmissions[tx_index].end_us <
              world.rx_windows[control_window_index].end_us);
        CHECK(mesh_sim_run_until(
                  &world,
                  world.transmissions[tx_index].end_us + 1u) == MESH_SIM_OK);
        CHECK(count_rf_receptions(
                  &world,
                  relay_id,
                  child_id,
                  MESH_SIM_PHY_CHANNEL5_WAKE) == wake_receptions);
        control_receptions++;
        CHECK(count_rf_receptions(
                  &world,
                  relay_id,
                  child_id,
                  MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) ==
              control_receptions);
    }

    CHECK(mesh_relay_handle_rx_with_random(
              &world.roles[relay_index].relay,
              &commands[1],
              payloads[1],
              payload_lengths[1],
              GATEWAY_ID,
              100u,
              (uint32_t)(world.now_us / 1000u),
              UINT32_C(0x53555253),
              &relay_results[1]) == PROTO_OK);
    CHECK(relay_results[1].status == PROTO_OK);
    CHECK(has_action(&relay_results[1], MESH_RELAY_ACTION_FORWARD));
    CHECK(relay_results[1].forward.flood_retry_count ==
          MESH_ENUMERATION_RELAY_COPY_COUNT - 1u);
    control_tx_us = world.now_us + UINT64_C(100000);
    CHECK(schedule_phy_only_outbound(&world,
                                     relay_index,
                                     control_tx_us,
                                     &relay_results[1].forward,
                                     &tx_index) == MESH_SIM_OK);
    CHECK(world.transmissions[tx_index].end_us <
          world.rx_windows[control_window_index].end_us);
    CHECK(mesh_sim_run_until(
              &world,
              world.transmissions[tx_index].end_us + 1u) == MESH_SIM_OK);
    CHECK(count_rf_receptions(
              &world,
              relay_id,
              child_id,
              MESH_SIM_PHY_CHANNEL5_WAKE) == wake_receptions);
    control_receptions++;
    CHECK(count_rf_receptions(
              &world,
              relay_id,
              child_id,
              MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) == control_receptions);
    CHECK(mesh_relay_handle_rx_with_random(
              &world.roles[child_index].relay,
              &relay_results[1].forward.packet,
              relay_results[1].forward.payload,
              relay_results[1].forward.payload_len,
              relay_id,
              100u,
              (uint32_t)(world.now_us / 1000u),
              UINT32_C(0x53555244),
              &child_result) == PROTO_OK);
    CHECK(child_result.status == PROTO_OK);
    CHECK(has_action(&child_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    CHECK(has_action(&child_result, MESH_RELAY_ACTION_FORWARD));
    CHECK(protocol_rx_lifecycle_set_deadline(
              &lifecycle,
              PROTOCOL_RX_OPERATION_SURVEY,
              RF_HIDDEN_SURVEY_GENERATION,
              (uint32_t)(world.now_us / 1000u),
              (uint32_t)(world.now_us / 1000u) + 5000u));
    CHECK(lifecycle.mode == PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5);
}

static void test_survey_rx_termination_returns_to_low_duty(void)
{
    enum survey_termination {
        SURVEY_TERMINATION_COMPLETE,
        SURVEY_TERMINATION_CANCEL,
        SURVEY_TERMINATION_EXPIRE,
        SURVEY_TERMINATION_COUNT,
    };
    static struct mesh_sim_world world;

    for (enum survey_termination termination = SURVEY_TERMINATION_COMPLETE;
         termination < SURVEY_TERMINATION_COUNT;
         termination++) {
        const uint64_t relay_id = ANCHOR_BASE + 340u + termination;
        const uint64_t child_id = ANCHOR_BASE + 350u + termination;
        const uint64_t generation = RF_HIDDEN_SURVEY_GENERATION + termination;
        const uint64_t terminal_us = UINT64_C(100000);
        struct protocol_rx_lifecycle lifecycle;
        struct mesh_outbound plan = {
            .packet = {
                .msg_type = MSG_COMMAND,
                .src_id = GATEWAY_ID,
                .dst_id = MESH_BROADCAST_ID,
                .session_id = 7300u + (uint32_t)termination,
                .seq = (uint16_t)(50u + termination),
                .ttl = FLOOD_EPOCH_GLOBAL_TTL,
            },
            .radio_channel = UWB_CHANNEL_WAKE_CONTACT,
        };
        size_t payload_len = 0u;
        size_t control_receptions;
        uint64_t low_duty_gap_tx_us;
        uint16_t continuous_window;
        uint16_t low_duty_window;
        uint16_t tx_index;
        uint8_t relay_index;
        uint8_t child_index;

        CHECK(mesh_append_command_id(plan.payload,
                                     sizeof(plan.payload),
                                     &payload_len,
                                     CMD_SURVEY_PLAN) == PROTO_OK);
        CHECK(tlv_append_u8(plan.payload,
                            sizeof(plan.payload),
                            &payload_len,
                            TLV_COMMAND_SCOPE,
                            CMD_SCOPE_ALL_HEARD) == PROTO_OK);
        CHECK(tlv_append_u8(plan.payload,
                            sizeof(plan.payload),
                            &payload_len,
                            TLV_COMMAND_RESPONSE_MODE,
                            CMD_RESPONSE_NONE) == PROTO_OK);
        CHECK(tlv_append_u32(plan.payload,
                             sizeof(plan.payload),
                             &payload_len,
                             TLV_COMMAND_SEQ,
                             plan.packet.session_id) == PROTO_OK);
        CHECK(tlv_append_u32(plan.payload,
                             sizeof(plan.payload),
                             &payload_len,
                             TLV_FLOOD_EPOCH_ID,
                             8300u + (uint32_t)termination) == PROTO_OK);
        plan.payload_len = (uint16_t)payload_len;
        plan.packet.payload_len = (uint16_t)payload_len;

        mesh_sim_init(&world, UINT32_C(0x53555260) + termination);
        CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, relay_id,
                  GATEWAY_ID, ROUTE_EPOCH, &relay_index) == MESH_SIM_OK);
        CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, child_id,
                  GATEWAY_ID, ROUTE_EPOCH, &child_index) == MESH_SIM_OK);
        CHECK(mesh_sim_set_link(&world, relay_index, child_index,
                                100u, 0u) == MESH_SIM_OK);
        protocol_rx_lifecycle_init(&lifecycle);
        CHECK(protocol_rx_lifecycle_begin(
                  &lifecycle,
                  PROTOCOL_RX_OPERATION_SURVEY,
                  generation,
                  0u,
                  (uint32_t)(terminal_us / 1000u)) ==
              PROTOCOL_RX_BEGIN_ACCEPTED);
        CHECK(mesh_sim_schedule_rx(&world,
                                   child_index,
                                   0u,
                                   terminal_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                   &continuous_window) == MESH_SIM_OK);
        CHECK(world.rx_windows[continuous_window].continuous_operation);
        CHECK(schedule_phy_only_outbound(&world,
                                         relay_index,
                                         UINT64_C(50000),
                                         &plan,
                                         &tx_index) == MESH_SIM_OK);
        CHECK(mesh_sim_run_until(&world, terminal_us) == MESH_SIM_OK);
        CHECK(count_rf_receptions(
                  &world,
                  relay_id,
                  child_id,
                  MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) == 1u);

        if (termination == SURVEY_TERMINATION_EXPIRE) {
            CHECK(protocol_rx_lifecycle_expire(
                      &lifecycle, (uint32_t)(terminal_us / 1000u)));
        } else {
            CHECK(protocol_rx_lifecycle_terminate(
                      &lifecycle,
                      PROTOCOL_RX_OPERATION_SURVEY,
                      generation));
        }
        CHECK(lifecycle.operation == PROTOCOL_RX_OPERATION_NONE);
        CHECK(lifecycle.mode == PROTOCOL_RX_MODE_LOW_DUTY);

        CHECK(mesh_sim_start_anchor_low_duty(
                  &world, child_index, world.now_us) == MESH_SIM_OK);
        CHECK(mesh_sim_run_until(&world, world.now_us) == MESH_SIM_OK);
        CHECK(world.rx_window_count >= 2u);
        low_duty_window = (uint16_t)(world.rx_window_count - 1u);
        CHECK(world.rx_windows[low_duty_window].periodic_low_duty);
        CHECK(world.rx_windows[low_duty_window].phy ==
              MESH_SIM_PHY_CHANNEL5_WAKE);
        CHECK(mesh_sim_run_until(
                  &world,
                  world.rx_windows[low_duty_window].end_us + 1u) ==
              MESH_SIM_OK);
        CHECK(world.transition_kind_counts[
                  MESH_SIM_TRANSITION_LOW_DUTY_RESCHEDULED] > 0u);

        control_receptions = count_rf_receptions(
            &world, relay_id, child_id, MESH_SIM_PHY_CHANNEL5_MESH_CONTROL);
        low_duty_gap_tx_us = world.now_us + UINT64_C(50000);
        CHECK(schedule_phy_only_outbound(&world,
                                         relay_index,
                                         low_duty_gap_tx_us,
                                         &plan,
                                         &tx_index) == MESH_SIM_OK);
        CHECK(mesh_sim_run_until(
                  &world,
                  world.transmissions[tx_index].end_us + 1u) == MESH_SIM_OK);
        CHECK(count_rf_receptions(
                  &world,
                  relay_id,
                  child_id,
                  MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) == control_receptions);
    }
}

static void test_here_i_am_ttl_and_epoch_fail_closed(void)
{
    struct mesh_relay gateway;
    struct mesh_relay chain[MESH_NETWORK_MAX_HOPS];
    struct operation_policy_set policy = complete_route_operation_policy();
    struct mesh_outbound current;
    uint8_t expected_policy_tlvs[OPERATION_POLICY_ALL_TLVS_LEN];
    size_t expected_policy_tlvs_len = 0u;
    uint64_t previous_id = GATEWAY_ID;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH);
    CHECK(mesh_relay_build_gateway_route_adv_with_policy(
              &gateway, ROUTE_ADV_SEQ, 1000u, &policy, &current) == PROTO_OK);
    CHECK(copy_operation_policy_tlvs(
              current.payload, current.payload_len,
              expected_policy_tlvs, &expected_policy_tlvs_len) == PROTO_OK);
    CHECK(expected_policy_tlvs_len == sizeof(expected_policy_tlvs));
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
        CHECK(has_action(&result,
                         MESH_RELAY_ACTION_INSTALL_OPERATION_POLICY));
        CHECK(result.operation_policy.assignment_present);
        CHECK(result.operation_policy.assignment.expected_anchor_count ==
              policy.assignment.expected_anchor_count);
        selected = route_selected(&chain[hop].upstream);
        CHECK(selected != NULL && selected->next_hop_id == previous_id);
        if (hop + 1u < MESH_NETWORK_MAX_HOPS) {
            uint8_t forwarded_policy_tlvs[OPERATION_POLICY_ALL_TLVS_LEN];
            size_t forwarded_policy_tlvs_len = 0u;

            CHECK(has_action(&result,
                              MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
            CHECK(copy_operation_policy_tlvs(
                      result.gateway_route_adv.payload,
                      result.gateway_route_adv.payload_len,
                      forwarded_policy_tlvs,
                      &forwarded_policy_tlvs_len) == PROTO_OK);
            CHECK(forwarded_policy_tlvs_len == expected_policy_tlvs_len);
            CHECK(memcmp(forwarded_policy_tlvs,
                         expected_policy_tlvs,
                         expected_policy_tlvs_len) == 0);
            current = result.gateway_route_adv;
        } else {
            CHECK(!has_action(&result,
                               MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
        }
        previous_id = id;
    }
    CHECK(current.payload_len == MESH_GATEWAY_ROUTE_ADV_MAX_PAYLOAD_LEN);

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
        out->packet.msg_type != MSG_COMMAND) {
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
        if (fixture->pending != NULL && fixture->pending->active) {
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
                gateway_command_pending_claim_result(
                    fixture->pending,
                    &command_result,
                    fixture->now_ms,
                    NULL,
                    NULL) !=
                    GATEWAY_COMMAND_PENDING_RESULT_CLAIM_ACCEPTED) {
                return -EIO;
            }
            fixture->command_results++;
        }
        if (out->packet.msg_type == MSG_COMMAND &&
            mesh_relay_commit_anchor_command_delivery(
                fixture->receiver,
                &out->packet,
                out->payload,
                out->payload_len,
                fixture->now_ms) != PROTO_OK) {
            return -EIO;
        }
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

static int host_submit_priority(void *ctx, uint32_t admission_cutoff)
{
    (void)admission_cutoff;
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

static void test_gateway_scheduled_delivery_due_handoff_sweep(void)
{
    struct app_mesh_rx_handoff_state handoff;
    uint32_t completed = 0u;
    uint32_t cancelled = 0u;

    for (uint32_t cycle = 0u; cycle < 4096u; cycle++) {
        const bool scan_active = (cycle % 8u) != 0u;
        const bool host_control_first = (cycle % 5u) == 0u;
        const bool cancel_before_delivery = (cycle % 13u) == 0u;
        const uint32_t abort_latency_ms = cycle % 25u;
        bool abort_scan = false;

        app_mesh_rx_handoff_reset(&handoff);
        if (scan_active) {
            CHECK(app_mesh_rx_handoff_try_begin_scan(&handoff));
        }
        CHECK(app_mesh_rx_handoff_request_scheduled_control(
            &handoff, &abort_scan));
        CHECK(abort_scan == scan_active);
        CHECK(!app_mesh_rx_handoff_scan_rearm_allowed(&handoff));

        /* Repeated due notifications coalesce behind the same gate. */
        CHECK(app_mesh_rx_handoff_request_scheduled_control(
            &handoff, &abort_scan));
        CHECK(abort_scan == scan_active);
        if (scan_active) {
            for (uint32_t elapsed_ms = 0u; elapsed_ms <= 25u;
                 elapsed_ms++) {
                CHECK(!app_mesh_rx_handoff_try_begin_scan(&handoff));
                if (elapsed_ms == abort_latency_ms) {
                    app_mesh_rx_handoff_end_scan(&handoff);
                    break;
                }
            }
        }
        CHECK(app_mesh_rx_handoff_scheduled_control_ready(&handoff));

        if (host_control_first) {
            /* Host work may run first, but it cannot release the due gate. */
            CHECK(app_mesh_rx_handoff_begin_control(&handoff, &abort_scan));
            CHECK(!abort_scan);
            app_mesh_rx_handoff_end_control(&handoff);
            CHECK(app_mesh_rx_handoff_scheduled_control_pending(&handoff));
            CHECK(!app_mesh_rx_handoff_try_begin_scan(&handoff));
        }

        if (cancel_before_delivery) {
            CHECK(app_mesh_rx_handoff_end_scheduled_control(&handoff));
            cancelled++;
        } else {
            CHECK(app_mesh_rx_handoff_begin_control(&handoff, &abort_scan));
            CHECK(!abort_scan);
            app_mesh_rx_handoff_end_control(&handoff);
            CHECK(!app_mesh_rx_handoff_scan_rearm_allowed(&handoff));
            CHECK(app_mesh_rx_handoff_end_scheduled_control(&handoff));
            completed++;
        }
        CHECK(app_mesh_rx_handoff_scan_rearm_allowed(&handoff));
        CHECK(app_mesh_rx_handoff_try_begin_scan(&handoff));
        app_mesh_rx_handoff_end_scan(&handoff);
    }

    CHECK(completed + cancelled == 4096u);
    CHECK(completed > 3700u);
    CHECK(cancelled > 300u);
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
    test_here_i_am_relay_wakes_rf_hidden_low_duty_child();
    test_compact_survey_relay_wakes_rf_hidden_low_duty_child();
    test_survey_rx_termination_returns_to_low_duty();
    test_here_i_am_ttl_and_epoch_fail_closed();
    test_gateway_control_click_and_busy_deferral();
    test_gateway_control_rx_handoff_stress_sweep();
    test_gateway_scheduled_delivery_due_handoff_sweep();
    test_here_i_am_radio_collision_containment_and_retry();
    test_simulator_fails_closed_without_flood_state_machine();
    if (failures == 0) {
        puts("mesh gateway control stress scenarios: PASS here-i-am=direct20/mixed50/rf-hidden-wake/ttl8 compact-survey=initial-wake/continuous-redrive-plan/low-duty-termination radio-handoff=512 due-gate=4096");
    }
    return failures == 0 ? 0 : 1;
}

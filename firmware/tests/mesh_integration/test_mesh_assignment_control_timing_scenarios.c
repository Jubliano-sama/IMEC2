#include "app_mesh_direct_gateway_retry.h"
#include "discovery_assignment.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_radio_timing.h"
#include "mesh_relay.h"
#include "mesh_sim.h"
#include "protocol.h"
#include "uwb.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GATEWAY_ID UINT64_C(0xA501000000000001)
#define ANCHOR_ID UINT64_C(0xA502000000000001)
#define RELAY_A_ID UINT64_C(0xA502000000000011)
#define RELAY_B_ID UINT64_C(0xA502000000000022)
#define DOWNSTREAM_ID UINT64_C(0xA502000000000033)
#define ASSIGNMENT_EPOCH UINT32_C(0xA5510001)
#define COMMAND_SESSION UINT32_C(0xA5510002)
#define COMMAND_SEQUENCE UINT16_C(1)
#define RESPONSE_SEQUENCE UINT16_C(2)
#define FIRST_COMMAND_TX_US UINT64_C(1000)
#define RX_GUARD_US UINT64_C(100)
#define WAKE_COPY_GAP_US UINT64_C(500)
#define ROUTE_ADV_RECEIVED_AT_MS UINT32_C(1000)
#define ROUTE_ADV_RANDOM UINT32_C(0x12345678)
#define DIRECT_ROUTE_TX_TIMEOUT_MS 20u
#define DIRECT_ROUTE_ATTEMPT_MAX_US \
    ((uint64_t)(DIRECT_ROUTE_TX_TIMEOUT_MS + \
                APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS + \
                APP_MESH_DIRECT_GATEWAY_ACK_RX_MS) * 1000u)
#define DIRECT_ROUTE_FIRST_BACKOFF_MAX_US \
    ((uint64_t)(2u * APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_BASE_MS) * 1000u)
#define DIRECT_ROUTE_RETRY_SNIFF_US MESH_RADIO_WAKE_POLITENESS_CHECK_US

_Static_assert(FLOOD_RELAY_REPEAT_COUNT == 4u,
               "scenario models every bounded control-flood opportunity");
_Static_assert(DISCOVERY_ASSIGNMENT_RESPONSE_BASE_MS >
                   FLOOD_RELAY_REPEAT_MS * 2u &&
               DISCOVERY_ASSIGNMENT_RESPONSE_BASE_MS <
                   FLOOD_RELAY_REPEAT_MS * 3u,
               "minimum assignment response must fall between bounded copies");
_Static_assert(DIRECT_ROUTE_ATTEMPT_MAX_US +
                   DIRECT_ROUTE_FIRST_BACKOFF_MAX_US +
                   DIRECT_ROUTE_RETRY_SNIFF_US <
                   (uint64_t)MESH_RADIO_WAKE_TRAIN_MS * 1000u,
               "direct route retry must recheck C5 inside a gateway wake train");

enum response_rx_mode {
    RESPONSE_RX_NONE = 0,
    RESPONSE_RX_PARTIAL,
    RESPONSE_RX_INTERLEAVED,
};

struct scenario_result {
    enum mesh_sim_rx_outcome response_outcome;
    size_t command_receptions;
    size_t response_receptions;
    bool response_decoded;
};

static int fail_at(int line, const char *format, ...)
{
    va_list args;

    fprintf(stderr, "FAIL assignment-control-timing line=%d: ", line);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    return 1;
}

#define REQUIRE(condition, ...)                    \
    do {                                           \
        if (!(condition)) {                        \
            return fail_at(__LINE__, __VA_ARGS__); \
        }                                          \
    } while (0)

static int encode_application_packet(const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint8_t *frame,
                                     size_t frame_cap,
                                     size_t *frame_len)
{
    if (packet == NULL || packet->payload_len != payload_len) {
        return PROTO_ERR_ARG;
    }
    return proto_packet_encode(packet, payload, frame, frame_cap, frame_len);
}

static int build_assignment_command(struct mesh_outbound *outbound)
{
    size_t payload_len = 0u;
    int ret;

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
                             COMMAND_SESSION);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_FLOOD_EPOCH_ID,
                             COMMAND_SESSION);
    }
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(
            outbound->payload,
            sizeof(outbound->payload),
            &payload_len,
            DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
            ASSIGNMENT_EPOCH);
    }
    if (ret != PROTO_OK || payload_len > UINT16_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }

    outbound->packet = (struct proto_packet) {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = GATEWAY_ID,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = COMMAND_SESSION,
        .seq = COMMAND_SEQUENCE,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
        .payload_len = (uint16_t)payload_len,
    };
    outbound->payload_len = (uint16_t)payload_len;
    outbound->next_hop_id = MESH_BROADCAST_ID;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return gateway_command_append_default_flood_controls(outbound);
}

static int build_assignment_claim(struct mesh_outbound *outbound)
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
        ret = discovery_assignment_append_control_tlvs(
            outbound->payload,
            sizeof(outbound->payload),
            &payload_len,
            DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
            ASSIGNMENT_EPOCH);
    }
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_claim_hash(
            outbound->payload,
            sizeof(outbound->payload),
            &payload_len,
            discovery_assignment_hash(ANCHOR_ID));
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound->payload,
                            sizeof(outbound->payload),
                            &payload_len,
                            TLV_HOP_COUNT,
                            1u);
    }
    if (ret != PROTO_OK || payload_len > UINT8_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    ret = mesh_init_command_result(&outbound->packet,
                                   ANCHOR_ID,
                                   GATEWAY_ID,
                                   COMMAND_SESSION,
                                   RESPONSE_SEQUENCE,
                                   (uint8_t)payload_len,
                                   true);
    if (ret != PROTO_OK) {
        return ret;
    }
    outbound->payload_len = (uint16_t)payload_len;
    return PROTO_OK;
}

static int build_enumeration_route_advertisement(
    struct mesh_relay *gateway,
    struct mesh_outbound *advertisement)
{
    struct operation_policy_set policy;
    struct mesh_gateway_route_adv_snapshot snapshot;
    int ret;

    if (gateway == NULL || advertisement == NULL) {
        return PROTO_ERR_ARG;
    }
    operation_policy_set_defaults(&policy);
    policy.assignment_present = true;
    policy.assignment.expected_anchor_count = 3u;
    policy.assignment.operation_budget_ms =
        OPERATION_POLICY_ASSIGNMENT_DEFAULT_BUDGET_MS;
    policy.assignment.response_spread_ms =
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS;
    policy.assignment.ram_only_iteration = true;
    ret = mesh_relay_capture_gateway_route_adv_snapshot_with_policy(
        gateway,
        ASSIGNMENT_EPOCH,
        ROUTE_ADV_RECEIVED_AT_MS,
        &policy,
        &snapshot);
    if (ret != PROTO_OK) {
        return ret;
    }
    snapshot.enumeration_prearm_epoch = ASSIGNMENT_EPOCH;
    snapshot.enumeration_prearm_hold_ms =
        DISCOVERY_ASSIGNMENT_PREARM_HOLD_MS;
    snapshot.enumeration_prearm_present = true;
    return mesh_relay_build_gateway_route_adv_from_snapshot(
        gateway, &snapshot, advertisement);
}

static int schedule_route_adv_wake_train(struct mesh_sim_world *world,
                                         uint8_t sender,
                                         uint64_t start_us,
                                         uint64_t *last_end_us)
{
    const uint64_t close_us = start_us +
        (uint64_t)MESH_RADIO_WAKE_TRAIN_MS * 1000u;
    uint64_t sender_id;
    uint64_t at_us = start_us;
    size_t sent_count = 0u;

    if (world == NULL || last_end_us == NULL || sender >= world->role_count) {
        return MESH_SIM_ERR_ARG;
    }
    sender_id = world->roles[sender].id;
    while (at_us < close_us) {
        struct uwb_wake_claim_frame claim = {0};
        uint8_t frame[UWB_WAKE_CLAIM_LEN];
        uint64_t remaining_us = close_us - at_us;
        uint16_t remaining_ms =
            (uint16_t)((remaining_us + UINT64_C(999)) / UINT64_C(1000));
        uint16_t claimed_ms = (uint16_t)(remaining_ms + 500u);
        uint16_t tx_index = UINT16_MAX;
        size_t frame_len = 0u;
        int ret;

        if (claimed_ms > UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS) {
            claimed_ms = UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS;
        }
        claim.network_id = UINT32_C(0x494d4543);
        claim.clicker_id = sender_id;
        claim.click_event_id = ASSIGNMENT_EPOCH;
        claim.attempt_index = 1u;
        claim.priority_id = sender_id;
        claim.wake_channel = UWB_CHANNEL_WAKE_CONTACT;
        claim.ranging_channel = UWB_CHANNEL_WAKE_CONTACT;
        claim.wake_train_ends_in_ms = remaining_ms;
        claim.discovery_starts_in_ms = remaining_ms;
        claim.claimed_duration_ms = claimed_ms;
        claim.min_anchor_count = 1u;
        claim.max_anchor_count = 1u;
        claim.nonce = sender_id ^ ASSIGNMENT_EPOCH;
        claim.flags = FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
                      FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;
        ret = uwb_encode_wake_claim(&claim,
                                    frame,
                                    sizeof(frame),
                                    &frame_len);
        if (ret != PROTO_OK) {
            return ret;
        }
        ret = mesh_sim_schedule_raw_tx(world,
                                       sender,
                                       at_us,
                                       UWB_CHANNEL_WAKE_CONTACT,
                                       MESH_SIM_PHY_CHANNEL5_WAKE,
                                       frame,
                                       frame_len,
                                       false,
                                       &tx_index);
        if (ret != MESH_SIM_OK) {
            return ret;
        }
        *last_end_us = world->transmissions[tx_index].end_us;
        at_us = *last_end_us + WAKE_COPY_GAP_US;
        sent_count++;
    }
    return sent_count == 0u ? MESH_SIM_ERR_EVENT_ORDER : MESH_SIM_OK;
}

static size_t decoded_receptions_from(const struct mesh_sim_world *world,
                                      uint64_t source_id,
                                      uint64_t receiver_id,
                                      enum mesh_sim_phy phy)
{
    size_t decoded = 0u;

    for (size_t i = 0u; i < world->reception_count; i++) {
        const struct mesh_sim_reception *reception = &world->receptions[i];

        if (reception->source_id == source_id &&
            reception->receiver_id == receiver_id &&
            reception->phy == phy &&
            reception->outcome == MESH_SIM_RX_DECODED) {
            decoded++;
        }
    }
    return decoded;
}

static int run_scenario(enum response_rx_mode mode,
                        struct scenario_result *result)
{
    static struct mesh_sim_world world;
    struct mesh_outbound command;
    struct mesh_outbound response;
    struct proto_packet decoded;
    uint8_t command_frame[PACKET_EXT_MAX_LEN];
    uint8_t response_frame[PACKET_EXT_MAX_LEN];
    const uint8_t *decoded_payload = NULL;
    size_t command_frame_len = 0u;
    size_t response_frame_len = 0u;
    size_t decoded_payload_len = 0u;
    uint16_t command_tx[FLOOD_RELAY_REPEAT_COUNT];
    uint16_t response_tx = UINT16_MAX;
    uint8_t gateway;
    uint8_t anchor;
    uint32_t response_delay_ms = 0u;
    uint64_t response_arrival_start_us;
    uint64_t response_arrival_end_us;
    int ret;

    memset(result, 0, sizeof(*result));
    result->response_outcome = MESH_SIM_RX_FRAME_TIMEOUT;
    mesh_sim_init(&world, UINT32_C(0xA55171A5));
    ret = mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID,
                            GATEWAY_ID,
                            ASSIGNMENT_EPOCH,
                            &gateway);
    REQUIRE(ret == MESH_SIM_OK, "add gateway ret=%d", ret);
    ret = mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            ANCHOR_ID,
                            GATEWAY_ID,
                            ASSIGNMENT_EPOCH,
                            &anchor);
    REQUIRE(ret == MESH_SIM_OK, "add anchor ret=%d", ret);
    ret = mesh_sim_set_link(&world, gateway, anchor, 100u, 10u);
    REQUIRE(ret == MESH_SIM_OK, "set direct link ret=%d", ret);

    ret = build_assignment_command(&command);
    REQUIRE(ret == PROTO_OK, "build assignment command ret=%d", ret);
    ret = build_assignment_claim(&response);
    REQUIRE(ret == PROTO_OK, "build assignment claim ret=%d", ret);
    ret = encode_application_packet(&command.packet,
                                    command.payload,
                                    command.payload_len,
                                    command_frame,
                                    sizeof(command_frame),
                                    &command_frame_len);
    REQUIRE(ret == PROTO_OK, "encode assignment command ret=%d", ret);
    ret = encode_application_packet(&response.packet,
                                    response.payload,
                                    response.payload_len,
                                    response_frame,
                                    sizeof(response_frame),
                                    &response_frame_len);
    REQUIRE(ret == PROTO_OK, "encode assignment claim ret=%d", ret);

    ret = mesh_sim_schedule_rx(
        &world,
        anchor,
        FIRST_COMMAND_TX_US - RX_GUARD_US,
        FIRST_COMMAND_TX_US +
            mesh_sim_frame_duration_us(MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                       command_frame_len) +
            world.propagation_us[gateway][anchor] + RX_GUARD_US,
        UWB_CHANNEL_WAKE_CONTACT,
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
        NULL);
    REQUIRE(ret == MESH_SIM_OK, "arm anchor command RX ret=%d", ret);
    for (uint8_t opportunity = 0u;
         opportunity < FLOOD_RELAY_REPEAT_COUNT;
         opportunity++) {
        uint64_t start_us = FIRST_COMMAND_TX_US +
            (uint64_t)opportunity * FLOOD_RELAY_REPEAT_MS * 1000u;

        ret = mesh_sim_schedule_raw_tx(&world,
                                       gateway,
                                       start_us,
                                       UWB_CHANNEL_WAKE_CONTACT,
                                       MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                       command_frame,
                                       command_frame_len,
                                       false,
                                       &command_tx[opportunity]);
        REQUIRE(ret == MESH_SIM_OK,
                "schedule command opportunity=%u ret=%d",
                opportunity, ret);
    }
    ret = discovery_assignment_response_delay_ms(
        0u,
        1u,
        1u,
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
        0u,
        0u,
        &response_delay_ms);
    REQUIRE(ret == PROTO_OK &&
                response_delay_ms == DISCOVERY_ASSIGNMENT_RESPONSE_BASE_MS,
            "minimum assignment response delay=%" PRIu32 " ret=%d",
            response_delay_ms, ret);

    ret = mesh_sim_run_until(
        &world,
        world.transmissions[command_tx[0]].end_us +
            world.propagation_us[gateway][anchor] + RX_GUARD_US + 1u);
    REQUIRE(ret == MESH_SIM_OK, "run first command ret=%d", ret);
    REQUIRE(world.roles[anchor].decoded_frames == 1u,
            "anchor decoded=%" PRIu32 " commands after first opportunity",
            world.roles[anchor].decoded_frames);

    response_arrival_start_us = world.now_us +
        (uint64_t)response_delay_ms * 1000u;
    ret = mesh_sim_schedule_raw_tx(&world,
                                   anchor,
                                   response_arrival_start_us,
                                   UWB_CHANNEL_MESH_PAYLOAD,
                                   MESH_SIM_PHY_CHANNEL9_MESH,
                                   response_frame,
                                   response_frame_len,
                                   false,
                                   &response_tx);
    REQUIRE(ret == MESH_SIM_OK, "schedule assignment claim ret=%d", ret);
    response_arrival_start_us =
        world.transmissions[response_tx].start_us +
        world.propagation_us[anchor][gateway];
    response_arrival_end_us =
        world.transmissions[response_tx].end_us +
        world.propagation_us[anchor][gateway];
    REQUIRE(response_arrival_start_us >
                world.transmissions[command_tx[2]].end_us &&
                response_arrival_end_us <
                world.transmissions[command_tx[3]].start_us,
            "claim airtime [%" PRIu64 ",%" PRIu64
            ") is not between command opportunities [%" PRIu64 ",%" PRIu64 ")",
            response_arrival_start_us,
            response_arrival_end_us,
            world.transmissions[command_tx[2]].end_us,
            world.transmissions[command_tx[3]].start_us);

    if (mode != RESPONSE_RX_NONE) {
        uint64_t rx_start_us =
            world.transmissions[command_tx[2]].end_us + RX_GUARD_US;
        uint64_t rx_end_us =
            world.transmissions[command_tx[3]].start_us - RX_GUARD_US;

        if (mode == RESPONSE_RX_PARTIAL) {
            rx_end_us = response_arrival_end_us - 1u;
        }
        ret = mesh_sim_schedule_rx(&world,
                                   gateway,
                                   rx_start_us,
                                   rx_end_us,
                                   UWB_CHANNEL_MESH_PAYLOAD,
                                   MESH_SIM_PHY_CHANNEL9_MESH,
                                   NULL);
        REQUIRE(ret == MESH_SIM_OK,
                "arm interleaved gateway RX mode=%u ret=%d",
                mode, ret);
    }

    ret = mesh_sim_run(&world);
    REQUIRE(ret == MESH_SIM_OK, "run scenario mode=%u ret=%d last=%d",
            mode, ret, world.last_error);
    for (size_t i = 0u; i < world.reception_count; i++) {
        const struct mesh_sim_reception *reception = &world.receptions[i];

        if (reception->source_id == GATEWAY_ID &&
            reception->receiver_id == ANCHOR_ID) {
            result->command_receptions++;
        }
        if (reception->source_id == ANCHOR_ID &&
            reception->receiver_id == GATEWAY_ID) {
            result->response_receptions++;
            result->response_outcome = reception->outcome;
        }
    }
    if (result->response_receptions == 1u &&
        result->response_outcome == MESH_SIM_RX_DECODED) {
        ret = proto_packet_decode(world.transmissions[response_tx].frame,
                                  world.transmissions[response_tx].frame_len,
                                  &decoded,
                                  &decoded_payload,
                                  &decoded_payload_len);
        REQUIRE(ret == PROTO_OK, "decode received assignment claim ret=%d", ret);
        REQUIRE(decoded.msg_type == MSG_COMMAND_RESULT &&
                    decoded.src_id == ANCHOR_ID &&
                    decoded.dst_id == GATEWAY_ID &&
                    decoded.session_id == COMMAND_SESSION &&
                    decoded_payload_len == response.payload_len,
                "decoded assignment claim envelope is wrong");
        result->response_decoded = true;
    }
    return 0;
}

static int test_gateway_command_yields_for_assignment_claim(void)
{
    struct scenario_result blocked;
    struct scenario_result partial;
    struct scenario_result interleaved;
    int ret;

    ret = run_scenario(RESPONSE_RX_NONE, &blocked);
    REQUIRE(ret == 0, "uninterrupted command scenario ret=%d", ret);
    REQUIRE(blocked.command_receptions == 1u,
            "anchor command receptions=%zu", blocked.command_receptions);
    REQUIRE(blocked.response_receptions == 0u && !blocked.response_decoded,
            "claim decoded without a gateway Channel 9 RX window");

    ret = run_scenario(RESPONSE_RX_PARTIAL, &partial);
    REQUIRE(ret == 0, "partial-window scenario ret=%d", ret);
    REQUIRE(partial.response_receptions == 1u &&
                partial.response_outcome != MESH_SIM_RX_DECODED &&
                !partial.response_decoded,
            "partial claim outcome=%u receptions=%zu decoded=%u",
            partial.response_outcome,
            partial.response_receptions,
            partial.response_decoded);

    ret = run_scenario(RESPONSE_RX_INTERLEAVED, &interleaved);
    REQUIRE(ret == 0, "interleaved-window scenario ret=%d", ret);
    REQUIRE(interleaved.command_receptions == 1u,
            "interleaved anchor command receptions=%zu",
            interleaved.command_receptions);
    REQUIRE(interleaved.response_receptions == 1u &&
                interleaved.response_outcome == MESH_SIM_RX_DECODED &&
                interleaved.response_decoded,
            "interleaved claim outcome=%u receptions=%zu decoded=%u",
            interleaved.response_outcome,
            interleaved.response_receptions,
            interleaved.response_decoded);
    return 0;
}

static int test_direct_route_retry_reenters_control_lane_inside_wake_train(void)
{
    static struct mesh_sim_world world;
    struct mesh_outbound command;
    uint8_t frame[PACKET_EXT_MAX_LEN];
    size_t frame_len = 0u;
    uint64_t wake_start_us = FIRST_COMMAND_TX_US;
    uint64_t sniff_start_us = wake_start_us +
        DIRECT_ROUTE_ATTEMPT_MAX_US + DIRECT_ROUTE_FIRST_BACKOFF_MAX_US;
    uint64_t sniff_end_us = sniff_start_us + DIRECT_ROUTE_RETRY_SNIFF_US;
    uint64_t wake_activity_tx_us = sniff_start_us + RX_GUARD_US;
    uint64_t command_tx_us = wake_start_us +
        (uint64_t)MESH_RADIO_WAKE_TRAIN_MS * 1000u + RX_GUARD_US;
    uint64_t command_end_us;
    uint16_t wake_activity_tx;
    uint16_t command_tx;
    uint8_t gateway;
    uint8_t anchor;
    int ret;

    mesh_sim_init(&world, UINT32_C(0xA55171C5));
    ret = mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                            &gateway);
    REQUIRE(ret == MESH_SIM_OK, "add preemption gateway ret=%d", ret);
    ret = mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            ANCHOR_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                            &anchor);
    REQUIRE(ret == MESH_SIM_OK, "add preemption anchor ret=%d", ret);
    ret = mesh_sim_set_link(&world, gateway, anchor, 100u, 10u);
    REQUIRE(ret == MESH_SIM_OK, "set preemption link ret=%d", ret);
    ret = build_assignment_command(&command);
    REQUIRE(ret == PROTO_OK, "build preemption command ret=%d", ret);
    ret = encode_application_packet(&command.packet,
                                    command.payload,
                                    command.payload_len,
                                    frame,
                                    sizeof(frame),
                                    &frame_len);
    REQUIRE(ret == PROTO_OK, "encode preemption command ret=%d", ret);

    /*
     * Worst phase: the gateway wake train begins immediately after the
     * anchor's previous C5 sniff, so the first direct route probe waits out
     * its full TX/ACK attempt and maximum first backoff. The next mandatory
     * sniff still lands inside the train and observes a complete C5 frame.
     */
    ret = mesh_sim_schedule_rx(&world, anchor,
                               sniff_start_us, sniff_end_us,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, NULL);
    REQUIRE(ret == MESH_SIM_OK, "arm retry-boundary sniff ret=%d", ret);
    ret = mesh_sim_schedule_raw_tx(&world, gateway, wake_activity_tx_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                   frame, frame_len, false,
                                   &wake_activity_tx);
    REQUIRE(ret == MESH_SIM_OK, "schedule wake activity ret=%d", ret);
    REQUIRE(world.transmissions[wake_activity_tx].end_us <= sniff_end_us,
            "wake activity is not fully contained in retry sniff");
    ret = mesh_sim_run_until(&world, sniff_end_us);
    REQUIRE(ret == MESH_SIM_OK && world.roles[anchor].decoded_frames == 1u,
            "retry sniff missed gateway wake activity ret=%d decoded=%" PRIu32,
            ret, world.roles[anchor].decoded_frames);

    command_end_us = command_tx_us +
        mesh_sim_frame_duration_us(MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                   frame_len) +
        world.propagation_us[gateway][anchor] + RX_GUARD_US;
    ret = mesh_sim_schedule_rx(&world, anchor,
                               sniff_end_us, command_end_us,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, NULL);
    REQUIRE(ret == MESH_SIM_OK, "arm post-wake command RX ret=%d", ret);
    ret = mesh_sim_schedule_raw_tx(&world, gateway, command_tx_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                   frame, frame_len, false, &command_tx);
    REQUIRE(ret == MESH_SIM_OK, "schedule post-wake command ret=%d", ret);
    ret = mesh_sim_run(&world);
    REQUIRE(ret == MESH_SIM_OK && world.roles[anchor].decoded_frames == 2u,
            "gateway command did not survive route-probe preemption ret=%d decoded=%" PRIu32,
            ret, world.roles[anchor].decoded_frames);
    REQUIRE(world.transmissions[command_tx].end_us +
                world.propagation_us[gateway][anchor] <= command_end_us,
            "gateway command airtime escaped bounded RX window");
    return 0;
}

static int test_two_simultaneous_relays_diversify_three_copy_schedule(void)
{
    static struct mesh_sim_world world;
    struct mesh_outbound command;
    uint8_t frame[PACKET_EXT_MAX_LEN];
    size_t frame_len = 0u;
    uint64_t starts_a[3] = {FIRST_COMMAND_TX_US};
    uint64_t starts_b[3] = {FIRST_COMMAND_TX_US};
    uint64_t frame_duration_us;
    uint64_t rx_end_us;
    uint8_t relay_a;
    uint8_t relay_b;
    uint8_t downstream;
    size_t decoded = 0u;
    int ret;

    mesh_sim_init(&world, UINT32_C(0xA55171D5));
    ret = mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            RELAY_A_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                            &relay_a);
    REQUIRE(ret == MESH_SIM_OK, "add relay A ret=%d", ret);
    ret = mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            RELAY_B_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                            &relay_b);
    REQUIRE(ret == MESH_SIM_OK, "add relay B ret=%d", ret);
    ret = mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            DOWNSTREAM_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                            &downstream);
    REQUIRE(ret == MESH_SIM_OK, "add downstream ret=%d", ret);
    REQUIRE(mesh_sim_set_link(&world, relay_a, downstream, 100u, 10u) ==
                MESH_SIM_OK,
            "set relay A link");
    REQUIRE(mesh_sim_set_link(&world, relay_b, downstream, 100u, 10u) ==
                MESH_SIM_OK,
            "set relay B link");
    REQUIRE(build_assignment_command(&command) == PROTO_OK,
            "build relay command");
    REQUIRE(encode_application_packet(&command.packet,
                                      command.payload,
                                      command.payload_len,
                                      frame,
                                      sizeof(frame),
                                      &frame_len) == PROTO_OK,
            "encode relay command");
    frame_duration_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, frame_len);
    for (uint8_t copy = 1u; copy < 3u; copy++) {
        starts_a[copy] = starts_a[copy - 1u] + frame_duration_us +
            ((uint64_t)FLOOD_POST_ROOT_GUARD_MS +
             mesh_flood_copy_diversification_ms(
                 RELAY_A_ID, &command.packet, copy)) * 1000u;
        starts_b[copy] = starts_b[copy - 1u] + frame_duration_us +
            ((uint64_t)FLOOD_POST_ROOT_GUARD_MS +
             mesh_flood_copy_diversification_ms(
                 RELAY_B_ID, &command.packet, copy)) * 1000u;
    }
    REQUIRE(starts_a[1] != starts_b[1] || starts_a[2] != starts_b[2],
            "node-specific repeat schedule remained collision-locked");
    rx_end_us = (starts_a[2] > starts_b[2] ? starts_a[2] : starts_b[2]) +
        frame_duration_us + RX_GUARD_US;
    REQUIRE(mesh_sim_schedule_rx(
                &world, downstream,
                FIRST_COMMAND_TX_US - RX_GUARD_US,
                rx_end_us,
                UWB_CHANNEL_WAKE_CONTACT,
                MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                NULL) == MESH_SIM_OK,
            "arm downstream continuous RX");
    for (uint8_t copy = 0u; copy < 3u; copy++) {
        REQUIRE(mesh_sim_schedule_raw_tx(
                    &world, relay_a, starts_a[copy],
                    UWB_CHANNEL_WAKE_CONTACT,
                    MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                    frame, frame_len, false, NULL) == MESH_SIM_OK,
                "schedule relay A copy=%u", copy);
        REQUIRE(mesh_sim_schedule_raw_tx(
                    &world, relay_b, starts_b[copy],
                    UWB_CHANNEL_WAKE_CONTACT,
                    MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                    frame, frame_len, false, NULL) == MESH_SIM_OK,
                "schedule relay B copy=%u", copy);
    }
    REQUIRE(mesh_sim_run(&world) == MESH_SIM_OK,
            "run diversified relay scenario");
    for (size_t i = 0u; i < world.reception_count; i++) {
        if (world.receptions[i].receiver_id == DOWNSTREAM_ID &&
            world.receptions[i].outcome == MESH_SIM_RX_DECODED) {
            decoded++;
        }
    }
    REQUIRE(decoded >= 1u,
            "all three copies stayed collision-locked decoded=%zu", decoded);
    return 0;
}

static int test_two_here_i_am_relays_do_not_collision_lock_hidden_child(void)
{
    static struct mesh_sim_world world;
    const struct uwb_anchor_config downstream_config = {
        .network_id = UINT32_C(0x494d4543),
        .anchor_id = DOWNSTREAM_ID,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
    };
    struct mesh_relay gateway_model;
    struct mesh_relay relay_a_model;
    struct mesh_relay relay_b_model;
    struct mesh_relay relay_a_repeat_model;
    struct mesh_anchor_downlink_store relay_a_store;
    struct mesh_anchor_downlink_store relay_b_store;
    struct mesh_anchor_downlink_store relay_a_repeat_store;
    struct mesh_outbound root_advertisement;
    struct mesh_relay_result result_a;
    struct mesh_relay_result result_b;
    struct mesh_relay_result result_a_repeat;
    const struct mesh_outbound *forwards[2];
    uint8_t encoded[2][PACKET_EXT_MAX_LEN];
    size_t encoded_len[2] = {0u, 0u};
    uint32_t frame_duration_us[2] = {0u, 0u};
    uint64_t control_starts[2][MESH_GATEWAY_ROUTE_ADV_COPY_COUNT];
    uint64_t wake_end_a = 0u;
    uint64_t wake_end_b = 0u;
    uint64_t wake_start_a;
    uint64_t wake_start_b;
    uint64_t latest_wake_end;
    uint64_t rx_start_us = UINT64_MAX;
    uint64_t rx_end_us = 0u;
    uint32_t delay_a_ms;
    uint32_t delay_b_ms;
    uint32_t first_due_ms;
    uint8_t gateway;
    uint8_t relay_a;
    uint8_t relay_b;
    uint8_t downstream;
    int ret;

    mesh_relay_init(&gateway_model,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY_ID,
                    GATEWAY_ID,
                    ASSIGNMENT_EPOCH);
    mesh_relay_init(&relay_a_model,
                    MESH_RELAY_ROLE_ANCHOR,
                    RELAY_A_ID,
                    GATEWAY_ID,
                    ASSIGNMENT_EPOCH);
    mesh_relay_init(&relay_b_model,
                    MESH_RELAY_ROLE_ANCHOR,
                    RELAY_B_ID,
                    GATEWAY_ID,
                    ASSIGNMENT_EPOCH);
    mesh_relay_init(&relay_a_repeat_model,
                    MESH_RELAY_ROLE_ANCHOR,
                    RELAY_A_ID,
                    GATEWAY_ID,
                    ASSIGNMENT_EPOCH);
    REQUIRE(mesh_relay_attach_anchor_downlink_store(
                &relay_a_model, &relay_a_store) == PROTO_OK,
            "attach relay A route store");
    REQUIRE(mesh_relay_attach_anchor_downlink_store(
                &relay_b_model, &relay_b_store) == PROTO_OK,
            "attach relay B route store");
    REQUIRE(mesh_relay_attach_anchor_downlink_store(
                &relay_a_repeat_model, &relay_a_repeat_store) == PROTO_OK,
            "attach repeated relay A route store");
    REQUIRE(build_enumeration_route_advertisement(
                &gateway_model, &root_advertisement) == PROTO_OK,
            "build enumeration Here-I-Am");

    REQUIRE(mesh_relay_handle_rx_with_random(
                &relay_a_model,
                &root_advertisement.packet,
                root_advertisement.payload,
                root_advertisement.payload_len,
                GATEWAY_ID,
                90u,
                ROUTE_ADV_RECEIVED_AT_MS,
                ROUTE_ADV_RANDOM,
                &result_a) == PROTO_OK &&
                result_a.status == PROTO_OK &&
                (result_a.actions &
                 MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV) != 0u,
            "relay A did not accept the Here-I-Am");
    REQUIRE(mesh_relay_handle_rx_with_random(
                &relay_b_model,
                &root_advertisement.packet,
                root_advertisement.payload,
                root_advertisement.payload_len,
                GATEWAY_ID,
                90u,
                ROUTE_ADV_RECEIVED_AT_MS,
                ROUTE_ADV_RANDOM,
                &result_b) == PROTO_OK &&
                result_b.status == PROTO_OK &&
                (result_b.actions &
                 MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV) != 0u,
            "relay B did not accept the Here-I-Am");
    REQUIRE(mesh_relay_handle_rx_with_random(
                &relay_a_repeat_model,
                &root_advertisement.packet,
                root_advertisement.payload,
                root_advertisement.payload_len,
                GATEWAY_ID,
                90u,
                ROUTE_ADV_RECEIVED_AT_MS,
                ROUTE_ADV_RANDOM,
                &result_a_repeat) == PROTO_OK &&
                result_a_repeat.status == PROTO_OK,
            "repeated relay A schedule setup failed");

    REQUIRE(result_a.gateway_route_adv.earliest_tx_valid &&
                result_b.gateway_route_adv.earliest_tx_valid &&
                result_a_repeat.gateway_route_adv.earliest_tx_valid,
            "Here-I-Am relay omitted its first-TX deadline");
    REQUIRE(result_a.gateway_route_adv.earliest_tx_ms ==
                result_a_repeat.gateway_route_adv.earliest_tx_ms,
            "same node/packet/random input produced a different relay slot");
    REQUIRE(result_a.gateway_route_adv.earliest_tx_ms >=
                    result_a.gateway_route_adv.route_wave_start_ms +
                        MESH_GATEWAY_ROUTE_ACTIVATION_ENVELOPE_MS &&
                result_b.gateway_route_adv.earliest_tx_ms >=
                    result_b.gateway_route_adv.route_wave_start_ms +
                        MESH_GATEWAY_ROUTE_ACTIVATION_ENVELOPE_MS,
            "first typed Here-I-Am starts before the relay activation envelope");
    delay_a_ms = result_a.gateway_route_adv.earliest_tx_ms -
                 result_a.gateway_route_adv.route_wave_start_ms -
                 MESH_GATEWAY_ROUTE_ACTIVATION_ENVELOPE_MS;
    delay_b_ms = result_b.gateway_route_adv.earliest_tx_ms -
                 result_b.gateway_route_adv.route_wave_start_ms -
                 MESH_GATEWAY_ROUTE_ACTIVATION_ENVELOPE_MS;
    REQUIRE(delay_a_ms <= MESH_GATEWAY_ROUTE_ADV_FIRST_COPY_JITTER_MAX_MS &&
                delay_b_ms <= MESH_GATEWAY_ROUTE_ADV_FIRST_COPY_JITTER_MAX_MS,
            "Here-I-Am relay deadlines escaped the 1.5-second window");
    REQUIRE(delay_a_ms != delay_b_ms,
            "distinct relay IDs remained in the same deterministic wake slot");

    /* First exercise the actual low-duty wake boundary. Both relays are
     * direct to the gateway, the child hears both relays, and the child has no
     * gateway link. */
    mesh_sim_init(&world, UINT32_C(0xA55171D6));
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                              GATEWAY_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                              &gateway) == MESH_SIM_OK,
            "add Here-I-Am gateway");
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                              RELAY_A_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                              &relay_a) == MESH_SIM_OK,
            "add Here-I-Am relay A");
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                              RELAY_B_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                              &relay_b) == MESH_SIM_OK,
            "add Here-I-Am relay B");
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                              DOWNSTREAM_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                              &downstream) == MESH_SIM_OK,
            "add RF-hidden downstream anchor");
    REQUIRE(mesh_sim_set_link(&world, gateway, relay_a, 100u, 10u) ==
                MESH_SIM_OK &&
                mesh_sim_set_link(&world, gateway, relay_b, 100u, 10u) ==
                MESH_SIM_OK &&
                mesh_sim_set_link(&world, relay_a, downstream, 100u, 10u) ==
                MESH_SIM_OK &&
                mesh_sim_set_link(&world, relay_b, downstream, 100u, 10u) ==
                MESH_SIM_OK,
            "build direct-parent/RF-hidden-child topology");
    REQUIRE(!world.reachable[gateway][downstream],
            "RF-hidden child unexpectedly has a gateway link");
    REQUIRE(mesh_sim_init_anchor_session(
                &world, downstream, &downstream_config) == PROTO_OK,
            "initialize RF-hidden low-duty listener");
    REQUIRE(mesh_sim_start_anchor_low_duty(&world, downstream, 0u) ==
                MESH_SIM_OK &&
                mesh_sim_run_until(&world, 0u) == MESH_SIM_OK,
            "start RF-hidden low-duty listener");

    wake_start_a =
        (uint64_t)(result_a.gateway_route_adv.route_wave_start_ms +
            mesh_gateway_route_activation_start_offset_ms(
                RELAY_A_ID,
                result_a.gateway_route_adv.packet.session_id,
                1u)) * 1000u;
    wake_start_b =
        (uint64_t)(result_b.gateway_route_adv.route_wave_start_ms +
            mesh_gateway_route_activation_start_offset_ms(
                RELAY_B_ID,
                result_b.gateway_route_adv.packet.session_id,
                1u)) * 1000u;
    REQUIRE(schedule_route_adv_wake_train(
                &world, relay_a, wake_start_a, &wake_end_a) == MESH_SIM_OK,
            "schedule relay A activation train");
    REQUIRE(schedule_route_adv_wake_train(
                &world, relay_b, wake_start_b, &wake_end_b) == MESH_SIM_OK,
            "schedule relay B activation train");
    latest_wake_end = wake_end_a > wake_end_b ? wake_end_a : wake_end_b;
    REQUIRE(mesh_sim_run_until(&world, latest_wake_end + 1u) == MESH_SIM_OK,
            "run simultaneous-parent activation trains");
    REQUIRE(decoded_receptions_from(&world,
                                    RELAY_A_ID,
                                    DOWNSTREAM_ID,
                                    MESH_SIM_PHY_CHANNEL5_WAKE) +
                decoded_receptions_from(&world,
                                        RELAY_B_ID,
                                        DOWNSTREAM_ID,
                                        MESH_SIM_PHY_CHANNEL5_WAKE) > 0u,
            "all relayed activation copies remained collision-locked");
    REQUIRE(world.roles[downstream].anchor_session.diagnostics.claims > 0u,
            "RF-hidden child never accepted a relayed activation claim");

    /* Reinitialize only the radio world and preserve the real forwarded wire
     * images and their relative production slots. A continuous listener here
     * represents the prearm lease established by the preceding wake. */
    mesh_sim_init(&world, UINT32_C(0xA55171D7));
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY,
                              GATEWAY_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                              &gateway) == MESH_SIM_OK,
            "add control-wave gateway");
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                              RELAY_A_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                              &relay_a) == MESH_SIM_OK,
            "add control-wave relay A");
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                              RELAY_B_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                              &relay_b) == MESH_SIM_OK,
            "add control-wave relay B");
    REQUIRE(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                              DOWNSTREAM_ID, GATEWAY_ID, ASSIGNMENT_EPOCH,
                              &downstream) == MESH_SIM_OK,
            "add control-wave downstream anchor");
    REQUIRE(mesh_sim_set_link(&world, gateway, relay_a, 100u, 10u) ==
                MESH_SIM_OK &&
                mesh_sim_set_link(&world, gateway, relay_b, 100u, 10u) ==
                MESH_SIM_OK &&
                mesh_sim_set_link(&world, relay_a, downstream, 100u, 10u) ==
                MESH_SIM_OK &&
                mesh_sim_set_link(&world, relay_b, downstream, 100u, 10u) ==
                MESH_SIM_OK,
            "rebuild direct-parent/RF-hidden-child topology");
    REQUIRE(!world.reachable[gateway][downstream],
            "control-wave child unexpectedly has a gateway link");

    forwards[0] = &result_a.gateway_route_adv;
    forwards[1] = &result_b.gateway_route_adv;
    first_due_ms = forwards[0]->earliest_tx_ms < forwards[1]->earliest_tx_ms ?
        forwards[0]->earliest_tx_ms : forwards[1]->earliest_tx_ms;
    for (uint8_t relay = 0u; relay < 2u; relay++) {
        ret = encode_application_packet(&forwards[relay]->packet,
                                        forwards[relay]->payload,
                                        forwards[relay]->payload_len,
                                        encoded[relay],
                                        sizeof(encoded[relay]),
                                        &encoded_len[relay]);
        REQUIRE(ret == PROTO_OK,
                "encode forwarded Here-I-Am relay=%u ret=%d", relay, ret);
        frame_duration_us[relay] = mesh_sim_frame_duration_us(
            MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, encoded_len[relay]);
        REQUIRE(frame_duration_us[relay] > 0u,
                "forwarded Here-I-Am has no modeled airtime relay=%u", relay);
        for (uint8_t copy = 0u;
             copy < MESH_GATEWAY_ROUTE_ADV_COPY_COUNT;
             copy++) {
            uint32_t copy_due_ms =
                forwards[relay]->route_wave_start_ms +
                MESH_GATEWAY_ROUTE_ACTIVATION_ENVELOPE_MS +
                mesh_gateway_route_adv_copy_offset_ms(
                    relay == 0u ? RELAY_A_ID : RELAY_B_ID,
                    forwards[relay]->packet.session_id,
                    1u,
                    copy);

            control_starts[relay][copy] = FIRST_COMMAND_TX_US +
                (uint64_t)(copy_due_ms - first_due_ms) * 1000u;
            REQUIRE(copy_due_ms + OPERATION_POLICY_RESPONSE_TX_TIMEOUT_MS <=
                        forwards[relay]->route_wave_start_ms +
                            MESH_GATEWAY_ROUTE_ACTIVATION_ENVELOPE_MS +
                            ((uint32_t)copy + 1u) *
                                MESH_GATEWAY_ROUTE_ADV_COPY_SPACING_MS,
                    "Here-I-Am copy escaped its 500 ms stratum");
        }
        if (control_starts[relay][0] < rx_start_us) {
            rx_start_us = control_starts[relay][0];
        }
        if (control_starts[relay][MESH_GATEWAY_ROUTE_ADV_COPY_COUNT - 1u] +
                frame_duration_us[relay] + 10u > rx_end_us) {
            rx_end_us =
                control_starts[relay]
                              [MESH_GATEWAY_ROUTE_ADV_COPY_COUNT - 1u] +
                frame_duration_us[relay] + 10u;
        }
    }
    REQUIRE(rx_start_us >= RX_GUARD_US,
            "control listener guard underflow");
    REQUIRE(mesh_sim_schedule_rx(&world,
                                 downstream,
                                 rx_start_us - RX_GUARD_US,
                                 rx_end_us + RX_GUARD_US,
                                 UWB_CHANNEL_WAKE_CONTACT,
                                 MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                 NULL) == MESH_SIM_OK,
            "arm prearmed child for both Here-I-Am bursts");
    for (uint8_t relay = 0u; relay < 2u; relay++) {
        uint8_t sender = relay == 0u ? relay_a : relay_b;

        for (uint8_t copy = 0u;
             copy < MESH_GATEWAY_ROUTE_ADV_COPY_COUNT;
             copy++) {
            REQUIRE(mesh_sim_schedule_raw_tx(
                        &world,
                        sender,
                        control_starts[relay][copy],
                        UWB_CHANNEL_WAKE_CONTACT,
                        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                        encoded[relay],
                        encoded_len[relay],
                        false,
                        NULL) == MESH_SIM_OK,
                    "schedule Here-I-Am relay=%u copy=%u", relay, copy);
        }
    }
    REQUIRE(mesh_sim_run(&world) == MESH_SIM_OK,
            "run staggered Here-I-Am copy bursts");
    REQUIRE(decoded_receptions_from(&world,
                                    RELAY_A_ID,
                                    DOWNSTREAM_ID,
                                    MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) > 0u &&
                decoded_receptions_from(
                    &world,
                    RELAY_B_ID,
                    DOWNSTREAM_ID,
                    MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) > 0u,
            "one current-wave parent remained collision-locked");
    return 0;
}

int main(void)
{
    int ret = test_gateway_command_yields_for_assignment_claim();

    if (ret != 0) {
        return ret;
    }
    ret = test_direct_route_retry_reenters_control_lane_inside_wake_train();
    if (ret != 0) {
        return ret;
    }
    ret = test_two_simultaneous_relays_diversify_three_copy_schedule();
    if (ret != 0) {
        return ret;
    }
    ret = test_two_here_i_am_relays_do_not_collision_lock_hidden_child();
    if (ret != 0) {
        return ret;
    }
    puts("mesh assignment control timing scenarios: PASS");
    return 0;
}

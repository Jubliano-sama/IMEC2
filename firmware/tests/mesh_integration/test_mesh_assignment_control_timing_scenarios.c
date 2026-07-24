#include "app_mesh_direct_gateway_retry.h"
#include "discovery_assignment.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_radio_timing.h"
#include "mesh_relay.h"
#include "mesh_sim.h"
#include "protocol.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GATEWAY_ID UINT64_C(0xA501000000000001)
#define ANCHOR_ID UINT64_C(0xA502000000000001)
#define ASSIGNMENT_EPOCH UINT32_C(0xA5510001)
#define COMMAND_SESSION UINT32_C(0xA5510002)
#define COMMAND_SEQUENCE UINT16_C(1)
#define RESPONSE_SEQUENCE UINT16_C(2)
#define FIRST_COMMAND_TX_US UINT64_C(1000)
#define RX_GUARD_US UINT64_C(100)
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
    puts("mesh assignment control timing scenarios: PASS");
    return 0;
}
